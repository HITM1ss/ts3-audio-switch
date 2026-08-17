# ============================================================
# audio_follow.dll - TeamSpeak 3 插件 "跟随系统音频"
# 交叉编译：在 Linux 上用 mingw-w64 编译 Windows x64 DLL
# 本地编译：Windows 上请用 build_windows.bat（MSVC）
#
# 用法：
#   make fetch-sdk   # 下载 TeamSpeak 3 Client Plugin SDK 头文件（API 26）
#   make             # 编译 audio_follow.dll
#   make clean
# ============================================================

CXX      = x86_64-w64-mingw32-g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -I sdk/include
LDFLAGS  = -shared -static -static-libgcc -static-libstdc++ \
           -Wl,--enable-stdcall-fixup -Wl,--kill-at
LIBS     = -lole32 -luuid -luser32

TARGET   = audio_follow.dll
OBJS     = audio_follow.o audio_follow_guids.o

SDK_BASE = https://cdn.jsdelivr.net/gh/teamspeak/ts3client-pluginsdk@master
SDK_HEADERS = \
	sdk/include/ts3_functions.h \
	sdk/include/plugin_definitions.h \
	sdk/include/teamspeak/public_definitions.h \
	sdk/include/teamspeak/public_errors.h \
	sdk/include/teamspeak/public_errors_rare.h \
	sdk/include/teamspeak/public_rare_definitions.h \
	sdk/include/teamlog/logtypes.h

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

audio_follow.o: audio_follow.cpp $(SDK_HEADERS)
	$(CXX) $(CXXFLAGS) -c -o $@ audio_follow.cpp

audio_follow_guids.o: audio_follow_guids.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ audio_follow_guids.cpp

# 从官方 ts3client-pluginsdk 仓库拉取头文件（不重新分发，仅供本地编译）
fetch-sdk:
	@mkdir -p sdk/include/teamspeak sdk/include/teamlog
	@for f in $(SDK_HEADERS); do \
		echo "下载 $$f ..."; \
		curl -sL --fail --retry 3 -o $$f $(SDK_BASE)/$${f#sdk/} || exit 1; \
	done
	@echo "SDK 头文件下载完成。"

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean fetch-sdk
