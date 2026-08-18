# Agent Guide

## Project scope

- This is a Windows x64 TeamSpeak 3 client plugin using SDK API 26. Its implementation is C++17 with Win32 and WASAPI/COM; the primary implementation is [audio_follow.cpp](audio_follow.cpp).
- Consult [README.md](README.md) for install, behavior, and manual runtime verification. Consult [Makefile](Makefile), [build_windows.bat](build_windows.bat), and [fetch_sdk.bat](fetch_sdk.bat) for build details.

## Build and validation

- On Windows, run `fetch_sdk.bat` before `build_windows.bat` from a Visual Studio x64 Native Tools Command Prompt.
- On Linux, cross-compile with `make fetch-sdk` followed by `make`; use `make clean` for generated MinGW artifacts.
- There are no automated tests. After behavior changes, compile for Windows and manually verify an established TeamSpeak connection follows a Windows `eRender` / `eConsole` default-device change. Enable logging in [audio_follow.ini](audio_follow.ini) while diagnosing runtime behavior.

## Implementation invariants

- Do not call TeamSpeak SDK APIs in the `IMMNotificationClient` callback. Preserve the thread boundary: callback posts a message, then the hidden window procedure performs the TeamSpeak work.
- Before switching playback devices, retain the existing safeguards: target device must exist in the TeamSpeak playback-device list, only established connections are updated, and failures attempt to restore the prior device.
- Free memory returned by TeamSpeak SDK functions with `ts3Functions.freeMemory()`. Release COM objects with `Release()` and strings returned by `IMMDevice::GetId()` with `CoTaskMemFree()`.
- Keep exported TeamSpeak plugin entry-point signatures, API version, and the dynamically created settings UI compatible with the installed SDK.

## Generated and upstream files

- Do not edit `sdk/include/`: it is upstream SDK content refreshed by the fetch scripts.
- Treat `audio_follow.dll`, `audio_follow.ts3_plugin`, object files, and MSVC `.obj`, `.exp`, and `.lib` outputs as generated artifacts.
- The README directory tree mentions `.rc` resource files, but the current settings UI is created in code; do not add resource dependencies unless explicitly requested.