/*
 * audio_follow.cpp - TeamSpeak 3 插件：跟随系统默认音频输出设备
 *
 * 功能：当用户在 Windows 任务栏右下角（或系统设置）切换默认音频输出设备时，
 *      自动把 TeamSpeak 3 的播放（输出）设备同步切换到同一个设备。
 *
 * 平台：Windows 7 ~ Windows 11（x64）
 * SDK ：TeamSpeak 3 Client Plugin SDK, API Version 26（要求 TS3 客户端 >= 3.6.0）
 *
 * 实现原理：
 *   1. 通过 Windows Core Audio API (WASAPI) 注册 IMMNotificationClient，
 *      监听系统默认播放设备的变化（OnDefaultDeviceChanged, eRender/eConsole）。
 *   2. 该回调运行在系统音频线程，不能直接调用 TS3 API，
 *      因此通过 PostMessage 把新的设备 ID 投递到隐藏窗口，
 *      在 TS3 客户端主线程的窗口过程中执行切换。
 *   3. 切换调用 ts3Functions.openPlaybackDevice(handler, mode, deviceId)，
 *      对所有已连接的服务器连接生效。
 *
 * 编译：见 Makefile（mingw-w64 交叉编译）或 build_windows.bat（MSVC）。
 * 配置：audio_follow.ini 位于 TS3 插件目录，可通过右键菜单"音频跟随设置..."修改。
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

/* ---------- TeamSpeak 3 SDK 头文件 ---------- */
#include "ts3_functions.h"
#include "teamspeak/public_definitions.h"
#include "teamspeak/public_errors.h"
#include "teamspeak/public_errors_rare.h"
#include "teamlog/logtypes.h"

/* 对话框控件 ID（取代 .rc，避免 windres 编码问题） */
#define IDC_CHECK_ENABLED 1001
#define IDC_CHECK_LOG     1002

/* ---------- 插件元数据 ---------- */
#define PLUGIN_API_VERSION 26
#define PLUGIN_NAME        "SystemAudioFollow"
#define PLUGIN_VERSION     "1.1.0"
#define PLUGIN_AUTHOR      "WorkBuddy"
#define PLUGIN_DESCRIPTION "Windows 默认输出设备切换时，自动同步 TeamSpeak 3 的输出设备"
#define CONFIG_FILENAME    "audio_follow.ini"

/* 自定义窗口消息 */
#define WM_APP_FOLLOW (WM_APP + 1)   /* 系统默认设备已切换，lParam = char* 设备ID(需free) */
#define WM_APP_SYNC   (WM_APP + 2)   /* 请求在隐藏窗口线程中同步当前默认设备 */

/* ---------- 全局状态 ---------- */
static struct TS3Functions ts3Functions;   /* 客户端注入的函数指针表 */
static bool    g_hasFunctions = false;      /* setFunctionPointers 是否已调用 */
static const char* pluginID = NULL;         /* 客户端分配的插件 ID */

static HINSTANCE g_instance = NULL;         /* DLL 实例句柄 */
static volatile HWND g_hwnd = NULL;         /* 隐藏窗口句柄（跨线程读，volatile 保证可见性） */
static bool      g_wndClassRegistered = false;

static bool      g_enabled = true;          /* 是否启用跟随 */
static bool      g_logEnabled = true;       /* 是否输出日志 */
static std::string g_configPath;            /* 配置文件完整路径 */

static IMMDeviceEnumerator* g_enumerator = NULL;   /* WASAPI 设备枚举器 */
static IMMNotificationClient* g_listener = NULL;   /* 设备变化监听器 */
static bool      g_comInitialized = false;         /* COM 是否由本插件初始化 */

/* ---------- 工具函数 ---------- */

static char* WideToUtf8(const wchar_t* wstr)
{
    if (!wstr) return NULL;
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    if (len <= 0) return NULL;
    char* buf = (char*)malloc(len);
    if (!buf) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, buf, len, NULL, NULL);
    return buf;
}

/* 把 TS3 错误码转换为可读文本（用客户端提供的 getErrorMessage） */
static const char* GetErrorString(unsigned int code)
{
    static thread_local char buf[256];
    buf[0] = '\0';
    if (g_hasFunctions) {
        char* err = NULL;
        if (ts3Functions.getErrorMessage(code, &err) == ERROR_ok && err) {
            strncpy(buf, err, sizeof(buf) - 1);
            ts3Functions.freeMemory(err);
        }
    }
    if (!buf[0]) snprintf(buf, sizeof(buf), "unknown error %u", code);
    return buf;
}

