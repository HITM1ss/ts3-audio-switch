@echo off
rem ============================================================
rem  fetch_sdk.bat - 下载 TeamSpeak 3 Client Plugin SDK 头文件
rem  （API 26，来自官方 ts3client-pluginsdk 仓库，仅供本地编译，
rem    不随本项目再分发）
rem ============================================================
setlocal
set BASE=https://cdn.jsdelivr.net/gh/teamspeak/ts3client-pluginsdk@master

mkdir sdk\include\teamspeak 2>nul
mkdir sdk\include\teamlog 2>nul

echo 下载 SDK 头文件...
curl -sL --fail --retry 3 -o sdk\include\ts3_functions.h                      %BASE%/include/ts3_functions.h || goto :err
curl -sL --fail --retry 3 -o sdk\include\plugin_definitions.h                 %BASE%/include/plugin_definitions.h || goto :err
curl -sL --fail --retry 3 -o sdk\include\teamspeak\public_definitions.h       %BASE%/include/teamspeak/public_definitions.h || goto :err
curl -sL --fail --retry 3 -o sdk\include\teamspeak\public_errors.h            %BASE%/include/teamspeak/public_errors.h || goto :err
curl -sL --fail --retry 3 -o sdk\include\teamspeak\public_errors_rare.h       %BASE%/include/teamspeak/public_errors_rare.h || goto :err
curl -sL --fail --retry 3 -o sdk\include\teamspeak\public_rare_definitions.h  %BASE%/include/teamspeak/public_rare_definitions.h || goto :err
curl -sL --fail --retry 3 -o sdk\include\teamlog\logtypes.h                   %BASE%/include/teamlog/logtypes.h || goto :err

echo SDK 头文件下载完成。
exit /b 0

:err
echo [错误] 下载失败，请检查网络后重试。
exit /b 1
