# 跟随系统音频（SystemAudioFollow）

一款 TeamSpeak 3 客户端插件（Windows x64）：**当你在 Windows 任务栏右下角或系统设置中切换默认音频输出设备时，TeamSpeak 3 的播放设备会自动跟随切换到同一个设备**。

> 适用场景：你有多个音频输出（耳机、音箱、显示器等），切系统输出时不再需要手动进 TS3 设置里再切一次。

---

## 一、安装

### 方式 A：一键安装（推荐）

双击 `audio_follow.ts3_plugin` 文件，TeamSpeak 3 会自动安装并提示重启客户端。重启后插件即生效。

### 方式 B：手动安装

1. 关闭 TeamSpeak 3 客户端。
2. 把 `audio_follow.dll` 和 `audio_follow.ini` 复制到插件目录：
   - 按 `Win + R` 输入 `%APPDATA%\TS3Client\plugins` 回车；
   - 或直接打开 `C:\Users\你的用户名\AppData\Roaming\TS3Client\plugins`。
3. 重新启动 TeamSpeak 3，插件自动加载（可在"工具 → 选项 → 插件"中看到 `SystemAudioFollow`）。

### 环境要求

- Windows 7 ~ Windows 11（x64）
- TeamSpeak 3 客户端 **3.6.0 或更高版本**（插件基于 SDK API 26 开发）

---

## 二、使用

1. 保持插件默认配置（跟随已启用）。
2. 连接任意服务器后，在 Windows 任务栏右下角点击音量图标，切换默认输出设备。
3. TeamSpeak 3 的输出设备会**自动**切换到新设备，无需任何手动操作。

### 设置面板

在 TeamSpeak 3 客户端中：**右键客户端空白处 → 全局**，有两个菜单项（界面为英文）：

- **Audio Follow Settings...**：打开设置窗口
  - **Follow Windows default playback device**：总开关。
  - **Enable logging**：是否在 TS3 日志中输出插件运行信息（排查问题时建议开启）。
- **Sync to System Default Now**：立即把 TS3 输出设备同步为当前系统默认设备（用于手动验证）。

也可以在插件目录的 `audio_follow.ini` 中直接编辑（改完需重启 TS3）：

```ini
[General]
Enabled=1      ; 1=启用跟随，0=禁用
LogEnabled=1   ; 1=输出日志，0=关闭日志
```

---

## 三、卸载

1. 关闭 TeamSpeak 3。
2. 删除插件目录下的 `audio_follow.dll` 和 `audio_follow.ini`（以及 `audio_follow.ts3_plugin` 安装包）。
3. 重启 TeamSpeak 3 即可。

---

## 四、工作原理

```
Windows 系统音频服务                     TeamSpeak 3 主线程
┌─────────────────────┐   PostMessage   ┌──────────────────────────┐
│ IMMNotificationClient│ ──────────────▶ │ 隐藏窗口 WndProc         │
│ OnDefaultDeviceChanged│   异步投递设备ID │  → openPlaybackDevice() │
│  (系统线程)           │                 │    同步所有已连接服务器   │
└─────────────────────┘                 └──────────────────────────┘
```

1. 插件通过 **Windows Core Audio API（WASAPI）** 注册 `IMMNotificationClient`，监听系统默认播放设备变化（`eRender` + `eConsole`，即任务栏音量所指的默认设备）。
2. 设备变化回调运行在**系统音频线程**，不能直接操作 TS3，因此通过 `PostMessage` 把新设备 ID 投递给插件创建的隐藏窗口。
3. 窗口过程运行在 **TS3 客户端主线程**，在其中调用 SDK 的 `openPlaybackDevice()`，对当前所有已连接的服务器连接同步切换输出设备。
4. 插件启动及新服务器连接建立时都会自动同步一次；连接建立后的同步会短暂延迟，等待 TS3 完成播放设备初始化。

---

## 五、从源码编译

### 在 Linux 上交叉编译（mingw-w64）

```bash
sudo apt install g++-mingw-w64-x86-64
make fetch-sdk      # 下载 TeamSpeak 3 Client Plugin SDK 头文件（API 26）
make                # 产出 audio_follow.dll
```

### 在 Windows 上编译（MSVC）

1. 安装 Visual Studio 2022 Build Tools（勾选 MSVC x64 工具集）。
2. 打开 **x64 Native Tools Command Prompt**。
3. 在项目目录依次执行：

```bat
fetch_sdk.bat       # 下载 SDK 头文件
build_windows.bat   # 编译 audio_follow.dll
```

---

## 六、已知限制与说明

- 插件只跟随 **"播放 / 控制台"（eConsole）** 默认设备的切换，即任务栏音量图标对应的设备。游戏/语音（eCommunications）默认设备的变化不会触发。
- **安全策略**：插件只在"目标设备确认存在于 TS3 播放设备列表中"时才执行切换，且不会在启动/连接服务器时主动改动你的音频设置，因此不会导致无声；如果设备不在列表中会直接跳过并记录日志。
- 插件切换 TS3 输出设备**不会**反向修改 Windows 默认设备，不会造成循环切换。
- 本插件在 Windows 上由官方 SDK 的 `openPlaybackDevice` 接口驱动，具体设备切换行为以客户端实际表现为准；如遇问题请先开启日志（`LogEnabled=1`）查看 TS3 日志（`%APPDATA%\TS3Client\logs\`）。

---

## 七、目录结构

```
ts3-audio-follow/
├── audio_follow.cpp         # 插件主源码（C++17）
├── audio_follow_guids.cpp   # WASAPI GUID 定义（mingw 专用）
├── audio_follow.rc          # 设置对话框资源
├── resource.h               # 资源 ID
├── audio_follow.ini         # 默认配置文件
├── audio_follow.dll         # 编译产物（可直接安装）
├── audio_follow.ts3_plugin  # 一键安装包（zip 格式）
├── Makefile                 # Linux/mingw-w64 交叉编译
├── build_windows.bat        # Windows/MSVC 编译脚本
├── fetch_sdk.bat            # 下载 SDK 头文件脚本
└── README.md                # 本说明
```

> SDK 头文件版权归 TeamSpeak Systems GmbH 所有，来自官方仓库 `teamspeak/ts3client-pluginsdk`（API 26），仅用于本地编译，不随本项目重新分发。