/* UTF-8 -> UTF-16（辅助） */
static std::wstring Utf8ToWide(const char* str)
{
    std::wstring wstr;
    if (!str) return wstr;
    int len = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
    if (len <= 0) return wstr;
    wstr.resize(len - 1);   /* len 含 null terminator，resize 去掉它 */
    MultiByteToWideChar(CP_UTF8, 0, str, -1, &wstr[0], len);
    return wstr;
}

/* 读取 WASAPI 设备的友好名称（如 "耳机 (Realtek(R) Audio)"） */
static std::wstring GetDeviceFriendlyName(IMMDevice* device)
{
    std::wstring name;
    if (!device) return name;
    IPropertyStore* store = NULL;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
        PROPVARIANT var;
        PropVariantInit(&var);
        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &var)) &&
            var.vt == VT_LPWSTR && var.pwszVal) {
            name = var.pwszVal;
        }
        PropVariantClear(&var);
        store->Release();
    }
    return name;
}

/* 向 TS3 客户端日志输出一行（仅当启用日志时） */
static void Log(const char* fmt, ...)
{
    if (!g_logEnabled) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_hasFunctions) {
        ts3Functions.logMessage(buf, LogLevel_INFO, "SystemAudioFollow", 0);
    }
}

/* ---------- 配置文件读写（%APPDATA%/TS3Client/plugins/audio_follow.ini） ---------- */

static std::string GetConfigPath()
{
    char path[MAX_PATH] = { 0 };
    if (g_hasFunctions) {
        ts3Functions.getPluginPath(path, sizeof(path), pluginID ? pluginID : "");
    }
    size_t len = strlen(path);
    if (len == 0) {
        /* 兜底：用 TS3 可执行文件所在目录 */
        GetModuleFileNameA(NULL, path, sizeof(path));
        char* slash = strrchr(path, '\\');
        if (slash) *slash = '\0';
        len = strlen(path);
    }
    /* 确保路径以分隔符结尾，再拼接文件名（用 snprintf 防止溢出） */
    if (len > 0 && path[len - 1] != '\\' && path[len - 1] != '/') {
        snprintf(path + len, sizeof(path) - len, "\\%s", CONFIG_FILENAME);
    } else {
        snprintf(path + len, sizeof(path) - len, "%s", CONFIG_FILENAME);
    }
    return std::string(path);
}

static void LoadConfig()
{
    g_configPath = GetConfigPath();
    g_enabled    = GetPrivateProfileIntA("General", "Enabled",    1, g_configPath.c_str()) != 0;
    g_logEnabled = GetPrivateProfileIntA("General", "LogEnabled", 1, g_configPath.c_str()) != 0;
}

static void SaveConfig()
{
    if (g_configPath.empty()) g_configPath = GetConfigPath();
    char v[4];
    sprintf(v, "%d", g_enabled ? 1 : 0);
    WritePrivateProfileStringA("General", "Enabled", v, g_configPath.c_str());
    sprintf(v, "%d", g_logEnabled ? 1 : 0);
    WritePrivateProfileStringA("General", "LogEnabled", v, g_configPath.c_str());
}

/* ---------- 核心：把 TS3 所有已连接服务器的输出设备切换为指定设备 ---------- */

/* 诊断：把 TS3 当前播放模式下的设备列表打印到日志（ID + 名称）。
 * 注意：TS3 设备列表条目格式为「名称\0ID\0」，即 [0]=名称 [1]=ID。 */
static void DumpDeviceList(const char* mode)
{
    char*** list = NULL;
    if (ts3Functions.getPlaybackDeviceList(mode, &list) != ERROR_ok || !list) {
        Log("getPlaybackDeviceList(%s) failed", mode);
        return;
    }
    Log("TS3 playback device list (mode=%s):", mode);
    for (int i = 0; list[i] != NULL; i++) {
        const char* name = list[i][0] ? list[i][0] : "";
        const char* id   = list[i][1] ? list[i][1] : "";
        Log("  [%d] id=%s | name=%s", i, id, name);
    }
    ts3Functions.freeMemory(list);
}

/* 在 TS3 播放设备列表中查找与目标（WASAPI 默认设备）匹配的设备。
 * 注意：TS3 设备列表条目格式为「名称\0ID\0」，即 [0]=名称 [1]=ID。
 * 匹配策略（依次尝试）：
 *   1. ID 精确匹配（忽略大小写）
 *   2. ID 子串匹配（TS3 列表中的 ID 可能只含 {GUID} 部分）
 *   3. 设备名称匹配（最可靠：不依赖 ID 格式） */
