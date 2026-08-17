@echo off
rem ============================================================
rem  build_windows.bat - 在 Windows 上本地编译 audio_follow.dll
rem
rem  前置要求：
rem    1. Visual Studio 2022 Build Tools（MSVC x64 工具集）
rem    2. 在 "x64 Native Tools Command Prompt" 中运行本脚本
rem    3. SDK 头文件（先运行 fetch_sdk.bat 下载）
rem
rem  输出：audio_follow.dll
rem  说明：对话框完全用代码动态构建（无 .rc 资源），避免编码问题
rem ============================================================
setlocal

if not exist sdk\include\ts3_functions.h (
    echo [错误] 未找到 sdk\include\ts3_functions.h
    echo        请先运行 fetch_sdk.bat 下载 SDK 头文件。
    exit /b 1
)

echo 编译插件...
cl /nologo /O2 /LD /EHsc /utf-8 /I sdk\include ^
   audio_follow.cpp audio_follow_guids.cpp ^
   /link /OUT:audio_follow.dll ole32.lib user32.lib uuid.lib || goto :err

echo.
echo 编译完成：audio_follow.dll
echo 安装：复制 audio_follow.dll 和 audio_follow.ini 到 %%APPDATA%%\TS3Client\plugins\
exit /b 0

:err
echo [错误] 编译失败，请检查错误信息。
exit /b 1