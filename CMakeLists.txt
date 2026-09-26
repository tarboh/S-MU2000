# S-MU2000 — CMake build (alternative to the Makefile)
#
#   cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-cmake -j
#   ./build-cmake/verify && ./build-cmake/render roms song.mid out.wav
#
# The Makefile remains the primary build (it also covers Windows/macOS and the
# Audio Unit). This file covers the portable core everywhere and the full
# Linux set (live + VST3 + CLAP + probes) where ALSA is available.
cmake_minimum_required(VERSION 3.16)
project(S-MU2000 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Non-literal printf formats are bugs (translated strings must go through "%s").
add_compile_options(-Wformat-security)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- Emulator core (same sources as SRCS in the Makefile) -------------------
add_library(smu2000_core STATIC
  src/compat/compat.cpp
  src/smartmedia.cpp
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
target_compile_options(smu2000_core PRIVATE -Wall -Wno-unused-variable -Wno-unused-but-set-variable)
set_target_properties(smu2000_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

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

  # Cairo + fontconfig for gdi_linux.cpp (panel drawing in gui and plug-ins),
  # SDL3 for the gui window (Phase 2, doc/porting-linux-gui.md)
  pkg_check_modules(CAIRO QUIET cairo fontconfig)
  pkg_check_modules(SDL3 QUIET sdl3)

  add_executable(live src/live.cpp
    src/ui/audio_out_linux.cpp
    src/ui/audio_in_linux.cpp
    src/ui/midi_in_linux.cpp
    src/ui/midi_out_linux.cpp)
  target_link_libraries(live PRIVATE smu2000_core ${ALSA_LIBRARIES} pthread dl)
  target_include_directories(live PRIVATE ${ALSA_INCLUDE_DIRS})

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
  set(LINUX_PANEL_SRCS
    src/compat/gdi_linux.cpp
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
    third_party/vst3 third_party/imgui ${CAIRO_INCLUDE_DIRS})
  target_link_libraries(S-MU2000-vst3 PRIVATE smu2000_core
    ${ALSA_LIBRARIES} ${CAIRO_LIBRARIES} pthread dl)
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
    third_party/vst3 third_party/clap third_party/imgui ${CAIRO_INCLUDE_DIRS})
  target_link_libraries(S-MU2000-clap PRIVATE smu2000_core
    ${ALSA_LIBRARIES} ${CAIRO_LIBRARIES} pthread dl)
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
  if(CAIRO_FOUND AND SDL3_FOUND)
    add_executable(gui src/gui_linux.cpp
      src/compat/gdi_linux.cpp
      src/ui/panel.cpp src/ui/layout.cpp src/ui/svg.cpp
      src/ui/editor.cpp src/ui/effects.cpp src/ui/png.cpp
      src/ui/player.cpp src/xg/model.cpp
      src/ui/xg_ui.cpp src/ui/fx_help.cpp src/ui/fx_icons.cpp
      src/ui/sdl_popup.cpp
      src/ui/window_sdl.cpp
      src/ui/app_linux.cpp
      src/ui/pc_window_linux.cpp
      src/ui/pc_editor.cpp src/ui/overview.cpp src/ui/fx_editor.cpp
      src/ui/part_shapes.cpp src/ui/master_editor.cpp
      ${IMGUI_CORE}
      third_party/imgui/backends/imgui_impl_sdl3.cpp
      third_party/imgui/backends/imgui_impl_sdlrenderer3.cpp
      src/ui/audio_out_linux.cpp
      src/ui/audio_in_linux.cpp
      src/ui/midi_in_linux.cpp
      src/ui/midi_out_linux.cpp)
    target_include_directories(gui PRIVATE src src/compat
      third_party/imgui ${ALSA_INCLUDE_DIRS} ${CAIRO_INCLUDE_DIRS} ${SDL3_INCLUDE_DIRS})
    target_link_libraries(gui PRIVATE smu2000_core
      ${ALSA_LIBRARIES} ${CAIRO_LIBRARIES} ${SDL3_LIBRARIES} pthread dl)
  else()
    message(STATUS "Cairo/SDL3 not found: skipping gui (needs libcairo2-dev libsdl3-dev)")
  endif()
endif()