struct Ts3DeviceRef {
    std::string id;    /* TS3 设备 ID（openPlaybackDevice 用它打开设备） */
    std::string name;  /* TS3 设备显示名称（日志用） */
};

static bool FindTs3Device(const char* mode, const char* wasapiId,
                          const std::wstring& wasapiName, Ts3DeviceRef& out)
{
    char*** list = NULL;
    if (ts3Functions.getPlaybackDeviceList(mode, &list) != ERROR_ok || !list) return false;

    std::string guidPart;   /* wasapiId 中的 {GUID} 部分（注意：设备 ID 形如
                             * "{0.0.0.00000000}.{GUID}"，有两组花括号，
                             * 必须取「最后一组」才是设备 GUID） */
    if (wasapiId) {
        const char* brace = strrchr(wasapiId, '{');   /* 最后一个 '{' */
        if (brace) {
            const char* end = strchr(brace, '}');
            if (end) guidPart.assign(brace, end - brace + 1);
        }
    }

    bool found = false;
    for (int i = 0; list[i] != NULL; i++) {
        const char* ts3Name = list[i][0] ? list[i][0] : "";   /* 条目[0]=名称 */
        const char* ts3Id   = list[i][1] ? list[i][1] : "";   /* 条目[1]=ID */

        /* 策略1：ID 精确匹配 */
        if (wasapiId && *ts3Id && _stricmp(ts3Id, wasapiId) == 0) {
            out.id = ts3Id; out.name = ts3Name; found = true; break;
        }
        /* 策略2：GUID 子串匹配（TS3 的 ID 可能是 {GUID} 或带前缀的完整 WASAPI ID） */
        if (!guidPart.empty() && strstr(ts3Id, guidPart.c_str()) != NULL) {
            out.id = ts3Id; out.name = ts3Name; found = true; break;
        }
        /* 策略3：名称匹配（WASAPI 友好名 vs TS3 设备名，UTF-8 -> UTF-16 后比较） */
        if (!wasapiName.empty() && *ts3Name) {
            std::wstring ts3NameW = Utf8ToWide(ts3Name);
            if (_wcsicmp(ts3NameW.c_str(), wasapiName.c_str()) == 0) {
                out.id = ts3Id; out.name = ts3Name; found = true; break;
            }
        }
    }
    ts3Functions.freeMemory(list);
    return found;
}

/* 从 TS3 的 "字符串1\0字符串2\0" 双 null 结尾格式中取第 n 个字段（0-based）。
 * 注意：不同 TS3 API 的字段顺序不同！
 *   - getPlaybackDeviceList 返回「名称\0ID\0」：[0]=名称 [1]=ID
 *   - getCurrentPlaybackDeviceName 返回「ID\0名称\0」：[0]=ID [1]=名称
 * 调用方必须根据具体 API 自行确定字段顺序。 */
static std::string FieldN(const char* pair, int n)
{
    if (!pair || n < 0) return "";
    const char* p = pair;
    for (int i = 0; i < n; i++) {
        size_t len = strlen(p);
        if (p[len + 1] == '\0') return "";   /* 双 null 结尾：没有更多字段 */
        p += len + 1;
    }
    return std::string(p);
}

/* 切换单个连接的处理设备：先关闭旧设备，再打开目标设备；失败则恢复旧设备。
 * 关键：openPlaybackDevice 的设备参数使用设备 ID（SDK 文档与成熟插件均如此，
 * 传显示名称会报 2321 device not registered/known）。 */
