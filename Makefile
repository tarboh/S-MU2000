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
BUILD ?= build

SRCS := \
	src/compat/compat.cpp \
	src/smartmedia.cpp \
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

$(BUILD)/verify$(EXE): $(OBJS) $(BUILD)/src/verify.o
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
PC_SRCS    := src/ui/pc_editor.cpp src/ui/pc_window.cpp src/ui/xg_ui.cpp src/ui/overview.cpp src/ui/fx_editor.cpp src/ui/fx_help.cpp src/ui/part_shapes.cpp src/ui/master_editor.cpp src/ui/fx_icons.cpp
PC_OBJS    := $(IMGUI_SRCS:%.cpp=$(BUILD)/imgui/%.o) $(PC_SRCS:%.cpp=$(BUILD)/imgui/%.o)

# gui は実機のフロントパネル風の画面を出す
UI_SRCS := src/ui/panel.cpp src/ui/editor.cpp src/ui/effects.cpp src/ui/png.cpp \
           src/ui/audio_out.cpp src/ui/audio_in.cpp src/ui/midi_in.cpp src/ui/midi_out.cpp \
           src/ui/layout.cpp src/ui/svg.cpp src/ui/player.cpp src/xg/model.cpp
UI_OBJS := $(UI_SRCS:%.cpp=$(BUILD)/%.o)

$(BUILD)/imgui/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(IMGUI_FLAGS) -c -o $@ $<

$(BUILD)/src/gui.o: CXXFLAGS += $(IMGUI_FLAGS)

# The app classes pull in app.h, whose editor headers want imgui.h
$(BUILD)/src/ui/app_win.o: CXXFLAGS += $(IMGUI_FLAGS)
$(BUILD)/src/ui/window_win.o: CXXFLAGS += $(IMGUI_FLAGS)

$(BUILD)/gui$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/smf.o $(UI_OBJS) $(PC_OBJS) $(BUILD)/src/gui.o $(BUILD)/src/ui/app_win.o $(BUILD)/src/ui/window_win.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lwinmm -lole32 -lgdi32 -luser32 -lavrt -lcomdlg32 -lshell32 	       -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -limm32

# midisend は MIDI ファイルを実時間で MIDI 出力へ流す（live の試験用）
$(BUILD)/midisend$(EXE): $(BUILD)/src/smf.o $(BUILD)/src/midisend.o $(BUILD)/src/compat/compat.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lwinmm -lole32 -lavrt

# rec は音声入力を WAV に録る。実機の音（S/PDIF 入力）と突き合わせるため。
# 録りながら MIDI を実機へ流せるので、同じ譜面の実機とこちらを 1 回で並べられる
$(BUILD)/rec$(EXE): $(BUILD)/src/smf.o $(BUILD)/src/rec.o $(BUILD)/src/compat/compat.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lwinmm -lole32 -luuid

# live は Windows の MIDI 入力と音声出力を使う
$(BUILD)/live$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/ui/midi_in.o $(BUILD)/src/ui/audio_out.o $(BUILD)/src/live.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lwinmm -lole32 -lavrt

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
             src/ui/panel.cpp src/ui/layout.cpp src/ui/svg.cpp src/ui/editor.cpp \
             src/ui/effects.cpp src/xg/model.cpp $(VST3_SDK_SRCS)
VST3_OBJS := $(VST3_SRCS:%.cpp=$(BUILD)/vst3obj/%.o)

$(BUILD)/vst3obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(VST3_INC) $(IMGUI_FLAGS) -c -o $@ $<

vst3: $(VST3_BIN)

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

install-vst3: $(VST3_BIN)
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

install-clap: $(CLAP_BIN)
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

install-vsti: $(VSTI_BIN)
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
# gui is an SDL3 window drawing the shared panel through Cairo. The plug-ins
# are ELF shared objects with headless editors for now (hosts fall back to
# their generic UI). Needs libcairo2-dev, libfontconfig-dev and libsdl3-dev
# alongside libasound2-dev.

LINUX_GUI_CFLAGS := $(shell pkg-config --cflags cairo fontconfig 2>/dev/null)
LINUX_GUI_LIBS := $(shell pkg-config --libs cairo fontconfig 2>/dev/null)
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

# Dear ImGui SDL3 backends for the PC editor windows.
# Vendored unmodified from the matching ImGui release, like the rest.
IMGUI_SDL_BACKENDS := third_party/imgui/backends/imgui_impl_sdl3.cpp \
                      third_party/imgui/backends/imgui_impl_sdlrenderer3.cpp
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
                  src/ui/part_shapes.cpp src/ui/master_editor.cpp
LINUX_GUI_OBJS := $(LINUX_GUI_SRCS:%.cpp=$(BUILD)/guiobj/%.o)

$(BUILD)/guiobj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(IMGUI_FLAGS) $(LINUX_SDL_CFLAGS) -c -o $@ $<

