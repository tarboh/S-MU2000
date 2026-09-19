# native/CMakeLists.txt — native tool exes (SMU2000_BUILD_TOOLS; Makefile's render/live/gui
# Windows targets). These are DESKTOP TOOLS: imgui / Direct3D / WinMM / WASAPI are fine here.
# The plugin graphics-free hard rule (ledger #6) applies to SMU2000_VST2/, never to this dir.
#
# Source lists mirror the Makefile link lines EXACTLY (the MSYS2 g++ Makefile stays canonical):
#   render$(EXE): OBJS + mu2000.o + smf.o + render.o
#   live$(EXE):   OBJS + mu2000.o + ui/midi_in.o + ui/audio_out.o + live.o
#                 -> -lwinmm -lole32 -lavrt
#   gui$(EXE):    OBJS + mu2000.o + smf.o + UI_SRCS + PC_SRCS(imgui app) + gui.o
#                 -> -lwinmm -lole32 -lgdi32 -luser32 -lavrt -lcomdlg32 -lshell32
#                    -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -limm32
# "OBJS + mu2000.cpp" = the smu2000_engine static lib (engine/CMakeLists.txt); its PUBLIC
# defs (_USE_MATH_DEFINES/NOMINMAX/_CRT_*/WIN32) and src|src/compat include roots flow in
# from there. The engine's one extra object (src/vst3/engine.cpp) is never pulled by these
# exes (static-lib member only links on reference) — matching the Makefile, which builds
# tools without the Steinberg glue (src/vst3/{plugin,view,iids} stay out; imgui gui.cpp is
# the GUI-only src/ui/* consumer, exactly like the Makefile's gui target).
#
# Target-name note: the imgui app's CMake target is `smu2000_gui_app` but its artifact is
# smu2000_gui.exe (OUTPUT_NAME). CMake target `smu2000_gui` belongs to the plugin's native
# GDI editor static lib (SMU2000_VST2/ui) when SMU2000_ENABLE_GUI=ON — the target-name space
# is shared per generator, so the exe keeps a distinct name while the .exe matches the
# Makefile's gui.exe. (The exe never links that static lib; it compiles its own copy of the
# shared src/ui sources, exactly as the Makefile's gui target does.)
#
# Output: build-cmake/tools/${SMU2000_ARCH}/<Config>/*.exe (root-convention helper below,
# mirrors smu2000_set_out_dirs in SMU2000_VST2/CMakeLists.txt — that function is scoped to
# its directory, hence the local twin). ROM staging mirrors smu2000_stage_roms.

# ---------------------------------------------------------------------------
# Output layout: build-cmake/tools/<arch>/<Config>/ (same helper shape as the plugin)
# ---------------------------------------------------------------------------
function(smu2000_native_out_dirs target)
  foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${cfg} CFG)
    set_target_properties(${target} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY_${CFG} "${SMU2000_OUTPUT_ROOT}/tools/${SMU2000_ARCH}/${cfg}")
  endforeach()
  # Multi-config (VS): generic dir is a fallback only. Single-config (Ninja/MinGW): the
  # generic dir is the real one, so append the build type (plugin helper kept in sync).
  if(CMAKE_CONFIGURATION_TYPES)
    set(generic_dir "${SMU2000_OUTPUT_ROOT}/tools/${SMU2000_ARCH}")
  else()
    set(generic_dir "${SMU2000_OUTPUT_ROOT}/tools/${SMU2000_ARCH}/${CMAKE_BUILD_TYPE}")
  endif()
  set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${generic_dir}")
endfunction()

# Post-build ROM staging (same pattern as SMU2000_VST2/CMakeLists.txt::smu2000_stage_roms):
# render/live take the rom dir as argv[1] but the smoke habit is "exe next to roms/"; the
# repo roms/ (36 MB, user-supplied, git-ignored) lands next to each exe when COPY_ROMS=ON.
# ci-* presets set SMU2000_COPY_ROMS=OFF. Re-runs only on relink; all three exes share one
# dir per config so the copy is redundant-but-harmless (kept per-target so a single
# `--target smu2000_live` build still gets ROMs).
function(smu2000_native_stage_roms target)
  if(SMU2000_COPY_ROMS AND EXISTS "${SMU2000_ROMS_DIR}")
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${SMU2000_ROMS_DIR}" "$<TARGET_FILE_DIR:${target}>/roms"
      COMMENT "SMU2000(tools): staging roms/ next to $<TARGET_FILE_NAME:${target}>")
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Shared tool flavor: engine's PUBLIC defs/includes + the /utf-8 /bigobj pair the
# Japanese-commented / section-heavy sources need under cl (engine + SMU2000_VST2/ui
# carry the same; PRIVATE here — engine PUBLIC defs are already inherited).
# MinGW mirror matches SMU2000_VST2/ui ($<CXX_COMPILER_ID:GNU> big-obj/permissive).
# ---------------------------------------------------------------------------
function(smu2000_tool_common target)
  target_compile_features(${target} PRIVATE cxx_std_20)
  target_compile_options(${target} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
    $<$<CXX_COMPILER_ID:GNU>:-Wa,-mbig-obj>
    $<$<CXX_COMPILER_ID:GNU>:-fpermissive>)
  # console subsystem: every tool main() prints usage/progress to stdout (Makefile has no
  # -mwindows / /SUBSYSTEM:WINDOWS anywhere).
  set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE FALSE)
