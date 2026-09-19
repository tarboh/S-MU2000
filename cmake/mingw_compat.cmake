# mingw_compat.cmake — MinGW-w64 (MSYS2) compatibility layer for the SMU2000_VST2 CMake build.
# Copied verbatim from ../sw10_plug/cmake/mingw_compat.cmake (P5 owns MinGW; inert on MSVC).
#
# Included from the root CMakeLists.txt AFTER find_package(iPlug2) when the compiler is
# the Windows GNU/Clang (MinGW) toolchain. The MSVC path is untouched by everything here.
#
# Scope (see VST2_LEDGER.md Phase 5; JIT status updated 2026-09-16, CPU32_LEDGER.md Phases 1-8):
#   - Both x86_64 (MSYS2 MINGW64 shell, preset mingw-x64/mingw-clang-x64) and x86
#     (MSYS2 MINGW32 shell, preset mingw-win32) are supported: the SMU2000 engine is
#     arch-clean. Both JITs are now DUAL-MODE (sh2_jit.cpp / swp30_jit.cpp: x64 under
#     __x86_64__||_M_X64 — MSVC x64 included — and x86-32 under __i386__||_M_IX86 via
#     SMU_JIT32_PORT_SH2/MEG self-defines), so win32 MinGW compiles the JIT path too, in
#     principle: the 32-bit emitter is validated (tools/x64asm32_test.cpp + MSVC x86 A/B,
#     bit-exact) and the x64 path is GCC+Clang-proven here. No SSE is emitted in either
#     mode, so no win32 stack-alignment/SSE flags are needed or set anywhere below.
#   - DEV-BOX STATUS (updated 2026-09): the MSYS2 i686 toolchain WORKS here (gcc 16.1
#     Rev5; the old cc1plus-silent-death breakage is gone). Full mingw-ci-win32 builds
#     run locally; keep using the `mingw-win32`/`mingw-ci-win32` presets with
#     C:\msys64\mingw32\bin first on PATH (bare g++ without it dies STATUS_DLL_NOT_FOUND).
#     Win32 plugin MODULE links need the i686 SEH fix below (Rev5 CRT packaging bug).
#   - Default build is GRAPHICS-FREE (SMU2000_ENABLE_GUI=OFF): no IGraphics/NanoVG.
#     The GUI-ON path keeps the sw10 NanoVG/GL2 notes (nanovg.c+glad.c unity-built
#     inside IGraphicsWin.cpp; glad LoadLibrary()s opengl32.dll; no prebuilt MSVC
#     graphics libs are linked, unlike SKIA).
#
# Everything here stays in the superproject; the iPlug2 submodule gitlink is never touched.

if(NOT MINGW)
  return()
endif()

# ---------------------------------------------------------------------------
# Static runtime + COFF section fixes.
#   -static-libgcc/-static-libstdc++ : plugins must not depend on libgcc/libstdc++ DLLs
#                                      (matches the MSVC /MT self-contained CRT intent).
#   -Wa,-mbig-obj                    : the huge template TUs (sh.cpp / mu2000.cpp — the
#                                      same ones that need /bigobj on MSVC — plus
#                                      IGraphicsWin.cpp when GUI is ON) exceed the plain
#                                      COFF assembler section limit.
#   -fpermissive                     : iPlug/APP glue assigns FARPROC (GetProcAddress) to
#                                      void* implicitly — valid-permissive MSVC, hard error
#                                      under libstdc++. Downgrades it back to a warning.
#   -include <prelude>               : libstdc++ does not pull <memory>/<cmath>/... in
#                                      transitively the way MSVC's <Windows.h>+SDK headers
#                                      happen to; force-include a small STL prelude into every
#                                      C++ TU (iPlug SDK + VST3 SDK + plugin sources) so the
#                                      upstream headers that use std::unique_ptr etc. resolve.
# COMPILE_LANGUAGE guards keep windres (RC) and plain-C TUs out of the C++-only flags.
# ---------------------------------------------------------------------------
add_compile_definitions(_USE_MATH_DEFINES)          # M_PI / M_PI_2 under __STRICT_ANSI__
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-Wa,-mbig-obj>")
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fpermissive>")
add_link_options(-static -static-libgcc -static-libstdc++)
string(APPEND CMAKE_CXX_FLAGS
  " -include \"${CMAKE_CURRENT_LIST_DIR}/mingw_portability_prelude.h\"")

# The GUI-ON editor (P7) is NATIVE GDI (ui::panel) — no IGraphicsWin.cpp, so NO opengl32/WGL is
# used. gdi32/comdlg32/user32 are what the child window + SmartMedia dialog need; smu2000_gui
# links them PUBLIC (MinGW maps Foo.lib->-lfoo). Kept as a global safety net for GUI-ON only;
# the graphics-free default (SMU2000_ENABLE_GUI=OFF) links NONE (hard rule #6).
if(SMU2000_ENABLE_GUI)
  link_libraries(gdi32 comdlg32 user32)
endif()

