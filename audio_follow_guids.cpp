/*
 * audio_follow_guids.cpp
 * 定义 WASAPI 接口 GUID / PROPERTYKEY（mingw-w64 头文件中 DEFINE_GUID /
 * DEFINE_PROPERTYKEY 仅为声明，需要在一个编译单元里定义 INITGUID 生成实际定义）。
 */
#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