static bool SwitchHandlerDevice(uint64 handler, const char* mode,
                                const Ts3DeviceRef& target)
{
    /* 当前设备（格式「名称\0ID\0」） */
    char* cur = NULL;
    int isDefault = 0;
    if (ts3Functions.getCurrentPlaybackDeviceName(handler, &cur, &isDefault) != ERROR_ok || !cur) {
        /* 无当前设备：直接打开目标（用 ID） */
        Log("[diag] handler=%llu no current device, opening target", (unsigned long long)handler);
        unsigned int err = ts3Functions.openPlaybackDevice(handler, mode, target.id.c_str());
        if (err != ERROR_ok) {
            Log("openPlaybackDevice(%s) failed: %u (%s)",
                target.name.c_str(), err, GetErrorString(err));
            return false;
        }
        Log("Output device opened: %s (%s)", target.name.c_str(), target.id.c_str());
        return true;
    }

    /* getCurrentPlaybackDeviceName 返回「ID\0NAME\0」：第一个字段是设备 ID，
     * 第二个是名称（与 getPlaybackDeviceList 的「名称\0ID\0」相反，实测确认） */
    std::string curId   = FieldN(cur, 0);   /* 第一个字段 = ID */
    std::string curName = FieldN(cur, 1);   /* 第二个字段 = 名称 */
    if (curId.empty())   curId   = cur;
    if (curName.empty()) curName = cur;

    Log("[diag] handler=%llu current device: id=%s (name=%s), target: id=%s (name=%s)",
        (unsigned long long)handler, curId.c_str(), curName.c_str(),
        target.id.c_str(), target.name.c_str());

    /* 已是指定设备则不动（按 ID 比较为主，名称兜底） */
    if ((!target.id.empty()   && _stricmp(curId.c_str(),   target.id.c_str())   == 0) ||
        (!target.name.empty() && _stricmp(curName.c_str(), target.name.c_str()) == 0)) {
        Log("[diag] handler=%llu already on target device, skip", (unsigned long long)handler);
        ts3Functions.freeMemory(cur);
        return true;
    }

    /* 关闭旧设备 */
    unsigned int closeErr = ts3Functions.closePlaybackDevice(handler);
    if (closeErr != ERROR_ok) {
        Log("closePlaybackDevice failed: %u (%s)", closeErr, GetErrorString(closeErr));
        ts3Functions.freeMemory(cur);
        return false;
    }
    Log("[diag] handler=%llu old device closed", (unsigned long long)handler);

    /* 打开新设备（用 ID） */
    unsigned int err = ts3Functions.openPlaybackDevice(handler, mode, target.id.c_str());
    if (err != ERROR_ok) {
        Log("openPlaybackDevice(%s) failed: %u (%s) - restoring previous device",
            target.name.c_str(), err, GetErrorString(err));
        unsigned int rerr = ts3Functions.openPlaybackDevice(handler, mode, curId.c_str());
        if (rerr != ERROR_ok) {
            Log("FAILED to restore previous device (%s): %u (%s)",
                curName.c_str(), rerr, GetErrorString(rerr));
        }
        ts3Functions.freeMemory(cur);
        return false;
    }

    /* 验证切换结果 */
    char* after = NULL;
    int afterIsDefault = 0;
    if (ts3Functions.getCurrentPlaybackDeviceName(handler, &after, &afterIsDefault) == ERROR_ok && after) {
        std::string afterId = FieldN(after, 0);
        std::string afterName = FieldN(after, 1);
        if (afterId.empty()) afterId = after;
        if (afterName.empty()) afterName = after;
        Log("Output device switched: %s -> %s (now: %s)", curName.c_str(), target.name.c_str(), afterName.c_str());
        if (_stricmp(afterId.c_str(), target.id.c_str()) != 0) {
            Log("[warn] TS3 actual device (%s) differs from target (%s) after switch",
                afterName.c_str(), target.name.c_str());
        }
        ts3Functions.freeMemory(after);
    } else {
        Log("Output device switched to: %s (%s)", target.name.c_str(), target.id.c_str());
    }
    ts3Functions.freeMemory(cur);
    return true;
}

/* 把 TS3 已连接服务器的输出设备切换为指定的系统默认设备。
 * 入参：wasapiId（默认设备 ID）、wasapiName（默认设备友好名称）。
 * 与成熟插件一致：仅切换已建立连接的 handler（未连接时 TS3 无活跃音频引擎，
 * 对其操作可能导致状态异常）。 */
static bool ApplyDeviceToAllConnected(const char* wasapiId, const std::wstring& wasapiName)
{
    if (!g_hasFunctions || !wasapiId || !*wasapiId) return false;

    /* 获取当前播放模式（如 "WASAPI" / "WDM"） */
    char* mode = NULL;
    if (ts3Functions.getDefaultPlayBackMode(&mode) != ERROR_ok || !mode) {
        Log("Failed to get default playback mode, skipping sync");
        if (mode) ts3Functions.freeMemory(mode);
        return false;
    }

    /* 在 TS3 设备列表中定位目标设备 */
    Ts3DeviceRef target;
    if (!FindTs3Device(mode, wasapiId, wasapiName, target)) {
        Log("Target device not found in TS3 playback device list - skipping to keep current output untouched");
        DumpDeviceList(mode);
        ts3Functions.freeMemory(mode);
        return false;
    }

    /* 遍历服务器连接，仅对已建立的连接执行切换 */
    uint64* handlers = NULL;
    bool applied = false;
    if (ts3Functions.getServerConnectionHandlerList(&handlers) == ERROR_ok) {
        for (int i = 0; handlers[i] != 0; i++) {
            int status = STATUS_DISCONNECTED;
            if (ts3Functions.getConnectionStatus(handlers[i], &status) == ERROR_ok &&
                status == STATUS_CONNECTION_ESTABLISHED) {
                if (SwitchHandlerDevice(handlers[i], mode, target)) {
                    applied = true;
                }
            }
        }
        ts3Functions.freeMemory(handlers);
    }
    ts3Functions.freeMemory(mode);
    return applied;
}

