# SMU2000_VST2 API targets — mirrors ../sw10_plug/SW10_PLUG/CMakeLists.txt.
# Both GUI-OFF (opt-out) and GUI-ON (default since 2026-09-16) use the SAME graphics-free manual recipe (NO
# iplug_configure_target, NO IGraphics): IPlugVST2/IPlugCLAP glue added by hand +
# iPlug2::IPlug + NO_IGRAPHICS. GUI-ON (P7) just adds SMU2000_ENABLE_GUI (→ PLUG_HAS_UI 1)
# and links smu2000_gui — the native Win32/GDI editor (ui::panel, port of src/vst3/view.cpp)
# hosted through IEditorDelegate::OpenWindow/CloseWindow. Neither path links IGraphics/
# NanoVG/OpenGL/Skia (ledger hard rule #6). Helper functions below are live since P0.

set(SMU2000_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# P2 landed the real plugin sources; the MODULE targets are created below.
set(SMU2000_HAS_PLUGIN_SOURCES TRUE)

# /MAP link maps were a P2 crash-symbolization debug aid, but they polluted the
# published artifact folders (.map shipped in CI uploads). OFF by default now;
# re-enable on the dev box with -DSMU2000_LINK_MAP=ON when symbolizing a crash.
option(SMU2000_LINK_MAP "Add MSVC /MAP to the plugin link steps (debug aid)" OFF)

# Phase 7 — native GDI editor lib (smu2000_gui): src/ui/{panel,editor,effects,layout,svg}
# + src/xg/model.cpp + ui/SMU2000Editor.cpp. Built only when GUI is ON; it does NOT pull
# IGraphics (raw GDI), so GUI-ON stays graphics-free just like the default GUI-OFF path.
if(SMU2000_ENABLE_GUI)
  add_subdirectory(ui)
endif()

# smu2000_set_out_dirs(<target> <api> <RUNTIME|LIBRARY>)
# Artifacts: build-cmake/<api>/<arch>/<Config>/ (arch dir set by the root project;
# MinGW appends -mingw[-clang]). Upstream's out/-dir defaults are overridden.
function(smu2000_set_out_dirs target api kind)
  foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${cfg} CFG)
    set(dir "${SMU2000_OUTPUT_ROOT}/${api}/${SMU2000_ARCH}/${cfg}")
    set_target_properties(${target} PROPERTIES ${kind}_OUTPUT_DIRECTORY_${CFG} "${dir}")
  endforeach()
  # Multi-config (VS): the generic dir is only a fallback. Single-config (Ninja/MinGW)
  # uses ONLY the generic dir, so append the build type there to keep the documented
  # <api>/<arch>-mingw/<Config>/ layout (MSVC behavior byte-unchanged).
  if(CMAKE_CONFIGURATION_TYPES)
    set(generic_dir "${SMU2000_OUTPUT_ROOT}/${api}/${SMU2000_ARCH}")
  else()
    set(generic_dir "${SMU2000_OUTPUT_ROOT}/${api}/${SMU2000_ARCH}/${CMAKE_BUILD_TYPE}")
  endif()
  set_target_properties(${target} PROPERTIES ${kind}_OUTPUT_DIRECTORY "${generic_dir}")
endfunction()

# Post-build ROM staging (mirrors sw10 SW10_COPY_ROM; wired for P2):
# copy the whole repo roms/ tree next to the produced .dll/.clap so
# src/vst3/engine.cpp::find_roms() hits <dllDir>\roms first. ci-* presets set
# SMU2000_COPY_ROMS=OFF (Yamaha ROMs are the user's, never distributed).
function(smu2000_stage_roms target)
  if(SMU2000_COPY_ROMS AND EXISTS "${SMU2000_ROMS_DIR}")
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${SMU2000_ROMS_DIR}" "$<TARGET_FILE_DIR:${target}>/roms"
      COMMENT "SMU2000: staging roms/ next to $<TARGET_FILE_NAME:${target}>")
  endif()
endfunction()

if(NOT SMU2000_HAS_PLUGIN_SOURCES)
  message(STATUS "SMU2000_VST2: P2/P3 pending — no MODULE targets created yet (smu2000_vst2/smu2000_clap arrive with the real sources; staging + out-dir helpers above are ready)")
