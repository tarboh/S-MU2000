# iplug2_paths.cmake — P0 SDK/dependency resolution for the SMU2000_VST2 CMake build.
# Ported from ../sw10_plug/cmake/iplug2_paths.cmake (SW10_* -> SMU2000_*) with deliberate
# divergences (see VST2_LEDGER.md §Phase 0):
#   - VST3 is OPTIONAL (no VST3 target in P0-P3): junction is opportunistic, never FATAL.
#   - Missing CLAP SDK/helpers is NON-FATAL: sets SMU2000_CLAP_SUPPORTED FALSE + fix hint.
#   - SW10_ROM_PATH (ROMSXGM.BIN) replaced by SMU2000_ROMS_DIR (a folder, staging-only,
#     never FATAL — the ROMs are the user's and git-ignored).
#
# User-facing knobs (cache vars; env fallbacks documented per variable):
#   SMU2000_IPLUG2_DIR     - iPlug2 submodule root (default: ${CMAKE_SOURCE_DIR}/iPlug2)
#   SMU2000_VST2_SDK_DIR   - vstsdk2.4 root ($ENV{VST2_SDK_DIR}, then D:/opt/vst/vstsdk2.4,
#                            then the iPlug2/Dependencies/IPlug/VST2_SDK stub); aeffect.h must
#                            be found in <root> or <root>/pluginterfaces/vst2.x
#   SMU2000_VST2_PREFER_COMPAT - OFF (default). When ON (or when no SDK is found — the CI
#                            case), cmake/vst2_compat's clean-room pair is copied (renamed)
#                            into the untracked stub dir instead. Never commit SDK files.
#   SMU2000_CLAP_DIR       - dir containing CLAP_SDK + CLAP_HELPERS
#                            (default: iPlug2/Dependencies/IPlug)
#   SMU2000_VST3_SDK_DIR   - OPTIONAL vst3sdk checkout ($ENV{VST3_SDK_DIR}, then D:/opt/vst/vst3sdk)
#   SMU2000_DEPS_WIN_DIR   - prebuilt Skia/Freetype libs (default: iPlug2/Dependencies/Build/win)
#   SMU2000_ROMS_DIR       - repo roms/ tree staged next to built binaries (P2 wiring; never FATAL)
#
# Upstream iPlug2 cmake modules resolve SDK paths from IPLUG2_DIR-relative locations
# (Scripts/cmake/VST2.cmake, VST3.cmake, CLAP.cmake) and only set IPLUG2_*_SUPPORTED
# booleans when an SDK is absent (no hard failure). This module maps the SMU2000_* knobs
# onto those locations: the two VST2 headers are copied into the VST2_SDK stub when
# missing, and the optional VST3 junction is created when a local SDK is found.
# Untracked files/junctions only — the iPlug2 gitlink and tracked tree are never edited
# (the VST3_SDK stub README disappearing behind the junction is the same accepted
# sw10 behavior; populate-untracked-deps only).

# ---------------------------------------------------------------------------
# iPlug2 root
# ---------------------------------------------------------------------------
if(NOT DEFINED SMU2000_IPLUG2_DIR)
  set(SMU2000_IPLUG2_DIR "${CMAKE_SOURCE_DIR}/iPlug2" CACHE PATH "iPlug2 submodule root")
endif()
if(NOT EXISTS "${SMU2000_IPLUG2_DIR}/iPlug2.cmake")
  message(FATAL_ERROR
    "SMU2000_IPLUG2_DIR='${SMU2000_IPLUG2_DIR}' is not an iPlug2 tree (iPlug2.cmake missing).\n"
    "Fix: git submodule update --init iPlug2  (or pass -DSMU2000_IPLUG2_DIR=<path>).")
endif()
# Upstream user-facing knob; FindiPlug2.cmake and all Scripts/cmake modules derive from it.
set(IPLUG2_DIR "${SMU2000_IPLUG2_DIR}" CACHE PATH "iPlug2 root directory" FORCE)
set(SMU2000_IPLUG_DEPS_DIR "${IPLUG2_DIR}/Dependencies/IPlug")

# ---------------------------------------------------------------------------
# VST2 SDK (required for the VST2 target; proprietary, never committed)
# ---------------------------------------------------------------------------
set(SMU2000_VST2_SDK_DIR "" CACHE PATH
  "VST2 SDK (vstsdk2.4) root; must contain aeffect.h (root or pluginterfaces/vst2.x/). Fallbacks: $ENV{VST2_SDK_DIR}, D:/opt/vst/vstsdk2.4, then iPlug2/Dependencies/IPlug/VST2_SDK")
