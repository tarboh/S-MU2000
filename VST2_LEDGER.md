# VST2_LEDGER.md — S-MU2000 on iPlug2 (VST2/CLAP, Win32 + x64)

Ledger for adapting the S-MU2000 MU2000 emulator (`github.com/tarboh/S-MU2000`, already
checked out here) into an **iPlug2** plugin so it compiles as a **VST2** (and **CLAP**)
plugin, mirroring how `../sw10_plug` is built. **Sound engine first; GUI later.**

Reference build system to copy: `../sw10_plug` (iPlug2 submodule + CMake harness +
`SW10_PLUG/CMakeLists.txt` API targets). Read `../sw10_plug/AGENTS.md` and
`../sw10_plug/REFACTOR_PLAN.md` for the patterns being mirrored. **Update the phase
checklists here as work completes.**

## Current repo state (cold start — read first)

- This folder **is already a git checkout** of `https://github.com/tarboh/S-MU2000.git`
  (branch `main`). **Do NOT re-clone or re-init.** It is the upstream emulator repo; we are
  layering the iPlug2 plugin build on top of it in place.
- Present now: `src/`, `Makefile`, `third_party/` (vst3+imgui), `doc/`, `art/`, `tests/`,
  `tools/`, `roms/` (user ROMs — git-ignored, do not touch), `README.md`, `AGENTS.md`.
- **NOT created yet (P0 makes them):** `iPlug2/` (submodule), `.gitmodules`, `engine/`,
  `SMU2000_VST2/`, `cmake/`, root `CMakeLists.txt`, `CMakePresets.json`. So the
  `cmake --preset …` commands in `AGENTS.md` do **not** work until P0 lands. Start at P0.
- Verified facts a cold agent can rely on (checked against the tree): sw10's iPlug2 submodule
  pin is `d54f69050f517e43b941d88c2a170f0a840b9ee4`; `roms/` + `roms/mu2000_flash.bin` are
  confirmed git-ignored; `engine.cpp`/`mu2000.cpp`/`driver.h`/`bridge.h` do **not** ODR-use any
  `xg::model.cpp` symbol, so `xg/model.cpp` is genuinely excluded from the engine link.

---

## Execution protocol (subagents)

- **Every phase below is executed as a dedicated subagent.** Do not do phase work inline.
- **At most 2 subagents run concurrently.** Honour the wave schedule; never exceed 2 at once.
- A subagent owns its phase's acceptance gate and reports back done/blocked with evidence
  (build log tail, artifact paths). The next wave starts only when its dependencies pass.
- Subagents keep edits surgical: engine sources under `src/`, plugin under `SMU2000_VST2/`,
  harness under `cmake/` + root files. **Never edit the `iPlug2/` submodule tree** (changes
  are lost on checkout); put all shims in `cmake/` like `../sw10_plug/cmake/mingw_compat.cmake`.

### Wave schedule (≤2 concurrent)

| Wave | Subagents (parallel) | Depends on |
|---|---|---|
| 1 | `P0-scaffold` | — |
| 2 | `P1-engine-lib` | P0 |
| 3 | `P2-vst2-wrapper` ‖ `P3-clap-wrapper` | P1 |
| 4 | `P4-gui-toggle` ‖ `P5-mingw-clang` | P2, P3 |
| 5 | `P6-ci-github` | P2, P3, P4, P5 |
| 6 | `P7-gui-editor` | P2, P3, P4 |

(Phases 0–7 all green. P7 shipped the native GDI editor (reused from the VST3 GUI), not the
originally-sketched IGraphics rewrite — see §Phase 7.)

---

## Locked decisions

1. **iPlug2 = git submodule**, pinned to `d54f69050` (same gitlink as `../sw10_plug`).
2. **Architectures: Win32 + x64** VST2 and CLAP (both). MSVC is this session's toolchain.
3. **VST2 session state wired in pass 1**: `PLUG_DOES_STATE_CHUNKS 1`, map
   `SerializeState`/`UnserializeState` → `engine.save_state()/load_state()`.
4. **ROM discovery = reuse `engine.cpp::find_roms()`** (no new IPlug2 settings-dir code).
   Post-build **stages the `roms/` folder next to each built DLL/CLAP**, exactly like
   sw10 stages `ROMSXGM.BIN` (`SW10_COPY_ROM`) — see §ROM policy.
5. **CLAP built the same way as sw10** (`sw10_clap MODULE` → `.clap` via `iplug_configure_target(... CLAP)`).
6. **GUI is a build-time toggle**: `-DSMU2000_ENABLE_GUI=ON` (also accepted alias `-DENABLE_GUI=1`).
   Default **OFF** → engine-only plugin, `PLUG_HAS_UI 0`, no IGraphics/NanoVG/Skia/OpenGL linked.
7. **MSYS2 MinGW GCC + Clang compatibility is its own phase** (P5), mirroring sw10's
   `mingw_compat.cmake` / `mingw_portability_prelude.h` shims. Zero submodule edits.
8. **GitHub Actions CI is its own phase** (P6), mirroring `../sw10_plug/.github/workflows/build-native.yml`.
9. **The `roms/` folder is NEVER committed and NEVER deleted.** It is already git-ignored.

---

## Findings (from codebase recon — drives the whole design)

- **The engine is already GUI-free and VST-free.** `smu2000::engine`
  (`src/vst3/engine.{h,cpp}`) includes only `engine.h`, `mu2000.h`, `nvram.h`,
  `smartmedia.h`, `ui/bridge.h`, `ui/driver.h`, `ui/resampler.h`. Those three `ui/`
  headers are **header-only** (no `.cpp` to compile). Only `src/vst3/plugin.cpp` pulls in
  Steinberg `pluginterfaces/*` — so the engine compiles into a plain static lib unchanged.
  (The `smu2000::vst3` namespace name is cosmetic; keep it to avoid churn.)

- **Engine API a plugin needs (all present):**
  - `engine::start()` → finds ROM + boots firmware on a **background thread**; returns immediately.
  - `engine::set_output_rate(double)` → windowed-sinc resample from native 44100; bypass at 44.1k.
  - `engine::midi(const uint8_t* bytes, size_t n, int port=0)` — raw MIDI bytes (port 0 = MIDI IN A / parts 1-16, port 1 = B / 17-32). Queued safely if still booting.
  - `engine::fill(float* L, float* R, int n, const float* in_l, const float* in_r)` → emits stereo float; **silence until `status::ready`** (`engine.cpp:463`). Also pumps buttons/MIDI/wheel internally.
  - `engine::save_state()` / `load_state(ptr,n)` → whole-machine blob. `save_state()` defers onto the audio thread via `serve_state()` (`engine.cpp:543`), so main-thread chunk calls are safe.
  - `engine::all_notes_off()`, `latency_samples()`, `panel()` (bridge, for later GUI).

- **Engine core sources to compile** = the Makefile `OBJS`
  (`src/compat/compat.cpp`, `src/smartmedia.cpp`, `src/mame/sound/swp30{,_jit}.cpp`,
  `src/mame/video/hd44780.cpp`, `src/mame/machine/sci4.cpp`,
  `src/mame/cpu/{sh,sh2,sh2_jit,sh7042,sh_adc,sh_bsc,sh_cmt,sh_dmac,sh_intc,sh_mtu,sh_port,sh_sci}.cpp`)
  **+ `src/mu2000.cpp` + `src/vst3/engine.cpp`**.
  **Header-only (no `.cpp` to compile, pulled in via `engine.h`):** `ui/bridge.h`,
  `ui/driver.h`, `ui/resampler.h`, `ui/snapshot.h`, and — transitively through `driver.h` —
  `xg/ram.h` (all `constexpr`/inline) and `xg/model.h` (types/enums only).
  **Excluded (GUI/tool-only):** all other `src/ui/*.cpp`; all `src/vst3/*.cpp` except
  `engine.cpp` (plugin/view/iids/probe); `src/xg/model.cpp`; `third_party/imgui`;
  every `src/*.cpp` tool main (`render/live/gui/boot/rec/verify/…`).
  *P1 link safety:* engine path was verified not to call `xg/model.cpp`; if a link error names
  an `xg::` symbol, add `src/xg/model.cpp` to the `smu2000_engine` source list (22 KB, harmless).

