# S-MU2000 — CMake build (alternative to the Makefile)
#
#   cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-cmake -j
#   ./build-cmake/verify && ./build-cmake/render roms song.mid out.wav
#
# MSVC (Windows; the Win32 and x64 platforms are the "profiles", picked with
# -A, and both compile here — x64 uses the x86-64 JITs in both cases, Win32
# the ported x86-32 JIT paths; Release defaults to /O2 + /GL + LTCG). It covers
# the core, the tools, gui and live, and all three plug-ins:
#
#   cmake -S . -B build-x64   -A x64
#   cmake -S . -B build-win32 -A Win32
#   cmake --build build-x64   --config Release -j
#   build-x64\bin\x64\Release\verify.exe
#
# The Makefile remains the primary build (it also covers Windows/macOS and the
# Audio Unit). This file covers the portable core everywhere and the full
# Linux set (live + VST3 + CLAP + probes) where ALSA is available.
cmake_minimum_required(VERSION 3.16)
if(POLICY CMP0091)
  # MSVC runtime picked through CMAKE_MSVC_RUNTIME_LIBRARY (default: /MD DLL,
  # like the MinGW builds) instead of hard-coded -MD flags.
  cmake_policy(SET CMP0091 NEW)
endif()
project(S-MU2000 LANGUAGES CXX)

