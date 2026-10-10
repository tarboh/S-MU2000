# S-MU2000
#
#   make          verify / boot / render / live を作る
#                 render は MIDI ファイルを WAV に書き出す
#                 live   は MIDI 入力を受けてその場で鳴らす
#   make check    run the checks that need no ROMs (verify)
#   make test     回帰試験（ROM が無ければ verify だけ）
#   make clean    消す
#
# MSYS2 / MinGW-w64 の g++ を想定している。
# C++20 が要る（sh.cpp が std::rotl / std::rotr を使う）。
#
#   make CROSS=windows   macOS から Windows 用 exe / VST3 を作る
#                 (mingw-w64 が要る: brew install mingw-w64)

# 音を作るのは重いので最適化を上げる。-O2 より 6% 速い
CXXFLAGS ?= -std=c++20 -O3 -Wall -Wformat-security -Wno-unused-variable -Wno-unused-but-set-variable

# ---- Platform ----------------------------------------------------------------
#
# The same file builds on Windows (MSYS2/MinGW-w64), Linux with a MinGW cross
# compiler, and macOS.
#   Windows ... OS holds Windows_NT
#   macOS   ... uname -s answers Darwin
#
# Cross-compile the Windows binaries on macOS with mingw-w64:
#   brew install mingw-w64
#   make CROSS=windows
# Objects go to build-windows/ so native and cross builds never mix.
# CXX/PYTHON/BUILD can still be overridden (e.g. CXX=x86_64-w64-mingw32-g++-posix).
PLATFORM := unknown
ifneq (,$(filter windows win win64 mingw mingw64,$(CROSS)))
PLATFORM := windows
CROSS_WINDOWS := 1
else ifeq ($(OS),Windows_NT)
PLATFORM := windows
else ifeq ($(shell uname -s),Darwin)
PLATFORM := macos
else ifeq ($(shell uname -s),Linux)
# Native Linux (issue #25). Cross-building the Windows binaries from Linux is
# still `make CROSS=windows`; it used to be picked automatically when mingw-w64
# was installed, which would now hide the native build
PLATFORM := linux
else ifneq ($(shell command -v x86_64-w64-mingw32-g++ 2>/dev/null),)
PLATFORM := windows
ifeq ($(origin CXX),default)
CXX := x86_64-w64-mingw32-g++
endif
endif

ifdef CROSS_WINDOWS
ifdef UNIVERSAL
$(error CROSS=windows and UNIVERSAL=1 do not mix)
endif
ifdef ARCH
$(error CROSS=windows and ARCH=$(ARCH) do not mix -- the target is always x86_64 Windows)
endif
ifdef MARCH
$(error CROSS=windows and MARCH=$(MARCH) do not mix -- -march=native would probe the Mac CPU, not the Windows target)
endif
# Windows binaries do not run on macOS. The run targets (check, probe, test)
# pass this through, so `make CROSS=windows check WINE=wine` works where Wine
# exists; otherwise they stop with a message instead of an Exec format error
WINE ?=
endif

ifeq ($(PLATFORM),windows)
ifdef CROSS_WINDOWS
# `CXX ?= ...` would keep make's built-in c++ (same reason as the clang++
# swap below), so swap it only while it is still the default
ifeq ($(origin CXX),default)
CXX      := x86_64-w64-mingw32-g++
endif
PYTHON   ?= python3
else
CXX      ?= g++
PYTHON   ?= python
endif
# MSYS2 の DLL に依存させない。動的リンクのままだと、MSYS2 の環境の外
# （素の PowerShell など）では起動に失敗して何も言わずに終わる
LDFLAGS  ?= -static -static-libgcc -static-libstdc++
EXE      := .exe
# 32 ビット MinGW (i686) は既定で浮動小数を x87 の 80 ビット中間値で計算する
# (__FLT_EVAL_METHOD__=2)。x86-64/arm64 は SSE2 の 64 ビットなので、
# 同じ C++ でも丸めが微妙に違い、サンプル精度の render がゴールデンからズレる
# （約 9 秒後に ±1 LSB ずつ現れる兆候）。SSE2 演算に寄せて x64 と
# ビット一致させる。x86-64 では既に既定なので実質 no-op、arm64 には当てない。
ifneq (,$(filter i386 i486 i586 i686,$(firstword $(subst -, ,$(shell $(CXX) -dumpmachine 2>/dev/null)))))
CXXFLAGS += -mfpmath=sse -msse2
# libmsvcrt の i386 用 __beginthreadex は SEH handler を .sxdata 経由で引く
# だけなので、ld が libmingw32 の crt_handler.o を取り出さない（x64 は SEH
# を使わないので起きない）。--undefined で引き込ませる
# （--require-defined だと、この名前を持たない新しい MSYS2 の i686 のランタイムでリンクが止まる。
# そちらでは引き込まなくてもスレッドを使う試験が通る）
#
# PLUGIN_API は __stdcall。32 ビットだと dll からの名前が
# GetPluginFactory@0 になって host が見つけられない。--kill-at で @0 を落とす
LDFLAGS  += -Wl,--undefined=___mingw_SEH_error_handler -Wl,--kill-at
endif
else ifeq ($(PLATFORM),linux)
CXX      ?= g++
PYTHON   ?= python3
LDFLAGS  ?=
EXE      :=
# std::thread and the ALSA backend want it on both sides of the link
CXXFLAGS += -pthread
LDFLAGS  += -pthread
else
# `CXX ?= clang++` would not work: make already has CXX set (to c++), and `?=`
# leaves a defined variable alone. So swap it only while it is still the default
ifeq ($(origin CXX),default)
CXX      := clang++
endif
# macOS answers to python3; plain "python" is not usually there
PYTHON   ?= python3
LDFLAGS  ?=
EXE      :=
# Apple Clang is stricter than GCC. The imported MAME sources do not put override
# on virtual functions, so turn off just this warning rather than edit them:
# that keeps the diff small for sending the changes back upstream
CXXFLAGS += -Wno-inconsistent-missing-override
#
# Build for one architecture or both. Default: this machine's own.
#   make ARCH=arm64   / make ARCH=x86_64   / make UNIVERSAL=1
# The x86-64 JIT backend emits x86-64 machine code and the arm64 one emits
# arm64, so each slice wants the objects built for it. The two do not mix, and
# a directory holding both fails to link (or links the wrong half), so each
# flavour gets a build directory of its own -- see BUILD below
ifdef UNIVERSAL
CXXFLAGS += -arch arm64 -arch x86_64
else ifdef ARCH
CXXFLAGS += -arch $(ARCH)
endif
endif

# 自分の CPU に合わせるとさらに 4% ほど速いが、他の機械では動かなくなる。
#   make MARCH=native
ifdef MARCH
CXXFLAGS += -march=$(MARCH)
endif
CXXFLAGS += -I src -I src/compat
# ヘッダを直したときに .o を作り直させる
CXXFLAGS += -MMD -MP

# Object files are per architecture, so a cross build gets its own directory
# (build-universal, build-x86_64) and never reuses the native build/ -- an
# `ARCH=x86_64 make` after a native one used to fail in the link step, with a
# message about which architecture the .o files were. Passing BUILD=... still
# overrides, and a plain `make` still uses build/
ifeq ($(origin BUILD),undefined)
ifeq ($(PLATFORM),linux)
# The working tree is often shared with a Windows build (WSL, a network share),
# and the two sets of objects must not mix
BUILD := build-linux
endif
ifdef CROSS_WINDOWS
BUILD := build-windows
else ifdef UNIVERSAL
BUILD := build-universal
else ifdef ARCH
BUILD := build-$(ARCH)
endif
endif
ASIO ?= 0
ifeq ($(ASIO),1)
ifeq ($(PLATFORM),windows)
ifeq ($(origin BUILD),undefined)
BUILD := build-asio
else ifeq ($(origin BUILD),file)
BUILD := $(BUILD)-asio
endif
else
$(error ASIO=1 is supported only by Windows builds)
endif
endif
BUILD ?= build

ifeq ($(PLATFORM),windows)
ifeq ($(ASIO),1)
CMAKE ?= cmake
PORTAUDIO_LIB := $(BUILD)/portaudio/libportaudio.a
CXXFLAGS += -DSMU2000_ASIO=1 -I third_party/portaudio/include
PORTAUDIO_SRC := src/ui/audio_out_portaudio.cpp
PORTAUDIO_LIBS := -ldsound -luuid -lwinmm -lole32
$(PORTAUDIO_LIB): third_party/portaudio/CMakeLists.txt third_party/portaudio/cmake/modules/FindASIO.cmake
	$(CMAKE) -S third_party/portaudio -B $(BUILD)/portaudio -G "$(if $(CROSS_WINDOWS),Unix Makefiles,MinGW Makefiles)" -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER="$(subst g++,gcc,$(CXX))" -DCMAKE_CXX_COMPILER="$(CXX)" -DCMAKE_BUILD_TYPE=Release -DPA_BUILD_SHARED_LIBS=OFF -DPA_USE_ASIO=ON -DPA_USE_DS=ON
	$(CMAKE) --build $(BUILD)/portaudio --parallel 4
$(BUILD)/ASIO-GPL-3.0.txt: third_party/portaudio/ASIO-GPL-3.0.txt
	@mkdir -p $(dir $@)
	cp $< $@
all: $(BUILD)/ASIO-GPL-3.0.txt
endif
endif

# Menu tests need no ROMs or playback hardware. Opt in to opening real
# outputs with silence: make check-audio-output AUDIO_DEVICES=1.
ifeq ($(PLATFORM),windows)
AUDIO_OUTPUT_TEST_SRC := src/ui/audio_out.cpp $(PORTAUDIO_SRC)
AUDIO_OUTPUT_TEST_LIBS := -lole32 -lavrt -lwinmm -ldsound -luuid
else ifeq ($(PLATFORM),macos)
# The macOS backend is the shared render path plus the HAL questions it asks, so
# the test links audio_apple.mm as well - and compiles it as Objective-C++ with
# ARC, the way the mac %.mm rule does.
# audio_in_mac.cpp comes along because audio_apple.mm holds both halves and the
# recording one asks its own questions of the platform (same reason live links
# it: MAC_IO_OBJS). The test never calls it.
AUDIO_OUTPUT_TEST_SRC := src/ui/audio_out_mac.cpp src/ui/audio_in_mac.cpp \
                        src/ui/audio_apple.mm src/ui/session_mac.cpp
AUDIO_OUTPUT_TEST_FLAGS := -fobjc-arc
AUDIO_OUTPUT_TEST_LIBS := -framework AudioToolbox -framework CoreAudio -framework CoreFoundation \
                         -framework AVFAudio -framework Foundation
else
AUDIO_OUTPUT_TEST_SRC := src/ui/audio_out_linux.cpp
AUDIO_OUTPUT_TEST_LIBS := -lasound
endif

$(BUILD)/audio_output_test$(EXE): tools/test_audio_output.cpp $(AUDIO_OUTPUT_TEST_SRC) $(PORTAUDIO_LIB) \
                               src/ui/audio_output_switch.h src/ui/audio_out.h \
                               src/ui/menu.h src/ui/texts.h src/ui/texts_en.h src/ui/texts_ja.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(AUDIO_OUTPUT_TEST_FLAGS) -o $@ tools/test_audio_output.cpp $(AUDIO_OUTPUT_TEST_SRC) $(PORTAUDIO_LIB) $(LDFLAGS) $(AUDIO_OUTPUT_TEST_LIBS)

.PHONY: check-audio-output
check-audio-output: $(BUILD)/audio_output_test$(EXE)
	$(WINE) $(BUILD)/audio_output_test$(EXE) $(if $(AUDIO_DEVICES),--devices)

# The recording half, same shape. The listing is the part every run can check;
# --devices opens a real device where one may be opened (AUDIO_DEVICES=1).
ifeq ($(PLATFORM),windows)
AUDIO_INPUT_TEST_SRC := src/ui/audio_in.cpp
AUDIO_INPUT_TEST_LIBS := -lole32 -lavrt -lwinmm
else ifeq ($(PLATFORM),macos)
# The input half lives in audio_apple.mm, next to the output half, so this test
# links the same pair the output one does: the backend that holds both, and the
# two platform files that answer the questions it asks. The test exercises list()
# and pop(), so it is the same coverage with the same sources.
AUDIO_INPUT_TEST_SRC := src/ui/audio_out_mac.cpp src/ui/audio_in_mac.cpp \
                        src/ui/audio_apple.mm src/ui/session_mac.cpp
AUDIO_INPUT_TEST_FLAGS := -fobjc-arc
AUDIO_INPUT_TEST_LIBS := -framework AudioToolbox -framework CoreAudio -framework CoreFoundation \
                        -framework AVFAudio -framework Foundation
else
AUDIO_INPUT_TEST_SRC := src/ui/audio_in_linux.cpp
AUDIO_INPUT_TEST_LIBS := -lasound
endif

$(BUILD)/audio_input_test$(EXE): tools/test_audio_input.cpp $(AUDIO_INPUT_TEST_SRC) \
                                src/ui/audio_in.h src/ui/audio_out.h src/ui/lang.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(AUDIO_INPUT_TEST_FLAGS) -o $@ tools/test_audio_input.cpp \
	    $(AUDIO_INPUT_TEST_SRC) $(LDFLAGS) $(AUDIO_INPUT_TEST_LIBS)

.PHONY: check-audio-input
check-audio-input: $(BUILD)/audio_input_test$(EXE)
	$(WINE) $(BUILD)/audio_input_test$(EXE) $(if $(AUDIO_DEVICES),--devices)