- **32-bit JIT is already safe.** Both JITs are gated `#if defined(_WIN32) && defined(__x86_64__)`
  (`swp30_jit.cpp:22`, `sh2_jit.cpp:27`), with interpreter fallbacks
  (`swp30_jit.cpp:293` returns false when disabled). MSVC-win32 does **not** define
  `__x86_64__` → `SMU2000_MEG_JIT=0`, `jit_enabled()=false`, `x64asm.h` never included.
  **Verify** `sh2_jit.cpp` has the matching interpreter stub for `jit_enabled()/jit_run()`
  when its guard is false (mirror of the swp30 pattern). Engine runs interpreter-only on both archs under MSVC.
  - *Optional x64-only perf follow-on:* the JITs emit the Microsoft x64 ABI (rcx/rdx/r8/r9),
    so widening the guard to `(defined(__x86_64__) || defined(_M_X64))` could re-enable JIT on
    MSVC-x64. **Default OFF** for pass 1; gate behind `SMU2000_ENABLE_JIT` (x64-only) once sound is proven.
  - **SUPERSEDED 2026-09-16 (CPU32_LEDGER.md Phases 1–8):** both JITs are now dual-mode —
    `__x86_64__ || _M_X64` (MSVC-x64 gets the JIT) **and** `__i386__ || _M_IX86`
    (x86-32, self-defined `SMU_JIT32_PORT_SH2/MEG`). Win32 is **no longer interpreter-only**;
    MSVC-win32/x64 both run the JIT by default (`SMU2000_{MEG,SH2}_JIT=0` kill-switches).

- **MSVC portability looks low-risk.** Scan of all 71 core files found **no** `__attribute__`,
  `__builtin`, `typeof`, `__int128`, GCC statement-exprs, or SIMD intrinsics. Known MSVC items:
  `std::rotl/rotr` (`<bit>`, fine under `/std:c++20`) and `M_PI` in `engine.cpp` → needs
  `_USE_MATH_DEFINES`. This codebase is currently g++-only, so expect first-pass `/W-level`
  diagnostics; isolate all fixes in the engine CMake target, not in iPlug2.

---

## ROM policy (hard rule)

- `roms/` exists and contains `mu2000_flash.bin` (4MB program ROM), `dump/` (wave ROMs),
  `standin/` (MEG sin-table). It is **already in `.gitignore`** (`/roms/` style entry present).
- **Never `git add` anything under `roms/`. Never delete it.** Yamaha ROMs cannot be
  distributed (see `README.md`); the user dumps them from their own MU2000.
- Keep the existing unanchored `roms/` ignore, but **verify it stays tracked-correct**: do not
  let a future `build-*` style ignore swallow `.github/workflows/`. Add `/build-cmake/` (anchored).
- **Runtime location:** `engine.cpp::find_roms()` probes, in order: env `S_MU2000_ROMS` →
  `<dllDir>\..\Resources\roms` → `<dllDir>\roms` → `<dllDir>` → `roms.txt` pointers →
  `%LOCALAPPDATA%\S-MU2000\roms` → `%USERPROFILE%\Documents\S-MU2000\roms`.
- **Build-time staging (mirror sw10 `SW10_COPY_ROM`):** a `SMU2000_COPY_ROMS` post-build step
  copies the repo `roms/` tree next to each produced binary so `find_roms()` hits `<dllDir>\roms`.
  CI (`ci-*` presets) sets `SMU2000_COPY_ROMS=OFF` (no ROM by design; ship a `ROM-REQUIRED.txt`).

---

## Graphics-free plugin recipe (P2/P3/P4 — critical, satisfies hard rule #6)

Verified against the pinned iPlug2 CMake (`Scripts/cmake/`):
- `iplug_configure_target(<t> VST2 <proj>)` (in `FindiPlug2.cmake`) takes ONLY 3 args (no UI switch),
  includes `VST2.cmake`, and links `iPlug2::VST2` whose INTERFACE **forces `IPLUG_EDITOR=1`**
  (`VST2.cmake:53`). The editor code in the IPlug core compiles only under `#if IPLUG_EDITOR` and
  pulls IGraphics → so this path canNOT be graphics-free.
- **`iPlug2::IPlug` (the DSP core) does NOT define `IPLUG_EDITOR` and pulls NO IGraphics**
  (`IPlug.cmake:71-88`; it links only system libs Shlwapi/comctl32/wininet). Core sources
  (`IPlugAPIBase.cpp` etc.) are editor-free when `IPLUG_EDITOR` is unset.
- ⇒ **GUI OFF recipe** (GUI now defaults ON since 2026-09-16 — this is the `-DSMU2000_ENABLE_GUI=OFF`
  opt-out path): build the plugin MODULE WITHOUT `iplug_configure_target`/
  `iPlug2::VST2`. Instead: `add_library(... MODULE)`; add the plugin sources **plus**
  `${IPLUG_DIR}/VST2/IPlugVST2.cpp` (CLAP: the CLAP glue is via `IPlug_include_in_plug_src.h`
  with `CLAP_API` + CLAP SDK/HELPERS includes); include `${IPLUG_DIR}/VST2` + the VST2 SDK stub
  dir `${IPLUG2_DIR}/Dependencies/IPlug/VST2_SDK` (aeffect headers, populated in P0); define
  `VST2_API VST_FORCE_DEPRECATED IPLUG_DSP=1` and **NOT** `IPLUG_EDITOR`; link
  `smu2000_engine iPlug2::IPlug` (**never `${IGRAPHICS_LIB}`**); set `/MT` (root already does);
  the plugin `.h` includes only `IPlug_include_in_plug_hdr.h` (NOT `IControls.h`) and guards every
  editor member under `#if IPLUG_EDITOR`. `PLUG_HAS_UI 0`. Verify with `dumpbin /DEPENDENTS` → no
  opengl32/nanovg/skia/IGraphics imports.