$(BUILD)/src/gui_linux.o: CXXFLAGS += $(IMGUI_FLAGS) $(LINUX_SDL_CFLAGS)

$(BUILD)/gui$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(BUILD)/src/smf.o \
                    $(BUILD)/src/compat/gdi_linux.o $(LINUX_GUI_OBJS) $(IMGUI_OBJS) \
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

LINUX_PANEL_SRCS := src/compat/gdi_linux.cpp \
              src/ui/panel.cpp src/ui/layout.cpp src/ui/svg.cpp src/ui/editor.cpp \
              src/ui/effects.cpp src/xg/model.cpp \
              src/ui/xg_ui.cpp src/ui/fx_help.cpp src/ui/fx_icons.cpp $(IMGUI_CORE)

VST3_SRCS := src/vst3/plugin.cpp src/vst3/engine.cpp src/vst3/iids.cpp src/vst3/automation.cpp \
             src/vst3/view.cpp src/vst3/plug_window_linux.cpp \
             $(LINUX_PANEL_SRCS) $(VST3_SDK_SRCS)
VST3_OBJS := $(VST3_SRCS:%.cpp=$(BUILD)/vst3obj/%.o)

$(BUILD)/vst3obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(VST3_INC) $(IMGUI_FLAGS) $(LINUX_SDL_CFLAGS) -c -o $@ $<

vst3: $(VST3_BIN)