else()
  if(SMU2000_BUILD_VST2)
    if(NOT SMU2000_VST2_SUPPORTED)
      message(FATAL_ERROR
        "SMU2000_BUILD_VST2=ON but SMU2000_VST2_SUPPORTED=FALSE (aeffect.h/aeffectx.h missing) — "
        "see cmake/iplug2_paths.cmake fix message, or -DSMU2000_BUILD_VST2=OFF.")
    endif()

    if(NOT SMU2000_ENABLE_GUI)
      # ------------------------------------------------------------------
      # GUI-OFF (opt-out: -DSMU2000_ENABLE_GUI=OFF) graphics-free VST2 — ledger §"Graphics-free plugin recipe".
      # NOT iplug_configure_target/iPlug2::VST2: those force IPLUG_EDITOR=1 (VST2.cmake:53)
      # and pull IGraphics. We add ${IPLUG2_DIR}/IPlug/VST2/IPlugVST2.cpp by hand and link
      # iPlug2::IPlug (DSP core only). Defines: NO IPLUG_EDITOR (editor code compiles out),
      # NO_IGRAPHICS (IPlug_include_in_plug_hdr.h:114 / IPlugAPIBase.h:103 /
      # IPlugDelegate_select.h then select the editor-free delegate + skip the
      # IGraphics_include_in_plug_* includes). No ${IGRAPHICS_LIB} anywhere (hard rule #6).
      # CRT (/MT) + NOMINMAX/_CRT_* come from iPlug2::IPlug's INTERFACE; cxx_std_20 +
      # src/src-compat includes come from smu2000_engine's INTERFACE.
      # ------------------------------------------------------------------
      add_library(smu2000_vst2 MODULE)
      target_sources(smu2000_vst2 PRIVATE
        SMU2000_VST2.cpp
        SMU2000_VST2.h
        midi_queue.h
        config.h
        ${IPLUG2_DIR}/IPlug/VST2/IPlugVST2.cpp)
      target_include_directories(smu2000_vst2 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}                            # config.h via IPlug_include_in_plug_hdr.h
        ${IPLUG2_DIR}/IPlug/VST2                               # IPlugVST2.h
        ${IPLUG2_DIR}/Dependencies/IPlug/VST2_SDK)             # aeffect.h/aeffectx.h (P0-populated stub)
      target_compile_definitions(smu2000_vst2 PRIVATE
        VST2_API VST_FORCE_DEPRECATED IPLUG_DSP=1 NO_IGRAPHICS
        # IPlugConstants.h defaults sample=double unless told otherwise; VST2 audio is
        # float and engine::fill wants float* — pin the whole target to float.
        SAMPLE_TYPE_FLOAT)
      target_compile_features(smu2000_vst2 PRIVATE cxx_std_20)
      # engine.h pulls Japanese-commented ui/*.h; /utf-8 is PRIVATE on the engine and
      # does not propagate — set it here too (misread DBCS tails can eat newlines).
      target_compile_options(smu2000_vst2 PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
      # Link map was the P2 crash-symbolization debug aid — now opt-in via
      # SMU2000_LINK_MAP (OFF) so Release folders ship binary-only.
      if(SMU2000_LINK_MAP)
        target_link_options(smu2000_vst2 PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/MAP>)
      endif()
      target_link_libraries(smu2000_vst2 PRIVATE smu2000_engine iPlug2::IPlug)
      set_target_properties(smu2000_vst2 PROPERTIES
        OUTPUT_NAME "SMU2000_VST2" PREFIX "" SUFFIX ".dll")
      smu2000_set_out_dirs(smu2000_vst2 vst2 LIBRARY)
      smu2000_stage_roms(smu2000_vst2)
    else()
      # ------------------------------------------------------------------
      # GUI-ON VST2 — NATIVE editor (Phase 7), still GRAPHICS-FREE. Same manual recipe as
      # the GUI-OFF branch (IPlugVST2.cpp by hand + iPlug2::IPlug + NO_IGRAPHICS) plus:
      #   * SMU2000_ENABLE_GUI → config.h PLUG_HAS_UI 1 → HasUI() true → the host drives
      #     effEditOpen/effEditClose, which iPlug routes to IEditorDelegate::OpenWindow/
      #     CloseWindow (IPlugVST2.cpp:466/483). NO_IGRAPHICS selects the plain
      #     IEditorDelegate base (IPlugDelegate_select.h), so OpenWindow is OUR override,
      #     not IGEditorDelegate's — no mMakeGraphicsFunc, no NanoVG/OpenGL/Skia.
      #   * smu2000_gui (ui::panel GDI port of src/vst3/view.cpp) + the Win32 libs it uses
      #     (gdi32 fonts/blit, comdlg32 SmartMedia file dialog, user32 child window/menus).
      # /DEPENDENTS therefore gains only gdi32/comdlg32 (no OPENGL32/nanovg/skia) — the
      # GUI-ON toggle is proven by the editor appearing, not by a GL import.
      # ------------------------------------------------------------------
      add_library(smu2000_vst2 MODULE)
      target_sources(smu2000_vst2 PRIVATE
        SMU2000_VST2.cpp
        SMU2000_VST2.h
        midi_queue.h
        config.h
        ${IPLUG2_DIR}/IPlug/VST2/IPlugVST2.cpp)
      target_include_directories(smu2000_vst2 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}                            # config.h via IPlug_include_in_plug_hdr.h
        ${IPLUG2_DIR}/IPlug/VST2                               # IPlugVST2.h
        ${IPLUG2_DIR}/Dependencies/IPlug/VST2_SDK)             # aeffect.h/aeffectx.h
      target_compile_definitions(smu2000_vst2 PRIVATE
        VST2_API VST_FORCE_DEPRECATED IPLUG_DSP=1 NO_IGRAPHICS
        SMU2000_ENABLE_GUI                                     # → PLUG_HAS_UI 1 (per-target)
        SAMPLE_TYPE_FLOAT)
      target_compile_features(smu2000_vst2 PRIVATE cxx_std_20)
      target_compile_options(smu2000_vst2 PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
      if(SMU2000_LINK_MAP)
        target_link_options(smu2000_vst2 PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/MAP>)
      endif()
      target_link_libraries(smu2000_vst2 PRIVATE
        smu2000_engine smu2000_gui iPlug2::IPlug)   # smu2000_gui brings gdi32/comdlg32/user32 PUBLIC
      set_target_properties(smu2000_vst2 PROPERTIES
        OUTPUT_NAME "SMU2000_VST2" PREFIX "" SUFFIX ".dll")
      smu2000_set_out_dirs(smu2000_vst2 vst2 LIBRARY)
      smu2000_stage_roms(smu2000_vst2)
    endif()
  endif()
  if(SMU2000_BUILD_CLAP)
    if(NOT SMU2000_CLAP_SUPPORTED)
      message(FATAL_ERROR
        "SMU2000_BUILD_CLAP=ON but SMU2000_CLAP_SUPPORTED=FALSE (CLAP_SDK/CLAP_HELPERS missing under "
        "${SMU2000_CLAP_DIR}) — run iPlug2/Dependencies/download-clap-sdks.sh (Git-Bash) or copy them "
        "there (see cmake/iplug2_paths.cmake fix message), or -DSMU2000_BUILD_CLAP=OFF.")
    endif()

    if(NOT SMU2000_ENABLE_GUI)
      # ------------------------------------------------------------------
      # GUI-OFF (opt-out) graphics-free CLAP — same recipe as smu2000_vst2 above:
      # NOT iplug_configure_target/iPlug2::CLAP (CLAP.cmake forces IPLUG_EDITOR=1 and
      # pulls IGraphics). Same API-agnostic plugin sources compiled with CLAP_API;
      # IPlug_include_in_plug_src.h picks the CLAP entry (clap_entry, CLAP_EXPORT).
      # Glue = ${IPLUG2_DIR}/IPlug/CLAP/IPlugCLAP.cpp. NO IPLUG_EDITOR + NO_IGRAPHICS
      # keep the editor out (P2-proven at this pin). No ${IGRAPHICS_LIB} (hard rule #6).
      # ------------------------------------------------------------------
      add_library(smu2000_clap MODULE)
      target_sources(smu2000_clap PRIVATE
        SMU2000_VST2.cpp
        SMU2000_VST2.h
        midi_queue.h
        config.h
        ${IPLUG2_DIR}/IPlug/CLAP/IPlugCLAP.cpp)
      target_include_directories(smu2000_clap PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}                              # config.h via IPlug_include_in_plug_hdr.h
        ${IPLUG2_DIR}/IPlug/CLAP                                 # IPlugCLAP.h
        ${IPLUG2_DIR}/Dependencies/IPlug/CLAP_SDK/include        # clap.h + clap/*
        ${IPLUG2_DIR}/Dependencies/IPlug/CLAP_HELPERS/include    # clap/helpers/*
        ${IPLUG2_DIR}/Dependencies/IPlug/CLAP_HELPERS/include/clap/helpers)  # plugin.hh etc. include bare
      target_compile_definitions(smu2000_clap PRIVATE
        CLAP_API IPLUG_DSP=1 NO_IGRAPHICS
        # engine::fill wants float* — pin the whole target to float (IPlugConstants.h).
        SAMPLE_TYPE_FLOAT)
      target_compile_features(smu2000_clap PRIVATE cxx_std_20)
      target_compile_options(smu2000_clap PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
      if(SMU2000_LINK_MAP)
        target_link_options(smu2000_clap PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/MAP>)
      endif()
      target_link_libraries(smu2000_clap PRIVATE smu2000_engine iPlug2::IPlug)
      set_target_properties(smu2000_clap PROPERTIES
        OUTPUT_NAME "SMU2000_VST2" PREFIX "" SUFFIX ".clap")
      smu2000_set_out_dirs(smu2000_clap clap LIBRARY)
      smu2000_stage_roms(smu2000_clap)
    else()
      # ------------------------------------------------------------------
      # GUI-ON CLAP — NATIVE editor (Phase 7), still GRAPHICS-FREE. Mirrors the VST2
      # GUI-ON branch above: manual IPlugCLAP.cpp glue + iPlug2::IPlug + NO_IGRAPHICS,
      # SMU2000_ENABLE_GUI (PLUG_HAS_UI 1) and smu2000_gui + Win32 libs. CLAP drives
      # OpenWindow/CloseWindow via guiSetParent/guiDestroy/guiShow (IPlugCLAP.cpp); the
      # clap::helpers base advertises clap_plugin_gui when implementsGui() (HasUI) is true.
      # ------------------------------------------------------------------
      add_library(smu2000_clap MODULE)
      target_sources(smu2000_clap PRIVATE
        SMU2000_VST2.cpp
        SMU2000_VST2.h
        midi_queue.h
        config.h
        ${IPLUG2_DIR}/IPlug/CLAP/IPlugCLAP.cpp)
      target_include_directories(smu2000_clap PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}                              # config.h via IPlug_include_in_plug_hdr.h
        ${IPLUG2_DIR}/IPlug/CLAP                                 # IPlugCLAP.h
        ${IPLUG2_DIR}/Dependencies/IPlug/CLAP_SDK/include        # clap.h + clap/*
        ${IPLUG2_DIR}/Dependencies/IPlug/CLAP_HELPERS/include    # clap/helpers/*
        ${IPLUG2_DIR}/Dependencies/IPlug/CLAP_HELPERS/include/clap/helpers)  # plugin.hh etc. include bare
      target_compile_definitions(smu2000_clap PRIVATE
        CLAP_API IPLUG_DSP=1 NO_IGRAPHICS
        SMU2000_ENABLE_GUI                                       # → PLUG_HAS_UI 1 (per-target)
        SAMPLE_TYPE_FLOAT)
      target_compile_features(smu2000_clap PRIVATE cxx_std_20)
      target_compile_options(smu2000_clap PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
      if(SMU2000_LINK_MAP)
        target_link_options(smu2000_clap PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/MAP>)
      endif()
      target_link_libraries(smu2000_clap PRIVATE
        smu2000_engine smu2000_gui iPlug2::IPlug)   # smu2000_gui brings gdi32/comdlg32/user32 PUBLIC
      set_target_properties(smu2000_clap PROPERTIES
        OUTPUT_NAME "SMU2000_VST2" PREFIX "" SUFFIX ".clap")
      smu2000_set_out_dirs(smu2000_clap clap LIBRARY)
      smu2000_stage_roms(smu2000_clap)
    endif()
  endif()

  # Both MODULE targets OUTPUT_NAME "SMU2000_VST2" (hosts want that file name). The VS
  # generator puts each MSVC import lib/.exp at <bin>/<srcdir>/<Config>/<OUTPUT_NAME>.{lib,exp}
  # — same path for both — so a parallel ALL_BUILD races the two link steps over the shared
  # .exp (LNK1104 on SMU2000_VST2.exp, typically on smu2000_clap). Neither MODULE's import
  # lib is ever linked by anything; distinct ARCHIVE_OUTPUT_NAME removes the collision
  # while the shipped .dll/.clap keep their name (MinGW: no import lib, harmless).
  if(TARGET smu2000_vst2)
    set_target_properties(smu2000_vst2 PROPERTIES ARCHIVE_OUTPUT_NAME "SMU2000_VST2_vst2")
  endif()
  if(TARGET smu2000_clap)
    set_target_properties(smu2000_clap PROPERTIES ARCHIVE_OUTPUT_NAME "SMU2000_VST2_clap")
  endif()
endif()

# MinGW i686 ONLY: append the configure-time-extracted libmsvcrt SEH handler object
# (cmake/mingw_compat.cmake -> SMU_MINGW_SEH_OBJ) to the two plugin MODULE links.
# Rev5 of mingw-w64-i686-gcc 16.1 cannot resolve __mingw_SEH_error_handler in DLL
# links otherwise. Standalone .o objects are position-independent for ld, so it is
# safe exactly here and nowhere else (exes already resolve the archive member —
# passing the .o to an exe link WOULD be a multiple definition).
if(MINGW AND SMU_MINGW_SEH_OBJ)
  foreach(_seh_t smu2000_vst2 smu2000_clap)
    if(TARGET ${_seh_t})
      target_link_options(${_seh_t} PRIVATE "${SMU_MINGW_SEH_OBJ}")
    endif()
  endforeach()
endif()
