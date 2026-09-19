# engine/CMakeLists.txt — smu2000_engine STATIC (P1).
# Source list = Makefile OBJS + src/mu2000.cpp + src/vst3/engine.cpp (VST2_LEDGER.md
# §Findings). Sources live under src/; this target only carries the build metadata.
# GUI-only (all other src/ui/*.cpp), src/vst3/{plugin,view,iids,probe}.cpp, src/xg/model.cpp
# and every tool main are deliberately EXCLUDED (header-only ui/bridge|driver|resampler|
# snapshot + xg/ram.h|xg/model.h come transitively via engine.cpp).
# CRT (/MT) is forced globally by the root project — not set here. MinGW flags live in
# cmake/mingw_compat.cmake (P5); keep this file toolchain-neutral outside if(MSVC).

add_library(smu2000_engine STATIC)

target_sources(smu2000_engine PRIVATE
  ${CMAKE_SOURCE_DIR}/src/compat/compat.cpp
  ${CMAKE_SOURCE_DIR}/src/compat/a64asm.cpp   # x86 では selftest の空スタブのみ（Makefile OBJS と同期）
  ${CMAKE_SOURCE_DIR}/src/smartmedia.cpp
  ${CMAKE_SOURCE_DIR}/src/mame/sound/swp30.cpp
  ${CMAKE_SOURCE_DIR}/src/mame/sound/swp30_jit.cpp
  ${CMAKE_SOURCE_DIR}/src/mame/video/hd44780.cpp
  ${CMAKE_SOURCE_DIR}/src/mame/machine/sci4.cpp
  ${CMAKE_SOURCE_DIR}/src/mame/cpu/sh.cpp
  ${CMAKE_SOURCE_DIR}/src/mame/cpu/sh2.cpp
  ${CMAKE_SOURCE_DIR}/src/mame/cpu/sh2_jit.cpp
  ${CMAKE_SOURCE_DIR}/src/mame/cpu/sh7042.cpp
  ${CMAKE_SOURCE_DIR}/src/mame/cpu/sh_adc.cpp
  ${CMAKE_SOURCE_DIR}/src/mame/cpu/sh_bsc.cpp
  ${CMAKE_SOURCE_DIR}/src/mame/cpu/sh_cmt.cpp
  ${CMAKE_SOURCE_DIR}/src/mame/cpu/sh_dmac.cpp
  ${CMAKE_SOURCE_DIR}/src/mame/cpu/sh_intc.cpp
  ${CMAKE_SOURCE_DIR}/src/mame/cpu/sh_mtu.cpp
  ${CMAKE_SOURCE_DIR}/src/mame/cpu/sh_port.cpp
  ${CMAKE_SOURCE_DIR}/src/mame/cpu/sh_sci.cpp
  ${CMAKE_SOURCE_DIR}/src/mu2000.cpp
  ${CMAKE_SOURCE_DIR}/src/vst3/engine.cpp
  # Plugin-only sink for ui::xgui::set_voice_rom (engine.cpp:555, added by upstream
  # 5901ea5 for the imgui 一覧/エディタ). The real definition (src/ui/xg_ui.cpp) is
  # imgui-only and excluded here by hard rule #6; the native GDI panel never reads the
  # voice ROM. Makefile/VST3 builds keep linking the real xg_ui.cpp — untouched.
  ${CMAKE_CURRENT_SOURCE_DIR}/xgui_plugin_stub.cpp
)

target_include_directories(smu2000_engine PUBLIC
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/compat
)

target_compile_features(smu2000_engine PUBLIC cxx_std_20)

target_compile_definitions(smu2000_engine PUBLIC
  _USE_MATH_DEFINES
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
  _CRT_NONSTDC_NO_WARNINGS
  WIN32
)

if(MSVC)
  target_compile_options(smu2000_engine PRIVATE
    /std:c++20      # std::rotl/rotr usage (Makefile: C++20)
    /utf-8          # Japanese comments in sources
    /bigobj         # sh.cpp / mu2000.cpp exceed the COFF 65279-section limit
  )
endif()