/* 把 TS3 输出设备同步为当前 Windows 默认输出设备 */
static void SyncToSystemDefault()
{
    if (!g_enabled || !g_enumerator) return;

    IMMDevice* device = NULL;
    HRESULT hr = g_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || !device) {
        Log("Failed to get system default output device: 0x%08lX", (unsigned long)hr);
        return;
    }

    LPWSTR id = NULL;
    hr = device->GetId(&id);
    if (SUCCEEDED(hr) && id) {
        char* utf8 = WideToUtf8(id);
        std::wstring friendlyName = GetDeviceFriendlyName(device);
        std::string nameUtf8;
        if (!friendlyName.empty()) {
            char* n = WideToUtf8(friendlyName.c_str());
            if (n) { nameUtf8 = n; free(n); }
        }
        if (utf8) {
            if (!nameUtf8.empty()) {
                Log("System default output device: %s (%s)", utf8, nameUtf8.c_str());
            } else {
                Log("System default output device: %s", utf8);
            }
            ApplyDeviceToAllConnected(utf8, friendlyName);
            free(utf8);
        }
        CoTaskMemFree(id);
    }
    device->Release();
}

/* ---------- WASAPI 设备变化监听器 ---------- */

class DeviceChangeListener : public IMMNotificationClient
{
public:
    DeviceChangeListener() : m_refCount(1) {}
    virtual ~DeviceChangeListener() {}

    /* IUnknown */
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override
    {
        if (ppv == NULL) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IMMNotificationClient) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refCount); }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG rc = InterlockedDecrement(&m_refCount);
        if (rc == 0) delete this;
        return rc;
    }

    /* IMMNotificationClient —— 只关心默认输出设备切换 */
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR /*deviceId*/, DWORD /*newState*/) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR /*deviceId*/) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR /*deviceId*/) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR /*pwstrDeviceId*/) override
    {
        /* 只关心"播放 / 控制台(任务栏音量所指的默认设备)"变化。
         * 这里不信任回调传入的设备 ID（Windows 切换瞬间可能给到瞬时/旧值），
         * 仅作为"唤醒"信号，由主线程 PollDefaultDevice 实时查询真实默认设备。 */
        if (flow == eRender && role == eConsole && g_hwnd != NULL) {
            PostMessageA(g_hwnd, WM_APP_FOLLOW, 0, 0);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR /*deviceId*/, const PROPERTYKEY /*key*/) override { return S_OK; }

private:
    LONG m_refCount;
};

/* ---------- 隐藏窗口（用于把系统线程回调转到 TS3 主线程） ---------- */

static std::wstring g_lastKnownDeviceId; /* 最近一次已知的系统默认设备 ID（仅主线程访问） */
static bool g_lastKnownValid = false;    /* lastKnown 是否已初始化 */

/* 轮询系统默认输出设备，发现变化则同步 TS3（兜底机制：
 * 即使 IMMNotificationClient 回调丢失/延迟，也能可靠跟随） */
static void PollDefaultDevice()
{
    if (!g_enabled || !g_enumerator) return;

    IMMDevice* device = NULL;
    HRESULT hr = g_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || !device) return;

    LPWSTR id = NULL;
    hr = device->GetId(&id);
    if (SUCCEEDED(hr) && id) {
        std::wstring curId = id;
        CoTaskMemFree(id);

        if (!g_lastKnownValid) {
            /* 首次轮询：只记录基线，不干预现有音频设置 */
            g_lastKnownDeviceId = curId;
            g_lastKnownValid = true;
            device->Release();
            return;
        }

        if (curId != g_lastKnownDeviceId) {
            g_lastKnownDeviceId = curId;
            char* utf8 = WideToUtf8(curId.c_str());
            std::wstring name = GetDeviceFriendlyName(device);
            if (utf8) {
                if (!name.empty()) {
                    std::string nameUtf8;
                    char* n = WideToUtf8(name.c_str());
                    if (n) { nameUtf8 = n; free(n); }
                    Log("Poll: default device changed -> %s (%s)", utf8, nameUtf8.c_str());
                } else {
                    Log("Poll: default device changed -> %s", utf8);
                }
                ApplyDeviceToAllConnected(utf8, name);
                free(utf8);
            }
        }
    }
    device->Release();
}