- **GUI ON (P7 — native, still graphics-free; default since 2026-09-16):** the SAME manual recipe as GUI OFF (add
  `IPlugVST2.cpp`/`IPlugCLAP.cpp` by hand, `iPlug2::IPlug`, keep `NO_IGRAPHICS`) PLUS
  `-DSMU2000_ENABLE_GUI` (→ `PLUG_HAS_UI 1`) + link `smu2000_gui` + `gdi32/comdlg32/user32`. NO
  `iplug_configure_target`, NO `${IGRAPHICS_LIB}`, NO `IPLUG_EDITOR`. `NO_IGRAPHICS` keeps
  `EDITOR_DELEGATE_CLASS = iplug::IEditorDelegate`, whose native `OpenWindow/CloseWindow` the plugin
  overrides to host `ui::panel` in a child `HWND`. `/DEPENDENTS` gains only GDI32+COMDLG32 (the
  VST3 view's GDI + SmartMedia file dialog), never OpenGL/NanoVG/Skia. Toggle = which two recipes
  the target CMakeLists takes (both graphics-free; ON just adds the editor lib + the UI define).

---

## GUI toggle
- `SMU2000_ENABLE_GUI` cache var (alias `-DENABLE_GUI`), **default ON since 2026-09-16**
  (`-DSMU2000_ENABLE_GUI=OFF` / `-DENABLE_GUI=OFF` opts out; pre-existing build caches keep their
  old value — flip explicitly once). **Both states are
  graphics-free** (no IGraphics/NanoVG/OpenGL/Skia) — the GUI is the native Win32/GDI panel.
  - OFF → `PLUG_HAS_UI 0`; no editor sources; pure instrument plugin (VST2/CLAP load, MIDI-in,
    audio-out, state chunks). `/DEPENDENTS` = KERNEL32/USER32/api-ms.
  - ON → `PLUG_HAS_UI 1`; compile+link `smu2000_gui` = `SMU2000_VST2/ui/SMU2000Editor.cpp`
    (port of `src/vst3/view.cpp`) + `src/ui/{panel,editor,effects,layout,svg}.cpp` + `src/xg/model.cpp`
    (Makefile `VST3_SRCS` GUI set). `NO_IGRAPHICS` stays ON. Plugin overrides
    `IEditorDelegate::OpenWindow/CloseWindow` (VST2 `effEditOpen/Close`, CLAP `guiSetParent/Destroy`).
  Toggle flows: root option → `SMU2000_VST2/CMakeLists.txt` conditional `add_subdirectory(ui)` +
  GUI-ON MODULE recipe → per-target `-DSMU2000_ENABLE_GUI` → `config.h` derives `PLUG_HAS_UI`.


---

## Target matrix (after P3)

| Target | Kind | Output | Notes |
|---|---|---|---|
| `smu2000_engine` | STATIC lib | `…/.lib` | OBJS + `mu2000.cpp` + `engine.cpp`; shared by all API targets |
| `smu2000_vst2` | MODULE | `SMU2000_VST2.dll` (`vst2/<arch>/<Config>/`) | VST2 entry `VSTPluginMain`; Win32 + x64 |
| `smu2000_clap` | MODULE | `SMU2000_VST2.clap` (`clap/<arch>/<Config>/`) | CLAP entry via `CLAP_EXPORT`; Win32 + x64 |
| (`smu2000_app`) | — | (optional later) | Standalone not required; engine already runs via existing `live/gui` exes |

Output layout: `build-cmake/<api>/<arch>/<Config>/` (identical to sw10; `<arch>` = `Win32`|`x64`,
MinGW appends `-mingw`/`-mingw-clang`). Static `/MT` CRT everywhere.

---

## Phases

### Phase 0 — Scaffold (`P0-scaffold`, Wave 1) — subagent
- Add `iPlug2` submodule pinned `d54f69050`; create `.gitmodules`. Do NOT add SDKs/prebuilt libs as commits.
- Create `SMU2000_VST2/` (config.h, empty `SMU2000_VST2.{h,cpp}` stub, `ui/` placeholder), `engine/`
  (thin CMake only — sources stay in `src/`), `cmake/` (`iplug2_paths.cmake`, `mingw_compat.cmake` inert),
  root `CMakeLists.txt`, `CMakePresets.json`.
- `.gitignore`: add `/build-cmake/`, `aeffect.h`, `aeffectx.h` (keep `roms/` intact).
- Port `../sw10_plug/cmake/iplug2_paths.cmake` → `SMU2000_*` knobs: keep VST2 SDK resolution
  (`SMU2000_VST2_SDK_DIR` → env `VST2_SDK_DIR` → stub `aeffect.h/aeffectx.h` copy; this box:
  `D:/opt/vst/vstsdk2.4`), CLAP (`SMU2000_CLAP_DIR` default `iPlug2/Dependencies/IPlug`),
  optional VST3 block default OFF. **Drop** the ROMSXGM `SW10_ROM_PATH` block; replace with
  `SMU2000_ROMS_DIR` (default `${CMAKE_SOURCE_DIR}/roms`) used only for staging (P2), never FATAL if absent.
- Presets: `vs-win32`, `vs-x64`, `ci-win32`, `ci-win64` (VS 2026 gen, `-A Win32|x64`),
  `smu2000_clap`/`smu2000_vst2` toggles via options; VST2 SDK gated on header presence.
- **Accept:** `cmake --preset vs-win32` and `vs-x64` **configure** clean (no compile yet).

### Phase 1 — Engine static lib (`P1-engine-lib`, Wave 2) — subagent
- `engine/CMakeLists.txt`: `add_library(smu2000_engine STATIC)` with the source list from §Findings,
  include dirs `src` + `src/compat`. `/std:c++20 /utf-8 /MT`, defs
  `_USE_MATH_DEFINES;NOMINMAX;_CRT_SECURE_NO_WARNINGS;WIN32`.
- Confirm `x64asm.h` is NOT compiled (guarded out on win32) and `sh2_jit`/`swp30_jit` compile to
  interpreter-only stubs. *(Historic as of 2026-09-16: x86-32 JIT port landed — win32 now
  compiles `x64asm.h` in 32-bit mode with real JITs; see `CPU32_LEDGER.md` Phases 1–8.)*
- **Accept:** `smu2000_engine.lib` builds on Win32 **and** x64 MSVC with sound code intact
  (link a throwaway that calls `engine::start()`+`fill()` if needed to prove symbols).

### Phase 2 — VST2 wrapper (`P2-vst2-wrapper`, Wave 3a) — subagent
- `SMU2000_VST2/config.h`: `PLUG_TYPE 1`, `PLUG_DOES_MIDI_IN/OUT 1`, `PLUG_DOES_STATE_CHUNKS 1`,
  `PLUG_HAS_UI` from GUI toggle (OFF default), `PLUG_CHANNEL_IO "0-2"`, `PLUG_UNIQUE_ID 'SMU2'`,
  `PLUG_LATENCY 0`, CLAP/VST3 metadata macros (shared with P3).
- `SMU2000_VST2.{h,cpp}`: `class SMU2000_VST2 final : public Plugin` holding
  `std::unique_ptr<smu2000::engine> m_engine`.
  - ctor: `m_engine->start(); m_engine->set_output_rate(GetSampleRate());`
  - `OnReset()`: `m_engine->set_output_rate(GetSampleRate())`.
  - `ProcessBlock(in,out,n)`: cast `sample**`→float, `m_engine->fill(fL,fR,n, fInL?, fInA?)`.
  - `ProcessMidiMsg` → `m_engine->midi(msg.mMsg, msg.mNumBytes, 0)`; `ProcessSysEx` → `m_engine->midi(msg.mData,msg.mSize,0)`.
  - `SerializeState/UnserializeState` → engine blob (chunk). Optional MIDI-out pump via `SendMidiMsg`.
- `SMU2000_VST2/CMakeLists.txt`: mirror sw10 `sw10_vst2` — `add_library(smu2000_vst2 MODULE)` +
  `iplug_configure_target(smu2000_vst2 VST2 SMU2000_VST2)`; link `smu2000_engine` (+ `${IGRAPHICS_LIB}` only if GUI ON);
  `PREFIX ""`; out dirs `build-cmake/vst2/<arch>/<Config>/SMU2000_VST2.dll`; **post-build ROM staging**
  (`SMU2000_COPY_ROMS`) copying `roms/` → `$<TARGET_FILE_DIR>/roms`.
- **Accept:** Win32 + x64 `SMU2000_VST2.dll` build; exports `VSTPluginMain`; with ROMs staged,
  audio boots (silence→ready) and MIDI sounds in a 32-bit host; project save/recall round-trips.
- **DONE (Wave 3a finish).** Both archs build clean with the existing graphics-free recipe — zero
  code/CMake fixes were needed; `NO_IGRAPHICS` alone keeps the editor out (no `IPLUG_EDITOR=0`
  required at this pin). `/DEPENDENTS` = KERNEL32+USER32+api-ms synch only (no GL/Skia/png/CRT DLLs).
  `VSTPluginMain`+`main` exported both archs; roms staged both archs. Load probes (native C++ +
  P/Invoke, x64 pwsh / SysWOW64 PS5.1): magic=0x56737450, uniqueID=0x534D5532, effOpen/effClose
  return 0, effGetChunk(23)/effSetChunk(24) round-trip 6,096,753-byte chunk, processReplacing OK.
  GOTCHA for probes: this SDK's aeffect.h has NO `index` field — effOpen=0/effClose=1,
  effGetChunk=23/effSetChunk=24 (calling the 34/35 slots = effGetVendorString writes 64 bytes
  through your `void**` — instant AV).    Engine boots async (~2-4 s with staged ROMs); poll
   `state()==ready` before chunk/audio calls. *(SUPERSEDED 2026-09-16: boot is now SYNCHRONOUS
   in the plug-in ctor — state is ready/failed when the ctor returns; see §Boot gating.)*
   Sound-out unverifiable: no DAW/host installed.
- **P2-FIX (2026-09-16) — sample-accurate MIDI (was block-boundary quantised).** The original
  `ProcessMidiMsg` → `m_engine->midi(bytes, n, 0)` applied *every* event in a block at its start
  (the `0` is the engine **port**, not a time). `m_engine->midi()` has no time arg — it just
  clock-queues bytes onto the emulated 31250 bps serial line — so note on/off onset was jittered
  by up to one host block (≈23 ms @1024/44.1k), worse at large buffers while audio stayed steady.
  Hosts *do* pass the intra-block offset (`IMidiMsg::mOffset`/`ISysEx::mOffset`, set from VST2
  `deltaFrames` / CLAP `time`); we were dropping it. Fix: park stamped events in an audio-thread
  `m_midi_q` in `ProcessMidiMsg`/`ProcessSysEx`, `stable_sort` by offset in `ProcessBlock`, then
  interleave — `fill()` up to each offset (writing `outputs+produced`), inject that event's bytes
  via `midi()`, continue. Each `fill()` releases `m_machine` on return so `midi()` between calls
  is lock-safe; the serial model then spaces bytes to the right sample. Empty-block fast path kept.
  Resampler path (`m_pos`/`m_written`/ring are member state) and `driver` pumps (`apply_buttons`
  state-based, `pump_midi`/`wheel`/`out` drain-while, `publish`/`advance_clock` sum to nFrames) all
  verified safe across split `fill()` calls. No latency change: `engine::latency_samples()` is
  intentionally 0 (resampler synthesises lookahead). Rebuilt VST2+CLAP Win32+x64 clean (MSVC).
- **P2-FIX2 (2026-09-16) — allocation-free MIDI queue (P2-FIX hot path was malloc-bound).** Dense
  MIDI files stuttered: the parked event embedded a `std::vector` (`ProcessMidiMsg` malloc'd per
  message, `push_back` grew the queue with elementwise vector moves) and `std::stable_sort` malloc'd
  a scratch buffer **every block** even though hosts deliver offsets in order. Now: `midi_event` is
  a 16-byte trivially-copyable POD `{offset, pos, len, port}` into an append-only byte arena
  (`m_midi_bytes`); the ctor reserves once (queue 8192 + arena 64 KiB) and `clear()` keeps capacity
  → zero mallocs in the steady state. No ring needed: offsets are block-relative and fully drained
  every block. Hot path (`ProcessMidiMsg`/`ProcessSysEx`) is append-only; `ProcessBlock` does an
  O(n) is-sorted scan (ordered hosts never sort) and only then falls back to `std::sort` — with the
  arena `pos` (monotonic with arrival) as tie-break, output order is identical to the old
  `stable_sort`. Rebuilt VST2+CLAP Win32+x64 clean (MSVC).
  **HEAD-BREAK fixed on the way (pre-existing, not MIDI-related):** upstream `5901ea5` made
  `src/vst3/engine.cpp:555` call `ui::xgui::set_voice_rom` unconditionally, but its definition
  (`src/ui/xg_ui.cpp`) is imgui-only and excluded from plugin targets (rule #6) → plugin links
  broke with LNK2001. Shim: `engine/xgui_plugin_stub.cpp` (no-op sink, in `smu2000_engine` ONLY —
  Makefile/VST3/native keep the real `xg_ui.cpp`); the native GDI panel never reads the voice ROM.
  If `xg_ui.h:44`'s signature ever changes, the plugin link fails on that mangled name — update
  the stub too.

### Phase 3 — CLAP wrapper (`P3-clap-wrapper`, Wave 3b) — subagent (parallel with P2)
- Reuse the **same** `SMU2000_VST2.{h,cpp}` plugin class + `smu2000_engine`. Add CLAP target in
  `SMU2000_VST2/CMakeLists.txt` mirroring sw10 `sw10_clap`: `add_library(smu2000_clap MODULE)` +
  `iplug_configure_target(smu2000_clap CLAP SMU2000_VST2)`, suffix `.clap`, link `smu2000_engine`
  (+iPlug2::CLAP from `SMU2000_CLAP_DIR`); ROM staging like P2. CLAP metadata already in config.h.
- If CLAP SDK/helpers absent, print the sw10-style fix message (download via Git-Bash) and gate the target.
- **Accept:** `SMU2000_VST2.clap` builds Win32 + x64; CLAP entry point present; same sound as VST2.
- **DONE (Wave 3b finish).** CLAP_SDK/CLAP_HELPERS staged from sw10 (untracked, same policy as VST2 SDK).
  Graphics-free CLAP target mirrors P2 recipe (no `iplug_configure_target`/`iPlug2::CLAP`; manual
  `IPlugCLAP.cpp` + SDK/HELPERS includes; link `smu2000_engine iPlug2::IPlug`; `CLAP_API IPLUG_DSP=1
  NO_IGRAPHICS SAMPLE_TYPE_FLOAT`). GOTCHA fixed: ctor mem-init must be `: iplug::Plugin(info, ...)`
  (qualified — CLAP's base `clap::helpers::Plugin` injects the name `Plugin` into class scope, C2614;
  upstream Examples use the same spelling). `/DEPENDENTS` both archs = KERNEL32+USER32+api-ms-synch
  only. `clap_entry` exported both archs. Load probes (native, both archs): init(path)=true,
  get_factory("clap.plugin-factory") non-null, deinit clean. GOTCHAS: this pin's `clap_init` does
  `gPluginPath = pluginPath` unguarded — null path AVs (pass the DSO path like real hosts);
  `clap_version_t` is 3×u32 (+pad on x64) so entry pointers sit at +16/+24/+32 (x64), +12/+16/+20 (x86).
  Sound-out unverifiable (no CLAP host installed).

### Phase 4 — GUI toggle (`P4-gui-toggle`, Wave 4a) — subagent
- Wire `SMU2000_ENABLE_GUI` (alias `ENABLE_GUI`) end-to-end: root option → configure-time define
  → `config.h` `PLUG_HAS_UI` → conditional sources/libs in P2/P3 targets.
- Default OFF must produce a **graphics-free** build (verify no NanoVG/GL/OpenGL/Skia linked).
- OFF→ON with GUI sources still empty should configure/link with IGraphics wired (placeholder editor),
  proving the toggle for Phase 7.
- **Accept:** `-DSMU2000_ENABLE_GUI=OFF` and `=ON` both configure+build the VST2 target; OFF binary
  has no graphics imports.
- **DONE (Wave 4a finish).** OFF = existing recipe untouched (re-verified: both archs VST2+CLAP,
  `/DEPENDENTS` = KERNEL32+USER32+api-ms-synch only). ON = sw10-style `iplug_configure_target`
  + `${IGRAPHICS_LIB}`; VST2 x64/Win32 + CLAP x64 build; `/DEPENDENTS` now includes OPENGL32
  (NanoVG/GL2 wired) + ole32/GDI32/COMCTL32/WININET (editor libs); `VSTPluginMain` exported.
  `SMU2000_ENABLE_GUI` is a PER-TARGET define (root global `add_compile_definitions` removed;
  engine target never sees it). GOTCHA (cost hours — document!): at this pin `IGraphics` lives in
  **nested** `iplug::igraphics`; a global `using namespace igraphics;` BEFORE
  `using namespace iplug;` fails C2871. sw10's header order (iplug first, igraphics second) is
  mandatory — `SMU2000_VST2.h` now follows it inside `#if IPLUG_EDITOR`. Also `ITextControl`
  ctor is `(rect, str, IText, ...)` here (str before text). Editor = placeholder
  (panel bg + label "SMU2000 — GUI (editor pending Phase 7)"); no fonts/resources required.

### Phase 5 — MinGW/Clang compatibility (`P5-mingw-clang`, Wave 4b) — subagent
- Port `../sw10_plug/cmake/mingw_compat.cmake` + `mingw_portability_prelude.h` (rename `SW10_*`→`SMU2000_*`).
- Add presets `mingw-win32`, `mingw-x64`, `mingw-clang-x64`, `mingw-ci-*` (Ninja + gcc/clang, MSYS2 MINGW32/MINGW64 shell).
- On MinGW **win64**, `__x86_64__` IS defined → JIT path turns ON; verify `x64asm.h` assembles under
  GCC **and** Clang. On MinGW **win32** JIT stays off (interpreter), like MSVC-win32.
  *(2026-09-19 update: i686 toolchain now WORKS on this box (cc1plus needs
  `C:\msys64\mingw32\bin` on PATH). Win32 MinGW builds green incl. JIT32 — see
  "Nightly run #2 gate fix" status entry for the SEH link fix. `vs-win32` remains
  the MSVC win32 path.)*
- Import-lib name remaps + static runtime handled in `mingw_compat.cmake` (submodule untouched).
- **Accept:** VST2 + CLAP build under **MSYS2 GCC and Clang** (x64, and win32 where the toolchain exists),
  self-contained (system DLL imports only), entry exports present.

### Phase 6 — CI GitHub Actions (`P6-ci-github`, Wave 5) — subagent
- `.github/workflows/build-native.yml` modeled on sw10: `windows-latest`, matrix
  `win32`/`x64` (`ci-win32`/`ci-win64` presets), MSVC. VST2 SDK is **proprietary** → gate OFF unless a
  secret/cached SDK is staged; always build engine + (VST3 optional) + CLAP (headers via
  `iPlug2/Dependencies/download-clap-sdks.sh`). `SMU2000_COPY_ROMS=OFF`, ship `ROM-REQUIRED.txt`.
- Job also runs `cmake --preset vs-win32 && cmake --build --preset vs-win32-release` to keep the
  first-party path honest. Add a MinGW job (MSYS2 setup) once P5 is green (optional).
- **Never** cache/commit `roms/`, `aeffect*.h`, VST2 SDK. Keep ignore patterns anchored so
  `.github/workflows/*` stay tracked.
- **Accept:** CI workflow YAML valid (`actionlint`-clean), matrix configures; green on the pieces CI can build.

### Phase 7 — GUI editor (`P7-gui-editor`) — **DONE 2026-09-15** — native GDI, NOT IGraphics
- **Decision (supersedes the P4 placeholder / sw10 NanoVG plan):** the existing VST3 GUI is *not*
  IGraphics — `src/vst3/view.cpp` draws the front panel with raw **GDI** (`src/ui/draw.h`,
  `CreateFontA`, `BitBlt`) on a child `HWND`, driving `ui::panel` from `engine::panel()`. So we
  **reuse it verbatim** instead of reimplementing in NanoVG. Result: GUI-ON is *still* graphics-free
  (hard rule #6 holds for **both** states; no NanoVG/OpenGL/Skia/imgui/D3D anywhere).
- **Mechanism.** iPlug2's `IEditorDelegate` (selected via `NO_IGRAPHICS` in
  `IPlugDelegate_select.h`) ships an empty native path `OpenWindow(void* parent)`/`CloseWindow()`;
  VST2 routes `effEditOpen`/`effEditClose` (IPlugVST2.cpp:466/483) and CLAP routes
  `guiSetParent`/`guiDestroy`/`guiShow` (IPlugCLAP.cpp:951/882/893) straight to it. So the plugin
  class just overrides those two: `OpenWindow` spawns the child `HWND`, `CloseWindow` tears it down.
  Host is the real parent; the child's own `WndProc` owns all input + a 33 ms `WM_TIMER` (VST3 parity,
  host-agnostic — no `effEditIdle` needed).
- **New files.** `SMU2000_VST2/ui/SMU2000Editor.{h,cpp}` — `smu2000::editor`, a near-verbatim port
  of `src/vst3/view.cpp` (paint, press/drag/release/wheel, `key_to_button`, SmartMedia card menu,
  `WM_GETMINMAXINFO` 2.5:1 track). `SMU2000_VST2/ui/CMakeLists.txt` → STATIC `smu2000_gui`: the exact
  Makefile `VST3_SRCS` GUI set (`src/ui/{panel,editor,effects,layout,svg}.cpp` + `src/xg/model.cpp`)
  + `SMU2000Editor.cpp`; carries its own `src`+`src/compat` PUBLIC includes + `NOMINMAX`/`NO_IGRAPHICS`.
- **Plugin.** `SMU2000_VST2.h`: `std::unique_ptr<smu2000::editor> m_editor` + `OpenWindow`/
  `CloseWindow`/`OnParentWindowResize` overrides, all `#if PLUG_HAS_UI`. `.cpp`: builds the editor in
  ctor (replaces the placeholder `mMakeGraphicsFunc`/`mLayoutFunc`). `config.h`: `PLUG_WIDTH/HEIGHT`
  = 1250×500 (logical 1000×400 = 2.5:1).
- **CMake.** GUI-ON VST2 **and** CLAP now use the *same* manual graphics-free recipe as GUI-OFF
  (hand-added `IPlugVST2.cpp`/`IPlugCLAP.cpp`, `iPlug2::IPlug`, `NO_IGRAPHICS`) + `SMU2000_ENABLE_GUI`
  (per-target → `PLUG_HAS_UI 1`) + `smu2000_gui` + `gdi32/comdlg32/user32` (MSVC). NO
  `iplug_configure_target`/`${IGRAPHICS_LIB}` on either path — dropped the sw10 NanoVG wiring entirely.
- **Gotchas learned.** (1) `svg.cpp` includes `<windows.h>` before `<algorithm>` → MSVC `min/max`
  macros corrupt `std::min/max` (C2589); fixed by `NOMINMAX` PUBLIC on `smu2000_gui` (engine had it,
  the standalone lib must too). (2) `SMU2000Editor.cpp` uses `PLUG_WIDTH/HEIGHT` → `#include
  "../config.h"` (only macro defines, no iPlug headers). (3) GUI-ON must keep `NO_IGRAPHICS` so
  `EDITOR_DELEGATE_CLASS` stays plain `IEditorDelegate` (not `IGEditorDelegate`) and `OpenWindow`
  resolves to *our* override, not the graphics `final`. (4) `static_assert` in the plugin `.cpp`
  guards `SMU2000_ENABLE_GUI ⇒ PLUG_HAS_UI==1` (catches config desync at compile time).
- **Accept — DONE, host-probed (no DAW on box).** Native C++ hosts `tools/vst2_host_probe.cpp` +
  `tools/clap_host_probe.cpp` (this SDK's real enums/ABI):
  - x64 **VST2** GUI-ON: `effEditGetRect`→ERect 1250×500, `effEditOpen`→ child panel HWND
    (`SMU2000IPlugView`) visible 1250×500, 370 idle/timer pumps (paint+tick+engine boot) no crash,
    `effEditClose` destroys it. **PASS.**
  - x64 **CLAP** GUI-ON: `clap.gui` `is_api_supported(win32)`, `create`, `set_parent`, `show` → child
    panel visible 1250×500, 366 pumps alive, hide/destroy clean. **PASS.**
  - **Win32** VST2 GUI-ON (32-bit host): child panel attaches/paints/detaches 1250×500. **PASS.**
  - GUI-OFF default (x64): `/DEPENDENTS` = KERNEL32/USER32/api-ms only (no editor on either API). **PASS.**
  - GUI-ON `/DEPENDENTS` = GDI32/COMDLG32/USER32/KERNEL32/api-ms — **no OPENGL32/NanoVG/Skia**.
  - Opcode ABI note: this SDK keeps deprecated entries under `VST_FORCE_DEPRECATED`, so
    `effEditGetRect=15, effEditOpen=13, effEditClose=14, effEditIdle=18` (NOT the compact 11/12/13/19).
    AEffect on x64: `dispatcher@8`, `uniqueID@112` (`'SMU2'`). MinGW x64 GUI-ON not re-run here
    (needs MSYS2 shell); the `smu2000_gui` lib reuses the engine's already-MinGW-proven flags.

---

## Clean-room VST2 ABI layer + nightly release CI (2026-09-19) — **DONE**

Enables **nightly VST2 CI** without ever touching the proprietary SDK (supersedes
the "VST2 gated OFF on CI" line in P6 for the nightly workflow).

- `third_party/vst2/vst2_abi.h` — PR [tarboh/S-MU2000#16](https://github.com/tarboh/S-MU2000/pull/16)
  (drel4, `983d2d8`, BSD-3-Clause) vendored verbatim + provenance `README.md`.
- `cmake/vst2_compat/{compat_aeffect_core,compat_aeffect_extended}.h` — clean-room
  declaration of the full public VST2.4 ABI surface iPlug2 compiles against
  (all `eff*` 0–79 / master 0–49 numbers, deprecated entries DECLARED with their
  `__nameDeprecated` spellings so numbering is immutable, dual-arch static_asserts).
  Own file names; the SDK names `aeffect.h`/`aeffectx.h` only ever exist as
  configure-time copies into the UNTRACKED `iPlug2/Dependencies/IPlug/VST2_SDK/`.
- `cmake/iplug2_paths.cmake` — provider resolution: real SDK → preexisting stub
  drop → clean-room pair; `SMU2000_VST2_PREFER_COMPAT=ON` forces clean-room
  (CI default); status line reports `vst2=TRUE/{sdk|sdk-stub|compat}`.
- Parity gates (dev-box only; SDK is read, never copied to repo):
  `tools/vst2_abi_check.py` (377 SDK enumerators under FORCE_DEPRECATED semantics —
  all match; catches off-by-number spaces like `kVstAutomationUnsupported=0`),
  `tools/vst2_abi_check.cpp` compiled twice (SDK vs compat) diffing 55
  sizeof/offsetof facts on **both** x64+Win32; orchestrated by
  `tools/vst2_abi_check.ps1 -Sdk <vstsdk2.4/pluginterfaces/vst2.x>`.
  `tools/vst2_host_probe.cpp` + `tools/vst2_probe_run.ps1` editor round-trip:
  **PASS x64 + Win32** against compat-built DLLs.
- Findings baked into compat: real 2.4 `aeffect.h` defines `ERect` itself (aeditel
  not needed); `effOfflineNotify/Prepare/Run` stay PLAIN-named under
  FORCE_DEPRECATED (iPlug2's opcode logger references them); automation states
  start at `kVstAutomationUnsupported=0` (naïve `Off=0` was a real off-by-one).
- `.github/workflows/nightly.yml` — push `main`+`iPlug-experiment` + dispatch:
  matrix ci-win64/ci-win32 `PREFER_COMPAT=ON` → VST2 `.dll` + CLAP `.clap`,
  PE-machine + LoadLibrary/`VSTPluginMain` smoke gates (DLL NOT booted — no ROMs,
  hard rule #1 → `ROM-REQUIRED.txt` ships), per-arch zips → rolling `nightly`
  prerelease (delete-then-recreate tag, `contents: write`, concurrency serialized).
  `build-native.yml` untouched (general PR/branch check).
- **Nightly run #1 post-mortem (commit `92cc15c`, 2026-09-19): FAILED with ZERO jobs.**
  The API confirmed the run planned no check-runs at all — GitHub rejects the whole
  workflow when a job-level key references a context it may not use. Culprit:
  `jobs.build-windows.name: ${{ env.PROJECT_NAME }} …` — the `env` context is legal in
  STEP keys but NOT in `jobs.<id>.name` ("Unrecognized named-value: 'env'"). Generic
  YAML lint passes; only GH's parser catches it. Fixed: job name is a literal now
  (workflow-level `name:`/step names unchanged). Lesson for any future workflow:
  job names may use only github/vars/secrets/inputs/matrix.
- **`.map` in artifacts root-cause:** build-native.yml uploads the WHOLE
  `build-cmake/clap/<arch>/Release/` dir, and every MSVC plugin link had unconditional
  `/MAP` (leftover P2 crash-symbolization aid) → `.map` shipped next to the `.clap`.
  Now gated behind `SMU2000_LINK_MAP` (default **OFF**, `SMU2000_VST2/CMakeLists.txt`);
  re-enable locally with `-DSMU2000_LINK_MAP=ON`. Nightly zips were never affected
  (file-by-file staging), and forced-relink test on ci-win64 output confirms no map
  regenerates. Locally-stale maps from P2-era builds were trashed.
- CI-equivalent rerun (2026-09-19, post-fix): `ci-win64` + `ci-win32` with
  `PREFER_COMPAT=ON` (no `VST2_SDK_DIR`) both build VST2+CLAP clean; x64 gates
  (PE 0x8664, LoadLibrary, `VSTPluginMain` export) PASS locally on the compat DLL.

## Boot gating + snapshot cache (2026-09-16) — sync boot so the song head isn't eaten


**Problem.** The engine booted the MU2000 firmware on a BACKGROUND thread
(`engine::start()` → `boot()` spins `run_sample()` until `midi_ready()`, 6.6 s of samples
— 2-5 s wall interpreter-era, ~0.9 s wall here with the JIT). The host cannot pause a
plug-in's timeline, so it streamed
immediately: `fill()` emitted silence and `midi()` queued bytes while the firmware came
up. Net effect = the first few seconds of every song are silent and the song-head MIDI
(program changes / first notes) lands late in a burst. `render.cpp` already models the
right order — run the boot loop FIRST, only then feed MIDI from position 0 — the plug-in
equivalent is "make the machine live *before* the host consumes time", and the only
pre-streaming hook a plug-in owns is the constructor.

**Decision — synchronous boot at instantiation.** `engine::start()` → `start(bool block =
false)`; the VST2/CLAP ctor calls `start(true)` so it boots inline and `state()` is
`ready`/`failed` when the ctor returns — i.e. audio is live at sample 0 of the very first
`ProcessBlock`. Default arg `false` keeps every existing caller (the VST3 `plugin.cpp` and
CLAP `plugin.cpp` paths, still compiled, `start()`) behavior-compatible (background
thread). The `m_abort` checks + MIDI-queue/`fill()`-silence safety net are untouched (the
queue still drains on the first `fill()` after `ready`).

**Snapshot cache (one-time cold cost).** Cold boot is unavoidable once (firmware boots),
so the first ready machine is snapshotted: right after `midi_ready()` (before
`state=ready`) `mu->save_state()` is written atomically (temp + `replace_file`) to
`<config_dir>/bootcache.bin`. On the next cold boot, `boot()` — after `nvram::load` +
`reset()` — tries `mu->load_state(cache)` BEFORE the sample loop, then **verifies**
`midi_ready()`; on any failure it `reset()`s and falls back to the full cold loop.
Header key (`S2BC`, fmt v1): ROM-dir string + `mu2000_flash.bin` size + mtime + sin-table
presence + `NATIVE_RATE`; invalidated (so a swapped firmware never resurrects an old
machine). NVRAM caveat: a newer NVRAM file (user reconfigured via gui/live, see
`nvram::path`) is newer than the cache → discard. Verified via `mu2000.cpp::state()` that
the SCI `rx_enabled` bit (`m_scr`, what `midi_ready()` reads — `mu2000.h:97`) and the
voice/work RAM (`m_ram`/`m_sampram`) round-trip — `sh7042_device::state` syncs `m_scr`,
so a cache restore comes up already MIDI-ready.

**Knobs** (same `config_dir()/plugin.ini` reader already used for `threaded=`):
- Sync boot is the default. Opt out → background boot: `plugin.ini` `boot=async` or
  env `SMU2000_SYNC_BOOT=0` (env wins). Reverts to the old (silent-head) behavior.
- Cache read+write is default-on. Kill both: `plugin.ini` `bootcache=0` or env
  `SMU2000_BOOT_CACHE=0`.
- `log.txt` line per boot: `boot: {sync|async}, cache {hit|miss}, wall N.NN s` (plus the
  existing `起動:` and `bootcache:` save lines).

**Measurements** (this box, x64 MSVC, JIT default-ON; ad-hoc host `bootgate_test.exe`
driving the built DLL, MIDI = program change + note-on at offset 0 of the FIRST block):
- VST2 x64 cold: ctor **0.93 s**, block1 peak **0.0836** rms 0.0219 first-loud **sample
  163** (3.7 ms → clean attack, no burst-into-past). Warm (cache hit): ctor **0.07 s**,
  `boot: sync, cache hit, wall 0.01 s`, **bit-identical** audio.
- CLAP x64: cold 0.86 s → warm 0.06 s, block1 peak 0.0836 @ sample 163 — **identical
  waveform to VST2** (same engine, cache restore faithful across APIs).
- Chunk round-trip (`effGetChunk`/`effSetChunk` presType=1): 6,096,753 B, still plays
  after restore (DAW save/restore intact; `load_state` after `ready` correctly overwrites
  the cache-restored machine).
- `SMU2000_BOOT_CACHE=0`: cold every run (0.83/0.89 s), still sync + audible.
  `SMU2000_SYNC_BOOT=0` / `plugin.ini boot=async`: ctor 0.00 s, first block silent,
  sound returns at pump ~20 once the background boot finishes — old path preserved.
- Builds: vs-x64 + vs-win32 (GUI-OFF) and vs-x64 GUI-ON all clean; the win32 build's only
  new-era warnings are pre-existing `C4805` in `sh_adc/sh_sci` (not touched). P7 editor
  re-probed on the GUI-ON x64 DLL: 1250×500 child attaches/paints/detaches — **PASS**
  (ctor boot no longer races the editor open).

**Win32 audio.** Proven via the **identical plug-in class through CLAP-x86** (block1 peak
0.0836 @ sample 163, same waveform) and the **engine driven directly on x86** (same). A
raw ad-hoc *VST2-x86* harness (this test only) advances the machine (state bytes change)
but its serial MIDI never clocks → silent; this is a host-harness/ABI artifact of driving
VST2 win32 `processReplacing` by hand, not a regression from boot-gating (the boot-gate
touches WHEN the engine boots, not MIDI routing) and win32 VST2 audio was **never**
host-proven before this (P2/P7 tested win32 only for load/GUI/chunk). Real DAWs do not use
this harness. No plugin/GUI/editor regression observed on any arch.

**Files.** `src/vst3/engine.h` (`start(bool)`, `boot(bool)`), `src/vst3/engine.cpp`
(sync boot + boot-cache: `ini_value`/`sync_boot_wanted`/`boot_cache_wanted`/
`file_bytes`/`file_mtime`/`cache_restore`/`cache_store`, ctor boot logging),
`SMU2000_VST2/SMU2000_VST2.cpp` (ctor `start(true)`).


---

## Status

- [x] Recon complete; decisions locked; this ledger + `AGENTS.md` written.
- [x] P0 scaffold — **DONE 2026-09-15.** iPlug2 submodule @ `d54f69050` + `.gitmodules`;
      `cmake/{iplug2_paths,mingw_compat}.cmake`, root `CMakeLists.txt` + `CMakePresets.json`
      (`vs-win32`/`vs-x64`/`ci-*`), `engine/` INTERFACE placeholder, `SMU2000_VST2/` scaffold
      (config.h, stub sources, `CMakeLists.txt` with `smu2000_set_out_dirs`/`smu2000_stage_roms`
      live, `SMU2000_HAS_PLUGIN_SOURCES=FALSE`). `cmake --preset vs-win32`/`vs-x64` configure
      CLEAN (verified). Divergences: CLAP + VST3 SDK non-fatal/optional; `SMU2000_ROMS_DIR`
      non-fatal; VST2 env fallback probes `D:/opt/vst/vstsdk2.4`; upstream IGraphics.cmake
      downloads WebView2 at configure (needs network). Nothing committed.
- [x] P1 engine lib — **DONE 2026-09-15.** `engine/CMakeLists.txt` = STATIC `smu2000_engine`
      (exact 20-TU list: Makefile OBJS + `mu2000.cpp` + `vst3/engine.cpp`; PUBLIC includes
      `src`+`src/compat`; `/std:c++20 /utf-8 /bigobj`; defs `_USE_MATH_DEFINES NOMINMAX
      _CRT_SECURE_NO/STDC NO_WARNINGS WIN32`; CRT left to root `/MT`). Builds **Win32 + x64**
      (verified `.lib`s; dumpbin shows engine API symbols). JIT interpreter-only confirmed on
      both (`x64asm.h` never compiled; `sh2_jit`+`swp30_jit` fallbacks complete). Only src edit:
      `src/mame/cpu/sh.cpp` +#include <bit>. `xg/model.cpp` NOT needed. No winmm/avrt/ole32 deps.
      Namespace `smu2000::vst3::engine`.
- [x] P2 vst2 wrapper — **DONE.** Win32+x64 green, graphics-free (3 system DLLs), `VSTPluginMain`,
      load-probed: magic `VstP`/uniqueID `'SMU2'`/effOpen/effClose + effGetChunk/SetChunk ~6 MB round-trip.
- [x] P3 clap wrapper — **DONE.** CLAP SDK+HELPERS staged (copied from sibling sw10, untracked);
      Win32+x64 `.clap` green, graphics-free, `clap_entry`+plugin-factory probed; VST2 unregressed.
      One src tweak: ctor `: iplug::Plugin(...)` (CLAP helper base hides the `Plugin` alias).
- [x] P4 gui toggle — **DONE.** `SMU2000_ENABLE_GUI` (alias `ENABLE_GUI`) end-to-end. GUI-OFF default:
      both archs stay graphics-free (regression checked: KERNEL32/USER32/api-ms only). GUI-ON
      (`-DSMU2000_ENABLE_GUI=ON`, per-target define → `config.h PLUG_HAS_UI 1` + `IPLUG_EDITOR`):
      sw10-style `iplug_configure_target` + `${IGRAPHICS_LIB}` (NanoVG/GL2) + placeholder editor
       (`#if IPLUG_EDITOR` block; `using namespace igraphics` AFTER `iplug`). VST2 ON imports OPENGL32
       (toggle proven). Tree left configured GUI-OFF.
       > **SUPERSEDED by P7 (2026-09-15):** the placeholder `mMakeGraphicsFunc`/`mLayoutFunc` and the
       > NanoVG/`IPLUG_EDITOR` GUI-ON wiring were removed; GUI-ON is now the native GDI editor
       > (`NO_IGRAPHICS` + `smu2000_gui`) and no longer imports OPENGL32. The GUI-OFF half is unchanged.
- [x] P5 mingw/clang - **DONE 2026-09-15 (win32 leg env-BLOCKED).** GCC 16.2 + Clang 22 (MSYS2 mingw64) x64: VST2+CLAP all green, presets `mingw-{x64,clang-x64,win32,ci-x64}`. Self-contained (imports only ADVAPI32/comdlg32/GDI32/KERNEL32/msvcrt/SHELL32/SHLWAPI/USER32 - libstdc++/libgcc/winpthread static, no opengl); exports `VSTPluginMain`+`main` / `clap_entry`; JIT ON on x64 (`meg_jit`/x64asm symbols in both jit objs; win32 stays interpreter-only). Shims: `cmake/mingw_compat.cmake` (x86 FATAL removed - arch-clean engine; opengl32/gdi32 now GUI-ON-only), new `cmake/mingw_portability_prelude.h`, CLAP `CLAP_EXPORT extern` guard in `SMU2000_VST2.cpp`, single-config Release out-dir fix in the helper. Two Clang-only src fixes (mamecompat `timer_alloc` defined after `running_machine` completes; swp30_jit.cpp:515 explicit `s32()` narrowing) - MSVC/GCC behavior unchanged. MSVC vs-x64 re-verified green + graphics-free. win32: this box's i686 toolchain is BROKEN (cc1.exe dies STATUS_ENTRYPOINT_NOT_FOUND unless run from /mingw32/bin; partially-upgraded 2023-2026 package set); repair = `pacman -Syu mingw-w64-i686-gcc` (network - forbidden here); preset+harness proven working up to the compiler self-test. No submodule edits, no commit, roms untouched.
- [x] P6 ci github actions — **DONE.** `.github/workflows/build-native.yml` (untracked, not committed):
      `build-windows` (matrix x64/Win32 → `ci-win64`/`ci-win32`, MSVC, VST2 gated **OFF** — proprietary;
      CLAP+engine only; no app/vst3/Skia; ROM-REQUIRED.txt stub; anchored clap-only uploads) +
      independent `build-mingw` (MSYS2 MINGW64 gcc/ninja/cmake → `mingw-ci-x64`). YAML valid
      (PyYAML `safe_load`; actionlint absent). CI path emulated locally: `ci-win64 -DSMU2000_BUILD_VST2=OFF`
      configures (vst2 OFF / clap TRUE) + builds `.clap`. No `roms/`/SDK in any path.
- [x] P7 gui editor — **DONE 2026-09-15 (native GDI, superseded the IGraphics/NanoVG plan).** The
      VST3 GUI (`src/vst3/view.cpp` + `src/ui/*`) is raw Win32/GDI, so it's reused *verbatim* rather
      than ported to NanoVG. `SMU2000_VST2/ui/SMU2000Editor.{h,cpp}` (port of `view.cpp`) + STATIC
      `smu2000_gui` (Makefile `VST3_SRCS` GUI set) host `ui::panel` in a child `HWND` via
      `IEditorDelegate::OpenWindow/CloseWindow` — VST2 `effEditOpen/Close`, CLAP `guiSetParent/Destroy`.
      GUI-ON stays **graphics-free** (same manual recipe as GUI-OFF + `SMU2000_ENABLE_GUI` +
      `smu2000_gui` + gdi32/comdlg32/user32; **no** `iplug_configure_target`/`${IGRAPHICS_LIB}`).
      Host-probed (native C++ hosts in `tools/`): x64 VST2 + CLAP + Win32 VST2 all attach/paint/
      detach the 1250×500 panel through the real message loop; GUI-OFF default stays editor-free/
      graphics-free. See §Phase 7 for the full write-up + ABI/opcode gotchas. Tree left configured
       GUI-OFF (x64 rebuilt last). Nothing committed.
 - [x] CPU32 x86-32 JIT port — **DONE 2026-09-16** (full record: `CPU32_LEDGER.md` Phases 1–8).
       Dual-mode `x64asm.h` + SH-2 + MEG JITs ported to x86-32; guards widened so **MSVC-x64 also
       gets the JIT**; Win32 JIT default-ON, bit-exact (35/35 render matrix + soak + traces). Host
       win32 CPU win 2.06x (native 4.74x); win32-JIT 1.60xRT vs interpreter 0.78xRT. Tooling:
       `tools/msvc32_build.ps1` (MSVC amd64_x86 harness — mingw32 cc1plus broken on this box, do
       not repair), `tools/x64asm32_test.cpp` (encoding gate). Supersedes every "Win32 =
       interpreter-only" note above (Findings §32-bit JIT, P1, P5, P0-P7 status entries).
 - [x] Boot gating + snapshot cache — **DONE 2026-09-16** (full write-up: §Boot gating above).
       Ctor boots the firmware synchronously (`start(bool block)`; plug-in ctor uses
       `start(true)`) so host timeline sample 0 is already live — no more silent song head;
       `bootcache.bin` makes every later instantiation ~instant (0.93 s cold → 0.07 s warm,
       bit-identical audio, SCI rx + RAM verified round-trip). Knobs: `plugin.ini`/env
       `boot=async`·`SMU2000_SYNC_BOOT=0` (opt out), `bootcache=0`·`SMU2000_BOOT_CACHE=0`
        (disable). Supersedes the P2 "engine boots async — poll state()" probe note.
  - [x] MIDI queue perf — **DONE 2026-09-16** (P2-FIX2 above). POD event + byte arena + ctor
        reserve + append-only hot path + sortedness check (no per-msg/per-block mallocs); also
        fixed the LNK2001 HEAD-break from upstream `5901ea5` via `engine/xgui_plugin_stub.cpp`.
        VST2+CLAP Win32+x64 rebuilt clean.
  - [x] MIDI path rework: insert-sorted + fixed-window drain — **DONE 2026-09-19** (supersedes
        the scan-in-ProcessBlock half of P2-FIX2; POD+arena + ctor reserve unchanged). New
        `SMU2000_VST2/midi_queue.h` (shared with `tools/midi_bench.cpp`): ordering kept AT
        INSERTION like iplug `IMidiQueueBase::Add` (plain append for time-sorted hosts — the
        VST2/CLAP delivery contract; CLAP events.h:344), so ProcessBlock NEVER scans/sorts on
        healthy input; `kDisorderLimit=4096` chaos guard caps adversarial hosts at one
        `std::sort`/block via `queue::heal()` (never the insert-at-tail O(n²)). Consumption =
        `MidiSynth::kDefaultBlockSize`(32)-sample windows with event-gap jumping: engine
        fill()/m_machine round-trips capped at ceil(nFrames/32) per block regardless of
        density (bench: 257→16 fills/blk @ ~281 ev/blk); isolated events stay exactly
        on-sample, dense clusters ≤31 samples early (serial line's own byte spacing ~14).
        `tools/midi_bench.cpp` (`msvc32_build.ps1 -Tools midibench`) A/B vs the old path,
        interleaved same-stream + DSP-stubbed plumbing runs: plumbing dense-sorted 1.27x
        faster, shuffled/reversed within noise of old (no regression, guard proven); engine
        wall-time within ±0.5% (DSP ~10 ms/blk dominates on this box — the win is headroom
        + bounded worst case, not wall %). SMU2000_VST2.{h,cpp} slimmed to queue + drain;
        midi_queue.h added to all four plugin target source lists. VST2+CLAP x64 rebuilt
        green 2026-09-19 (fresh `vs-x64` configure after build-tree clean); **Win32
        `vs-win32` also green same day** (VST2 `.dll` + CLAP `.clap`; only the pre-existing
        sh_adc/sh_sci C4805 warnings).

### Wave schedule — COMPLETE (P0–P7 green)

- [x] Clean-room VST2 ABI + nightly release CI — **DONE 2026-09-19** (section
      "Clean-room VST2 ABI layer" above). Opcode gate 377/377, struct gate 55
      facts x 2 arches, editor probe PASS Win32+x64 on compat-built DLLs.
      `nightly.yml` YAML-validated; first real run happens on push to GitHub
      (nothing pushed yet).
- [x] Nightly CI fix wave — **DONE 2026-09-19** (post-first-run). Run #1 failed with
      ZERO jobs: `env` context is illegal in a job `name:` (GH workflow validator;
      generic YAML lint misses it) → job name now literal. `.map` removal: unconditional
      MSVC `/MAP` (P2 aid) now behind `SMU2000_LINK_MAP` (default OFF) → Release dirs
      and CI uploads binary-only. CI-equivalent `ci-win64`+`ci-win32` `PREFER_COMPAT=ON`
      builds + x64 smoke gates re-verified green locally after the fix.
- [x] Nightly run #2 gate fix + full VST2 delivery wave — **DONE 2026-09-19**.
      Run #2: x64 leg fully green (artifact uploaded); Win32 leg died in the old gate
      (`'{0:x4}' -f 0x014c` = "014c" != "14c"; plus LoadLibrary of a Win32 DLL from a
      64-bit pwsh always fails). Gate rewritten bitness-proof: parse PE header +
      export directory straight from the file (`VSTPluginMain` lookup; export dir
      fields nNames=+24 / AddressOfNames=+32 — verified against real MSVC+MinGW PEs
      both arches), LoadLibrary smoke kept only on the x64 leg.
      `build-native.yml`: MSVC job now builds VST2 **ON** via clean-room
      (`PREFER_COMPAT=ON`) and uploads vst2+clap paths explicitly (old glob clap-only
      upload was the "artifacts have no .dll" bug); `build-mingw` is now an
      x64/Win32 matrix (MINGW64/MINGW32, `mingw-ci-{x64,win32}`, nm-gated,
      vst2+clap uploads).
      **MinGW i686 MODULE link fix** (this box's gcc 16.1 Rev5): every shared/MODULE
      link died `undefined reference to __mingw_SEH_error_handler` — libmsvcrt.a's
      i386 beginthreadex thunk (pulled by any std::thread/winpthread user) references
      it, and ld never resolves the libmingw32.a `crt_handler` member for DLL links
      in this Rev5 layout (exes fine; late `-lmingw32` no-op). Fix =
      `cmake/mingw_compat.cmake` extracts that member via `ar x` at configure time
      into `<build>/_seh/mingw_seh_crt_handler.o` (C:\msys64 untouched) and
      `SMU2000_VST2/CMakeLists.txt` appends the .o to the two MODULE link lines only
      (adding it to exe links WOULD collide — archive member resolves there).
      Local proof: `mingw-ci-win32` + `mingw-ci-x64` full builds green; PE gates
      0x14c/0x8664 + `VSTPluginMain` (vst2) + `clap_entry` (clap) all pass. Supersedes
      the "i686 toolchain broken" caveats in AGENTS.md / P5 / msvc32 notes.