if(SMU2000_VST2_SDK_DIR STREQUAL "")
  if(DEFINED ENV{VST2_SDK_DIR} AND NOT "$ENV{VST2_SDK_DIR}" STREQUAL "")
    set(SMU2000_VST2_SDK_DIR "$ENV{VST2_SDK_DIR}")
  elseif(EXISTS "D:/opt/vst/vstsdk2.4/pluginterfaces/vst2.x/aeffect.h")
    # This-box convenience fallback (matches the VST3 D:/opt probe below); CI passes
    # SMU2000_VST2_SDK_DIR=$env{VST2_SDK_DIR} explicitly, where this path does not exist.
    set(SMU2000_VST2_SDK_DIR "D:/opt/vst/vstsdk2.4")
  else()
    set(SMU2000_VST2_SDK_DIR "${SMU2000_IPLUG_DEPS_DIR}/VST2_SDK")
  endif()
endif()
file(TO_CMAKE_PATH "${SMU2000_VST2_SDK_DIR}" SMU2000_VST2_SDK_DIR)

# Where the headers actually live (accept both layouts) — "aeffect.h" is the hard requirement.
set(_smu2000_vst2_hdr "")
foreach(_cand "${SMU2000_VST2_SDK_DIR}/pluginterfaces/vst2.x" "${SMU2000_VST2_SDK_DIR}")
  if(EXISTS "${_cand}/aeffect.h")
    set(_smu2000_vst2_hdr "${_cand}")
    break()
  endif()
endforeach()

set(SMU2000_VST2_STUB_DIR "${SMU2000_IPLUG_DEPS_DIR}/VST2_SDK")

# Clean-room VST2 ABI layer (see third_party/vst2/README.md). Committed under
# OUR names only; at configure time the two files are copied (renamed) into the
# UNTRACKED stub dir so stock IPlugVST2.cpp resolves its includes there. Used
# when no proprietary SDK is available (CI) — or forced via PREFER_COMPAT to
# validate the clean-room build on SDK-bearing machines.
option(SMU2000_VST2_PREFER_COMPAT
  "Ignore any found vstsdk2.4 and build against the committed clean-room VST2 ABI layer (cmake/vst2_compat)." OFF)
set(SMU2000_VST2_COMPAT_DIR "${CMAKE_CURRENT_LIST_DIR}/vst2_compat")
set(SMU2000_VST2_PROVIDER "")    # sdk | sdk-stub | compat (informational)

# Did a previous configure drop OUR clean-room pair into the stub?
set(_smu2000_stub_ae "${SMU2000_VST2_STUB_DIR}/aeffect.h")
set(_smu2000_stub_is_compat FALSE)
if(EXISTS "${_smu2000_stub_ae}")
  file(READ "${_smu2000_stub_ae}" _smu2000_stub_head LIMIT 2048)
  if(_smu2000_stub_head MATCHES "SMU2000_VST2_COMPAT_CORE_H")
    set(_smu2000_stub_is_compat TRUE)
  endif()
endif()

set(SMU2000_VST2_SUPPORTED TRUE)
if(SMU2000_VST2_PREFER_COMPAT)
  if(EXISTS "${SMU2000_VST2_COMPAT_DIR}/compat_aeffect_core.h" AND EXISTS "${SMU2000_VST2_COMPAT_DIR}/compat_aeffect_extended.h")
    file(MAKE_DIRECTORY "${SMU2000_VST2_STUB_DIR}")
    configure_file("${SMU2000_VST2_COMPAT_DIR}/compat_aeffect_core.h"    "${_smu2000_stub_ae}" COPYONLY)
    configure_file("${SMU2000_VST2_COMPAT_DIR}/compat_aeffect_extended.h" "${SMU2000_VST2_STUB_DIR}/aeffectx.h" COPYONLY)
    set(SMU2000_VST2_PROVIDER compat)
    message(STATUS "SMU2000: PREFER_COMPAT — clean-room VST2 ABI layer dropped into ${SMU2000_VST2_STUB_DIR} (any real SDK ignored)")
  else()
    set(SMU2000_VST2_SUPPORTED FALSE)
  endif()