endfunction()

# ---------------------------------------------------------------------------
# smu2000_render — MIDI -> WAV offline render (Makefile render target)
# ---------------------------------------------------------------------------
add_executable(smu2000_render)
target_sources(smu2000_render PRIVATE
  ${CMAKE_SOURCE_DIR}/src/render.cpp
  ${CMAKE_SOURCE_DIR}/src/smf.cpp)
target_link_libraries(smu2000_render PRIVATE smu2000_engine)
smu2000_tool_common(smu2000_render)
smu2000_native_out_dirs(smu2000_render)
smu2000_native_stage_roms(smu2000_render)

# ---------------------------------------------------------------------------
# smu2000_live — live MIDI-in + WASAPI/WaveOut output (Makefile live target:
# OBJS + mu2000 + ui/midi_in + ui/audio_out + live; -lwinmm -lole32 -lavrt)
# ---------------------------------------------------------------------------
add_executable(smu2000_live)
target_sources(smu2000_live PRIVATE
  ${CMAKE_SOURCE_DIR}/src/live.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/midi_in.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/audio_out.cpp)
target_link_libraries(smu2000_live PRIVATE
  smu2000_engine
  winmm ole32 avrt)   # bare names: MSVC resolves gdi32-style as <name>.lib, GNU as -l<name>
smu2000_tool_common(smu2000_live)
smu2000_native_out_dirs(smu2000_live)
smu2000_native_stage_roms(smu2000_live)

# ---------------------------------------------------------------------------
# smu2000_gui_app -> smu2000_gui.exe — front-panel + PC-editor app (Makefile gui target).
# The only imgui/D3D consumer in the CMake tree (Makefile IMGUI_SRCS + PC_SRCS + UI_SRCS,
# with -DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD; no gamepad, so no xinput — imgui_impl_win32
# needs dwmapi+imm32 instead, both in the Makefile LDLIBS line).
# ---------------------------------------------------------------------------
set(SMU2000_IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")

add_executable(smu2000_gui_app)
set_target_properties(smu2000_gui_app PROPERTIES OUTPUT_NAME smu2000_gui)
target_sources(smu2000_gui_app PRIVATE
  # main + sequencer helper (Makefile: $(BUILD)/src/smf.o + gui.o)
  ${CMAKE_SOURCE_DIR}/src/gui.cpp
  ${CMAKE_SOURCE_DIR}/src/smf.cpp
  # UI_SRCS (panel/audio/midi/io layer; the GDI panel .cpp shared with the plugin GUI lib)
  ${CMAKE_SOURCE_DIR}/src/ui/panel.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/editor.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/effects.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/png.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/audio_out.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/audio_in.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/midi_in.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/midi_out.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/layout.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/svg.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/player.cpp
  ${CMAKE_SOURCE_DIR}/src/xg/model.cpp
  # PC_SRCS (imgui views + Win32/DX11 window; doc/pc-editor.md)
  ${CMAKE_SOURCE_DIR}/src/ui/pc_editor.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/pc_window.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/xg_ui.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/overview.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/fx_editor.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/fx_help.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/part_shapes.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/master_editor.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/fx_icons.cpp
  # IMGUI_SRCS (vendored, untouched — hard rule: third_party/ is read-only)
  ${SMU2000_IMGUI_DIR}/imgui.cpp
  ${SMU2000_IMGUI_DIR}/imgui_draw.cpp
  ${SMU2000_IMGUI_DIR}/imgui_tables.cpp
  ${SMU2000_IMGUI_DIR}/imgui_widgets.cpp
  ${SMU2000_IMGUI_DIR}/backends/imgui_impl_win32.cpp
  ${SMU2000_IMGUI_DIR}/backends/imgui_impl_dx11.cpp)

# Makefile IMGUI_FLAGS: -I third_party/imgui -DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD. The
# backends find their own headers via same-directory quoted-include lookup (as under make).
target_include_directories(smu2000_gui_app PRIVATE ${SMU2000_IMGUI_DIR})
target_compile_definitions(smu2000_gui_app PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

# Makefile gui LDLIBS, verbatim (see the block header for which lib belongs to what).
target_link_libraries(smu2000_gui_app PRIVATE
  smu2000_engine
  winmm ole32 gdi32 user32 avrt comdlg32 shell32
  d3d11 dxgi d3dcompiler dwmapi imm32)
smu2000_tool_common(smu2000_gui_app)
smu2000_native_out_dirs(smu2000_gui_app)
smu2000_native_stage_roms(smu2000_gui_app)

message(STATUS "SMU2000(tools): smu2000_render + smu2000_live + smu2000_gui(.exe) -> ${SMU2000_OUTPUT_ROOT}/tools/${SMU2000_ARCH}/<Config>/")