LRESULT CALLBACK FollowWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_APP_FOLLOW) {
        /* 系统默认设备变化事件（仅作"唤醒"信号）：
         * 目标设备一律由 PollDefaultDevice 实时查询，避免信任回调 ID */
        if (g_enabled) {
            SetTimer(hwnd, 1, 200, NULL);   /* 200ms 去抖 */
        }
        return 0;
    }

    if (msg == WM_APP_SYNC) {
        /* TS3 在插件加载或连接刚建立时可能仍在初始化播放设备。
         * 短暂延迟后再同步，同时保证所有 SDK 调用都发生在隐藏窗口线程。 */
        SetTimer(hwnd, 3, 500, NULL);
        return 0;
    }

    if (msg == WM_TIMER && wParam == 1) {
        KillTimer(hwnd, 1);
        PollDefaultDevice();   /* 事件唤醒后立即执行与轮询相同的可靠检查 */
        return 0;
    }

    if (msg == WM_TIMER && wParam == 2) {
        PollDefaultDevice();   /* 2 秒轮询兜底 */
        return 0;
    }

    if (msg == WM_TIMER && wParam == 3) {
        KillTimer(hwnd, 3);
        SyncToSystemDefault();
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static bool CreateHiddenWindow()
{
    if (g_hwnd) return true;
    if (!g_wndClassRegistered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = FollowWndProc;
        wc.hInstance = g_instance;
        wc.lpszClassName = "SystemAudioFollowWnd";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        if (!RegisterClassA(&wc)) {
            /* 已注册则忽略（同一进程内重复加载时） */
            if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        }
        g_wndClassRegistered = true;
    }
    g_hwnd = CreateWindowA("SystemAudioFollowWnd", "SystemAudioFollow",
                           WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, g_instance, NULL);
    return g_hwnd != NULL;
}

/* ---------- 初始化 / 销毁 WASAPI 监听 ---------- */

static bool InitDeviceListener()
{
    /* 线程可能已被 TS3/Qt 初始化过 COM：这里接受 STA 或 MTA */
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        g_comInitialized = true;
    } else if (hr == RPC_E_CHANGED_MODE) {
        g_comInitialized = false;   /* 线程已是 MTA，不重复初始化/反初始化 */
    } else if (hr == S_FALSE) {
        g_comInitialized = false;   /* 已初始化 */
    } else {
        Log("COM initialization failed: 0x%08lX", (unsigned long)hr);
        return false;
    }

    hr = CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          IID_IMMDeviceEnumerator, (void**)&g_enumerator);
    if (FAILED(hr) || !g_enumerator) {
        Log("Failed to create WASAPI device enumerator: 0x%08lX", (unsigned long)hr);
        return false;
    }

    g_listener = new DeviceChangeListener();
    hr = g_enumerator->RegisterEndpointNotificationCallback(g_listener);
    if (FAILED(hr)) {
        Log("Failed to register device change listener: 0x%08lX", (unsigned long)hr);
        g_listener->Release();
        g_listener = NULL;
        return false;
    }

    Log("Now listening for Windows default playback device changes");
    return true;
}

static void DestroyDeviceListener()
{
    if (g_listener && g_enumerator) {
        g_enumerator->UnregisterEndpointNotificationCallback(g_listener);
    }
    if (g_listener) {
        g_listener->Release();
        g_listener = NULL;
    }
    if (g_enumerator) {
        g_enumerator->Release();
        g_enumerator = NULL;
    }
    if (g_comInitialized) {
        CoUninitialize();
        g_comInitialized = false;
    }
}

/* ---------- 设置窗口（CreateWindowExW 手工构建，全 Unicode，无资源/模板依赖） ---------- */

#define IDC_BTN_OK        1003
#define IDC_BTN_CANCEL    1004

static HWND g_cfgHwnd = NULL;   /* 设置窗口句柄（防止重复打开，仅设置窗口线程访问） */
static DWORD g_cfgThreadId = 0; /* 设置窗口所在线程 ID（用于跨线程检查） */