elseif(_smu2000_vst2_hdr AND (NOT EXISTS "${_smu2000_stub_ae}" OR _smu2000_stub_is_compat))
  # Upstream VST2.cmake compiles against the submodule stub dir: drop the two
  # headers (the only ones IPlugVST2.cpp needs) there when missing — or refresh
  # them when a previous configure had placed our compat pair and a real SDK
  # became available. Untracked, gitignored.
  file(MAKE_DIRECTORY "${SMU2000_VST2_STUB_DIR}")
  foreach(_h aeffect.h aeffectx.h)
    if(EXISTS "${_smu2000_vst2_hdr}/${_h}")
      configure_file("${_smu2000_vst2_hdr}/${_h}" "${SMU2000_VST2_STUB_DIR}/${_h}" COPYONLY)
    endif()
  endforeach()
  set(SMU2000_VST2_PROVIDER sdk)
  message(STATUS "SMU2000: copied VST2 headers from ${_smu2000_vst2_hdr} to ${SMU2000_VST2_STUB_DIR}")
  if(NOT EXISTS "${SMU2000_VST2_STUB_DIR}/aeffect.h" OR NOT EXISTS "${SMU2000_VST2_STUB_DIR}/aeffectx.h")
    set(SMU2000_VST2_SUPPORTED FALSE)
  endif()
elseif(EXISTS "${_smu2000_stub_ae}" AND EXISTS "${SMU2000_VST2_STUB_DIR}/aeffectx.h")
  if(_smu2000_stub_is_compat)
    # Re-drop our pair so a configure always refreshes it from the committed source.
    configure_file("${SMU2000_VST2_COMPAT_DIR}/compat_aeffect_core.h"    "${_smu2000_stub_ae}" COPYONLY)
    configure_file("${SMU2000_VST2_COMPAT_DIR}/compat_aeffect_extended.h" "${SMU2000_VST2_STUB_DIR}/aeffectx.h" COPYONLY)
    set(SMU2000_VST2_PROVIDER compat)
    message(STATUS "SMU2000: clean-room VST2 ABI pair refreshed in ${SMU2000_VST2_STUB_DIR} (no vstsdk2.4 found)")
  else()
    set(SMU2000_VST2_PROVIDER sdk-stub)
    message(STATUS "SMU2000: VST2 headers already present in ${SMU2000_VST2_STUB_DIR}; SMU2000_VST2_SDK_DIR unused")
  endif()
elseif(EXISTS "${SMU2000_VST2_COMPAT_DIR}/compat_aeffect_core.h" AND EXISTS "${SMU2000_VST2_COMPAT_DIR}/compat_aeffect_extended.h")
  # No SDK anywhere: this is the CI/nightly path.
  file(MAKE_DIRECTORY "${SMU2000_VST2_STUB_DIR}")
  configure_file("${SMU2000_VST2_COMPAT_DIR}/compat_aeffect_core.h"    "${_smu2000_stub_ae}" COPYONLY)
  configure_file("${SMU2000_VST2_COMPAT_DIR}/compat_aeffect_extended.h" "${SMU2000_VST2_STUB_DIR}/aeffectx.h" COPYONLY)
  set(SMU2000_VST2_PROVIDER compat)
  message(STATUS "SMU2000: clean-room VST2 ABI layer -> ${SMU2000_VST2_STUB_DIR} (aeffect.h/aeffectx.h generated at build time; SDK never committed)")
else()
  set(SMU2000_VST2_SUPPORTED FALSE)
endif()
if(NOT SMU2000_VST2_SUPPORTED)
  if(SMU2000_BUILD_VST2)
    message(FATAL_ERROR
      "VST2 SDK headers (aeffect.h/aeffectx.h) not found: SMU2000_VST2_SDK_DIR='${SMU2000_VST2_SDK_DIR}' has no aeffect.h, ${SMU2000_VST2_STUB_DIR} is a bare stub, and the clean-room pair (${SMU2000_VST2_COMPAT_DIR}) is missing.\n"
      "Fix: copy aeffect.h + aeffectx.h from vstsdk2.4\\pluginterfaces\\vst2.x into\n"
      "  ${SMU2000_VST2_STUB_DIR}\n"
      "  (this machine: D:/opt/vst/vstsdk2.4), or pass -DSMU2000_VST2_SDK_DIR / set env VST2_SDK_DIR,\n"
      "  or restore cmake/vst2_compat/ (clean-room, used automatically on CI),\n"
      "  or configure with -DSMU2000_BUILD_VST2=OFF.")
  else()
    message(STATUS "SMU2000: VST2 SDK absent — VST2 target disabled (SMU2000_BUILD_VST2=OFF)")
  endif()
endif()

# ---------------------------------------------------------------------------
# CLAP SDK + helpers — NON-FATAL (P0/P2 must configure clean before CLAP is staged)
# ---------------------------------------------------------------------------
set(SMU2000_CLAP_DIR "${IPLUG2_DIR}/Dependencies/IPlug" CACHE PATH
  "Directory containing CLAP_SDK and CLAP_HELPERS (run iPlug2/Dependencies/download-clap-sdks.sh via Git-Bash)")