# The native effects on their own (src/dsp/README.md): a small tool and worked
# example that includes nothing of the emulator. check-fx runs every effect type
# at the ends and the middle of its parameter ranges and fails on NaN, infinity
# or a blow-up. No ROM needed.
$(BUILD)/fxdemo$(EXE): tools/fxdemo/fxdemo.cpp $(wildcard src/dsp/*.h) src/xg/fx_params.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ tools/fxdemo/fxdemo.cpp $(LDFLAGS)

.PHONY: fxdemo check-fx
fxdemo: $(BUILD)/fxdemo$(EXE)
check-fx: $(BUILD)/fxdemo$(EXE)
	$(WINE) $(BUILD)/fxdemo$(EXE) --selftest

# The per-user data directory -- the same place compat/paths.h's config_dir()
# points at, where roms/, nvram/ and the .ini files already live. The panel art
# goes in a panel/ beside them, and find_default() looks there (step 3), which
# is the only place a one-file plug-in format can keep artwork: a lone .clap or
# .dll has no bundle to put Resources/panel in.
#
# Keyed off PLATFORM, not off uname: this Makefile cross-builds, and asking the
# *host* would put a Linux or Windows target's art in the macOS directory.
ifeq ($(PLATFORM),windows)
# LOCALAPPDATA is a Windows path; the slashes suit cp and mkdir better. It is
# unset when configuring from another host, and an empty prefix would have the
# recipe write to "/S-MU2000/panel", so fall back to where MSYS2 puts $HOME.
PANEL_DATA_DIR := $(if $(LOCALAPPDATA),$(subst \,/,$(LOCALAPPDATA)),$(HOME)/AppData/Local)/S-MU2000/panel
else ifeq ($(PLATFORM),macos)
PANEL_DATA_DIR := $(HOME)/Library/Application Support/S-MU2000/panel
else
# config_dir() prefers XDG_DATA_HOME when it is set, so ask it first
PANEL_DATA_DIR := $(if $(XDG_DATA_HOME),$(XDG_DATA_HOME),$(HOME)/.local/share)/S-MU2000/panel
endif

# **The default goal is `all`, said out loud.** Without this it is whichever
# rule comes first in the file, and this block sits above the `all:` lines: a
# plain `make` ran install-panel-art and built nothing, and `make CROSS=windows`
# compiled zero files and wrote the panel art to ~/AppData/Local instead.
.DEFAULT_GOAL := all

# Neither the pictures nor panel.txt are overwritten. panel.txt is the one file
# here a person edits (doc/panel-editing.md), and a panel.txt of yours may well
# point at pictures of your own, so replacing the pictures while keeping the
# panel.txt would leave the two describing different panels. Delete what you
# want the shipped versions of. Phony because it produces no file of its own.
.PHONY: install-panel-art
install-panel-art:
	@mkdir -p "$(PANEL_DATA_DIR)"
	@for f in art/real/*.png art/real/panel.txt; do \
		test -e "$$f" || continue; \
		cp -n "$$f" "$(PANEL_DATA_DIR)/" 2>/dev/null || \
			test -e "$(PANEL_DATA_DIR)/$$(basename $$f)" || \
			cp -f "$$f" "$(PANEL_DATA_DIR)/"; \
	done
	@echo "絵を置いておいた: $(PANEL_DATA_DIR)"

SRCS := \
	src/compat/compat.cpp \
	src/sampling.cpp \
	src/smartmedia.cpp \
	src/card_fs.cpp \
	src/m2a.cpp \
	src/mame/sound/swp30.cpp \
	src/mame/sound/swp30_jit.cpp \
	src/mame/video/hd44780.cpp \
	src/mame/machine/sci4.cpp \
	src/mame/cpu/sh.cpp \
	src/mame/cpu/sh2.cpp 	src/mame/cpu/sh2_jit.cpp \
	src/compat/a64asm.cpp \
	src/mame/cpu/sh7042.cpp \
	src/mame/cpu/sh_adc.cpp \
	src/mame/cpu/sh_bsc.cpp \
	src/mame/cpu/sh_cmt.cpp \
	src/mame/cpu/sh_dmac.cpp \
	src/mame/cpu/sh_intc.cpp \
	src/mame/cpu/sh_mtu.cpp \
	src/mame/cpu/sh_port.cpp \
	src/mame/cpu/sh_sci.cpp

OBJS := $(SRCS:%.cpp=$(BUILD)/%.o)

ifeq ($(PLATFORM),windows)
# vst3 と vst3probe は下で定義している。変数はまだ空なので名前で書く
all: $(BUILD)/verify$(EXE) $(BUILD)/boot$(EXE) $(BUILD)/render$(EXE) \
     $(BUILD)/live$(EXE) $(BUILD)/midisend$(EXE) $(BUILD)/panel$(EXE) $(BUILD)/gui$(EXE) \
     $(BUILD)/statetest$(EXE) $(BUILD)/rec$(EXE) $(BUILD)/blocktime$(EXE) \
     vst3 $(BUILD)/vst3probe$(EXE) clap $(BUILD)/clapprobe$(EXE) \
     vsti $(BUILD)/vstiprobe$(EXE)
else ifeq ($(PLATFORM),linux)
# Linux (issue #25). The windowed program (gui, SDL3 + Cairo) and the
# headless plug-ins build here too (doc/porting-linux-gui.md)
all: $(BUILD)/verify$(EXE) $(BUILD)/boot$(EXE) $(BUILD)/render$(EXE) \
     $(BUILD)/panel$(EXE) $(BUILD)/statetest$(EXE) $(BUILD)/blocktime$(EXE) \
     $(BUILD)/live$(EXE) $(BUILD)/gui$(EXE) \
     vst3 $(BUILD)/vst3probe$(EXE) clap $(BUILD)/clapprobe$(EXE)
else
# macOS. vst3 and vst3probe are defined below
all: $(BUILD)/verify$(EXE) $(BUILD)/boot$(EXE) $(BUILD)/render$(EXE) \
     $(BUILD)/panel$(EXE) $(BUILD)/statetest$(EXE) $(BUILD)/live$(EXE) \
     $(BUILD)/gui$(EXE) $(BUILD)/blocktime$(EXE) vst3 $(BUILD)/vst3probe$(EXE) \
     au $(BUILD)/aubprobe$(EXE)
endif

$(BUILD)/verify$(EXE): $(OBJS) $(BUILD)/src/smf.o $(BUILD)/src/verify.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/boot$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/boot.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# 1 ブロックを作るのに何 ms かかるかを測る。音声デバイスは使わない。
# 待ち時間の下限はこの最悪値で決まる（doc/todo.md 2 番）
#
# It takes its clock from smu2000::perf_ticks(), which is QueryPerformanceCounter
# on Windows and a monotonic clock on macOS, so both platforms build it now
$(BUILD)/blocktime$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/smf.o $(BUILD)/src/blocktime.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# パラメータの層の定義表を firmware に確かめさせる（doc/params.md）
$(BUILD)/xgtest$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/xg/model.o $(BUILD)/src/xgtest.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# エフェクトのパラメータ番地（1-16）を firmware に確かめさせる（doc/fx-params.md）
$(BUILD)/fx_probe$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/fx_probe.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# インサーションのパラメータの表（src/xg/fx_params.h）を firmware の LCD から作る（doc/pc-editor.md）。
#   build/fxsweep.exe ../MU2000/roms > fxsweep.txt
#   python tools/fxsweep/make_fx_params.py fxsweep.txt src/xg/fx_params.h
$(BUILD)/fxsweep$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/tools/fxsweep/fxsweep.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# リバーブ・コーラス・バリエーションのパラメータを、種類ごとに firmware に確かめる（src/xg/sysfx.h）。
#   build/sysfx_check.exe ../MU2000/roms > sysfx.txt
$(BUILD)/sysfx_check$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/tools/fxsweep/sysfx_check.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# SH-2 を止めたまま音を出す（doc/native-engine.md の段 2）。
#   build/nativeplay.exe ../MU2000/roms out.wav -b 0,0,0 -n 60
$(BUILD)/nativeplay$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/tools/native/nativeplay.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# 音色の記録（ROM の 84 バイト）と SWP30 のレジスタを組で集める（doc/native-engine.md の段 1）。
#   build/voicesweep.exe ../MU2000/roms > voicesweep.txt
$(BUILD)/voicesweep$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/tools/voicesweep/voicesweep.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/render$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/smf.o $(BUILD)/src/render.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# samptest はサンプリング（録音して試聴する）が一回りするかを確かめる
$(BUILD)/samptest$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/samptest.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# smpre はサンプリングの管理情報を探す解析用の道具（all には入れない）
$(BUILD)/smpre$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/smpre.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# statetest は状態の保存と復元が正しいかを確かめる
$(BUILD)/statetest$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/smf.o $(BUILD)/src/statetest.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# panel はフロントパネル（LCD とボタン）を文字だけで動かす
$(BUILD)/panel$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/smf.o $(BUILD)/src/panel.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# PC editor (doc/pc-editor.md). Dear ImGui (MIT), vendored in third_party/imgui.
# Only the window and the renderer are the platform's job: Windows uses Win32 +
# Direct3D 11, macOS uses AppKit + Metal (src/ui/pc_window*.cpp/.mm). The views
# are the same files.
IMGUI_DIR   := third_party/imgui
IMGUI_CORE  := $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp \
               $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp
IMGUI_FLAGS := -I $(IMGUI_DIR)

# Triangulation for SVG fills with holes
EARCUT_INC  := -I third_party/earcut.hpp/include
CXXFLAGS += $(EARCUT_INC)

# ---- Windows-side ports (audio, MIDI, display) and VST3 ----------------------
#
# These still call the Windows APIs directly. The macOS ones are added at each
# step of the port as src/ui/*_mac.cpp and listed in the branch below
ifeq ($(PLATFORM),windows)

# no gamepad support, so no XInput
IMGUI_FLAGS += -DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD
IMGUI_SRCS := $(IMGUI_CORE) \
              $(IMGUI_DIR)/backends/imgui_impl_win32.cpp \
              $(IMGUI_DIR)/backends/imgui_impl_dx11.cpp
PC_SRCS    := src/ui/pc_editor.cpp src/ui/pc_window.cpp src/ui/xg_ui.cpp src/ui/overview.cpp src/ui/fx_editor.cpp src/ui/fx_help.cpp src/ui/part_shapes.cpp src/ui/master_editor.cpp src/ui/sampling_editor.cpp src/ui/sampling_romwave.cpp src/ui/sampling_presets.cpp src/ui/sampling_library.cpp src/ui/fx_icons.cpp
PC_OBJS    := $(IMGUI_SRCS:%.cpp=$(BUILD)/imgui/%.o) $(PC_SRCS:%.cpp=$(BUILD)/imgui/%.o)

# gui は実機のフロントパネル風の画面を出す
UI_SRCS := src/ui/panel.cpp src/ui/editor.cpp src/ui/effects.cpp src/ui/png.cpp \
           src/ui/audio_out.cpp $(PORTAUDIO_SRC) src/ui/audio_in.cpp src/ui/midi_in.cpp src/ui/midi_out.cpp \
           src/ui/layout.cpp src/ui/svg.cpp src/ui/player.cpp src/xg/model.cpp
UI_OBJS := $(UI_SRCS:%.cpp=$(BUILD)/%.o)

$(BUILD)/imgui/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(IMGUI_FLAGS) -c -o $@ $<

$(BUILD)/src/gui.o: CXXFLAGS += $(IMGUI_FLAGS)

# The app classes pull in app.h, whose editor headers want imgui.h
$(BUILD)/src/ui/app_win.o: CXXFLAGS += $(IMGUI_FLAGS)
$(BUILD)/src/ui/window_win.o: CXXFLAGS += $(IMGUI_FLAGS)
# src/ui/ paints through ui/draw_imgui.h, so these need imgui.h on the include
# path; one rule beats per-file lines (matches before generic below)
$(BUILD)/src/ui/%.o: src/ui/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(IMGUI_FLAGS) -c -o $@ $<

$(BUILD)/gui$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/smf.o $(UI_OBJS) $(PC_OBJS) $(BUILD)/src/gui.o $(BUILD)/src/ui/app_win.o $(BUILD)/src/ui/window_win.o $(PORTAUDIO_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lwinmm -lole32 -lgdi32 -luser32 -lavrt -lcomdlg32 -lshell32 	       -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -limm32 $(PORTAUDIO_LIBS)

# midisend は MIDI ファイルを実時間で MIDI 出力へ流す（live の試験用）
$(BUILD)/midisend$(EXE): $(BUILD)/src/smf.o $(BUILD)/src/midisend.o $(BUILD)/src/compat/compat.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lwinmm -lole32 -lavrt

# rec は音声入力を WAV に録る。実機の音（S/PDIF 入力）と突き合わせるため。
# 録りながら MIDI を実機へ流せるので、同じ譜面の実機とこちらを 1 回で並べられる
$(BUILD)/rec$(EXE): $(BUILD)/src/smf.o $(BUILD)/src/rec.o $(BUILD)/src/compat/compat.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lwinmm -lole32 -luuid

# live uses only WASAPI, including in an ASIO=1 build.
LIVE_AUDIO_OBJ := $(BUILD)/src/ui/audio_out.o
LIVE_MAIN_OBJ := $(BUILD)/src/live.o
ifeq ($(ASIO),1)
LIVE_AUDIO_OBJ := $(BUILD)/native/src/ui/audio_out.o
LIVE_MAIN_OBJ := $(BUILD)/native/src/live.o
$(BUILD)/native/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(filter-out -DSMU2000_ASIO=1,$(CXXFLAGS)) -c -o $@ $<
endif
$(BUILD)/live$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/ui/midi_in.o $(LIVE_AUDIO_OBJ) $(LIVE_MAIN_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lwinmm -lole32 -luuid -lavrt

# ---- VST3 プラグイン
#
# Steinberg の SDK は使わず、インターフェース定義（MIT）だけを取り込んである。
# third_party/vst3/README.md を見よ。
#
#   make vst3      build/S-MU2000.vst3/ にバンドルを作る
#   make install-vst3   それを VST3 の置き場へ複製する

VST3_DIR  := $(BUILD)/S-MU2000.vst3
VST3_BIN  := $(VST3_DIR)/Contents/x86_64-win/S-MU2000.vst3
VST3_INC  := -I third_party/vst3

VST3_SDK_SRCS := 	third_party/vst3/pluginterfaces/base/funknown.cpp 	third_party/vst3/pluginterfaces/base/coreiids.cpp 	third_party/vst3/pluginterfaces/base/conststringtable.cpp 	third_party/vst3/pluginterfaces/base/ustring.cpp

VST3_SRCS := src/vst3/plugin.cpp src/vst3/engine.cpp src/vst3/iids.cpp src/vst3/automation.cpp \
             src/vst3/view.cpp src/vst3/view_win.cpp \
             src/ui/panel.cpp src/ui/layout.cpp src/ui/svg.cpp src/ui/png.cpp src/ui/editor.cpp \
             src/ui/effects.cpp src/xg/model.cpp $(VST3_SDK_SRCS)
VST3_OBJS := $(VST3_SRCS:%.cpp=$(BUILD)/vst3obj/%.o)

$(BUILD)/vst3obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(VST3_INC) $(IMGUI_FLAGS) -c -o $@ $<

# 写真調のパネルの絵（art/real）を束の中へ。プラグインは自分の場所から
# ../Resources/panel/panel.txt を探す（doc/panel-editing.md）
VST3_PANEL := $(VST3_DIR)/Contents/Resources/panel/panel.txt
vst3: $(VST3_BIN) $(VST3_PANEL)

$(VST3_PANEL): $(wildcard art/real/*.png) art/real/panel.txt
	@mkdir -p $(dir $@)
	@cp -f art/real/*.png art/real/panel.txt $(dir $@)

# PC で触る窓（一覧・エディタ）はプラグインからも開ける。gui.exe と同じ
# ui::pc_window なので、ImGui と PC 側の絵を一式こちらにも入れる
$(VST3_BIN): $(OBJS) $(BUILD)/src/mu2000.o $(VST3_OBJS) $(PC_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(LDFLAGS) -lwinmm -lole32 -lgdi32 -luser32 -lavrt -lcomdlg32 -lshell32 -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -limm32
	@mkdir -p $(VST3_DIR)/Contents/Resources
	@cp -f doc/vst3-readme.txt $(VST3_DIR)/Contents/Resources/README.txt 2>/dev/null || true
	# 取り込んだものの著作権表示。BSD-3 はバイナリで配るときも添えろと言っている
	@cp -f LICENSE $(VST3_DIR)/Contents/Resources/LICENSE.txt
	@cp -f NOTICE.txt $(VST3_DIR)/Contents/Resources/NOTICE.txt

# 既定の置き場へ入れる。管理者権限が要ることがある
VST3_INSTALL ?= $(PROGRAMFILES)/Common Files/VST3

install-vst3: $(VST3_BIN) $(VST3_PANEL)
ifdef CROSS_WINDOWS
ifeq ($(PROGRAMFILES),)
	$(error CROSS=windows: there is no Program Files here -- pass VST3_INSTALL=<dir> to copy the bundle somewhere you can pick it up from)
endif
endif
	rm -rf "$(VST3_INSTALL)/S-MU2000.vst3"
	cp -r $(VST3_DIR) "$(VST3_INSTALL)/"
	@echo "入れた: $(VST3_INSTALL)/S-MU2000.vst3"

# 工場が名乗るかどうかだけを確かめる小さな道具
$(BUILD)/vst3probe$(EXE): $(BUILD)/vst3obj/src/vst3/probe.o $(BUILD)/vst3obj/src/vst3/probe_host_win.o                         $(BUILD)/vst3obj/src/vst3/iids.o                         $(BUILD)/vst3obj/third_party/vst3/pluginterfaces/base/funknown.o                         $(BUILD)/vst3obj/third_party/vst3/pluginterfaces/base/coreiids.o                         $(BUILD)/vst3obj/third_party/vst3/pluginterfaces/base/conststringtable.o                         $(BUILD)/vst3obj/third_party/vst3/pluginterfaces/base/ustring.o                         $(BUILD)/src/smf.o $(BUILD)/src/compat/compat.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lole32

probe: $(BUILD)/vst3probe$(EXE) $(VST3_BIN)
ifdef CROSS_WINDOWS
	$(if $(WINE),$(WINE) $(BUILD)/vst3probe$(EXE) $(VST3_BIN),$(error CROSS=windows: the probe is a Windows binary -- pass WINE=wine or copy build-windows/ to Windows))
else
	$(BUILD)/vst3probe$(EXE) $(VST3_BIN)
endif

# ---- CLAP プラグイン
#
# CLAP の口の定義（MIT）を third_party/clap に取り込んである。中身は VST3 版の
# engine と画面をそのまま使い、口だけ src/clap/plugin.cpp に書いた。
#
#   make clap          build/S-MU2000.clap を作る（CLAP は DLL 1 本）
#   make install-clap  それを CLAP の置き場へ複製する

CLAP_BIN  := $(BUILD)/S-MU2000.clap
CLAP_INC  := -I third_party/clap $(VST3_INC)
CLAP_OBJS := $(BUILD)/clapobj/src/clap/plugin.o $(filter-out $(BUILD)/vst3obj/src/vst3/plugin.o,$(VST3_OBJS))

$(BUILD)/clapobj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CLAP_INC) $(IMGUI_FLAGS) -c -o $@ $<

clap: $(CLAP_BIN)

# CLAP を DAW 無しで鳴らす小さなホスト。vst3probe と同じ MIDI を流して出音を比べる
$(BUILD)/clapprobe$(EXE): $(BUILD)/clapobj/src/clap/probe.o $(BUILD)/src/smf.o $(BUILD)/src/compat/compat.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# VST3 と同じく、PC で触る窓（一覧・エディタ）も入れる
$(CLAP_BIN): $(OBJS) $(BUILD)/src/mu2000.o $(CLAP_OBJS) $(PC_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(LDFLAGS) -lwinmm -lole32 -lgdi32 -luser32 -lavrt -lcomdlg32 -lshell32 -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -limm32

CLAP_INSTALL ?= $(PROGRAMFILES)/Common Files/CLAP

install-clap: $(CLAP_BIN) install-panel-art
	mkdir -p "$(CLAP_INSTALL)"
	cp -f $(CLAP_BIN) "$(CLAP_INSTALL)/"
	@echo "入れた: $(CLAP_INSTALL)/S-MU2000.clap"

# ---- VST 2.4 instrument (Windows)
#
# The discontinued SDK is not used. src/vsti/vst2_abi.h declares only the
# binary interface needed by this wrapper. The engine and panel are shared with
# VST3 and CLAP.

VSTI_BIN  := $(BUILD)/S-MU2000.dll
VSTI_OBJS := $(BUILD)/vstiobj/src/vsti/plugin.o \
             $(filter-out $(BUILD)/vst3obj/src/vst3/plugin.o,$(VST3_OBJS))

$(BUILD)/vstiobj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(VST3_INC) $(IMGUI_FLAGS) -c -o $@ $<

vsti: $(VSTI_BIN)

$(VSTI_BIN): $(OBJS) $(BUILD)/src/mu2000.o $(VSTI_OBJS) $(PC_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(LDFLAGS) -lwinmm -lole32 -lgdi32 -luser32 -lavrt -lcomdlg32 -lshell32 -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -limm32

VSTI_INSTALL ?= $(PROGRAMFILES)/VstPlugins

install-vsti: $(VSTI_BIN) install-panel-art
	mkdir -p "$(VSTI_INSTALL)"
	cp -f $(VSTI_BIN) "$(VSTI_INSTALL)/"
	@echo "入れた: $(VSTI_INSTALL)/S-MU2000.dll"

$(BUILD)/vstiprobe$(EXE): $(BUILD)/src/vsti/probe.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -luser32

vsti-probe: $(BUILD)/vstiprobe$(EXE) $(VSTI_BIN)
	$(BUILD)/vstiprobe$(EXE) $(VSTI_BIN)

# The Audio Unit is a macOS port; nothing to build here
au install-au au-probe check-au:
	@echo "Audio Unit は macOS の口です。doc/porting-macos.md を見よ"

else ifeq ($(PLATFORM),linux)

# ---- Linux-side ports (ALSA: PCM out, sequencer in). issue #25
#
# The same ui:: interfaces as on the other two platforms; the shape follows the
# macOS files, with the device handle and the worker thread inside the impl
LINUX_IO_OBJS := $(BUILD)/src/ui/audio_out_linux.o $(BUILD)/src/ui/midi_in_linux.o

$(BUILD)/live$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(LINUX_IO_OBJS) $(BUILD)/src/live.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lasound

# ---- Linux GUI + plug-ins (doc/porting-linux-gui.md) --------------------------
#
# gui is an SDL3 window drawing the shared panel through Dear ImGui. The plug-ins
# are ELF shared objects with headless editors for now (hosts fall back to
# their generic UI). Needs libfontconfig-dev and libsdl3-dev
# alongside libasound2-dev.

LINUX_GUI_CFLAGS := $(shell pkg-config --cflags fontconfig 2>/dev/null)
LINUX_GUI_LIBS := $(shell pkg-config --libs fontconfig 2>/dev/null)
LINUX_SDL_CFLAGS := $(shell pkg-config --cflags sdl3 2>/dev/null)
LINUX_SDL_LIBS := $(shell pkg-config --libs sdl3 2>/dev/null)
CXXFLAGS += $(LINUX_GUI_CFLAGS)
# Everything also links into .so plug-ins, so build position-independent
CXXFLAGS += -fPIC

# ALSA MIDI out + capture in (upstream issue #25 covers PCM out + sequencer
# in only). Same POSIX class shape as the macOS headers.
LINUX_EXTRA_IO_OBJS := $(BUILD)/src/ui/midi_out_linux.o $(BUILD)/src/ui/audio_in_linux.o

$(BUILD)/src/ui/midi_out_linux.o $(BUILD)/src/ui/audio_in_linux.o: CXXFLAGS += $(shell pkg-config --cflags alsa 2>/dev/null)

# The PC editor views are compiled for gui; no backend is linked until one is
# used (Dear ImGui SDL3 backends below, vendored unmodified).
IMGUI_DIR   := third_party/imgui
IMGUI_CORE  := $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp \
               $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp
IMGUI_FLAGS := -I $(IMGUI_DIR)

$(BUILD)/imgui/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(IMGUI_FLAGS) $(LINUX_SDL_CFLAGS) -c -o $@ $<

IMGUI_OBJS := $(IMGUI_CORE:%.cpp=$(BUILD)/imgui/%.o)

# Dear ImGui SDL3 backends: the platform one plus SDL_gpu as the renderer.
# Vendored unmodified from the matching ImGui release, like the rest.
# Upstream prefers SDL_gpu over SDL_Renderer where both exist
# (docs/BACKENDS.md), which is also what keeps the LCD's per-frame texture
# upload off the CPU blitter.
IMGUI_SDL_BACKENDS := third_party/imgui/backends/imgui_impl_sdl3.cpp \
                      third_party/imgui/backends/imgui_impl_sdlgpu3.cpp
IMGUI_SDL_OBJS := $(IMGUI_SDL_BACKENDS:%.cpp=$(BUILD)/imgui/%.o)

LINUX_GUI_SRCS := src/ui/panel.cpp src/ui/layout.cpp src/ui/svg.cpp \
                  src/ui/editor.cpp src/ui/effects.cpp src/ui/png.cpp \
                  src/ui/player.cpp src/xg/model.cpp \
                  src/ui/xg_ui.cpp src/ui/fx_help.cpp src/ui/fx_icons.cpp \
                  src/ui/sdl_popup.cpp \
                  src/ui/window_sdl.cpp \
                  src/ui/app_linux.cpp \
                  src/ui/pc_window_linux.cpp \
                  src/ui/pc_editor.cpp src/ui/overview.cpp src/ui/fx_editor.cpp \
                  src/ui/part_shapes.cpp src/ui/master_editor.cpp src/ui/sampling_editor.cpp src/ui/sampling_romwave.cpp src/ui/sampling_presets.cpp src/ui/sampling_library.cpp
LINUX_GUI_OBJS := $(LINUX_GUI_SRCS:%.cpp=$(BUILD)/guiobj/%.o)

$(BUILD)/guiobj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(IMGUI_FLAGS) $(LINUX_SDL_CFLAGS) -c -o $@ $<

$(BUILD)/src/gui_linux.o: CXXFLAGS += $(IMGUI_FLAGS) $(LINUX_SDL_CFLAGS)

$(BUILD)/gui$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/smf.o \
                    $(LINUX_GUI_OBJS) $(IMGUI_OBJS) \
                    $(IMGUI_SDL_OBJS) $(LINUX_IO_OBJS) $(LINUX_EXTRA_IO_OBJS) \
                    $(BUILD)/src/gui_linux.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lasound $(LINUX_GUI_LIBS) $(LINUX_SDL_LIBS)

# ---- VST3 plug-in (Linux)
#
# Steinberg's SDK is not used, only the MIT interface headers in
# third_party/vst3. The bundle holds an ELF .so at Contents/x86_64-linux/;
# hosts dlopen it and call GetPluginFactory directly (no InitDll, no CFBundle).

VST3_DIR  := $(BUILD)/S-MU2000.vst3
VST3_BIN  := $(VST3_DIR)/Contents/x86_64-linux/S-MU2000.so
VST3_INC  := -I third_party/vst3

VST3_SDK_SRCS := \
	third_party/vst3/pluginterfaces/base/funknown.cpp \
	third_party/vst3/pluginterfaces/base/coreiids.cpp \
	third_party/vst3/pluginterfaces/base/conststringtable.cpp \
	third_party/vst3/pluginterfaces/base/ustring.cpp

LINUX_PANEL_SRCS := src/ui/panel.cpp src/ui/layout.cpp src/ui/svg.cpp src/ui/png.cpp src/ui/editor.cpp \
              src/ui/effects.cpp src/xg/model.cpp \
              src/ui/xg_ui.cpp src/ui/fx_help.cpp src/ui/fx_icons.cpp $(IMGUI_CORE)

VST3_SRCS := src/vst3/plugin.cpp src/vst3/engine.cpp src/vst3/iids.cpp src/vst3/automation.cpp \
             src/vst3/view.cpp src/vst3/plug_window_linux.cpp \
             $(LINUX_PANEL_SRCS) $(VST3_SDK_SRCS)
VST3_OBJS := $(VST3_SRCS:%.cpp=$(BUILD)/vst3obj/%.o)

$(BUILD)/vst3obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(VST3_INC) $(IMGUI_FLAGS) $(LINUX_SDL_CFLAGS) -c -o $@ $<

# 写真調のパネルの絵（art/real）を束の中へ。プラグインは自分の場所から
# ../Resources/panel/panel.txt を探す（doc/panel-editing.md）
VST3_PANEL := $(VST3_DIR)/Contents/Resources/panel/panel.txt
vst3: $(VST3_BIN) $(VST3_PANEL)

$(VST3_PANEL): $(wildcard art/real/*.png) art/real/panel.txt
	@mkdir -p $(dir $@)
	@cp -f art/real/*.png art/real/panel.txt $(dir $@)

$(VST3_BIN): $(OBJS) $(BUILD)/src/mu2000.o $(VST3_OBJS) $(IMGUI_SDL_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(LDFLAGS) -lasound $(LINUX_GUI_LIBS) $(LINUX_SDL_LIBS)
	@mkdir -p $(VST3_DIR)/Contents/Resources
	@cp -f doc/vst3-readme.txt $(VST3_DIR)/Contents/Resources/README.txt 2>/dev/null || true
	@cp -f LICENSE $(VST3_DIR)/Contents/Resources/LICENSE.txt
	@cp -f NOTICE.txt $(VST3_DIR)/Contents/Resources/NOTICE.txt

VST3_INSTALL ?= $(HOME)/.vst3

install-vst3: $(VST3_BIN) $(VST3_PANEL)
	rm -rf "$(VST3_INSTALL)/S-MU2000.vst3"
	mkdir -p "$(VST3_INSTALL)"
	cp -r $(VST3_DIR) "$(VST3_INSTALL)/"
	@echo "入れた: $(VST3_INSTALL)/S-MU2000.vst3"

$(BUILD)/vst3probe$(EXE): $(BUILD)/vst3obj/src/vst3/probe.o \
                          $(BUILD)/vst3obj/src/vst3/probe_host_linux.o \
                          $(BUILD)/vst3obj/src/vst3/iids.o \
                          $(BUILD)/vst3obj/third_party/vst3/pluginterfaces/base/funknown.o \
                          $(BUILD)/vst3obj/third_party/vst3/pluginterfaces/base/coreiids.o \
                          $(BUILD)/vst3obj/third_party/vst3/pluginterfaces/base/conststringtable.o \
                          $(BUILD)/vst3obj/third_party/vst3/pluginterfaces/base/ustring.o \
                          $(BUILD)/src/smf.o $(BUILD)/src/compat/compat.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -ldl

ROMS ?= roms

probe: $(BUILD)/vst3probe$(EXE) $(VST3_BIN)
	S_MU2000_ROMS=$(ROMS) $(BUILD)/vst3probe$(EXE) $(VST3_BIN)

# ---- CLAP plug-in (Linux)
#
# Same engine and headless panel as the VST3; only src/clap/plugin.cpp differs.
# A CLAP on Linux is one ELF .so, conventionally with a .clap suffix.

CLAP_BIN  := $(BUILD)/S-MU2000.clap
CLAP_INC  := -I third_party/clap $(VST3_INC)
CLAP_OBJS := $(BUILD)/clapobj/src/clap/plugin.o $(filter-out $(BUILD)/vst3obj/src/vst3/plugin.o,$(VST3_OBJS))

$(BUILD)/clapobj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CLAP_INC) $(IMGUI_FLAGS) $(LINUX_SDL_CFLAGS) -c -o $@ $<

clap: $(CLAP_BIN)

$(CLAP_BIN): $(OBJS) $(BUILD)/src/mu2000.o $(CLAP_OBJS) $(IMGUI_SDL_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(LDFLAGS) -lasound $(LINUX_GUI_LIBS) $(LINUX_SDL_LIBS)

$(BUILD)/clapprobe$(EXE): $(BUILD)/clapobj/src/clap/probe.o $(BUILD)/src/smf.o $(BUILD)/src/compat/compat.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -ldl

CLAP_INSTALL ?= $(HOME)/.clap

install-clap: $(CLAP_BIN) install-panel-art
	mkdir -p "$(CLAP_INSTALL)"
	cp -f $(CLAP_BIN) "$(CLAP_INSTALL)/"
	@echo "入れた: $(CLAP_INSTALL)/S-MU2000.clap"

# The Audio Unit is a macOS port; nothing to build here
au install-au au-probe check-au:
	@echo "Audio Unit は macOS の口です。doc/porting-macos.md を見よ"

else # macOS

# ---- macOS-side ports (CoreAudio output, CoreMIDI input and output)
#
# The Windows side calls WinMM / WASAPI directly; here the same ui:: interfaces
# are filled in with CoreAudio and CoreMIDI. live and gui both go through them
#
# The GUI additionally needs a window, which is AppKit (Cocoa) plus CoreText
# for the panel's labels.
#
# packaging/auv3-app-Info.plist and packaging/auv3-appex-Info.plist both say
# LSMinimumSystemVersion 11.0, so that is the floor this project has always
# claimed. Saying the same thing to the compiler keeps the binaries honest: left
# unset, the toolchain stamps whatever SDK is installed (27.2 at the time of
# writing) into minos, and a VST3 or AU built on a new Mac then refuses to load
# on the very machines the plists promise to support. Exported rather than added
# to CXXFLAGS so the driver applies it to the link steps too, and to anything
# the recipes shell out to.
export MACOSX_DEPLOYMENT_TARGET := 11.0

MAC_FRAMEWORKS := -framework CoreAudio -framework AudioToolbox \
                  -framework AVFAudio -framework Foundation \
                  -framework CoreMIDI -framework AudioUnit \
                  -framework CoreFoundation -framework CoreGraphics \
                  -framework CoreText -framework Cocoa \
                  -framework UniformTypeIdentifiers \
                  -framework QuartzCore

# live needs a backend of its own; gui gets the same two through MAC_GUI_SRCS.
# audio_apple.o is the render path audio_out_mac.cpp shares with iOS; live links
# it directly, gui through MAC_GUI_SRCS below.
MAC_IO_OBJS := $(BUILD)/src/ui/audio_out_mac.o $(BUILD)/src/ui/audio_apple.o \
               $(BUILD)/src/ui/audio_in_mac.o $(BUILD)/src/ui/session_mac.o \
               $(BUILD)/src/ui/midi_in_apple.o

$(BUILD)/live$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(MAC_IO_OBJS) $(BUILD)/src/live.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(MAC_FRAMEWORKS)

# gui draws the front-panel look of the real machine.
#
# panel.cpp and its neighbours are the **same source** as the Windows build; only
# what is underneath differs. Dear ImGui + Metal fill the interface in with
# Dear ImGui and window_mac.mm fills the window in with AppKit
# (doc/porting-macos.md).
#
# window_mac.mm is compiled as Objective-C++, and so is src/ui/shot_mac.mm.
MAC_GUI_SRCS := src/ui/panel.cpp src/ui/editor.cpp src/ui/effects.cpp \
                src/ui/png.cpp src/ui/layout.cpp src/ui/svg.cpp src/ui/player.cpp \
                src/ui/audio_out_mac.cpp src/ui/audio_apple.mm src/ui/audio_in_mac.cpp \
                src/ui/session_mac.cpp \
                src/ui/midi_in_apple.cpp src/ui/midi_out_apple.cpp \
                src/xg/model.cpp \
                src/ui/window_mac.mm src/ui/app_mac.cpp src/ui/shot_mac.mm \
                src/gui_mac.cpp

# PC editor (doc/pc-editor.md). The views are the same files as on Windows;
# the window is AppKit + Metal (pc_window_mac.mm). imgui_impl_osx is not used
# here, because the input would arrive in another window's context
MAC_IMGUI_SRCS := $(IMGUI_CORE) \
                  $(IMGUI_DIR)/backends/imgui_impl_metal.mm
MAC_PC_SRCS    := src/ui/pc_editor.cpp src/ui/pc_window_mac.mm src/ui/xg_ui.cpp \
                  src/ui/file_ask_mac.mm \
                  src/ui/overview.cpp src/ui/fx_editor.cpp src/ui/fx_help.cpp src/ui/part_shapes.cpp \
                  src/ui/master_editor.cpp src/ui/sampling_editor.cpp src/ui/sampling_romwave.cpp src/ui/sampling_presets.cpp src/ui/sampling_library.cpp src/ui/fx_icons.cpp
MAC_PC_OBJS    := $(MAC_IMGUI_SRCS) $(MAC_PC_SRCS)
MAC_PC_OBJS    := $(MAC_PC_OBJS:%.cpp=$(BUILD)/imgui/%.o)
MAC_PC_OBJS    := $(MAC_PC_OBJS:%.mm=$(BUILD)/imgui/%.o)

$(BUILD)/imgui/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(IMGUI_FLAGS) -c -o $@ $<

$(BUILD)/imgui/%.o: %.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(IMGUI_FLAGS) -fobjc-arc -c -o $@ $<

MAC_GUI_OBJS := $(MAC_GUI_SRCS:%.cpp=$(BUILD)/%.o)
MAC_GUI_OBJS := $(MAC_GUI_OBJS:%.mm=$(BUILD)/%.o)

$(BUILD)/%.o: %.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fobjc-arc -c -o $@ $<

# The macOS front end pulls in the editor's headers (fx_editor.h and friends)
# through app.h, which want imgui.h on the include path. Same reason as gui.o
# on Windows -- and window_mac.mm too now, since it includes app.h directly
$(BUILD)/src/ui/app_mac.o: CXXFLAGS += $(IMGUI_FLAGS)
$(BUILD)/src/ui/window_mac.o: CXXFLAGS += $(IMGUI_FLAGS)
$(BUILD)/src/gui_mac.o: CXXFLAGS += $(IMGUI_FLAGS)
# src/ui/ paints through ui/draw_imgui.h, so these need imgui.h on the include
# path; one rule beats per-file lines (matches before generic below)
$(BUILD)/src/ui/%.o: src/ui/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(IMGUI_FLAGS) -c -o $@ $<

# --shot renders headless through Metal on macOS (ui/shot_mac.mm): the same
# renderer the window uses, into an ordinary texture, because a CAMetalLayer
# drawable is framebufferOnly and wants presenting. SDL3 stays a Linux-only
# dependency; asking macOS for it is what broke the macOS CI build, whose
# runner has no SDL3. shot_mac.mm is Objective-C++, so it needs the ImGui
# include path spelled out like the other .mm files above it.
$(BUILD)/src/ui/shot_mac.o: CXXFLAGS += $(IMGUI_FLAGS)

MAC_FRAMEWORKS += -framework Metal

$(BUILD)/gui$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/smf.o $(MAC_GUI_OBJS) $(MAC_PC_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(MAC_FRAMEWORKS)

# ---- VST3 plug-in (macOS)
#
# The bundle layout differs from Windows: the binary goes in Contents/MacOS and
# Contents/Info.plist declares what the package is. A host opens it with CFBundle
# rather than dlopen and calls bundleEntry (end of plugin.cpp).

VST3_DIR  := $(BUILD)/S-MU2000.vst3
VST3_BIN  := $(VST3_DIR)/Contents/MacOS/S-MU2000
VST3_INC  := -I third_party/vst3

VST3_SDK_SRCS := \
	third_party/vst3/pluginterfaces/base/funknown.cpp \
	third_party/vst3/pluginterfaces/base/coreiids.cpp \
	third_party/vst3/pluginterfaces/base/conststringtable.cpp \
	third_party/vst3/pluginterfaces/base/ustring.cpp

# Uses the **same** panel.cpp / layout.cpp / svg.cpp as the Windows build, with
# Dear ImGui + Metal underneath. The window is view_mac.mm
#
# Both plug-in formats show this one panel, so the view and the drawing layer are
# named once and the VST3 bundle and the AU both build them. (The Windows side of
# this Makefile names the same drawing layer in its own VST3_SRCS, with
# view_win.cpp in place of view_mac.mm)
PANEL_VIEW_SRCS := src/vst3/view.cpp src/vst3/view_mac.mm
PANEL_SRCS := src/ui/panel.cpp src/ui/layout.cpp src/ui/svg.cpp src/ui/png.cpp src/ui/editor.cpp \
              src/ui/effects.cpp src/xg/model.cpp

VST3_SRCS := src/vst3/plugin.cpp src/vst3/engine.cpp src/vst3/iids.cpp src/vst3/automation.cpp \
             $(PANEL_VIEW_SRCS) $(PANEL_SRCS) $(VST3_SDK_SRCS)
VST3_OBJS := $(VST3_SRCS:%.cpp=$(BUILD)/vst3obj/%.o)
VST3_OBJS := $(VST3_OBJS:%.mm=$(BUILD)/vst3obj/%.o)

$(BUILD)/vst3obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(VST3_INC) $(IMGUI_FLAGS) -c -o $@ $<

$(BUILD)/vst3obj/%.o: %.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(VST3_INC) $(IMGUI_FLAGS) -fobjc-arc -c -o $@ $<

# 写真調のパネルの絵（art/real）を束の中へ。プラグインは自分の場所から
# ../Resources/panel/panel.txt を探す（doc/panel-editing.md）
VST3_PANEL := $(VST3_DIR)/Contents/Resources/panel/panel.txt
vst3: $(VST3_BIN) $(VST3_PANEL)

$(VST3_PANEL): $(wildcard art/real/*.png) art/real/panel.txt
	@mkdir -p $(dir $@)
	@cp -f art/real/*.png art/real/panel.txt $(dir $@)

# -bundle, not -shared: a VST3 is read with CFBundle, not dlopen
# The overview/editor PC windows open from the plug-in too, so the ImGui
# views and the AppKit window come along in the bundle as well
$(VST3_BIN): $(OBJS) $(BUILD)/src/mu2000.o $(VST3_OBJS) $(MAC_PC_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -bundle -o $@ $^ $(LDFLAGS) $(MAC_FRAMEWORKS)
	@mkdir -p $(VST3_DIR)/Contents/Resources
	@cp -f doc/vst3-readme.txt $(VST3_DIR)/Contents/Resources/README.txt 2>/dev/null || true
	# 取り込んだものの著作権表示。BSD-3 はバイナリで配るときも添えろと言っている
	@cp -f LICENSE $(VST3_DIR)/Contents/Resources/LICENSE.txt
	@cp -f NOTICE.txt $(VST3_DIR)/Contents/Resources/NOTICE.txt
	@cp -f packaging/vst3-macos-Info.plist $(VST3_DIR)/Contents/Info.plist
	@printf 'APPL????' > $(VST3_DIR)/Contents/PkgInfo

# Install into the default location. No admin rights needed on macOS
VST3_INSTALL ?= $(HOME)/Library/Audio/Plug-Ins/VST3

install-vst3: $(VST3_BIN) $(VST3_PANEL)
	rm -rf "$(VST3_INSTALL)/S-MU2000.vst3"
	mkdir -p "$(VST3_INSTALL)"
	cp -r $(VST3_DIR) "$(VST3_INSTALL)/"
	@echo "入れた: $(VST3_INSTALL)/S-MU2000.vst3"

# ---- CLAP plug-in (macOS)
#
# The same src/clap/plugin.cpp as on Windows, around the VST3 engine and view.
# On macOS a CLAP is a bundle like the VST3: the binary in Contents/MacOS, found
# through Contents/Info.plist. Not in `all` yet -- it has not been tried in a
# macOS host
CLAP_DIR  := $(BUILD)/S-MU2000.clap
CLAP_BIN  := $(CLAP_DIR)/Contents/MacOS/S-MU2000
CLAP_INC  := -I third_party/clap $(VST3_INC)
CLAP_OBJS := $(BUILD)/clapobj/src/clap/plugin.o $(filter-out $(BUILD)/vst3obj/src/vst3/plugin.o,$(VST3_OBJS))

$(BUILD)/clapobj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CLAP_INC) $(IMGUI_FLAGS) -c -o $@ $<

# フォト調のパネルの絵（art/real）も CLAP の束の中へ。macOS の CLAP は VST3 や
# AU と同じ束（Contents/MacOS にバイナリ）なので、同じ居場所から見つかる。
# 定義は clap: より前に置く。:= は読んだ時点で展開される
CLAP_PANEL := $(CLAP_DIR)/Contents/Resources/panel/panel.txt

clap: $(CLAP_BIN) $(CLAP_PANEL)

$(CLAP_PANEL): $(wildcard art/real/*.png) art/real/panel.txt
	@mkdir -p $(dir $@)
	@cp -f art/real/*.png art/real/panel.txt $(dir $@)

$(CLAP_BIN): $(OBJS) $(BUILD)/src/mu2000.o $(CLAP_OBJS) $(MAC_PC_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -bundle -o $@ $^ $(LDFLAGS) $(MAC_FRAMEWORKS)
	@mkdir -p $(CLAP_DIR)/Contents/Resources
	@cp -f LICENSE $(CLAP_DIR)/Contents/Resources/LICENSE.txt
	@cp -f NOTICE.txt $(CLAP_DIR)/Contents/Resources/NOTICE.txt
	@cp -f packaging/clap-macos-Info.plist $(CLAP_DIR)/Contents/Info.plist
	@printf 'BNDL????' > $(CLAP_DIR)/Contents/PkgInfo

CLAP_INSTALL ?= $(HOME)/Library/Audio/Plug-Ins/CLAP

install-clap: $(CLAP_BIN) $(CLAP_PANEL)
	rm -rf "$(CLAP_INSTALL)/S-MU2000.clap"
	mkdir -p "$(CLAP_INSTALL)"
	cp -r $(CLAP_DIR) "$(CLAP_INSTALL)/"
	@echo "入れた: $(CLAP_INSTALL)/S-MU2000.clap"

# The same small CLAP host as on Windows (src/clap/probe.cpp opens the module
# with dlopen here, so it wants the executable inside the bundle, not the
# bundle). `make clap-probe` runs its automation and state checks without a
# DAW; to hear it, give it a song:
#   build/clapprobe build/S-MU2000.clap/Contents/MacOS/S-MU2000 song.mid out.wav
$(BUILD)/clapprobe$(EXE): $(BUILD)/clapobj/src/clap/probe.o $(BUILD)/src/smf.o $(BUILD)/src/compat/compat.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

CLAP_MODULE := $(CLAP_DIR)/Contents/MacOS/S-MU2000

clap-probe: $(BUILD)/clapprobe$(EXE) $(CLAP_BIN)
	S_MU2000_ROMS=$(ROMS) $(BUILD)/clapprobe$(EXE) $(CLAP_MODULE) --automation

# Small tool that pretends to be a host. Same as the Windows one, except that the
# module is opened with CFBundle and the parent window is probe_host_mac.mm
$(BUILD)/vst3probe$(EXE): $(BUILD)/vst3obj/src/vst3/probe.o \
                          $(BUILD)/vst3obj/src/vst3/probe_host_mac.o \
                          $(BUILD)/vst3obj/src/vst3/iids.o \
                          $(BUILD)/vst3obj/third_party/vst3/pluginterfaces/base/funknown.o \
                          $(BUILD)/vst3obj/third_party/vst3/pluginterfaces/base/coreiids.o \
                          $(BUILD)/vst3obj/third_party/vst3/pluginterfaces/base/conststringtable.o \
                          $(BUILD)/vst3obj/third_party/vst3/pluginterfaces/base/ustring.o \
                          $(BUILD)/src/smf.o $(BUILD)/src/compat/compat.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -framework CoreFoundation -framework Cocoa

# Where the ROMs live. The plug-in searches env var -> bundle -> well-known
# locations in that order (doc/porting-macos.md), so pass one in from here
ROMS ?= roms

# Both probes open the bundle with CFBundle, so they want the bundle itself and
# not the executable: given the executable they stop with "cannot open bundle"
probe: $(BUILD)/vst3probe$(EXE) $(VST3_BIN)
	S_MU2000_ROMS=$(ROMS) $(BUILD)/vst3probe$(EXE) $(VST3_DIR)

# ---- Audio Unit v2 (macOS)
#
# Runs the same engine as the VST3 build (src/vst3/engine.h); only the host
# interface differs. The bundle follows AU convention instead, and Info.plist's
# AudioComponents declares what it is

AU_DIR := $(BUILD)/S-MU2000.component
AU_BIN := $(AU_DIR)/Contents/MacOS/S-MU2000

# The editor is the VST3 view, so the AU carries that too: panel_nsview.mm makes
# a smu2000::vst3::plug_view and hands it back inside an NSView, and the AUv3
# asks that same file for the same view. Its own files are plugin.cpp and
# editor_mac.mm, which is now only the AUv2 way of being asked; everything below
# them is the same panel
# iids.cpp is view.cpp's: it answers IPlugView's interface id, and view.cpp
# refers to it even when the host on the other side is an AU rather than a VST3
AU_SRCS := src/au/plugin.cpp src/au/editor_mac.mm src/vst3/engine.cpp src/vst3/iids.cpp \
           src/vst3/panel_nsview.mm \
           $(PANEL_VIEW_SRCS) $(PANEL_SRCS) $(VST3_SDK_SRCS)
AU_OBJS := $(AU_SRCS:%.cpp=$(BUILD)/vst3obj/%.o)
AU_OBJS := $(AU_OBJS:%.mm=$(BUILD)/vst3obj/%.o)

# 写真調のパネルの絵（art/real）も AU の中へ。VST3 と同じ居場所、同じ理由
# （doc/panel-editing.md）。これがないと find_default() が
# layout.cpp の内蔵の配置（"YAMAHA" の古い絵）に落ちて、素の GDI 風の
# パネルになる。プラグインの型式は違っても、中身は同じ一枚であるべき。
AU_PANEL := $(AU_DIR)/Contents/Resources/panel/panel.txt

au: $(AU_BIN) $(AU_PANEL)
$(AU_PANEL): $(wildcard art/real/*.png) art/real/panel.txt
	@mkdir -p $(dir $@)
	@cp -f art/real/*.png art/real/panel.txt $(dir $@)

# -bundle like the VST3: an AU is also read with CFBundle
$(AU_BIN): $(OBJS) $(BUILD)/src/mu2000.o $(AU_OBJS) $(MAC_PC_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -bundle -o $@ $^ $(LDFLAGS) $(MAC_FRAMEWORKS)
	@mkdir -p $(AU_DIR)/Contents/Resources
	@cp -f doc/vst3-readme.txt $(AU_DIR)/Contents/Resources/README.txt 2>/dev/null || true
	@cp -f LICENSE $(AU_DIR)/Contents/Resources/LICENSE.txt
	@cp -f NOTICE.txt $(AU_DIR)/Contents/Resources/NOTICE.txt
	@cp -f packaging/au-macos-Info.plist $(AU_DIR)/Contents/Info.plist
	@printf 'BNDL????' > $(AU_DIR)/Contents/PkgInfo

# Where the AU goes. auval looks here
AU_INSTALL ?= $(HOME)/Library/Audio/Plug-Ins/Components

install-au: $(AU_BIN) $(AU_PANEL)
	rm -rf "$(AU_INSTALL)/S-MU2000.component"
	mkdir -p "$(AU_INSTALL)"
	cp -r $(AU_DIR) "$(AU_INSTALL)/"
	@echo "入れた: $(AU_INSTALL)/S-MU2000.component"
	@echo "auval -v aumu SMU2 Trbh で確かめられる (auval -real-time-safety は最近の OS では動かない)"

# Small host that runs the AU without a DAW. -lobjc is for the editor check:
# it makes the view class the way a host does, with NSClassFromString
$(BUILD)/aubprobe$(EXE): $(BUILD)/vst3obj/src/au/probe.o $(BUILD)/src/smf.o \
                        $(BUILD)/src/compat/compat.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -framework AudioToolbox \
		-framework CoreFoundation -lobjc

au-probe: $(BUILD)/aubprobe$(EXE) $(AU_BIN)
	S_MU2000_ROMS=$(ROMS) $(BUILD)/aubprobe$(EXE) $(AU_DIR) --list

check-au: $(BUILD)/aubprobe$(EXE) $(AU_BIN)
	S_MU2000_ROMS=$(ROMS) $(BUILD)/aubprobe$(EXE) $(AU_DIR) --torture

# ---- AUv3 plug-in (macOS)
#
# The hardware jacks become ports as-is:
#   out MAIN OUT L/R / in A/D INPUT / MIDI in cable 0=IN A, 1=IN B / MIDI out MIDI OUT
#
# An AUv3 is only recognized by the system inside an app, so a silent
# container app is built alongside it. Launch it once and the unit shows
# up in DAW lists.
# The subtype differs from AUv2 (SMU3/Trbh vs SMU2/Trbh), so installing
# both never confuses them.
#
#   make auv3           build build/S-MU2000.app (with the .appex inside)
#   make install-auv3   copy it to ~/Applications and launch once (registers it)
#   make auv3-roms      put AUV3_ROMS where the extension can read them
#   make auval3         validate the plug-in (aumu SMU3 Trbh)

AUV3_APP   := $(BUILD)/S-MU2000.app
AUV3_APPEX := $(AUV3_APP)/Contents/PlugIns/S-MU2000AU.appex
AUV3_BIN   := $(AUV3_APPEX)/Contents/MacOS/S-MU2000AU
AUV3_HOST  := $(AUV3_APP)/Contents/MacOS/S-MU2000

# The sound engine is the same one VST3 uses (no VST3 types in it).
# The UI is the same panel VST3 and AUv2 show, and literally the same editor:
# panel_nsview.mm builds the NSView, view_controller.mm only puts it in the
# NSViewController the AUv3 hands its host
AUV3_SRCS := src/auv3/audio_unit.mm src/auv3/factory.mm src/auv3/view_controller.mm \
             src/vst3/engine.cpp src/vst3/iids.cpp src/vst3/panel_nsview.mm \
             $(PANEL_VIEW_SRCS) $(PANEL_SRCS) $(VST3_SDK_SRCS)
AUV3_OBJS := $(AUV3_SRCS:%.cpp=$(BUILD)/auv3obj/%.o)
AUV3_OBJS := $(AUV3_OBJS:%.mm=$(BUILD)/auv3obj/%.o)

# Certificate for signing. Ad-hoc (-) registers fine (the sandbox
# entitlements are what matter). Use a Developer ID for distribution.
#   security find-identity -v -p codesigning   lists local certificates
CODESIGN_ID ?= -

# ROMs in the bundle.
#
# **Off by default.** The images are Yamaha's, so nothing we hand out may carry
# them -- and an AUv3 cannot be handed out alone anyway (macOS only recognizes
# one inside an app), which made baking them the reason the plug-in could not
# be distributed at all.
#
# A sandboxed extension does read its own container: $HOME points at
# ~/Library/Containers/<appex id>/Data, so config_dir() (src/compat/paths.h)
# lands on .../Data/Library/Application Support/S-MU2000 -- the same directory
# the engine writes log.txt and boot snapshots into. The container app puts the
# files there once (src/auv3/main_app.mm, "Install ROMs..."), and after that the
# engine finds them like any other per-user copy.
#
#   make auv3                      a bundle with no ROMs. This is what gets
#                                  distributed
#   make auv3 AUV3_ROMS=roms       also bake the local ROMs into the bundle.
#                                  Useful while developing (nothing to install,
#                                  works in any sandbox), but the bundle then
#                                  cannot be given to anyone else
#   make auv3 AUV3_ROMS=none       take baked ROMs back out
#   make auv3-roms AUV3_ROMS=roms  put them in the extension's own Application
#                                  Support directory instead of the bundle
#
# ROMs are never redistributed, so they stay out of git (roms/ is ignored)
AUV3_ROMS ?=

AUV3_FLAGS := -fobjc-arc
AUV3_FW    := -framework Foundation -framework AudioToolbox -framework AVFoundation \
              -framework CoreAudio -framework CoreMIDI -framework Cocoa -framework CoreAudioKit \
              -framework Metal -framework QuartzCore \
              -framework UniformTypeIdentifiers

$(BUILD)/auv3obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(VST3_INC) $(IMGUI_FLAGS) -c -o $@ $<

$(BUILD)/auv3obj/%.o: %.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(VST3_INC) $(IMGUI_FLAGS) $(AUV3_FLAGS) -ObjC++ -c -o $@ $<

# Bundle finishing (ROMs in, then sign) runs every time. Doing it only
# when binaries rebuild would ignore a later-added AUV3_ROMS.
# An explicitly empty AUV3_ROMS leaves the contents as they are (so a bare
# rebuild never wipes baked ROMs). Write AUV3_ROMS=none to take them out
# 写真調のパネルの絵（art/real）も appex の中へ。VST3 / AUv2 と同じ居場所、
# 同じ理由（doc/panel-editing.md）：無いと find_default() が layout.cpp の
# 内蔵の配置に落ちて、.panel の絵が入れ替わった版と別の古い絵になる
AUV3_PANEL := $(AUV3_APPEX)/Contents/Resources/panel/panel.txt

auv3: $(AUV3_HOST) $(BUILD)/autest$(EXE) $(AUV3_PANEL)
	# ROMs into the bundle. Before signing (adding them later breaks the seal).
	# An explicitly empty AUV3_ROMS leaves a bare install alone.
	# AUV3_ROMS=none takes them out
ifeq ($(AUV3_ROMS),none)
	@rm -rf $(AUV3_APPEX)/Contents/Resources/roms
	@rm -rf $(AUV3_APPEX)/Contents/Resources/bootcache
	@echo "ROM を抜いた"
endif
ifneq ($(AUV3_ROMS),)
ifneq ($(AUV3_ROMS),none)
	@rm -rf $(AUV3_APPEX)/Contents/Resources/roms
	@mkdir -p $(AUV3_APPEX)/Contents/Resources
	@cp -R $(AUV3_ROMS) $(AUV3_APPEX)/Contents/Resources/roms
	@echo "ROM を入れた: $(AUV3_ROMS)"
	# Boot the image once here and bake the snapshot, so first insert never waits.
	# A sandboxed plug-in owns no NVRAM (empty container), so build it under an
	# empty HOME to match keys. Otherwise local settings leak in, the key
	# changes, and the baked snapshot goes unused
	@rm -rf $(AUV3_APPEX)/Contents/Resources/bootcache
	@tmp=$$(mktemp -d); \
	 HOME=$$tmp S_MU2000_ROMS=$(AUV3_ROMS) $(BUILD)/autest$(EXE) --state $$tmp/state.bin >/dev/null 2>&1; \
	 if [ -d "$$tmp/Library/Application Support/S-MU2000/bootcache" ]; then \
	   mkdir -p $(AUV3_APPEX)/Contents/Resources/bootcache; \
	   cp "$$tmp/Library/Application Support/S-MU2000/bootcache/"*.bin \
	      $(AUV3_APPEX)/Contents/Resources/bootcache/ 2>/dev/null; \
	   echo "起動の写しを焼いた: $$(ls $(AUV3_APPEX)/Contents/Resources/bootcache | head -1)"; \
	 else echo "起動の写しを作れなかった（初回は待たされる）"; fi; \
	 rm -rf "$$tmp"
endif
endif
	# Info.plists are refreshed here. The copies in the binary rules alone
	# would leave a plist-only edit stale under its signature
	@cp -f packaging/auv3-appex-Info.plist $(AUV3_APPEX)/Contents/Info.plist
	@cp -f packaging/auv3-app-Info.plist $(AUV3_APP)/Contents/Info.plist
	# The extension needs the App Sandbox entitlement: a macOS app extension
	# outside the sandbox never registers. The container app is signed without
	# it on purpose, so it can write the ROMs into the extension's container
	# (packaging/auv3-app.entitlements). Certificate kind does not matter
	# (ad-hoc works)
	@codesign --force --sign "$(CODESIGN_ID)" --timestamp=none \
	          --entitlements packaging/auv3-appex.entitlements $(AUV3_APPEX)
	@codesign --force --sign "$(CODESIGN_ID)" --timestamp=none \
	          --entitlements packaging/auv3-app.entitlements $(AUV3_APP)
	@echo "出来た: $(AUV3_APP)"

# The panel art the plug-in draws with. Its own rule, so that adding it does not
# land in the middle of the auv3 recipe above (they share one target)
$(AUV3_PANEL): $(wildcard art/real/*.png) art/real/panel.txt
	@mkdir -p $(dir $@)
	@cp -f art/real/*.png art/real/panel.txt $(dir $@)

# The .appex itself. Entry point is NSExtensionMain (it owns no main())
$(AUV3_BIN): $(OBJS) $(BUILD)/src/mu2000.o $(AUV3_OBJS) $(MAC_PC_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(AUV3_FW) \
	       -e _NSExtensionMain -fapplication-extension
	@cp -f packaging/auv3-appex-Info.plist $(AUV3_APPEX)/Contents/Info.plist

# The container app. Silent. Exists only to carry the .appex into registration
$(AUV3_HOST): $(AUV3_BIN) $(BUILD)/auv3obj/src/auv3/main_app.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $(BUILD)/auv3obj/src/auv3/main_app.o $(LDFLAGS) \
	      -framework Cocoa -framework Security
	@cp -f packaging/auv3-app-Info.plist $(AUV3_APP)/Contents/Info.plist
	@mkdir -p $(AUV3_APP)/Contents/Resources
	@cp -f LICENSE $(AUV3_APP)/Contents/Resources/LICENSE.txt
	@cp -f NOTICE.txt $(AUV3_APP)/Contents/Resources/NOTICE.txt

# Register it. Place under ~/Applications and launch once
install-auv3: auv3
	rm -rf "$(HOME)/Applications/S-MU2000.app"
	@mkdir -p "$(HOME)/Applications"
	cp -R $(AUV3_APP) "$(HOME)/Applications/"
	@echo "入れた: $(HOME)/Applications/S-MU2000.app"
	@echo "一度起動すると DAW の一覧に出る（open してよいか聞かれたら許可する）"
	@echo "ROM は入れていない。音を出すには窓の「Install ROMs...」で場所を指定する"
	@echo "（あるいは make auv3-roms AUV3_ROMS=roms / make auv3 AUV3_ROMS=roms）"

# Put the ROMs where a sandboxed .appex can read them: the extension's own
# Application Support directory, which is inside its container. No ROMs in the
# bundle, so this is what a distributed app relies on. The app's own window
# does the same thing (and asks first); this is for a script or a machine with
# no GUI session. The container exists once the app has been launched, and
# mkdir -p makes it either way.
#
# AUV3_APPEX_ID is the extension's bundle id, which is what names its container.
# Read out of the Info.plist rather than written here, so the two cannot drift:
# a plist-only edit is the kind that gets made and forgotten. Override it only
# for an odd setup (the app itself reads the id out of its own PlugIns).
AUV3_APPEX_ID ?= $(shell /usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" \
                          packaging/auv3-appex-Info.plist 2>/dev/null)
AUV3_SUPPORT   = $(HOME)/Library/Containers/$(AUV3_APPEX_ID)/Data/Library/Application Support/S-MU2000

auv3-roms:
ifneq ($(strip $(AUV3_ROMS)),)
	@test -f "$(AUV3_ROMS)/mu2000_flash.bin" || \
	  { echo "AUV3_ROMS に mu2000_flash.bin が無い: $(AUV3_ROMS)"; exit 1; }
	@mkdir -p "$(AUV3_SUPPORT)/roms/dump"
	@cp -f "$(AUV3_ROMS)/mu2000_flash.bin" "$(AUV3_SUPPORT)/roms/"
	@for f in xv364a0.ic49 xv365a0.ic50 xw848a0.ic53 xw849a0.ic54; do \
	   test -f "$(AUV3_ROMS)/dump/$$f" || { echo "dump/$$f が無い"; exit 1; }; \
	   cp -f "$(AUV3_ROMS)/dump/$$f" "$(AUV3_SUPPORT)/roms/dump/"; \
	 done
	@for f in standin/sin-table.bin hd44780u_b04.bin standin/hd44780u_b04.bin; do \
	   test -f "$(AUV3_ROMS)/$$f" && cp -f "$(AUV3_ROMS)/$$f" "$(AUV3_SUPPORT)/roms/$$f"; \
	   true; \
	 done
	@echo "入れた: $(AUV3_SUPPORT)/roms"
else
	@echo "AUV3_ROMS が空。make auv3-roms AUV3_ROMS=roms"
endif

# Register in-process (no .appex) to check ports and sound on the spot
$(BUILD)/autest$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/smf.o $(AUV3_OBJS) \
                       $(BUILD)/auv3obj/src/auv3/autotest.o $(MAC_PC_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(AUV3_FW)

autest: $(BUILD)/autest$(EXE)

auval3: install-auv3
	@sleep 2
	auval -v aumu SMU3 Trbh

endif # windows / macOS

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# 内蔵周辺のレジスタ振り分けは MAME の map() から起こす。
# MAME のソースの場所は MAME_SH7042 で渡す
MAME_SH7042 ?= ../MU2000/mame-src/src/devices/cpu/sh/sh7042.cpp

regen:
	$(PYTHON) tools/gen_sh7042_map.py $(MAME_SH7042)

# Checks that need no ROMs; this is how the port is shown to hold together
ifeq ($(PLATFORM),windows)
CHECK_PLUGIN := $(BUILD)/vstiprobe$(EXE) $(VSTI_BIN)
endif

check: $(BUILD)/verify$(EXE) $(CHECK_PLUGIN)
	$(BUILD)/verify$(EXE)
ifeq ($(PLATFORM),windows)
	$(BUILD)/vstiprobe$(EXE) $(VSTI_BIN)
endif

# 回帰試験。直したことで音が変わっていないかを見る。
#
# ROM は同梱できないので、ROM が無い機械では verify だけが走る（それが正しい）。
# ROM の置き場は SMU2000_ROMS で渡せる。既定は roms/ か ../MU2000/roms。
#   make test                     全部
#   make test T=piano             1 件だけ
#   make test-update              指紋を焼き直す（意図して音を変えたとき）
#
# The test names are the same on both platforms: run_tests.py is the one that
# knows whether the binaries carry an .exe suffix (tools/run_tests.py)
TEST_EXES := $(BUILD)/verify$(EXE) $(BUILD)/statetest$(EXE) $(BUILD)/render$(EXE) $(BUILD)/xgtest$(EXE) \
             $(BUILD)/samptest$(EXE)

test: $(TEST_EXES)
	SMU_BUILD=$(BUILD) $(PYTHON) tools/run_tests.py $(if $(T),--only $(T),)

test-update: $(TEST_EXES)
	SMU_BUILD=$(BUILD) $(PYTHON) tools/run_tests.py --update $(if $(T),--only $(T),)

# Actual ImGui widget interactions, sample conversion/routing and persistence.
$(BUILD)/settings_test$(EXE): tools/test_settings.cpp $(IMGUI_CORE) $(wildcard src/ui/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -DIMGUI_ENABLE_TEST_ENGINE -I third_party/imgui -o $@ tools/test_settings.cpp $(IMGUI_CORE) $(LDFLAGS)

$(BUILD)/midi_routing_test$(EXE): tools/test_midi_routing.cpp src/ui/midi_router.h src/ui/midi_routes.h src/ui/midi_split.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

.PHONY: check-settings
check-settings: $(BUILD)/settings_test$(EXE) $(BUILD)/midi_routing_test$(EXE)
	$(WINE) $(BUILD)/settings_test$(EXE)
	$(WINE) $(BUILD)/midi_routing_test$(EXE)

clean:
	rm -rf $(BUILD)

# ヘッダを直したときに .o を作り直させる仕掛け（-MMD -MP が置く .d）。
#
# **1 つずつ並べてはいけない。** 並べ忘れた .o はヘッダを直しても作り直されず、
# 型の大きさが食い違ったまま繋がって落ちる（statetest がこれで落ちていた。
# swp30.h に変数を 1 つ足したら、古い大きさのまま繋がった mu2000.o が
# 別の場所を触りに行っていた）。だから build の下にある .d を全部拾う
#
# build-ios も同じ仕掛けに入れる。iOS の .d は置かれるのに読まれていなかった
# （window_ios.d は app_ios.h を正しく挙げているのに、make は見ない）ので、
# ヘッダだけ直したときは .o が作り直されず、rm -rf build-ios しないと直らない
# ように見えた。 simulator/ と device/ の両方が build-ios の下なので、文字通りの
# build-ios で両方拾う（変数にすると定義順の罠 - $(IOS_ROOT) はこの行より後で定義）。
#
# 1 つだけ手で払う場合: ソースを改名・削除したとき、古い .d が残って消えた
# .cpp を指し続ける ("No rule to make target ... audio_ios.cpp")。audio_ios.cpp →
# .mm の改名で実際に踏んだ。find build-ios -name '*.d' -delete で直る（次に
# 付く .o が .d を作り直す）。改名は稀なので、仕掛けにはしない。
-include $(shell find $(BUILD) build-ios -name '*.d' 2>/dev/null)

.PHONY: all clean regen check test test-update vst3 install-vst3 probe clap install-clap vsti install-vsti vsti-probe au install-au au-probe check-au

# ---- iOS AUv3 ----------------------------------------------------------------
#
# Everything from here to the end of the file is inside the two fences below, and
# that is the point of them: this block asks xcrun where the SDK is, runs
# tools/ios_sign.py four times over and reads a profile directory, and none of
# that is any use to a Windows or Linux build - which still had to pay for it,
# because a makefile is read whole before any target runs. So it is read only when
# an ios% goal was asked for, and only on macOS, where xcrun and the signing
# identities are.
#
# MAKECMDGOALS rather than a variable: there is no earlier point at which "which
# target was asked for" is known. The cost is that an ios% goal has to be spelled
# on the command line (it always is - these targets cannot be reached as a
# prerequisite of anything), and that a target defined here cannot be a
# prerequisite of a target defined above, which nothing wants.
ifeq ($(PLATFORM),macos)
ifneq ($(filter ios%,$(MAKECMDGOALS)),)

# The AUv3 extension built for iOS. Reuses AUV3_SRCS unchanged - engine, AUv3 core and
# the shared ImGui panel are already platform-free (doc/ios-auv3.md) - and only replaces
# the toolchain flags and the bundle layout.
#
# Two things are NOT macOS-shaped and are handled here rather than in the sources:
#
#   * Bundle layout. macOS is App.app/Contents/PlugIns/X.appex/Contents/MacOS/X.
#     iOS is flat: App.app/PlugIns/X.appex/X. There is no Contents and no MacOS
#     subdirectory.
#   * Toolchain. -isysroot plus -target arm64-apple-ios, and the macOS deployment
#     target has to be filtered out or clang rejects the combination.
#
# The AppKit-only files are excluded (pc_window_mac.mm, window_mac.mm): UIKit versions
# are step 7, and the extension builds and installs without them.
# Two SDKs, and they are not interchangeable: iphoneos builds for a device
# (LC_BUILD_VERSION platform 2), iphonesimulator for the simulator (platform 7).
# Installing a device build on the simulator fails with "does not contain code for any
# platform ... this device can run code for iOS-simulator", so each gets its own tree.
#   make ios-app                                        device
#   make ios-app IOS_SDK_NAME=iphonesimulator           simulator
IOS_SDK_NAME ?= iphoneos
IOS_SDK    := $(shell xcrun --sdk $(IOS_SDK_NAME) --show-sdk-path)
IOS_MIN    ?= 17.0
# 17.0, not 14.0: the AUv3 only needs 14 (UMP arrived there), but the current
# Xcode's libc++ no longer supports 14 as a deployment target and warns
# "The selected platform is no longer supported by libc++" for it. 17 costs
# nothing - every device that can host an AUv3 in 2026 runs it - and the plists
# below carry the same floor so an install can never disagree with the binary.
ifeq ($(IOS_SDK_NAME),iphonesimulator)
IOS_TARGET := arm64-apple-ios$(IOS_MIN)-simulator
IOS_ROOT   := build-ios/simulator
else
IOS_TARGET := arm64-apple-ios$(IOS_MIN)
IOS_ROOT   := build-ios/device
endif
# IOS_DEBUG defaults to 1 and should stay that way until the extension has booted, found its
# ROMs and produced audio. Without debug info a crash report is a wall of hex offsets
# (the first launch died with a frame at 0xccc6e4 and no name); with it the same report
# names UIApplicationEvaluateRuntimeIssueForNoSceneLifecycleAdoption immediately.
#
# -Og rather than -O0: -O0 is the reflex answer and it is wrong here, because this process
# emulates a 28 MHz SH2 in a C++ interpreter and an unoptimised interpreter can slow a boot
# enough to muddy the measurement we are trying to make. -Og keeps the frames and their
# callers - it does not inline away the function being debugged - and leaves the hot loop
# largely alone.
#
# -Og is the part that matters. Apple's linker discards DWARF from the linked executable
# (verified: obj-g/src/ios/smoke.o has 10 debug sections, the linked binary has 0, even when
# linked by hand with -g), so -g buys nothing in the artefact. What it does keep is the symbol
# table - 91 symbols, 15 of them SmokeDelegate methods - and that is what a crash report uses
# to symbolicate. -Og is therefore there to stop the optimiser inlining those names away, not
# to carry debug info. Keep it until this stops crashing; the cost is interpreter speed.
#
# The object directory is suffixed because make does not know that a flag changed. Sharing one
# directory would let a toggle silently link a mixture of -O3 and -Og objects, which is worse
# than either setting.
#
# This block must stay ABOVE the IOS_BUILD assignment: := expands immediately, so a use above
# the definition yields an empty value and no error. That happened once - IOS_BUILD came out
# as build-ios/simulator/ with no object subdirectory and IOS_OPTFLAGS was never applied.
IOS_DEBUG ?= 1
ifeq ($(IOS_DEBUG),1)
IOS_OPTFLAGS := -Og -g
IOS_OBJ      := obj-g
else
IOS_OPTFLAGS := -O3
IOS_OBJ      := obj
endif

IOS_BUILD  := $(IOS_ROOT)/$(IOS_OBJ)
IOS_APP    := $(IOS_ROOT)/S-MU2000.app
IOS_APPEX  := $(IOS_APP)/PlugIns/S-MU2000AU.appex
IOS_BIN    := $(IOS_APPEX)/S-MU2000AU

# The -mXXX-version-min flag is per-SDK and does not exist for the other one; the
# -target triple already carries the deployment version, so drop it and let the triple
# do that job rather than passing a flag the SDK rejects.
IOS_CXXFLAGS := $(filter-out -mmacosx-version-min=% -O3,$(CXXFLAGS)) $(IOS_OPTFLAGS) \
                -isysroot $(IOS_SDK) -target $(IOS_TARGET)
# CoreText is not optional: src/ui/font_file.h walks family name -> font file through
# CTFontDescriptorCreateWithAttributes / CTFontDescriptorCopyAttribute to find the CJK
# face. The header compiles on iOS; it is the link that needs the framework.
IOS_FW := -framework Foundation -framework AudioToolbox -framework AVFoundation \
          -framework AVFAudio -framework CoreAudioKit -framework UniformTypeIdentifiers \
          -framework CoreAudio -framework CoreMIDI -framework UIKit -framework Metal \
          -framework QuartzCore -framework CoreGraphics -framework CoreText

IOS_ENGINE_OBJS := $(SRCS:%.cpp=$(IOS_BUILD)/%.o)

# Deliberately not AUV3_SRCS. That list carries the AppKit view layer -
# src/auv3/view_controller.{h,mm}, src/vst3/panel_nsview.h and src/vst3/view_mac.mm -
# which cannot compile for iOS. audio_unit.mm itself is portable (only AUAudioUnit,
# AUMIDIEventList and AUEventBlock) and is reused unchanged; the factory is the iOS
# no-UI variant. The view layer is step 7.
# src/mu2000.cpp is separate because the macOS rule passes it separately too
# ($(BUILD)/src/mu2000.o): it is the machine's own API - run_sample, lcd_render,
# native_midi, load_state and the rest - and the engine calls into it directly.
# The PC-editor sources are the ui::xgui layer the engine also calls; they come from
# MAC_PC_SRCS with pc_window_mac.mm dropped, since that is AppKit. They are ImGui and
# shared, which is the point - nothing here is rewritten for iOS.
IOS_PC_SRCS := src/ui/pc_editor.cpp src/ui/xg_ui.cpp src/ui/overview.cpp \
               src/ui/fx_editor.cpp src/ui/fx_help.cpp src/ui/part_shapes.cpp \
               src/ui/master_editor.cpp src/ui/sampling_editor.cpp \
               src/ui/sampling_romwave.cpp src/ui/sampling_presets.cpp \
               src/ui/sampling_library.cpp src/ui/fx_icons.cpp

# The AUv3-UI: factory_ios.mm is the AUViewController + factory (one class,
# like macOS), view_controller_ios.mm hosts the shared panel through
# src/vst3/panel_uiview.mm, and view.cpp is the shared plug_view both draw.
# view_mac.mm / panel_nsview.mm stay mac-only; this is their UIKit twin.
# pc_window_ios.mm is the editors' iOS host: the plugin UI opens the same five
# editors through it (ios_window::open_pc_window), so it links here, not only
# in the standalone.
IOS_AUV3_SRCS := src/auv3/audio_unit.mm src/auv3/factory_ios.mm \
                 src/auv3/view_controller_ios.mm \
                 src/ui/presenter_ios.mm \
                 src/mu2000.cpp \
                 src/vst3/engine.cpp src/vst3/iids.cpp src/vst3/view.cpp \
                 src/vst3/panel_uiview.mm src/vst3/view_ios.mm src/ui/menu_ios.mm src/ui/pc_window_ios.mm \
                 src/ui/rom_import_ios.mm src/ui/file_ask_ios.mm \
                 $(PANEL_SRCS) $(IOS_PC_SRCS) $(VST3_SDK_SRCS)
IOS_AUV3_OBJS := $(IOS_AUV3_SRCS:%.cpp=$(IOS_BUILD)/%.o)
IOS_AUV3_OBJS := $(IOS_AUV3_OBJS:%.mm=$(IOS_BUILD)/%.o)

# ImGui core plus the Metal backend, which is iOS's own GPU API and is already vendored.
# The PC-editor window (pc_window_mac.mm) is AppKit and is left out.
IOS_IMGUI_OBJS := $(IMGUI_CORE:%.cpp=$(IOS_BUILD)/%.o) \
                  $(IOS_BUILD)/$(IMGUI_DIR)/backends/imgui_impl_metal.o

$(IOS_BUILD)/$(IMGUI_DIR)/backends/imgui_impl_metal.o: $(IMGUI_DIR)/backends/imgui_impl_metal.mm
	@mkdir -p $(dir $@)
	$(CXX) $(IOS_CXXFLAGS) $(IMGUI_FLAGS) -fobjc-arc -c -o $@ $<

$(IOS_BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(IOS_CXXFLAGS) $(VST3_INC) $(IMGUI_FLAGS) -c -o $@ $<

$(IOS_BUILD)/%.o: %.mm
	@mkdir -p $(dir $@)
	$(CXX) $(IOS_CXXFLAGS) $(VST3_INC) $(IMGUI_FLAGS) $(AUV3_FLAGS) -ObjC++ -c -o $@ $<

# ROMs baked into the extension. Off by default for the same reason as macOS: the
# images are Yamaha's and must not travel in anything we hand out. The normal
# path on iOS is not baking at all - the user picks the dump from Files or
# iCloud Drive and it is copied into the app's own container
# (src/ui/rom_import_ios.mm), which the shared ROM search finds ahead of the bundle.
# Baking stays only as a development shortcut (no picker round trip per launch).
# engine.cpp already searches module_dir()/../Resources/roms, which lands here on the
# flat iOS layout.
#   make ios-auv3                     no ROMs (a build to look at)
#   make ios-auv3 IOS_ROMS=roms       ROMs baked into S-MU2000AU.appex/Resources/roms
IOS_ROMS ?=

# The appex's ROMs are a *prerequisite of its signing*, not a step that follows it.
# codesign hashes every file in the bundle, so ROMs copied in after the sign
# invalidate it, and codesign reports that as the deeply misleading
#   "invalid Info.plist (plist or signature have been modified)"
# while the plist is in fact perfect - simctl then fails the whole install with
# "Missing bundle ID", because it cannot read the extension. This bit: the signature
# was from 15:17 and Resources/ from 15:27.
IOS_APPEX_STAMP := $(IOS_ROOT)/.appex.stamp

# Defined before first use, always: := expands immediately, so a use above the
# definition gives an empty target name and make reports nothing to do, silently.
IOS_APPEX_PLIST := $(IOS_APPEX)/Info.plist

# Device provisioning lives HERE, above first use: every SIGN_DEPS/SIGN_FLAGS
# below is :=-expanded at its rule, and a use above the definition silently
# expands empty (the fourth time this trap has bitten - see the comment inside).
# ---- device provisioning (iphoneos only) --------------------------------------
# A real device's installd refuses a bundle without embedded.mobileprovision:
# "Application is missing the application-identifier entitlement". The profile
# comes from one manual Xcode pass (team + run on the device) and lives in
# ~/Library/MobileDevice/Provisioning Profiles/. This finds it by the bundle id
# read from the packaging plist (so renames propagate) and fails LOUDLY when
# absent, instead of producing an app that installs nowhere. The simulator needs
# ad-hoc and nothing else, so all of it is iphoneos-only.
#
# Only the app profile is handled: whether installd also demands one for the
# appex id is unknown until an install says so - that is the next round IF the
# error names it, not a structure built on a guess.
ifeq ($(IOS_SDK_NAME),iphoneos)
# Who is signing. A bundle id belongs to a team, so a third-party build needs its
# own ids and they have to be registered before a profile exists; tools/ios_sign.py
# derives them from the team and finds the identity and the profiles. The team
# comes from TEAM_ID=, SMU2000_IOS_TEAM_ID or .ios-team-id (see ios-team-id below),
# and with none of them set everything is the shipped id and an ad-hoc signature -
# which is all the simulator needs, and all a device install will refuse.
#
# `:=` so each query runs once at parse time, not once per rule that mentions it.
IOS_TEAM_ID    := $(shell $(PYTHON) tools/ios_sign.py team --team "$(TEAM_ID)")
IOS_BUNDLE_ID  := $(shell $(PYTHON) tools/ios_sign.py app-id --team "$(TEAM_ID)")
IOS_APPEX_ID   := $(shell $(PYTHON) tools/ios_sign.py appex-id --team "$(TEAM_ID)")
IOS_IDENTITY   := $(shell $(PYTHON) tools/ios_sign.py identity --team "$(TEAM_ID)")
# With a team, sign as that team's identity; without one, ad-hoc (-), which is
# what a build for the simulator or for a jailbroken device wants.
#
# IOS_CODESIGN_ID, not CODESIGN_ID: the shared CODESIGN_ID ?= above is what the
# macOS auv3 recipes sign with, and `CODESIGN_ID="Developer ID: ..." make auv3`
# is a documented thing to type. This line used `:=` on the same name, so on any
# invocation that reached it the macOS value was replaced by an ad-hoc one - and
# because the iphoneos branch is the default, that was every make that read this
# block, including one that only wanted to know whether the mac plug-ins were
# signed. Two names, one meaning each.
IOS_CODESIGN_ID := $(if $(IOS_IDENTITY),$(IOS_IDENTITY),-)
# A team with profiles but no certificate signs ad-hoc, and an ad-hoc signature
# installs nowhere: the device refuses it at install time, long after the build
# said nothing was wrong. So this is checked before signing rather than left for
# installd to find. A build that wants ad-hoc on purpose (a jailbroken device)
# asks for it by passing an empty team, which is what the simulator does anyway.
.PHONY: FORCE

define IOS_REQUIRE_IDENTITY
	@if [ -n "$(IOS_TEAM_ID)" ] && [ -z "$(IOS_IDENTITY)" ]; then \
	  echo "ios: team $(IOS_TEAM_ID) has no signing certificate on this machine"; \
	  echo "     Xcode > Settings > Accounts > $(IOS_TEAM_ID) > Manage Certificates"; \
	  echo "     then + Apple Development. A profile alone does not sign anything."; \
	  echo "     (an ad-hoc device build on purpose: make ... IOS_TEAM_ID=)"; \
	  exit 1; \
	fi
endef
# Two locations: Xcode 27 keeps managed profiles under UserData, older Xcode (and
# manual downloads) under MobileDevice. Both are searched; the bundle id decides.
IOS_PROV_DIRS := $(HOME)/Library/Developer/Xcode/UserData/Provisioning\ Profiles $(HOME)/Library/MobileDevice/Provisioning\ Profiles

# The embedded profiles, found by bundle id: tools/ios_sign.py reads each one's
# own application-identifier (a wildcard counts - free provisioning hands out
# those), newest first. IOS_PROFILE / IOS_APPEX_PROFILE override with an explicit
# path when several match or the wrong one wins; the printed basename says which
# one won, so a surprise is visible rather than silent.
#
# FORCE re-runs both rules on every build, and cmp makes that a no-op when the
# profile that is there is already the right one. It is there because the bundle
# id is derived from the team: a build that switched teams would otherwise keep
# the previous team's profile, and installd would refuse the install with a
# message about an id nobody asked for.
$(IOS_APP)/embedded.mobileprovision: FORCE
	@prof="$(IOS_PROFILE)"; \
	if [ -z "$$prof" ]; then prof="$(shell $(PYTHON) tools/ios_sign.py profile --which APP --team "$(TEAM_ID)")"; fi; \
	if [ -z "$$prof" ]; then \
	  echo "ios: no provisioning profile for $(IOS_BUNDLE_ID)"; \
	  echo "     a profile belongs to the team that registered the id, so set yours:"; \
	  echo "       make ios-team-id TEAM_ID=ABCDE12345   (then 'make ios-sign-info')"; \
	  echo "     already have one for this exact id? IOS_PROFILE=/path/to/one.mobileprovision"; \
	  exit 1; \
	fi; \
	if cmp -s "$$prof" $@; then exit 0; fi; \
	cp -f "$$prof" $@; \
	echo "ios: embedded $$(basename "$$prof")"

# The appex carries its own profile too (its id differs): installd checks nested
# code, so hoping the app's profile covers it is a guess the error would bill.
# Same search, same loud failure, its own override.
$(IOS_APPEX)/embedded.mobileprovision: FORCE
	@prof="$(IOS_APPEX_PROFILE)"; \
	if [ -z "$$prof" ]; then prof="$(shell $(PYTHON) tools/ios_sign.py profile --which APPEX --team "$(TEAM_ID)")"; fi; \
	if [ -z "$$prof" ]; then \
	  echo "ios: no provisioning profile for $(IOS_APPEX_ID) (the AUv3's own id)"; \
	  echo "     the extension target needs its own; 'make ios-sign-info' says what is missing"; \
	  echo "     already have one? IOS_APPEX_PROFILE=/path/to/one.mobileprovision"; \
	  exit 1; \
	fi; \
	if cmp -s "$$prof" $@; then exit 0; fi; \
	cp -f "$$prof" $@; \
	echo "ios: embedded $$(basename "$$prof") in appex"

# The entitlements the profiles carry (application-identifier above all).
# PlistBuddy prints the Entitlements subdict as XML, which is what --entitlements
# wants at top level.
$(IOS_ROOT)/app.xcent: $(IOS_APP)/embedded.mobileprovision
	@security cms -D -i $< -o $(IOS_ROOT)/prov.plist
	@/usr/libexec/PlistBuddy -x -c "Print :Entitlements" $(IOS_ROOT)/prov.plist > $@

$(IOS_ROOT)/appex.xcent: $(IOS_APPEX)/embedded.mobileprovision
	@security cms -D -i $< -o $(IOS_ROOT)/prov-appex.plist
	@/usr/libexec/PlistBuddy -x -c "Print :Entitlements" $(IOS_ROOT)/prov-appex.plist > $@

IOS_APP_SIGN_DEPS  := $(IOS_APP)/embedded.mobileprovision $(IOS_ROOT)/app.xcent
IOS_APP_SIGN_FLAGS := --entitlements $(IOS_ROOT)/app.xcent
IOS_APPEX_SIGN_DEPS  := $(IOS_APPEX)/embedded.mobileprovision $(IOS_ROOT)/appex.xcent
IOS_APPEX_SIGN_FLAGS := --entitlements $(IOS_ROOT)/appex.xcent
else
IOS_APP_SIGN_DEPS  :=
IOS_APP_SIGN_FLAGS :=
# Simulator: the shipped (empty) entitlements file. One --entitlements only -
# codesign takes a single one, so the device branch above replaces this rather
# than adding to it.
IOS_APPEX_SIGN_DEPS  :=
IOS_APPEX_SIGN_FLAGS := --entitlements packaging/auv3-ios-appex.entitlements
# Ad-hoc on purpose, and set here rather than inherited: a simulator install
# wants no identity and no profile, and IOS_CODESIGN_ID is only defined in the
# branch above, so without this the recipes below would ask codesign to sign with
# an empty string.
IOS_CODESIGN_ID := -
endif

$(IOS_APPEX_STAMP): $(IOS_BIN) $(IOS_APPEX_PLIST) ios-auv3-roms $(IOS_APPEX)/art/real/panel.txt $(IOS_APPEX_SIGN_DEPS)
	$(IOS_REQUIRE_IDENTITY)
	@codesign --force --sign "$(IOS_CODESIGN_ID)" --timestamp=none \
	          $(IOS_APPEX_SIGN_FLAGS) $(IOS_APPEX)
	@touch $@

.PHONY: ios-auv3-signed

ios-auv3-signed: $(IOS_APPEX_STAMP)

ios-auv3-roms:
ifneq ($(strip $(IOS_ROMS)),)
	@rm -rf $(IOS_APPEX)/roms
	@cp -R $(IOS_ROMS) $(IOS_APPEX)/roms
	@echo "ROM を入れた: $(IOS_ROMS) -> $(IOS_APPEX)/roms"
endif

# The appex Info.plist is its own target, not a step inside the link rule.
# Copied from inside that rule it could go stale: a relink produced a fresh
# binary while the plist stayed at its old mtime, and codesign compares the
# binary's embedded plist against the file, so it failed with
#   "invalid Info.plist (plist or signature have been modified)"
# naming the plist rather than the staleness. simctl then refused the whole
# install with "Missing bundle ID", because it cannot read the extension.
#
# Nothing to do with nested Resources subdirectories: the signature covers
# roms/, roms/dump/mu1000/ and roms/standin/ correctly. Nor with hashes -
# Info.plist is deliberately omitted from CodeResources by the signature rules
# ('^Info\.plist$': omit), which is why nothing in CodeResources could have
# flagged this.
$(IOS_APPEX_PLIST): packaging/auv3-ios-appex-Info.plist
	@mkdir -p $(dir $@)
	@cp -f $< $@
	@if [ -n "$(IOS_APPEX_ID)" ] && \
	    [ "$(IOS_APPEX_ID)" != "$$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" $<)" ]; then \
	  /usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier $(IOS_APPEX_ID)" $@; \
	  echo "ios: appex id -> $(IOS_APPEX_ID)"; \
	fi

$(IOS_BIN): $(IOS_ENGINE_OBJS) $(IOS_AUV3_OBJS) $(IOS_IMGUI_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(IOS_CXXFLAGS) -o $@ $^ $(IOS_FW) -e _NSExtensionMain -fapplication-extension
	@echo "出来た: $(IOS_BIN)"

# The thin host app. iOS only discovers app extensions inside a containing app, so the
# extension cannot be tested without one. src/ios/smoke.mm renders offline and reports a
# peak, which needs no audio session, no output route and no hardware - so the same test
# runs on the simulator and on a device.
IOS_APP_BIN   := $(IOS_APP)/S-MU2000        # the smoke-test host
IOS_STANDALONE := $(IOS_APP)/Standalone       # the real front end, step 1

# ---- iOS standalone front end -------------------------------------------------
#
# Shaped like MAC_GUI_SRCS + MAC_PC_SRCS, so the two read as twins:
#
#   *_GUI_SRCS   the shared panel, the per-concern ports (_ios), the window shell
#                and the main
#   *_PC_SRCS    the five PC editors, which are the same files as everywhere
#
# The deliberate difference from the macOS list: no *_mac equivalents for MIDI,
# and none for audio either - audio is shared instead (src/ui/audio_apple.mm is in
# IOS_AUDIO_SRCS and answers the questions the shared core cannot).
#
# src/ios/smoke.mm is not in here. It has its own main() and answers a different
# question ("does the extension register?"), so it is a separate executable.
IOS_GUI_SRCS := src/ui/panel.cpp src/ui/editor.cpp src/ui/effects.cpp \
                src/ui/png.cpp src/ui/layout.cpp src/ui/svg.cpp src/ui/player.cpp \
                src/xg/model.cpp \
                src/ui/window_ios.mm src/ui/app_ios.cpp src/ui/pc_window_ios.mm \
                src/ui/menu_ios.mm src/ui/presenter_ios.mm \
                src/ui/rom_import_ios.mm src/ui/file_ask_ios.mm \
                src/ios/app.mm

# MIDI: CoreMIDI, shared with macOS. CoreMIDI.h is complete on iOS -
# MIDIClientCreate, MIDIPortConnectSource and the event-block variants are all
# declared there - so these two compile for it as they are: plain C++ with no #if
# and no Objective-C, which is also why they need no -ObjC++. Named _apple because
# src/ui/audio_apple.mm for the same reason: one file, both platforms.
#
# Audio: NOT portable. CoreMIDI is the same API on iOS and CoreAudio is not: iOS
# ships CoreAudio.framework with only three headers (AudioHardwareBase.h,
# AudioServerPlugIn.h, CoreAudioTypes.h) and no umbrella, and
# AudioObjectGetPropertyData appears in no public header - only in the link stub
# CoreAudio.tbd. The AudioHardware HAL that audio_out_mac.cpp and audio_in_mac.cpp
# are built on therefore does not exist in public form on iOS, and those two fail
# to compile there on "CoreAudio/CoreAudio.h file not found". iOS gets
# AVAudioSession + AVAudioEngine against the same class interfaces instead.
IOS_APPLE_PORT_SRCS := src/ui/midi_in_apple.cpp src/ui/midi_out_apple.cpp
# Three files, one per direction as on every other platform, plus the core the
# two share. audio_core_ios.mm holds audio_out's and audio_in's own methods - the
# render block, the input tap, the resampler, the ring - because both directions
# use the same ring and the same WAV header; audio_out_ios.mm and
# audio_in_ios.mm answer what only iOS can (which device, which permission, which
# sample rate the device runs at). session_ios.mm is the AVAudioSession, which
# both directions watch.
# audio_apple.mm is the render path both platforms share, and it holds
# audio_out's and audio_in's own methods; the two _ios files are what only iOS
# can answer, one per direction as on every other platform. session_ios.mm is
# the AVAudioSession both of them share, and its counterpart on the macOS side
# is session_mac.cpp, which answers the same two questions with nothing.
IOS_AUDIO_SRCS := src/ui/audio_out_ios.mm src/ui/audio_in_ios.mm \
                  src/ui/audio_apple.mm src/ui/session_ios.mm

IOS_GUI_OBJS := $(IOS_GUI_SRCS:%.cpp=$(IOS_BUILD)/%.o)
IOS_GUI_OBJS := $(IOS_GUI_OBJS:%.mm=$(IOS_BUILD)/%.o)

IOS_APPLE_PORT_OBJS := $(IOS_APPLE_PORT_SRCS:%.cpp=$(IOS_BUILD)/%.o)

IOS_AUDIO_OBJS := $(IOS_AUDIO_SRCS:%.mm=$(IOS_BUILD)/%.o)

# PC editor: the same view files as macOS and Windows, built once.
IOS_PC_OBJS := $(IOS_PC_SRCS:%.cpp=$(IOS_BUILD)/%.o)

# IMGUI_FLAGS on the three front-end files: app.h reaches fx_editor.h, which
# includes imgui.h. The generic %.mm rule below carries it for the extension, but
# these rules exist to add -ObjC++ without the AUv3 flags, so they must not drop it.
#
# -fobjc-arc on the two .mm files because every other Objective-C++ file here uses
# it (the mac %.mm rule, AUV3_FLAGS): without it these built as MRC, which the
# compiler reported as a missing [super dealloc] in dealloc - and an MRC
# CADisplayLink target is a dangling-pointer crash waiting for the autorelease pool
# to drain. pc_window_ios.mm already had ARC through the generic %.mm rule, so this
# also makes the three iOS files consistent with each other.
$(IOS_BUILD)/src/ios/app.o: src/ios/app.mm
	@mkdir -p $(dir $@)
	$(CXX) $(IOS_CXXFLAGS) $(IMGUI_FLAGS) -ObjC++ -fobjc-arc -c -o $@ $<

$(IOS_BUILD)/src/ui/window_ios.o: src/ui/window_ios.mm
	@mkdir -p $(dir $@)
	$(CXX) $(IOS_CXXFLAGS) $(IMGUI_FLAGS) -ObjC++ -fobjc-arc -c -o $@ $<

$(IOS_BUILD)/src/ui/app_ios.o: src/ui/app_ios.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(IOS_CXXFLAGS) $(IMGUI_FLAGS) -c -o $@ $<

# mu2000.o and smf.o are named explicitly because IOS_ENGINE_OBJS is SRCS - the SH2
# and its peripherals - and neither of those two is in it. Every desktop target links
# the same pair ($(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/smf.o), and smf.cpp is
# what ui/player.cpp calls to read a MIDI file: without it, smf::load is undefined.
# Neither file has a platform dependency.
#
# Deliberately NOT $(IOS_BIN): that is the appex *executable*, and linking one gives
# "ld: unsupported mach-o filetype (only MH_OBJECT and MH_DYLIB can be linked)". The
# two ship in one bundle and do not share a binary.
$(IOS_STANDALONE): $(IOS_GUI_OBJS) $(IOS_APPLE_PORT_OBJS) $(IOS_AUDIO_OBJS) \
                   $(IOS_PC_OBJS) \
                   $(IOS_IMGUI_OBJS) $(IOS_ENGINE_OBJS) \
                   $(IOS_BUILD)/src/mu2000.o $(IOS_BUILD)/src/smf.o
	@mkdir -p $(dir $@)
	$(CXX) $(IOS_CXXFLAGS) -o $@ $^ $(IOS_FW)

$(IOS_BUILD)/src/ios/smoke.o: src/ios/smoke.mm
	@mkdir -p $(dir $@)
	$(CXX) $(IOS_CXXFLAGS) -fobjc-arc -c -o $@ $<

$(IOS_APP_BIN): $(IOS_BUILD)/src/ios/smoke.o $(IOS_BIN)
	@mkdir -p $(dir $@)
	$(CXX) $(IOS_CXXFLAGS) -o $@ $(IOS_BUILD)/src/ios/smoke.o $(IOS_FW)
	$(IOS_REQUIRE_IDENTITY)
	@codesign --force --sign "$(IOS_CODESIGN_ID)" --timestamp=none $(IOS_APP)
	@echo " smoke host: $(IOS_APP)"

# The standalone, not the smoke host. Same bundle, different executable, so the
# .appex still registers; the two never ship together.
#
# Simulator first: no signing, no certificate, no device.
#   make ios-standalone IOS_SDK_NAME=iphonesimulator
#   xcrun simctl install booted build-ios/simulator/S-MU2000.app
#   xcrun simctl launch booted com.tarboh.smu2000.ios.app
#   make ios-app IOS_SDK_NAME=iphonesimulator
#   xcrun simctl boot "iPhone 18 Pro"
#   xcrun simctl install booted build-ios/simulator/S-MU2000.app
#   xcrun simctl spawn booted log stream --predicate 'process == "S-MU2000"'
#   xcrun simctl launch booted com.tarboh.smu2000
# The Info.plist is its own target, not a step inside one of the executables' rules.
# It used to be copied by the smoke rule only, so `make ios-standalone` reused
# whatever plist happened to be in the bundle - and a stale UISceneDelegateClassName
# is invisible: UIKit resolves no such class, never calls the scene delegate, and the
# app shows a black screen with no log line and no crash.
$(IOS_APP)/Info.plist: packaging/ios-app-Info.plist
	@mkdir -p $(dir $@)
	@cp -f $< $@
	@if [ -n "$(IOS_BUNDLE_ID)" ] && \
	    [ "$(IOS_BUNDLE_ID)" != "$$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" $<)" ]; then \
	  /usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier $(IOS_BUNDLE_ID)" $@; \
	  echo "ios: app id -> $(IOS_BUNDLE_ID)"; \
	fi

.PHONY: ios ios-team-id ios-sign-info

# `make ios` on its own: the iOS targets and what each is for. The build has a
# dozen iOS targets and the one a newcomer wants is usually two steps away from
# its name (a simulator run needs a ROM import first, a device run needs a
# profile), so the list says which is which.
ios:
	@echo "iOS targets (SDK: IOS_SDK_NAME=iphoneos (default) or iphonesimulator)"
	@echo
	@echo "  simulator, no signing needed (IOS_SDK_NAME=iphonesimulator):"
	@echo "    ios-standalone            the synth, for the simulator"
	@echo "    ios-auv3                  the AUv3 extension on its own"
	@echo "    ios-app                   the smoke host (renders offline, checks the appex)"
	@echo "    ios-standalone IOS_ROMS=roms  ... with the ROM images in the bundle"
	@echo "    ios-install               build, install and launch on the booted simulator"
	@echo
	@echo "  device (a team id, and profiles for the ids it derives):"
	@echo "    ios-sign-info TEAM_ID=...  what signing would use, and what is missing"
	@echo "    ios-team-id   TEAM_ID=...  remember the team in .ios-team-id, once"
	@echo "    ios-app       TEAM_ID=...  signed .app"
	@echo "    ios-standalone TEAM_ID=... signed, without the app wrapper"
	@echo
	@echo "  ROMs (one-time; the images are Yamaha's and not shipped):"
	@echo "    ios-app-roms IOS_ROMS=dir copy a set into the app bundle"
	@echo
	@echo "  doc/ios-auv3.md has the whole story, install and troubleshooting."
	@echo "  Start with 'make ios-sign-info TEAM_ID=...' if you are building for a device."


# Remember the team in the checkout, so the iOS targets need no argument after
# this once. The file is git-ignored (see .gitignore) and only ever read.
ios-team-id:
	@if [ -z "$(TEAM_ID)" ]; then \
	  echo "usage: make ios-team-id TEAM_ID=ABCDE12345"; \
	  echo "  (find it in Xcode > Settings > Accounts, or the developer portal)"; \
	  exit 1; \
	fi
	@echo "$(TEAM_ID)" > .ios-team-id
	@echo "ios: team $(TEAM_ID) written to .ios-team-id (git-ignored)"

# What signing would use, and what is still missing. The first thing to run when
# a device install is refused: it prints the two ids a profile has to exist for.
ios-sign-info:
	@$(PYTHON) tools/ios_sign.py report --team "$(TEAM_ID)"

.PHONY: ios-app-info ios-install ios-install-sim

ios-app-info: $(IOS_APP)/Info.plist

# Build, install and launch on the booted simulator: the loop that runs dozens of
# times a day, so it should not be three commands with a device name in them. The
# device is named rather than left as "booted", which is ambiguous with two
# simulators up and installs to the wrong one. Simulator only - a device needs a
# team and a profile, which is `ios-app TEAM_ID=...` plus devicectl (doc).
#
# --console-pty is deliberately absent: it attaches stdout for as long as the app
# runs, which is what you want while watching and what you do not want in a target
# that is supposed to return.
# Any booted iOS simulator, preferring an iPhone where both are up: the name is
# the only thing to go on, and an iPad is just as good a target.
IOS_SIMULATOR_UDID := $(shell xcrun simctl list devices booted -j 2>/dev/null | \
	python3 -c 'import json,sys; ds=[d for v in json.load(sys.stdin)["devices"].values() for d in v]; print(next((d["udid"] for d in ds if "iPhone" in d["name"]), ds[0]["udid"]) if ds else "")' 2>/dev/null)

# A second invocation, not a target-specific variable: IOS_SDK_NAME defaults to
# the device, and every path below (IOS_ROOT, IOS_APP) was resolved while this
# makefile was read, so changing it per target would be too late - the install
# would run against the device tree. So the entry point re-executes make with
# the simulator SDK set from the start.
ios-install:
	@$(MAKE) --no-print-directory ios-install-sim IOS_SDK_NAME=iphonesimulator

.PHONY: ios-install-sim

ios-install-sim:
	@$(MAKE) --no-print-directory ios-standalone
	@# The name in the plist is whichever front end was built last (both live in
	@# one bundle), and the stamps mean a target whose binary is current does not
	@# re-run its recipe - so an `ios-app` before this leaves CFBundleExecutable
	@# saying S-MU2000 and installd refuses the bundle. Set it here and re-sign,
	@# because changing the plist changes what is signed.
	@/usr/libexec/PlistBuddy -c "Set :CFBundleExecutable Standalone" $(IOS_APP)/Info.plist
	@# Ad-hoc, with no entitlements: a simulator install wants neither an identity
	@# nor a profile, and the device answers are not resolved in this invocation.
	@codesign --force --sign - --timestamp=none $(IOS_APP)
	@if [ -z "$(IOS_SIMULATOR_UDID)" ]; then \
	  echo "ios: no booted iPhone simulator - start one in Xcode, or:"; \
	  echo "     xcrun simctl boot \"iPhone 17\""; \
	  exit 1; \
	fi
	@xcrun simctl install "$(IOS_SIMULATOR_UDID)" $(IOS_APP)
	@xcrun simctl launch "$(IOS_SIMULATOR_UDID)" $(shell /usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" $(IOS_APP)/Info.plist)
	@echo "ios: launched on $(IOS_SIMULATOR_UDID)"
	@echo "     logs:    xcrun simctl spawn $(IOS_SIMULATOR_UDID) log stream --predicate 'processImagePath CONTAINS \"S-MU2000\"'"
	@echo "     picture: xcrun simctl io $(IOS_SIMULATOR_UDID) screenshot /tmp/panel.png"
	@echo "     ROMs:    launch it once and use the card menu, or IOS_ROMS=roms on the build above"

# Both front ends live in one bundle and choose between themselves by name, so each
# target must SET the name rather than inherit whatever the other left behind.
# Neither did at first, and the symptom was confusing rather than obvious:
#   make ios-standalone ... ; make ios-app ...   -> nothing to do, and the app still
#     ran the standalone, because CFBundleExecutable still said Standalone
# deleting the app first "fixed" it, because that re-copied Info.plist from source.
#
# A stamp per front end, each depending on the binary, the plist and the shared
# assets, with the sign last - the sign has to cover the final contents.
# Both stamps must be DEFINED before they are USED as targets. := expands
# immediately, so a use above the definition yields an empty target name and
# make reports nothing to do with no error at all. That happened once: the
# standalone's stamp was defined 25 lines below its own rule.
IOS_PANEL_DIR   := $(IOS_APP)/art/real


IOS_SMOKE_STAMP := $(IOS_ROOT)/.smoke.stamp
IOS_APP_STAMP  := $(IOS_ROOT)/.standalone.stamp

$(IOS_SMOKE_STAMP): $(IOS_APP_BIN) $(IOS_APP)/Info.plist $(IOS_APPEX_STAMP) $(IOS_APP_SIGN_DEPS)
	@/usr/libexec/PlistBuddy -c "Set :CFBundleExecutable S-MU2000" \
	    $(IOS_APP)/Info.plist
	$(IOS_REQUIRE_IDENTITY)
	@codesign --force --sign "$(IOS_CODESIGN_ID)" --timestamp=none $(IOS_APP_SIGN_FLAGS) $(IOS_APP)
	@touch $@

.PHONY: ios-app

ios-app: $(IOS_SMOKE_STAMP)
	@echo "smoke host: $(IOS_APP)"

# Points CFBundleExecutable at Standalone and re-signs, so one .app can carry either
# front end. Two things make this its own target rather than a step inside
# ios-app-roms: the plist is a *parallel* dependency, so PlistBuddy rewriting it while
# codesign was reading it gave "bundle format unrecognized, invalid, or unsuitable";
# and the sign has to come after the swap, or the signature covers the wrong contents.
# **Signing is last, and everything that goes into the bundle is a prerequisite of
# this rule** - the binary, the plist, the artwork and the ROMs. As siblings of the
# stamp they were all runnable in parallel with codesign, which signed a bundle whose
# Resources/panel was still being filled; the signature then covered the wrong
# contents and the install failed. Ordering inside a rule cannot fix that, only a
# dependency can.
#
# The two ROM targets are .PHONY and often no-ops (IOS_ROMS unset), which is fine:
# a phony prerequisite simply always runs, and running it with an empty IOS_ROMS
# does nothing.
$(IOS_APP_STAMP): $(IOS_STANDALONE) $(IOS_APP)/Info.plist \
                  $(IOS_PANEL_DIR)/panel.txt ios-app-roms $(IOS_APPEX_STAMP) $(IOS_APP_SIGN_DEPS)
	@/usr/libexec/PlistBuddy -c "Set :CFBundleExecutable Standalone" \
	    $(IOS_APP)/Info.plist
	$(IOS_REQUIRE_IDENTITY)
	@codesign --force --sign "$(IOS_CODESIGN_ID)" --timestamp=none $(IOS_APP_SIGN_FLAGS) $(IOS_APP)
	@touch $@

.PHONY: ios-app-stamp

ios-standalone: $(IOS_APP_STAMP)
	@echo "standalone: $(IOS_APP)"

# The panel artwork, for the standalone. Same shape as the VST3 bundle's
# $(VST3_PANEL) rule, but NOT under Resources/panel/: any subdirectory under the
# app's Resources/ breaks ad-hoc codesign with
#   "bundle format unrecognized, invalid, or unsuitable"
# verified by bisection - an empty Resources/emptydir/ fails, a lone
# Resources/lone.txt signs, and even the Apple-blessed Resources/en.lproj/ fails.
# Top-level directories (Frameworks/sub/, roms/, art/) sign fine. The likely
# mechanism, offered as a hypothesis rather than a fact: in iOS bundles lproj dirs
# and artwork live at the top level, and Resources/-with-subdirs is a macOS-bundle
# shape (Contents/Resources), so format detection misfires on a flat bundle that has
# one. Either way the empirical rule is solid: keep subdirs out of Resources/.
#
# So the art goes to S-MU2000.app/art/real/, which layout::find_default() reaches
# through its step 4 (module_dir + "art/real/panel.txt") with no code change: on the
# flat iOS bundle module_dir() is the app root itself. The images resolve relative
# to panel.txt, so they sit beside it.
#
# These are our own artwork (drawn by tools/panel_art/make_panel.py), not Yamaha's, so
# unlike the ROMs they are not gated behind IOS_ROMS.
$(IOS_PANEL_DIR)/panel.txt: $(wildcard art/real/*.png) art/real/panel.txt
	@mkdir -p $(dir $@)
	@cp -f art/real/*.png art/real/panel.txt $(dir $@)

.PHONY: ios-panel-art

ios-panel-art: $(IOS_PANEL_DIR)/panel.txt

# The appex gets its own copy: S-MU2000AU.appex/art/real/. The extension is a
# separate process with its own module_dir (the appex root), so the app's copy is
# unreachable from it - and without this the AUv3 UI would silently fall back to
# built-in defaults while the standalone shows full art. Same step-4 lookup, same
# top-level shape that signs. Duplicated bytes, zero shared-container wrangling.
$(IOS_APPEX)/art/real/panel.txt: $(wildcard art/real/*.png) art/real/panel.txt
	@mkdir -p $(dir $@)
	@cp -f art/real/*.png art/real/panel.txt $(dir $@)

.PHONY: ios-appex-art

ios-appex-art: $(IOS_APPEX)/art/real/panel.txt

# The standalone's ROMs. Beside the binary (S-MU2000.app/roms/), which is engine
# candidate 3b (module_dir + "roms" in src/vst3/engine.cpp) - so no code change is
# needed, and a top-level directory signs fine.
#
# Deliberately NOT Resources/roms: besides the codesign rule above, that path was
# never searched for the iOS layouts anyway (the candidates are ../Resources[/roms]
# for Contents/MacOS-style bundles, then roms/ beside the binary).
#
# Off by default like every other ROM rule in this Makefile: the images are Yamaha's
# and must not travel in anything we hand out. Locally, pass the directory:
#   make ios-standalone IOS_ROMS=roms
ios-app-roms:
ifneq ($(strip $(IOS_ROMS)),)
	@rm -rf $(IOS_APP)/roms
	@cp -R $(IOS_ROMS) $(IOS_APP)/roms
	@echo "ROM を app に入れた: $(IOS_ROMS) -> $(IOS_APP)/roms"
endif

.PHONY: ios-app-roms

.PHONY: ios-app ios-standalone

ios-auv3: $(IOS_BIN) ios-auv3-roms
	@echo "iOS 拡張: $(IOS_APPEX)"
	@echo "これを .app に入れて起動すれば登録される（ROM は app group に置く）"

.PHONY: ios-auv3

endif # filter ios%,$(MAKECMDGOALS)

# The fence above means these two exist only on macOS, where nothing above can
# build them. Saying so beats "No rule to make target", which is what a Windows
# or Linux builder would otherwise get from `make ios`.
else

.PHONY: ios ios-install

ios:
	@echo "iOS builds need macOS with Xcode: the iOS toolchain, the simulator and"
	@echo "the signing identities are all macOS tools (make ios on macOS lists the"
	@echo "targets)."

ios-install:
	@$(MAKE) ios

endif # PLATFORM is macos