# ---------------------------------------------------------------------------
# smu_mingw_seh_i686 — MSYS2 mingw-w64-i686-gcc 16.1.0 Rev5 (2026-08) CRT packaging bug.
# Any shared/MODULE target that transitively pulls libmsvcrt.a's i386 beginthreadex
# thunk (member lib32_libmsvcrt_extra_a-i386__beginthreadex.o — every std::thread /
# winpthread user does it: IPlugTimer, engine async boot) fails to link: that thunk
# calls __mingw_SEH_error_handler, defined only in libmingw32.a's crt_handler member,
# and with this Rev5 archive layout ld never resolves it for DLL links (exe links are
# fine; a late -lmingw32 on the command line does NOT help — the archive has already
# been scanned). Every plugin MODULE link dies with
#   "undefined reference to `__mingw_SEH_error_handler`".
# Fix: extract the correct member from libmingw32.a at configure time into the build
# tree (C:\msys64 stays read-only — hard rules) and pass the .o directly on the
# plugin MODULE link lines. Drop this whole block when an MSYS2 update fixes the
# packaging. x64 is unaffected (no such member in the x86_64 libmsvcrt).
# ---------------------------------------------------------------------------
set(SMU_MINGW_SEH_OBJ "")
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(_seh_dir "${CMAKE_BINARY_DIR}/_seh")
  file(MAKE_DIRECTORY "${_seh_dir}")
  set(SMU_MINGW_SEH_OBJ "${_seh_dir}/mingw_seh_crt_handler.o")
  if(NOT EXISTS "${SMU_MINGW_SEH_OBJ}")
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libmingw32.a
                    OUTPUT_VARIABLE _libmingw32 OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT IS_ABSOLUTE "${_libmingw32}" OR NOT EXISTS "${_libmingw32}")
      message(FATAL_ERROR "SMU2000/mingw: cannot locate libmingw32.a via ${CMAKE_CXX_COMPILER} -print-file-name")
    endif()
    # Discover the crt_handler member under this archive's own naming
    execute_process(COMMAND ${CMAKE_AR} t "${_libmingw32}"
                    OUTPUT_VARIABLE _seh_members OUTPUT_STRIP_TRAILING_WHITESPACE
                    COMMAND_ERROR_IS_FATAL ANY)
    string(REGEX MATCH "[^\n;]*crt_handler[^ \n;\r]*\\.o" _seh_member "${_seh_members}")
    
    # --- CHANGED BLOCK: Fall back gracefully if the member isn't found ---
    if(NOT _seh_member)
      message(STATUS "SMU2000/mingw: crt_handler not found in libmingw32.a (Likely fixed upstream in GCC 16.2+). Skipping workaround.")
      # FIX: Unset the variable so it evaluates to FALSE in conditional checks
      unset(SMU_MINGW_SEH_OBJ) 
    else()
      # ar x extracts under the member name into CWD; run it in the build dir, rename.
      execute_process(COMMAND ${CMAKE_AR} x "${_libmingw32}" "${_seh_member}"
                      WORKING_DIRECTORY "${_seh_dir}" COMMAND_ERROR_IS_FATAL ANY)
      if(NOT EXISTS "${_seh_dir}/${_seh_member}")
        message(FATAL_ERROR "SMU2000/mingw: ar x extracted no ${_seh_member} into ${_seh_dir}")
      endif()
      file(RENAME "${_seh_dir}/${_seh_member}" "${SMU_MINGW_SEH_OBJ}")
      message(STATUS "SMU2000/mingw: i686 SEH link fix active -> ${SMU_MINGW_SEH_OBJ}")
    endif()
    # ----------------------------------------------------------------------
  endif()
endif()

# ---------------------------------------------------------------------------
# smu2000_mingw_fixup_imported_libs() — the upstream iPlug2 INTERFACE targets
# (iPlug2::IPlug, iPlug2::APP, iPlug2::Extras::OSC) list MSVC import-library names
# (Shlwapi.lib, comctl32.lib, wininet.lib, dsound.lib, winmm.lib, ws2_32.lib). MinGW
# ships lib<name>.a and wants -l<name>, and ld will NOT find a "Foo.lib". Rewrite those
# bare *.lib entries in the imported targets' INTERFACE_LINK_LIBRARIES to plain names
# (CMake then emits -lfoo). Real file paths (skia.lib / WebView2LoaderStatic.lib) are
# left alone — those live behind backends/features the NanoVG MinGW build doesn't use.
# ---------------------------------------------------------------------------
function(smu2000_mingw_fixup_lib_names target)
  if(NOT TARGET ${target})
    return()
  endif()
  get_target_property(_libs ${target} INTERFACE_LINK_LIBRARIES)
  if(NOT _libs)
    return()
  endif()
  set(_rewritten "")
  set(_changed FALSE)
  foreach(_lib IN LISTS _libs)
    # Only bare "Name.lib" tokens (no directory, no generator expression, no path).
    if(_lib MATCHES "^([A-Za-z0-9_]+)\.lib$")
      list(APPEND _rewritten "${CMAKE_MATCH_1}")
      set(_changed TRUE)
    else()
      list(APPEND _rewritten "${_lib}")
    endif()
  endforeach()
  if(_changed)
    set_target_properties(${target} PROPERTIES INTERFACE_LINK_LIBRARIES "${_rewritten}")
    message(STATUS "SMU2000/mingw: rewrote MSVC .lib names in ${target} -> MinGW -l names")
  endif()
endfunction()

foreach(_t iPlug2::IPlug iPlug2::APP iPlug2::Extras::OSC iPlug2::Extras::Synth iPlug2::VST3 iPlug2::VST2 iPlug2::CLAP iPlug2::IGraphics)
  smu2000_mingw_fixup_lib_names(${_t})
endforeach()