set(SMU2000_CLAP_SUPPORTED TRUE)
foreach(_d CLAP_SDK CLAP_HELPERS)
  if(NOT EXISTS "${SMU2000_CLAP_DIR}/${_d}/include")
    set(SMU2000_CLAP_SUPPORTED FALSE)
    message(STATUS
      "SMU2000: CLAP ${_d} missing (expected ${SMU2000_CLAP_DIR}/${_d}/include) — CLAP target will be skipped.\n"
      "  Fix: run iPlug2/Dependencies/download-clap-sdks.sh (Git-Bash, needs network),\n"
      "  or copy CLAP_SDK/CLAP_HELPERS into ${SMU2000_CLAP_DIR},\n"
      "  or pass -DSMU2000_CLAP_DIR=<dir>. (Not an error: the VST2 build proceeds without CLAP.)")
  endif()
endforeach()
if(SMU2000_CLAP_DIR STREQUAL "${SMU2000_IPLUG_DEPS_DIR}")
  # standard location — upstream CLAP.cmake finds it directly
else()
  if(SMU2000_CLAP_SUPPORTED)
    message(WARNING
      "SMU2000_CLAP_DIR='${SMU2000_CLAP_DIR}' differs from the iPlug2 standard location; upstream CLAP.cmake resolves from ${SMU2000_IPLUG_DEPS_DIR}. "
      "Point CLAP_SDK/CLAP_HELPERS there (junction or download script) if the CLAP target fails to configure.")
  endif()
endif()
if(SMU2000_CLAP_SUPPORTED)
  message(STATUS "SMU2000: CLAP SDK + helpers: ${SMU2000_CLAP_DIR}")
endif()

# ---------------------------------------------------------------------------
# VST3 SDK — OPTIONAL only (no VST3 target exists in P0-P3; never FATAL).
# If a local checkout is found and the iPlug2 standard junction dir is empty, create
# the junction upstream VST3.cmake would use (untracked; mirrors sw10, incl. the
# native-backslash-path fix for mklink). Otherwise just report unsupported.
# ---------------------------------------------------------------------------
set(SMU2000_VST3_SDK_DIR "" CACHE PATH
  "OPTIONAL vst3sdk root (no VST3 target is built; junctioned for convenience). Fallbacks: $ENV{VST3_SDK_DIR}, D:/opt/vst/vst3sdk")
set(SMU2000_VST3_LINK "${SMU2000_IPLUG_DEPS_DIR}/VST3_SDK")
if(SMU2000_VST3_SDK_DIR STREQUAL "")
  if(DEFINED ENV{VST3_SDK_DIR} AND NOT "$ENV{VST3_SDK_DIR}" STREQUAL "")
    set(SMU2000_VST3_SDK_DIR "$ENV{VST3_SDK_DIR}")
  elseif(EXISTS "D:/opt/vst/vst3sdk/public.sdk/source/main/dllmain.cpp")
    set(SMU2000_VST3_SDK_DIR "D:/opt/vst/vst3sdk")
  endif()
endif()
if(SMU2000_VST3_SDK_DIR STREQUAL "")
  set(SMU2000_VST3_SUPPORTED FALSE)
  message(STATUS "SMU2000: no local VST3 SDK ($ENV{VST3_SDK_DIR}/D:/opt/vst/vst3sdk absent) — VST3 stays unsupported (fine: no VST3 target is built)")
