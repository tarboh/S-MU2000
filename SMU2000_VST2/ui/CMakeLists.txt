# SMU2000_VST2/ui/CMakeLists.txt — smu2000_gui STATIC (Phase 7). Native Win32/GDI editor.
# Sources under src/ are the unchanged upstream GUI code; the host bridge
# is SMU2000Editor.{h,cpp} (a port of src/vst3/view.cpp). NO IGraphics / NanoVG / OpenGL /
# Skia / imgui / D3D — the panel is raw GDI (src/ui/draw.h) and fonts are system fonts, so
# the GUI-ON plug-in stays graphics-free (ledger hard rule #6), identical recipe to GUI-OFF
# apart from SMU2000_ENABLE_GUI (→ PLUG_HAS_UI 1) and this lib. CRT (/MT) comes from the
# root; this standalone target carries its own src/src-compat includes + NOMINMAX PUBLIC.

add_library(smu2000_gui STATIC)

target_sources(smu2000_gui PRIVATE
  # --- VST3 GUI panel (the exact set Makefile VST3_SRCS pulls, minus the Steinberg glue)
  ${CMAKE_SOURCE_DIR}/src/ui/panel.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/editor.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/effects.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/layout.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/svg.cpp
  ${CMAKE_SOURCE_DIR}/src/xg/model.cpp
  # --- iPlug2 host: wraps ui::panel in a child HWND + WndProc (port of src/vst3/view.cpp)
  ${CMAKE_CURRENT_SOURCE_DIR}/SMU2000Editor.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/SMU2000Editor.h
)

# GUI-only sources (src/**) and the host bridge don't link the engine, so they need the
# engine's PUBLIC include roots directly. PUBLIC so the plugin target inherits them too
# (its SMU2000_VST2.cpp reaches engine.h / ui/*.h via the same roots).
target_include_directories(smu2000_gui PUBLIC
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/compat
  ${CMAKE_CURRENT_SOURCE_DIR})   # SMU2000_VST2/ui → plugin `#include "ui/SMU2000Editor.h"`

target_compile_features(smu2000_gui PUBLIC cxx_std_20)

# NOMINMAX is REQUIRED here, not just on the engine: svg.cpp includes <windows.h> (via
# svg.h) BEFORE <algorithm>, so without it MSVC's windows.h min/max macros corrupt
# std::min/std::max (C2589). Engine has it; this target is standalone so it must carry its
# own. PUBLIC keeps dependents consistent.
target_compile_definitions(smu2000_gui PUBLIC
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
  _CRT_NONSTDC_NO_WARNINGS
  WIN32
  NO_IGRAPHICS)   # native editor is IGraphics-free by construction (SMU2000Editor.h guard)

# GUI-ON editor: the Win32 libs the child window needs (gdi32 fonts/blit, comdlg32 SmartMedia
# file dialog, user32 child window/menus). PUBLIC so both the lib and the plugin MODULE link them.
# MSVC resolves the .lib names directly; MinGW maps them to -lgdi32/-lcomdlg32/-luser32.
# (opengl32 is NOT needed — the native editor has no GL; unlike the old IGraphicsWin path.)
target_link_libraries(smu2000_gui PUBLIC
  $<$<CXX_COMPILER_ID:MSVC>:gdi32 comdlg32 user32>
  $<$<CXX_COMPILER_ID:GNU>:gdi32 comdlg32 user32>)

# Japanese-commented UTF-8 sources; MSVC needs /utf-8 (mirrors engine + plugin targets),
# MinGW passes the UTF-8 bytes through as-is but needs big-obj (svg/layout) + permissive like
# the engine. PUBLIC so the plugin MODULE's editor TU (SMU2000_VST2.cpp) matches under MinGW.
target_compile_options(smu2000_gui PUBLIC
  $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
  $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
  $<$<CXX_COMPILER_ID:GNU>:-Wa,-mbig-obj -fpermissive>
)