static LRESULT CALLBACK ConfigWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_OK:
            g_enabled = (SendMessage(GetDlgItem(hwnd, IDC_CHECK_ENABLED), BM_GETCHECK, 0, 0) == BST_CHECKED);
            g_logEnabled = (SendMessage(GetDlgItem(hwnd, IDC_CHECK_LOG), BM_GETCHECK, 0, 0) == BST_CHECKED);
            SaveConfig();
            Log("Config updated: follow=%s log=%s",
                g_enabled ? "ON" : "OFF", g_logEnabled ? "ON" : "OFF");
            DestroyWindow(hwnd);
            return 0;
        case IDC_BTN_CANCEL:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        /* 仅由本线程的消息循环处理，PostQuitMessage 让 GetMessageW 退出循环 */
        g_cfgHwnd = NULL;
        g_cfgThreadId = 0;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* 在调用线程中打开设置窗口并运行消息循环（阻塞直到窗口关闭） */
static void OpenConfigWindow()
{
    /* 已有窗口则前置（跨线程：通过窗口句柄检查，IsWindow 确保句柄仍有效） */
    if (g_cfgHwnd && IsWindow(g_cfgHwnd)) {
        SetForegroundWindow(g_cfgHwnd);
        return;
    }

    static bool clsRegistered = false;
    if (!clsRegistered) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = ConfigWndProc;
        wc.hInstance     = g_instance;
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"SystemAudioFollowConfigWnd";
        if (RegisterClassW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return;
        }
        clsRegistered = true;
    }

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME,
                                L"SystemAudioFollowConfigWnd",
                                L"SystemAudioFollow - Settings",
                                WS_CAPTION | WS_SYSMENU | WS_OVERLAPPED,
                                CW_USEDEFAULT, CW_USEDEFAULT, 380, 240,
                                NULL, NULL, g_instance, NULL);
    if (!hwnd) {
        Log("Failed to create settings window, error: %lu", (unsigned long)GetLastError());
        return;
    }
    g_cfgHwnd = hwnd;
    g_cfgThreadId = GetCurrentThreadId();

    /* 创建控件（像素坐标） */
    CreateWindowExW(0, L"BUTTON", L"Follow Windows default playback device(&F)",
                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                    16, 16, 340, 22, hwnd, (HMENU)(INT_PTR)IDC_CHECK_ENABLED, g_instance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Enable logging(&L)",
                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                    16, 44, 340, 22, hwnd, (HMENU)(INT_PTR)IDC_CHECK_LOG, g_instance, NULL);
    CreateWindowExW(0, L"STATIC",
                    L"When you switch the default playback device in the Windows taskbar\r\n"
                    L"or System Settings, TeamSpeak 3 will automatically switch to the\r\n"
                    L"same output device.\r\n"
                    L"\r\n"
                    L"Config file: audio_follow.ini (in the TS3 plugins folder)",
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    16, 78, 340, 72, hwnd, (HMENU)(INT_PTR)-1, g_instance, NULL);
    CreateWindowExW(0, L"BUTTON", L"OK",
                    WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                    196, 166, 76, 28, hwnd, (HMENU)(INT_PTR)IDC_BTN_OK, g_instance, NULL);
    CreateWindowExW(0, L"BUTTON", L"Cancel",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                    286, 166, 76, 28, hwnd, (HMENU)(INT_PTR)IDC_BTN_CANCEL, g_instance, NULL);

    /* 初始化勾选状态 */
    SendMessage(GetDlgItem(hwnd, IDC_CHECK_ENABLED), BM_SETCHECK,
                g_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessage(GetDlgItem(hwnd, IDC_CHECK_LOG), BM_SETCHECK,
                g_logEnabled ? BST_CHECKED : BST_UNCHECKED, 0);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    /* 本线程消息循环（直到窗口销毁） */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static DWORD WINAPI ConfigThreadProc(LPVOID /*param*/)
{
    OpenConfigWindow();
    return 0;
}

/* ---------- 以下为 TS3 客户端要求的插件导出接口 ---------- */

extern "C" {

__declspec(dllexport) const char* ts3plugin_name()
{
    return PLUGIN_NAME;
}

__declspec(dllexport) const char* ts3plugin_version()
{
    return PLUGIN_VERSION;
}

__declspec(dllexport) int ts3plugin_apiVersion()
{
    return PLUGIN_API_VERSION;
}

__declspec(dllexport) const char* ts3plugin_author()
{
    return PLUGIN_AUTHOR;
}

__declspec(dllexport) const char* ts3plugin_description()
{
    return PLUGIN_DESCRIPTION;
}

__declspec(dllexport) void ts3plugin_setFunctionPointers(const struct TS3Functions funcs)
{
    ts3Functions = funcs;
    g_hasFunctions = true;
}

__declspec(dllexport) int ts3plugin_init()
{
    LoadConfig();
    CreateHiddenWindow();
    InitDeviceListener();
    if (g_hwnd) {
        /* 2 秒轮询兜底：即使系统设备变化事件丢失也能可靠跟随 */
        SetTimer((HWND)g_hwnd, 2, 2000, NULL);
        /* 启动时同步一次。若此时尚未建立服务器连接，连接建立回调会再次请求同步。 */
        PostMessageA((HWND)g_hwnd, WM_APP_SYNC, 0, 0);
    }
    Log("SystemAudioFollow %s loaded (%s)", PLUGIN_VERSION,
        g_enabled ? "follow enabled" : "follow disabled");
    return 0;   /* 0 = 加载成功 */
}

__declspec(dllexport) void ts3plugin_shutdown()
{
    DestroyDeviceListener();
    if (g_hwnd) {
        KillTimer((HWND)g_hwnd, 2);
        KillTimer((HWND)g_hwnd, 1);
        KillTimer((HWND)g_hwnd, 3);
        DestroyWindow((HWND)g_hwnd);
        g_hwnd = NULL;
    }
    g_lastKnownDeviceId.clear();
    g_lastKnownValid = false;
    Log("SystemAudioFollow unloaded");
    pluginID = NULL;
    g_hasFunctions = false;
}

__declspec(dllexport) void ts3plugin_registerPluginID(const char* id)
{
    pluginID = id;
    /* 插件 ID 此时才可用：重新解析插件目录，确保配置文件路径正确 */
    LoadConfig();
}

__declspec(dllexport) int ts3plugin_offersConfigure()
{
    return PLUGIN_OFFERS_CONFIGURE_NEW_THREAD;
}

__declspec(dllexport) void ts3plugin_configure(void* /*handle*/, void* /*qParentWidget*/)
{
    /* 客户端会在独立线程中调用本函数，可直接开窗口 */
    OpenConfigWindow();
}

/* 全局右键菜单项 ID */
#define MENU_ID_SETTINGS 1
#define MENU_ID_SYNC_NOW 2

/* 全局右键菜单 */
__declspec(dllexport) void ts3plugin_initMenus(struct PluginMenuItem*** menuItems, char** menuIcon)
{
    *menuItems = (struct PluginMenuItem**)malloc(sizeof(struct PluginMenuItem*) * 3);
    for (int i = 0; i < 2; i++) {
        (*menuItems)[i] = (struct PluginMenuItem*)malloc(sizeof(struct PluginMenuItem));
        memset((*menuItems)[i], 0, sizeof(struct PluginMenuItem));
        (*menuItems)[i]->type = PLUGIN_MENU_TYPE_GLOBAL;
        (*menuItems)[i]->id = (i == 0) ? MENU_ID_SETTINGS : MENU_ID_SYNC_NOW;
        strncpy((*menuItems)[i]->text,
                (i == 0) ? "Audio Follow Settings..." : "Sync to System Default Now",
                PLUGIN_MENU_BUFSZ - 1);
    }
    (*menuItems)[2] = NULL;
    *menuIcon = NULL;
}

__declspec(dllexport) void ts3plugin_onMenuItemEvent(uint64 /*serverConnectionHandlerID*/,
                                                     enum PluginMenuType type,
                                                     int menuItemID,
                                                     uint64 /*selectedItemID*/)
{
    if (type != PLUGIN_MENU_TYPE_GLOBAL) return;
    if (menuItemID == MENU_ID_SETTINGS) {
        CreateThread(NULL, 0, ConfigThreadProc, NULL, 0, NULL);
    } else if (menuItemID == MENU_ID_SYNC_NOW) {
        SyncToSystemDefault();   /* 立即把 TS3 输出设备同步为当前系统默认设备 */
    }
}

/* 连接建立后再次请求同步：插件加载时通常还没有已建立的服务器连接。 */
__declspec(dllexport) void ts3plugin_onConnectStatusChangeEvent(uint64 /*serverConnectionHandlerID*/,
                                                                int newStatus,
                                                                unsigned int /*errorNumber*/)
{
    if (newStatus == STATUS_CONNECTION_ESTABLISHED && g_enabled && g_hwnd) {
        /* 回调不直接调用 TS3 SDK；由隐藏窗口线程执行实际同步。 */
        PostMessageA((HWND)g_hwnd, WM_APP_SYNC, 0, 0);
    }
}

/* 释放客户端要求插件释放的内存 */
__declspec(dllexport) void ts3plugin_freeMemory(void* data)
{
    free(data);
}

} /* extern "C" */

/* ---------- DLL 入口 ---------- */

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID /*lpvReserved*/)
{
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_instance = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
    }
    return TRUE;
}