else()
  file(TO_CMAKE_PATH "${SMU2000_VST3_SDK_DIR}" SMU2000_VST3_SDK_DIR)
  if(EXISTS "${SMU2000_VST3_SDK_DIR}/public.sdk/source/main/dllmain.cpp")
    if(EXISTS "${SMU2000_VST3_LINK}/public.sdk/source/main/dllmain.cpp")
      set(SMU2000_VST3_SUPPORTED TRUE)
      message(STATUS "SMU2000: VST3 SDK present (optional) at ${SMU2000_VST3_SDK_DIR}")
    elseif(WIN32)
      # Junction dir empty/bare stub -> (re)create it. Native backslash paths are mandatory:
      # find_program returns slash-y cmd.exe paths that cmd rejects, and mklink parses
      # "/D:/..." as option switches (sw10 CI failures 2026-09-15 — kept verbatim).
      file(REMOVE_RECURSE "${SMU2000_VST3_LINK}")  # removes the bare stub README dir only
      find_program(CMD_EXE cmd.exe)
      file(TO_NATIVE_PATH "${CMD_EXE}" _smu2000_cmd_native)
      file(TO_NATIVE_PATH "${SMU2000_VST3_LINK}" _smu2000_link_native)
      file(TO_NATIVE_PATH "${SMU2000_VST3_SDK_DIR}" _smu2000_target_native)
      execute_process(COMMAND "${_smu2000_cmd_native}" /c mklink /J "${_smu2000_link_native}" "${_smu2000_target_native}"
                      RESULT_VARIABLE _smu2000_junction_rc
                      OUTPUT_VARIABLE _smu2000_junction_msg ERROR_VARIABLE _smu2000_junction_msg)
      if(EXISTS "${SMU2000_VST3_LINK}/public.sdk/source/main/dllmain.cpp")
        set(SMU2000_VST3_SUPPORTED TRUE)
        message(STATUS "SMU2000: junctioned ${SMU2000_VST3_LINK} -> ${SMU2000_VST3_SDK_DIR} (optional; unused in P0-P3)")
      else()
        string(STRIP "${_smu2000_junction_msg}" _smu2000_junction_msg)
        set(SMU2000_VST3_SUPPORTED FALSE)
        message(STATUS
          "SMU2000: optional VST3 junction failed (mklink /J rc=${_smu2000_junction_rc}: ${_smu2000_junction_msg}) — continuing; no VST3 target is built.\n"
          "  Manual fix if ever needed: cmd /c mklink /J \"${SMU2000_VST3_LINK}\" \"${SMU2000_VST3_SDK_DIR}\"")
      endif()
    else()
      set(SMU2000_VST3_SUPPORTED FALSE)
    endif()
  else()
    set(SMU2000_VST3_SUPPORTED FALSE)
    message(STATUS "SMU2000: SMU2000_VST3_SDK_DIR='${SMU2000_VST3_SDK_DIR}' incomplete (dllmain.cpp missing) — VST3 stays unsupported (fine: no VST3 target is built)")
  endif()
endif()

# ---------------------------------------------------------------------------
# Prebuilt graphics deps (Skia/Freetype) — backend stays NANOVG (both GUI states graphics-free),
# so a missing download tree is a WARNING here, never FATAL.
# ---------------------------------------------------------------------------
set(SMU2000_DEPS_WIN_DIR "${IPLUG2_DIR}/Dependencies/Build/win" CACHE PATH
  "Prebuilt Skia/Freetype lib root (<arch>/<Config>/*.lib from iPlug2/Dependencies/download-prebuilt-libs.sh)")
if(IGRAPHICS_BACKEND STREQUAL "SKIA")
  foreach(_arch x64 Win32)
    if(NOT EXISTS "${SMU2000_DEPS_WIN_DIR}/${_arch}/Release")
      message(WARNING
        "IGRAPHICS_BACKEND=SKIA needs prebuilt libs for ${_arch} at ${SMU2000_DEPS_WIN_DIR}/${_arch}/Release (missing).\n"
        "  Fix: run iPlug2/Dependencies/download-prebuilt-libs.sh win (Git-Bash, needs network),\n"
        "  or pass -DSMU2000_DEPS_WIN_DIR=<dir>, or use -DIGRAPHICS_BACKEND=NANOVG (default, no download needed).")
    endif()
  endforeach()
else()
  message(STATUS "SMU2000: NanoVG/GL2 backend — prebuilt Skia libs not required (${SMU2000_DEPS_WIN_DIR} may be empty)")
endif()

# ---------------------------------------------------------------------------
# ROMs (user-supplied, git-ignored; staging-only, consumed by the P2 post-build step)
# ---------------------------------------------------------------------------
set(SMU2000_ROMS_DIR "${CMAKE_SOURCE_DIR}/roms" CACHE PATH
  "Repo roms/ tree staged post-build next to each binary (SMU2000_COPY_ROMS). Never committed, never FATAL if absent.")
if(SMU2000_COPY_ROMS AND NOT EXISTS "${SMU2000_ROMS_DIR}")
  message(STATUS "SMU2000: SMU2000_ROMS_DIR='${SMU2000_ROMS_DIR}' not present — ROM staging will no-op (dump your MU2000 ROMs there; see README.md)")
else()
  message(STATUS "SMU2000: roms: ${SMU2000_ROMS_DIR} (staging ${SMU2000_COPY_ROMS})")
endif()

message(STATUS "SMU2000: SDK wiring OK — iPlug2=${IPLUG2_DIR} (vst2=${SMU2000_VST2_SUPPORTED}/${SMU2000_VST2_PROVIDER} clap=${SMU2000_CLAP_SUPPORTED} vst3-optional=${SMU2000_VST3_SUPPORTED})")