option(ASIO "Enable Windows standalone DirectSound and ASIO output" OFF)
if(ASIO AND NOT WIN32)
  message(FATAL_ERROR "ASIO=ON is supported only by Windows builds")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Triangulation for SVG fills with holes (same include the Makefile adds on
# every platform; harmless where nothing uses it).
include_directories(third_party/earcut.hpp/include)
# MSVC: the sources are UTF-8 (Japanese comments); /utf-8 keeps execs on the
# portable default DLL runtime and silences codepage C4819s. Release gets full
# optimization (/O2) plus /GL + /LTCG link-time codegen unless overridden.
add_compile_options(
  $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
  $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/GL>
  $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/O2>)
add_link_options(
  $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/LTCG>)
# Non-literal printf formats are bugs (translated strings must go through "%s").
add_compile_options(
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wformat-security>)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- Emulator core (same sources as SRCS in the Makefile) -------------------
add_library(smu2000_core STATIC
  src/compat/compat.cpp
  src/sampling.cpp
  src/smartmedia.cpp
  src/card_fs.cpp
  src/m2a.cpp
  src/mame/sound/swp30.cpp
  src/mame/sound/swp30_jit.cpp
  src/mame/video/hd44780.cpp
  src/mame/machine/sci4.cpp
  src/mame/cpu/sh.cpp
  src/mame/cpu/sh2.cpp
  src/mame/cpu/sh2_jit.cpp
  src/compat/a64asm.cpp
  src/mame/cpu/sh7042.cpp
  src/mame/cpu/sh_adc.cpp
  src/mame/cpu/sh_bsc.cpp
  src/mame/cpu/sh_cmt.cpp
  src/mame/cpu/sh_dmac.cpp
  src/mame/cpu/sh_intc.cpp
  src/mame/cpu/sh_mtu.cpp
  src/mame/cpu/sh_port.cpp
  src/mame/cpu/sh_sci.cpp
  src/mu2000.cpp
  src/smf.cpp
)
target_include_directories(smu2000_core PUBLIC src src/compat)
# -Wall and friends are GCC/Clang spellings; MSVC builds on the /W3 default
# (the JITs emit raw machine code, so silence only the noise the Makefile also
# silences where it can).
target_compile_options(smu2000_core PRIVATE
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-unused-variable>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-unused-but-set-variable>)
set_target_properties(smu2000_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- Windows (MSVC Win32/x64 and MinGW): platform echo + output layout ------
# The -A Win32 / -A x64 generator platform is the profile; the sources pick
# their paths from _M_IX86/_M_X64 (x86-32 JIT port / x86-64 JIT). Binaries go
# to bin/<arch>/ so the two profiles can share a tree without clobbering.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(SMU_WIN_ARCH "Win32")
  else()
    set(SMU_WIN_ARCH "x64")
  endif()
  message(STATUS "Windows target platform: ${SMU_WIN_ARCH} (${CMAKE_CXX_COMPILER_ID})")
  if(MSVC AND NOT CMAKE_GENERATOR_PLATFORM)
    # cl's default on an x86 host is 32-bit; say so when -A pinned nothing.
    message(STATUS "no -A platform given: using the compiler's default architecture")
  endif()
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${SMU_WIN_ARCH}")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${SMU_WIN_ARCH}")
  # console.h/paths.h already insist on these locally; do it tree-wide so the
  # windows.h min/max macros cannot poison std::numeric_limits<> in tool TUs.
  add_compile_definitions(NOMINMAX)
  # The ASIO SDK needs the full Windows/COM declarations. Keep lean headers
  # on our targets instead of passing that restriction into dependencies.
  target_compile_definitions(smu2000_core PUBLIC WIN32_LEAN_AND_MEAN)
endif()

# ---- Offline tools (run anywhere; need no ROMs except via argv) -------------
foreach(tool verify boot render panel statetest blocktime)
  add_executable(${tool} src/${tool}.cpp)
  target_link_libraries(${tool} PRIVATE smu2000_core)
endforeach()
foreach(tool xgtest samptest)
  add_executable(${tool} src/${tool}.cpp src/xg/model.cpp)
  target_link_libraries(${tool} PRIVATE smu2000_core)
endforeach()

# Regression suite entry point: ctest runs what `make test` runs without ROMs.
enable_testing()
add_test(NAME verify COMMAND verify)

# Shared building blocks for the plug-in targets on every desktop platform
# (the Makefile names them VST3_SDK_SRCS / IMGUI_CORE too).
set(VST3_SDK_SRCS
  third_party/vst3/pluginterfaces/base/funknown.cpp
  third_party/vst3/pluginterfaces/base/coreiids.cpp
  third_party/vst3/pluginterfaces/base/conststringtable.cpp
  third_party/vst3/pluginterfaces/base/ustring.cpp)
set(IMGUI_CORE
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp)

# ---- Linux: live + VST3 + CLAP (needs ALSA) ----------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(ALSA QUIET alsa)
  endif()
  if(NOT ALSA_FOUND)
    message(STATUS "ALSA not found: building offline tools only (no live/VST3/CLAP)")
    return()
  endif()

  # fontconfig for the panel's CJK fonts, SDL3 for the gui window
  pkg_check_modules(FONTCONFIG QUIET fontconfig)
  pkg_check_modules(SDL3 QUIET sdl3)

  add_executable(live src/live.cpp
    src/ui/audio_out_linux.cpp
    src/ui/audio_in_linux.cpp
    src/ui/midi_in_linux.cpp
    src/ui/midi_out_linux.cpp)
  target_link_libraries(live PRIVATE smu2000_core ${ALSA_LIBRARIES} pthread dl)
  target_include_directories(live PRIVATE ${ALSA_INCLUDE_DIRS})

  set(LINUX_PANEL_SRCS
    src/ui/panel.cpp src/ui/layout.cpp src/ui/svg.cpp src/ui/png.cpp src/ui/editor.cpp
    src/ui/effects.cpp src/xg/model.cpp
    src/ui/xg_ui.cpp src/ui/fx_help.cpp src/ui/fx_icons.cpp
    ${IMGUI_CORE})

  # VST3 bundle layout: S-MU2000.vst3/Contents/x86_64-linux/S-MU2000.so
  add_library(S-MU2000-vst3 MODULE
    src/vst3/plugin.cpp src/vst3/engine.cpp src/vst3/iids.cpp
    src/vst3/automation.cpp src/vst3/view.cpp src/vst3/plug_window_linux.cpp
    ${LINUX_PANEL_SRCS} ${VST3_SDK_SRCS})
  target_include_directories(S-MU2000-vst3 PRIVATE src src/compat
    third_party/vst3 third_party/imgui ${FONTCONFIG_INCLUDE_DIRS})
  target_link_libraries(S-MU2000-vst3 PRIVATE smu2000_core
    ${ALSA_LIBRARIES} ${FONTCONFIG_LIBRARIES} pthread dl)
  set_target_properties(S-MU2000-vst3 PROPERTIES
    PREFIX "" SUFFIX ".so" OUTPUT_NAME "S-MU2000"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/S-MU2000.vst3/Contents/x86_64-linux")
  # the photo-like panel art goes in the bundle; the plugin looks for
  # ../Resources/panel/panel.txt from where it is loaded
  file(GLOB REAL_PANEL_FILES "${CMAKE_SOURCE_DIR}/art/real/*.png" "${CMAKE_SOURCE_DIR}/art/real/panel.txt")
  add_custom_command(TARGET S-MU2000-vst3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/S-MU2000.vst3/Contents/Resources/panel"
    COMMAND ${CMAKE_COMMAND} -E copy ${REAL_PANEL_FILES} "${CMAKE_BINARY_DIR}/S-MU2000.vst3/Contents/Resources/panel/")

  add_library(S-MU2000-clap MODULE
    src/clap/plugin.cpp src/vst3/engine.cpp src/vst3/iids.cpp
    src/vst3/automation.cpp src/vst3/view.cpp src/vst3/plug_window_linux.cpp
    ${LINUX_PANEL_SRCS} ${VST3_SDK_SRCS})
  target_include_directories(S-MU2000-clap PRIVATE src src/compat
    third_party/vst3 third_party/clap third_party/imgui ${FONTCONFIG_INCLUDE_DIRS})
  target_link_libraries(S-MU2000-clap PRIVATE smu2000_core
    ${ALSA_LIBRARIES} ${FONTCONFIG_LIBRARIES} pthread dl)
  set_target_properties(S-MU2000-clap PROPERTIES
    PREFIX "" SUFFIX ".clap" OUTPUT_NAME "S-MU2000"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

  add_executable(vst3probe
    src/vst3/probe.cpp src/vst3/probe_host_linux.cpp
    src/vst3/iids.cpp ${VST3_SDK_SRCS})
  target_include_directories(vst3probe PRIVATE src src/compat third_party/vst3)
  target_link_libraries(vst3probe PRIVATE smu2000_core dl)

  add_executable(clapprobe src/clap/probe.cpp)
  target_include_directories(clapprobe PRIVATE src src/compat third_party/clap)
  target_link_libraries(clapprobe PRIVATE smu2000_core dl)

  # gui (Phase 4: SDL3 window + PC editor windows, doc/porting-linux-gui.md)
  if(FONTCONFIG_FOUND AND SDL3_FOUND)
    add_executable(gui src/gui_linux.cpp
      src/ui/panel.cpp src/ui/layout.cpp src/ui/svg.cpp
      src/ui/editor.cpp src/ui/effects.cpp src/ui/png.cpp
      src/ui/player.cpp src/xg/model.cpp
      src/ui/xg_ui.cpp src/ui/fx_help.cpp src/ui/fx_icons.cpp
      src/ui/sdl_popup.cpp
      src/ui/window_sdl.cpp
      src/ui/app_linux.cpp
      src/ui/pc_window_linux.cpp
      src/ui/pc_editor.cpp src/ui/overview.cpp src/ui/fx_editor.cpp
      src/ui/part_shapes.cpp src/ui/master_editor.cpp src/ui/sampling_editor.cpp src/ui/sampling_romwave.cpp src/ui/sampling_presets.cpp src/ui/sampling_library.cpp
      ${IMGUI_CORE}
      third_party/imgui/backends/imgui_impl_sdl3.cpp
      third_party/imgui/backends/imgui_impl_sdlrenderer3.cpp
      src/ui/audio_out_linux.cpp
      src/ui/audio_in_linux.cpp
      src/ui/midi_in_linux.cpp
      src/ui/midi_out_linux.cpp)
    target_include_directories(gui PRIVATE src src/compat
      third_party/imgui ${ALSA_INCLUDE_DIRS} ${FONTCONFIG_INCLUDE_DIRS} ${SDL3_INCLUDE_DIRS})
    target_link_libraries(gui PRIVATE smu2000_core
      ${ALSA_LIBRARIES} ${FONTCONFIG_LIBRARIES} ${SDL3_LIBRARIES} pthread dl)
  else()
    message(STATUS "fontconfig/SDL3 not found: skipping gui (needs libfontconfig-dev libsdl3-dev)")
  endif()
endif()

# ---- Windows: VST3 + CLAP + VST2 (Win32 and x64 profiles) --------------------
# Mirrors the Makefile's windows branch: same source lists (view_win instead of
# plug_window_linux, imgui win32+dx11 backends), same import libraries. The
# toolchain only shows up where its CRT/winapi headers disagree (font_file.h);
# the plug-in exports are __declspec(dllexport) either way.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  # no gamepad support, so no XInput (Makefile: IMGUI_FLAGS)
  add_compile_definitions(IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

  # VST3 bundle directory name by profile (Makefile's x86_64-win for x64).
  # MSVC sets CMAKE_SYSTEM_PROCESSOR=AMD64 even for -A Win32, so width decides
  # first and the ISA only separates ARM on the 64-bit path; native arm64(x)
  # goes to arm_64-win, switch to arm64ec-win for emulated x64-on-arm builds.
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM|arm64")
      set(VST3_WIN_SUBDIR "arm_64-win")
    else()
      set(VST3_WIN_SUBDIR "x86_64-win")
    endif()
  else()
    set(VST3_WIN_SUBDIR "x86-win")
  endif()

  # PC editor windows (the Makefile's PC_SRCS; gui.exe keeps its own copy of
  # the app/window wrappers on the side, plugins embed pc_window.cpp itself)
  set(IMGUI_WIN_BACKENDS
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp)
  set(PC_WIN_SRCS
    src/ui/pc_editor.cpp src/ui/pc_window.cpp src/ui/xg_ui.cpp src/ui/overview.cpp
    src/ui/fx_editor.cpp src/ui/fx_help.cpp src/ui/part_shapes.cpp src/ui/master_editor.cpp
    src/ui/sampling_editor.cpp src/ui/sampling_romwave.cpp src/ui/sampling_presets.cpp
    src/ui/sampling_library.cpp src/ui/fx_icons.cpp)

  # engine+view body and panel art shared by all three kinds — exactly the
  # Makefile's VST3_SRCS without plugin.cpp (CLAP/VST2 swap only that piece)
  set(PLUGIN_BODY_SRCS
    src/vst3/engine.cpp src/vst3/iids.cpp src/vst3/automation.cpp
    src/vst3/view.cpp src/vst3/view_win.cpp)
  set(PLUGIN_PANEL_SRCS
    src/ui/panel.cpp src/ui/layout.cpp src/ui/svg.cpp src/ui/png.cpp src/ui/editor.cpp
    src/ui/effects.cpp src/xg/model.cpp)

  set(PLUGIN_WIN_LIBS winmm ole32 gdi32 user32 avrt comdlg32 shell32
    d3d11 dxgi d3dcompiler dwmapi imm32)

  # VST3: S-MU2000.vst3/Contents/<arch>-win/S-MU2000.vst3 (like x86_64-linux)
  add_library(smu2000-vst3 MODULE
    src/vst3/plugin.cpp ${PLUGIN_BODY_SRCS} ${PLUGIN_PANEL_SRCS}
    ${PC_WIN_SRCS} ${IMGUI_CORE} ${IMGUI_WIN_BACKENDS} ${VST3_SDK_SRCS})

  # CLAP: one file, plugin.cpp is the only difference from VST3
  add_library(smu2000-clap MODULE
    src/clap/plugin.cpp ${PLUGIN_BODY_SRCS} ${PLUGIN_PANEL_SRCS}
    ${PC_WIN_SRCS} ${IMGUI_CORE} ${IMGUI_WIN_BACKENDS} ${VST3_SDK_SRCS})

  # VST2.4: hand-rolled ABI header, no Steinberg SDK (src/vsti)
  add_library(smu2000-vst2 MODULE
    src/vsti/plugin.cpp ${PLUGIN_BODY_SRCS} ${PLUGIN_PANEL_SRCS}
    ${PC_WIN_SRCS} ${IMGUI_CORE} ${IMGUI_WIN_BACKENDS} ${VST3_SDK_SRCS})

  foreach(p smu2000-vst3 smu2000-clap smu2000-vst2)
    target_include_directories(${p} PRIVATE src src/compat
      third_party/vst3 third_party/clap third_party/imgui)
    target_link_libraries(${p} PRIVATE smu2000_core ${PLUGIN_WIN_LIBS})
    # All three plugins share OUTPUT_NAME "S-MU2000" (that is the filename a
    # DAW scans for). MSVC would stamp one shared S-MU2000.exp/.implib into
    # the default output dir and the parallel links would fight over it, so
    # every module gets its own inside its per-target build dir.
    target_link_options(${p} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/IMPLIB:${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${p}.dir/S-MU2000.lib>)
    set_target_properties(${p} PROPERTIES PREFIX "" OUTPUT_NAME "S-MU2000")
  endforeach()
  set_target_properties(smu2000-vst3 PROPERTIES SUFFIX ".vst3")
  # hosts scan exactly Contents/<arch>-win/..., so pin every config's output
  # there (a plain output dir gains a per-config subdir). The <CONFIG>
  # property suffixes must be UPPERCASE -- mixed-case spellings are not real
  # properties and get silently dropped.
  set(SMU_VST3_BUNDLE "${CMAKE_BINARY_DIR}/S-MU2000.vst3/Contents/${VST3_WIN_SUBDIR}")
  # (A MODULE with a non-.dll suffix is the LIBRARY class on Windows; pin it
  # per config, uppercase, or the artifact falls back to bin/<arch>/<config>.)
  set_target_properties(smu2000-vst3 PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY_DEBUG     "${SMU_VST3_BUNDLE}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE   "${SMU_VST3_BUNDLE}"
    LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${SMU_VST3_BUNDLE}"
    LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL     "${SMU_VST3_BUNDLE}")
  set_target_properties(smu2000-clap PROPERTIES SUFFIX ".clap")
  set_target_properties(smu2000-vst2 PROPERTIES SUFFIX ".dll")

  # bundle contents: panel art + notices, same files the Makefile copies
  file(GLOB REAL_PANEL_FILES "${CMAKE_SOURCE_DIR}/art/real/*.png" "${CMAKE_SOURCE_DIR}/art/real/panel.txt")
  add_custom_command(TARGET smu2000-vst3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/S-MU2000.vst3/Contents/Resources/panel"
    COMMAND ${CMAKE_COMMAND} -E copy ${REAL_PANEL_FILES} "${CMAKE_BINARY_DIR}/S-MU2000.vst3/Contents/Resources/panel/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/S-MU2000.vst3/Contents/Resources/LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/NOTICE.txt" "${CMAKE_BINARY_DIR}/S-MU2000.vst3/Contents/Resources/NOTICE.txt")

  # standalone probes (Makefile: vst3probe / clapprobe / vstiprobe). MinGW
  # pulls the GDI/USER import libs implicitly; MSVC needs them spelled out.
  add_executable(vst3probe src/vst3/probe.cpp src/vst3/probe_host_win.cpp
    src/vst3/iids.cpp ${VST3_SDK_SRCS})
  target_include_directories(vst3probe PRIVATE src src/compat third_party/vst3 third_party/imgui)
  target_link_libraries(vst3probe PRIVATE smu2000_core ole32 gdi32 user32)

  add_executable(clapprobe src/clap/probe.cpp)
  target_include_directories(clapprobe PRIVATE src src/compat third_party/clap)
  target_link_libraries(clapprobe PRIVATE smu2000_core)

  add_executable(vstiprobe src/vsti/probe.cpp)
  target_include_directories(vstiprobe PRIVATE src src/compat)
  target_link_libraries(vstiprobe PRIVATE user32)

  # ---- The two desktop apps (Makefile: gui / live in the windows branch) -----
  # src/ui/audio_out.cpp, midi_in.cpp and friends are this platform's versions
  # (the Linux branch picks the *_linux.cpp ones), so gui.exe and live.exe link
  # WinMM/WASAPI audio + MIDI straight from them.

  # live: real-time MIDI in -> engine -> audio out, no window
  add_executable(live src/live.cpp src/ui/audio_out.cpp src/ui/midi_in.cpp)
  target_include_directories(live PRIVATE src src/compat)
  target_link_libraries(live PRIVATE smu2000_core winmm ole32 uuid avrt)

  # gui: the front-panel window (Win32 + D3D11), plus the PC editor windows
  set(GUI_WIN_UI_SRCS
    src/ui/panel.cpp src/ui/editor.cpp src/ui/effects.cpp src/ui/png.cpp
    src/ui/audio_out.cpp src/ui/audio_in.cpp src/ui/midi_in.cpp src/ui/midi_out.cpp
    src/ui/layout.cpp src/ui/svg.cpp src/ui/player.cpp src/xg/model.cpp)
  add_executable(gui src/gui.cpp ${GUI_WIN_UI_SRCS} ${PC_WIN_SRCS}
    ${IMGUI_CORE} ${IMGUI_WIN_BACKENDS} src/ui/app_win.cpp src/ui/window_win.cpp)
  target_include_directories(gui PRIVATE src src/compat third_party/imgui)
  target_link_libraries(gui PRIVATE smu2000_core ${PLUGIN_WIN_LIBS})
  if(ASIO)
    include(cmake/OptionalAudio.cmake)
  endif()
endif()