$(VST3_BIN): $(OBJS) $(BUILD)/src/mu2000.o $(VST3_OBJS) $(IMGUI_SDL_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(LDFLAGS) -lasound $(LINUX_GUI_LIBS) $(LINUX_SDL_LIBS)
	@mkdir -p $(VST3_DIR)/Contents/Resources
	@cp -f doc/vst3-readme.txt $(VST3_DIR)/Contents/Resources/README.txt 2>/dev/null || true
	@cp -f LICENSE $(VST3_DIR)/Contents/Resources/LICENSE.txt
	@cp -f NOTICE.txt $(VST3_DIR)/Contents/Resources/NOTICE.txt

VST3_INSTALL ?= $(HOME)/.vst3

install-vst3: $(VST3_BIN)
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

install-clap: $(CLAP_BIN)
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
MAC_FRAMEWORKS := -framework CoreAudio -framework AudioToolbox \
                  -framework CoreMIDI -framework AudioUnit \
                  -framework CoreFoundation -framework CoreGraphics \
                  -framework CoreText -framework Cocoa \
                  -framework UniformTypeIdentifiers \
                  -framework QuartzCore

MAC_IO_OBJS := $(BUILD)/src/ui/audio_out_mac.o $(BUILD)/src/ui/midi_in_mac.o

$(BUILD)/live$(EXE): $(OBJS) $(BUILD)/src/mu2000.o $(MAC_IO_OBJS) $(BUILD)/src/live.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(MAC_FRAMEWORKS)

# gui draws the front-panel look of the real machine.
#
# panel.cpp and its neighbours are the **same source** as the Windows build; only
# what is underneath differs. compat/gdi_mac.cpp fills the GDI interface in with
# CoreGraphics and window_mac.mm fills the window in with AppKit
# (doc/porting-macos.md).
#
# window_mac.mm is the one file compiled as Objective-C++.
MAC_GUI_SRCS := src/ui/panel.cpp src/ui/editor.cpp src/ui/effects.cpp \
                src/ui/png.cpp src/ui/layout.cpp src/ui/svg.cpp src/ui/player.cpp \
                src/ui/audio_out_mac.cpp src/ui/audio_in_mac.cpp \
                src/ui/midi_in_mac.cpp src/ui/midi_out_mac.cpp \
                src/xg/model.cpp \
                src/compat/gdi_mac.cpp src/ui/window_mac.mm src/ui/app_mac.cpp \
                src/gui_mac.cpp

# PC editor (doc/pc-editor.md). The views are the same files as on Windows;
# the window is AppKit + Metal (pc_window_mac.mm). imgui_impl_osx is not used
# here, because the input would arrive in another window's context
MAC_IMGUI_SRCS := $(IMGUI_CORE) \
                  $(IMGUI_DIR)/backends/imgui_impl_metal.mm
MAC_PC_SRCS    := src/ui/pc_editor.cpp src/ui/pc_window_mac.mm src/ui/xg_ui.cpp \
                  src/ui/overview.cpp src/ui/fx_editor.cpp src/ui/fx_help.cpp src/ui/part_shapes.cpp \
                  src/ui/master_editor.cpp src/ui/fx_icons.cpp
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
# compat/gdi_mac.cpp filling in CoreGraphics underneath. The window is view_mac.mm
#
# Both plug-in formats show this one panel, so the view and the drawing layer are
# named once and the VST3 bundle and the AU both build them. (The Windows side of
# this Makefile names the same drawing layer in its own VST3_SRCS, with
# view_win.cpp in place of view_mac.mm)
PANEL_VIEW_SRCS := src/vst3/view.cpp src/vst3/view_mac.mm
PANEL_SRCS := src/compat/gdi_mac.cpp \
              src/ui/panel.cpp src/ui/layout.cpp src/ui/svg.cpp src/ui/editor.cpp \
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

vst3: $(VST3_BIN)

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

install-vst3: $(VST3_BIN)
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
	$(CXX) $(CXXFLAGS) $(CLAP_INC) -c -o $@ $<

clap: $(CLAP_BIN)

$(CLAP_BIN): $(OBJS) $(BUILD)/src/mu2000.o $(CLAP_OBJS) $(MAC_PC_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -bundle -o $@ $^ $(LDFLAGS) $(MAC_FRAMEWORKS)
	@mkdir -p $(CLAP_DIR)/Contents/Resources
	@cp -f LICENSE $(CLAP_DIR)/Contents/Resources/LICENSE.txt
	@cp -f NOTICE.txt $(CLAP_DIR)/Contents/Resources/NOTICE.txt
	@cp -f packaging/clap-macos-Info.plist $(CLAP_DIR)/Contents/Info.plist
	@printf 'BNDL????' > $(CLAP_DIR)/Contents/PkgInfo

CLAP_INSTALL ?= $(HOME)/Library/Audio/Plug-Ins/CLAP

install-clap: $(CLAP_BIN)
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

au: $(AU_BIN)

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

install-au: $(AU_BIN)
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

# Copy ROMs into the bundle.
#
# A sandboxed extension can only read its own bundle. The AUv3 extension
# lives in a sandbox (it would not register otherwise), so $HOME points
# at the container and neither ~/Library/Application Support nor whatever
# roms.txt names is reachable. Baking ROMs in is the only way an AUv3 sings.
#
#   make auv3 AUV3_ROMS=/path/to/roms
#
# ROMs are never redistributed, so they stay out of git (roms/ is ignored).
# Local builds bake them in from ./roms by default so the unit always sings;
# pass another path, empty to leave the bundle as it is, or none to take them
# out (without them the unit registers and renders, silently)
AUV3_ROMS ?= roms

AUV3_FLAGS := -fobjc-arc
AUV3_FW    := -framework Foundation -framework AudioToolbox -framework AVFoundation \
              -framework CoreAudio -framework CoreMIDI -framework Cocoa -framework CoreAudioKit \
              -framework Metal -framework QuartzCore

$(BUILD)/auv3obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(VST3_INC) -c -o $@ $<

$(BUILD)/auv3obj/%.o: %.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(VST3_INC) $(IMGUI_FLAGS) $(AUV3_FLAGS) -ObjC++ -c -o $@ $<

# Bundle finishing (ROMs in, then sign) runs every time. Doing it only
# when binaries rebuild would ignore a later-added AUV3_ROMS.
# An explicitly empty AUV3_ROMS leaves the contents as they are (so a bare
# rebuild never wipes baked ROMs). Write AUV3_ROMS=none to take them out
auv3: $(AUV3_HOST) $(BUILD)/autest$(EXE)
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
	# The App Sandbox entitlement is required. A macOS app extension outside
	# the sandbox never registers. Certificate kind does not matter (ad-hoc works)
	@codesign --force --sign "$(CODESIGN_ID)" --timestamp=none \
	          --entitlements packaging/auv3-appex.entitlements $(AUV3_APPEX)
	@codesign --force --sign "$(CODESIGN_ID)" --timestamp=none \
	          --entitlements packaging/auv3-app.entitlements $(AUV3_APP)
	@echo "出来た: $(AUV3_APP)"

# The .appex itself. Entry point is NSExtensionMain (it owns no main())
$(AUV3_BIN): $(OBJS) $(BUILD)/src/mu2000.o $(AUV3_OBJS) $(MAC_PC_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(AUV3_FW) \
	       -e _NSExtensionMain -fapplication-extension
	@cp -f packaging/auv3-appex-Info.plist $(AUV3_APPEX)/Contents/Info.plist

# The container app. Silent. Exists only to carry the .appex into registration
$(AUV3_HOST): $(AUV3_BIN) $(BUILD)/auv3obj/src/auv3/main_app.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $(BUILD)/auv3obj/src/auv3/main_app.o $(LDFLAGS) -framework Cocoa
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

clean:
	rm -rf $(BUILD)

# ヘッダを直したときに .o を作り直させる仕掛け（-MMD -MP が置く .d）。
#
# **1 つずつ並べてはいけない。** 並べ忘れた .o はヘッダを直しても作り直されず、
# 型の大きさが食い違ったまま繋がって落ちる（statetest がこれで落ちていた。
# swp30.h に変数を 1 つ足したら、古い大きさのまま繋がった mu2000.o が
# 別の場所を触りに行っていた）。だから build の下にある .d を全部拾う
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

.PHONY: all clean regen check test test-update vst3 install-vst3 probe clap install-clap vsti install-vsti vsti-probe au install-au au-probe check-au
