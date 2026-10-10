# Porting to macOS

The project was written for MSYS2 / MinGW-w64 on Windows. This documents the
macOS port: what works today, how the platform split is laid out, and what is
left. It is a port, not a rewrite — the emulator core is untouched apart from
one compiler fix.

Target: **Apple silicon (arm64) only.** Build with the system clang++.

## Status

| Step | What | State |
|---|---|---|
| 1 | Core emulator + offline tools (`verify`, `boot`, `render`, `panel`, `statetest`) | **done** |
| 2 | Real-time audio (CoreAudio) + MIDI in/out (CoreMIDI) for `live` | **done** |
| 3 | GUI window (`gui`) — CoreGraphics drawing + Cocoa window | **done** |
| 4 | VST3 bundle for `Contents/MacOS` + `probe` | **done** |
| 5 | Audio Unit wrapper (AUv2, `aumu`), its editor + `au-probe` | **done** |
| 6 | CLAP bundle, loaded and played by `clap-probe` (`src/clap/probe.cpp`) | **done** (2026-09-20; no DAW yet) |

```
make          build every tool and both plug-in bundles
make check    ROM-free sanity check (runs build/verify)
make test     the regression suite (audio fingerprints + xgtest)
make probe    load the VST3 bundle in a headless host
make au-probe load the AU bundle in a headless host
make clap-probe load the CLAP bundle in the headless host and run its automation checks
make check-au same, plus the AU's torture test
```

### Since the upstream merge

37 commits were merged in from `tarboh/S-MU2000` (the parameter layer, the
regression suite, MIDI OUT, the NVRAM settings file, the driver/resampler
rewrite). Most of it is shared code and arrived working; `make test` passes here
with upstream's fingerprints unchanged, which is the strongest statement that
both platforms compute the same audio.

Four macOS gaps were left out deliberately, so that the merge stayed a merge.
All four are closed now:

* **`live --exclusive`, `--audio <name>` and `--dump-dev`.** They were parsed but
  only the Windows side acted on them; `src/ui/audio_out_mac.cpp` now implements
  all three — hog mode (`kAudioDevicePropertyHogMode`), a device chosen by name,
  and a WAV of what was handed to the unit.
* **The GUI could not pick the machine's own MIDI OUT** (`mout_mu`), so an
  external editor could not read or write the settings over a virtual port. The
  engine already routed it; `gui_mac.cpp` now has the menu entry.
* **`src/blocktime.cpp` was Windows-only** — it called `QueryPerformanceCounter`
  directly and was therefore not in the macOS `all` target. It measures through
  `smu2000::perf_ticks()` now, and builds on both platforms.
* **The GUI did not take part in the settings file.** It does: `use_nvram` is set
  from `--factory`, the machine's NVRAM is written on exit, and `gui.ini` now
  remembers the audio device and the VOLUME knob as well as the MIDI ports.

The Audio Unit was also missing its editor — a host could open it but had only
Apple's generic two-slider panel. It has one now; see
[The editor](#the-editor) below.

### Since the second merge (upstream `96f9c26`)

65 more commits came in. Most are docs, but the code side needed the macOS
branch caught up, and three things the merge left as Windows-only are here now.

**What the merge itself needed**

* **The arm64 MEG JIT had to follow upstream's rewrite.** Upstream gave the
  x86-64 backend the second index (`idx2`), `t` on branch instructions, MULTI
  COMP, the table reads, the wrap/saturation and rounding fixes and the `e == 0`
  case in `revram_decode`. `verify` compares the hand-written machine code of the
  reverb encode/decode and `m1_expand` against the C++ functions over every
  input, and that is what caught the divergence: it reports **0 mismatches** now.
* **The arm64 SH2 JIT took the lazy `pc` and the native delay-slot emission**
  (`SMU2000_SH2_LAZYPC=0` and `SMU2000_SH2_SLOTNATIVE=0` turn them off). Those
  were most of the instructions that still fell back to the interpreter.
* **The Audio Unit got an A/D INPUT bus and the card in its saved state.** The
  host feeds the unit's input bus (element 0, `SetRenderCallback`) and that goes
  to `engine::fill()`, the same job the VST3's `A/D Input` bus does. The card is
  remembered in the preset by file name only, as on the VST3 side. The bus is
  declared in `SupportedNumChannels` as `[2, 2]` and its scope configures
  always (format, callback, connection), but it is **not counted in
  `ElementCount` until the host asks for it** — the count is the only gate, and
  a host that writes `1` there gets the bus and the input pulled with it.
  The whole of
  [the note below](#the-ad-input-bus).
* **`ARCH=` / `UNIVERSAL=1` now pick their own build directory** (`build-arm64`,
  `build-x86_64`, `build-universal`). Before this an `ARCH=x86_64 make` after a
  native build failed in the link step, with a message about which architecture
  the object files were.

**Three things that were still Windows-only**

* **Branch programs on arm64.** The MEG JIT compiled a program with jumps only
  on x86-64; on arm64 it handed such a program back to the interpreter (correct,
  but slower). It compiles them now, reading and writing the delay ring on every
  instruction instead of committing three instructions later, and clearing the
  write and storing only the `t` value for an instruction that was jumped over —
  the same shape as the x86-64 backend and as `meg_state::run_program()`.
* **The SmartMedia slot in the macOS GUI.** Its menu had the MIDI file player
  only; it now has new (16/32/64/128MB) / open / eject, writes changed blocks
  back every two seconds, remembers the card in `gui.ini` and puts it back on the
  next start. It is the same `src/smartmedia.h` the Windows side uses.
* **A/D INPUT capture on macOS** (`src/ui/audio_in_mac.cpp`). `ui/audio_in.h` now
  splits by platform the way `audio_out.h` does: the engine's input node, tapped
  on its real-time thread, feeds the ring that `pop()` reads — the same code iOS
  runs, in `src/ui/audio_apple.mm`. The device's own rate is converted to 44100Hz
  with the same `ui::resampler` the Windows side uses. The device is chosen from the port menu
  (`ui::audio_in::list()`), remembered by name in `gui.ini`, and `gui --list`
  prints the list.

**The regression suite grew two things**

* A `lofi` case (`tools/make_test_midi.py`) that selects the XG insertion effect
  `LO-FI` (type `5E-00`), whose MEG program is the one with the seven jumps. No
  song in the suite reached that path before.
* Step 4, **JIT on/off**: every song is rendered twice, with both JITs and with
  neither, and the two WAVs are compared byte for byte. The fingerprint in
  `tests/*.json` is a single hash and cannot catch that on its own.

One bug turned up while wiring the menus, and it is worth knowing about: the
A/D INPUT and SmartMedia menu ids sat inside `ID_OUTMU_BASE + 256`, so picking an
input device selected a MIDI OUT instead and the tick never moved to what was
picked. They have ranges of their own now, on both platforms (`gui.cpp` had the
same defect).

Checked on arm64, with the ROMs in `roms/`:

| Check | Result |
|---|---|
| `make test` | verify **0 mismatches**; statetest packs to **472113** bytes, restore exact; **8** audio fingerprints (including `lofi`); **JIT on/off 8 of 8 byte-identical**; threaded matches; `xg` 526 round-trips, 0 mismatches; sampling 0 |
| `lofi` (LO-FI insertion, 7 jumps) | arm64 JIT vs arm64 interpreter: WAV byte-identical; arm64 and x86-64 renders identical too |
| the other 7 songs | JIT on vs off: byte-identical |
| `ui::audio_in` (standalone smoke test) | 2 devices listed; `Micro MacBook Pro` opens at 44100 Hz 1 ch → no conversion, real signal captured; the 48000 Hz device → resampled to 44100, ring fills |
| `gui roms` with `smartmedia=…` in `gui.ini` | `SmartMedia を差した: …（16MB）` before the window opens |
| `gui roms` with `audio_in=Micro MacBook Pro` | `A/D INPUT: Micro MacBook Pro（CoreAudio / 44100 Hz 1 ch float32 → 44100 Hz）` |
| `gui --list` | prints the MIDI ports, the outputs and the **input** devices |
| `make ARCH=x86_64 build-x86_64/render` / `…/gui` | both build in their own directory |
| `blocktime` (see doc/benchmarks.md) | arm64 both JITs: 0.865 ms per 256-frame block, **14.9%** of real time, worst 71%, 0 overruns |

Not checked here: the card and input-picking **menus** were not clicked (that
needs someone at the machine), so what is verified is the code behind them — the
ids reach the right handler (checked by simulating the dispatch chain), the card
is loaded into the machine at startup, and the input device opens and delivers
audio. The card file written by the machine was not compared against a real card.

## The front ends, diffed against the Windows one

`diff src/gui.cpp src/gui_mac.cpp` (comparing function lists, then the bodies of
the functions both have) is the way to tell a platform difference from a feature
that was left behind. Most of what comes out of it is platform:

* `ui::audio_out` has no `late()`, `format_line()`, `latency_line()` or
  `output_ms()` on macOS. Those read the WASAPI clock (`IAudioClock`,
  `GetStreamLatency`) and have no CoreAudio counterpart, so the status line says
  `枯渇` where Windows says `待ち … 遅れ …`. The answer here is `starved()`, which
  counts the blocks that ran out of sound.
* There is no MMCSS to register with; the counterpart is CoreAudio's real-time
  thread, so the line reads `CoreAudio の実時間スレッド`.
* The PC editor, the overview and the insertion editor (`ui::pc_editor`,
  `ui::overview`, `ui::fx_editor` — Dear ImGui) are not built for macOS, so
  `--editor`, `--list-window`, `--fx-window`, `F2`/`F3` and
  `ui::xgui::set_voice_rom()` have no macOS side.
* The file panels and the modal dialogs are AppKit, not `GetOpenFileNameW()` /
  `MessageBoxW()`.

Four things were real gaps and are ported now:

* **Dropping a MIDI file on the window.** Windows catches it with
  `WM_DROPFILES`; the macOS window registers `NSPasteboardTypeFileURL` and
  hands the path to `ui::app::file_dropped()`, which plays it exactly as
  `--play` does. The hook is virtual with a playing default, so a front end
  with nothing to do with a drop does not have to say anything.
* **Ports 3 and 4 of a MIDI file.** `ui::player` already had `ports_used()` and
  `fold_extra_ports()`; the macOS GUI was not using either. The card menu now
  carries the same two items as `gui.cpp` (`口 3・4 を A・B に重ねて鳴らす` /
  `口 3・4 は鳴らさない`), the choice is kept in `gui.ini` as `ports34=` with the
  same spelling as Windows, and a four-port file says which of the two it did.
* **All notes off on the way out.** Windows sends all-sound-off/all-notes-off
  (CC120/CC123, all 16 channels) to the THRU ports before closing them; macOS
  closed them and left notes held on whatever is listening.
* **`--nomidi` also means "do not remember".** On Windows it clears every port
  and sets `keep_settings`, because that is what the test runs use. On macOS it
  cleared the two inputs only, so a test run wrote its empty port names over the
  remembered ones.

The same diff over the plug-ins turned up two more. One is a regression the
last merge introduced:

* **A blank SmartMedia is announced again.** Upstream's Windows view showed
  `空の SmartMedia を差しました。… UTIL → CARD → Format で書式化してください。` when
  one was made. Moving card creation into the shared `plug_view::card_make()`
  dropped that notice, so the VST3 on both platforms and the AU handed the user an
  unformatted card in silence. `plug_view` says it through
  `plug_window::alert()` now. The standalone macOS GUI, which never had it, uses a
  new `ui::alert_modal()`.
* **The AU did not report its tail.** The VST3 answers `getTailSamples()` with
  four seconds so a host keeps rendering until the reverb has died; the AU
  answered nothing for `kAudioUnitProperty_TailTime`, so a host stopped at the
  last note and clipped the tail. It answers 4.0 seconds now.

The AU **view** cannot diverge from the VST3 one: `src/vst3/panel_nsview.mm`
builds a `vst3::plug_view`, and it, `src/vst3/view.cpp` and `view_mac.mm` are the
same files in both bundles (see `AU_SRCS` in the Makefile). What the AU has of its own is
`au/plugin.cpp` and `au/probe.cpp`, and those were compared against
`vst3/plugin.cpp` and `vst3/probe.cpp` instead.

Checked on arm64, with the ROMs in `roms/`:

| Check | Result |
|---|---|
| `make all au au-probe vst3` | clean; also with `ARCH=x86_64` (`build-x86_64/gui`, `…/S-MU2000.component`) |
| `make test` | verify **0 mismatches**; 8 fingerprints, JIT on/off **8 of 8 byte-identical**; `lofi` 4.3 s; sampling 0; xg 0 |
| `make check-au` | **0** problems; `残響の長さ` read back and required to be ≥ 3 s |
| `make probe` | `遅れ 0 サンプル / 残響 192000 サンプル` (4 s at 48 kHz — the same four seconds the AU now reports) |
| `gui roms --play` a four-port MIDI | `この曲は 4 口ぶん。…口 3 以降は A・B に重ねて鳴らす` (default) |
| the same with `ports34=drop` in `gui.ini` | `…口 3 以降は鳴らさない`, and the key is written back unchanged — the load/apply/save round trip |
| `gui roms --nomidi` with no `gui.ini` | no `gui.ini` is created, so a test run leaves the real one alone |

What is not checked: the drag-and-drop path itself (a drop has to be dragged by
someone at the machine — what is verified is that the window registers the type
and the app's `file_dropped()` is the handler behind it), and the two new menu
items were reached through the dispatch rather than clicked.

## The JIT debug switches, and what they cost

Every switch the two JITs consult is read once into a constant at startup, so a
switch that is off costs nothing per block. Four did not follow that rule:

* **`meg_jit_run()` is called once per audio sample** (44100 times a second per
  MEG program), and it read its two switches from function-local statics — a
  guard check each, every sample. They are file-scope constants now
  (`g_meg_bake`, `g_meg_check`).
* **`SMU2000_MEG_JIT_UPTO` was a raw `getenv()` in the code emitter**, and what
  it does is *truncate the block being compiled*. A stray variable therefore
  silently made the JIT run a partial program whenever the check harness was
  off. It is read once now, and ignored unless `SMU2000_MEG_JIT_CHECK` is on —
  the only situation where it means anything, since the check steps the
  interpreter the same number of times.
* **Both copies of `jit::compile()`** (x86-64 and arm64) read `lazy_pc`,
  `slot_native`, `native_enabled()` and `jit_trace_on()` *per instruction
  compiled*, the last three through a function call, and `slot_native` was a
  static sitting inside the compiling loop. All four are read once at the top of
  `compile()` now. The x86-64 copy is upstream's; the arm64 copy is the port's.
* **`SMU2000_JIT_DUMP`** (arm64 only, "temporary aid while porting") did a
  `getenv()` and a `strtoul()` on every block compiled. Removed.
  `tools/jit_dump.py` only decoded its output, so it is unused now — left in
  place rather than deleted.

Left alone, because it is upstream's: `sh2.cpp`'s interpreter loop calls
`jit_trace_on()` once per **interpreted instruction**, tracing or not. With the
JIT on that is only the fallback path; with `SMU2000_SH2_JIT=0` it is every
instruction. Hoisting it out of the loop is a one-line change to a file the
merge already conflicts in, so it is noted rather than done.

`SMU2000_MEG_JIT_CHECK=1` is expensive on purpose: it copies the whole
`meg_state` and a 256 KB reverb-RAM vector per block, and runs the block twice.
Two seconds of audio takes about two and a half minutes that way. It reports 0
mismatches on the current tree, which is what it is for.

Checked with the switches on both architectures (`build/` and `build-x86_64/`,
`lofi` — the branchy program — and `piano`):

| Check | Result |
|---|---|
| `SMU2000_MEG_JIT_UPTO=1` **without** the check | WAV byte-identical to the default, as intended |
| `SMU2000_MEG_JIT_UPTO=1` **with** `SMU2000_MEG_JIT_CHECK=1` | still bisects: 20 mismatch lines, versus 0 for the check alone |
| both JITs off, `SMU2000_SH2_LAZYPC=0`, `SMU2000_SH2_SLOTNATIVE=0`, `SMU2000_SH2_JIT=1` | every combination byte-identical to the default |
| `make test` | verify 0 mismatches, `JIT 入切` 8 of 8 |
| arm64 vs x86-64 | the same WAV |

## arm64 JIT: closing the gap to x86-64 (`opt/arm64-jit`)

The SH2 backend was already at opcode-for-opcode parity with x86-64 (same
`native()` coverage, lazy `pc`, slot-native, block chaining), so the work was
all on the MEG side: four x86-64-only optimizations ported, plus two small
arm64-specific ones. Each item was measured alone and kept only on a win.
Numbers are `build/blocktime roms build/tests/dense.mid 256 5 3` (median of
runs, MEG ns per sample) on this machine; every step stayed bit-exact
(`verify`, the full suite, JIT-vs-interpreter WAVs on dense/piano/effects/
lofi/chord, `SMU2000_MEG_JIT_CHECK` clean).

| Step | MEG master / slave | Kept |
|---|---|---|
| branch start | 828 / 737 | — |
| minimal `mov_imm64` (was always 4 insns) + rand seed in `x24` | 725 / 656 | yes |
| const region off `x25` (`m_const`/`t`/`offset` sit past the halfword imm12 reach) | 686 / 639 | yes |
| saturation limits in `x26–x28` (only 3 regs free: `-0x800001`/`-0x800000`/`0x3fffffffff`) | 646 / 605 | yes |
| early delay-ring commit (`SMU2000_MEG_EARLY`, same analysis as x86-64) | 641 / 601 | yes |
| inline `get_lfo` (same tables and rule as x86-64) | **634 / 599** | yes |

Block average 0.833 → 0.78 ms (14.3% → 13.4% of real time) over the branch.

Two x86-64 wins did **not** transfer and were reverted, recorded so nobody
re-tries them:

* **Baked constants (`SMU2000_MEG_BAKE`) lose ~3–4% on arm64** (dense and
  effects alike). The baked multiply needs the 32-bit coefficient widened to
  64 bits (`sxtw64`), which x86-64 gets for free inside `imul64i`, and dense
  has almost no skippable zero-coefficient ops. The `if (bake) return false`
  refusal is back in place.
* **Hoisting the SH2 address bounds into `x23–x28` measured flat.**
  Four instructions saved per memory op against a six-`mov` prologue on every
  block — the prologue eats the saving on blocks with few memory ops.

One thing the porting turned up was fixed right after the merge:
`swp30_jit.cpp`'s x86-64 LFO helper call hardcoded the Windows convention
(`RCX`/`RDX`), and `SEED`/`K_MAX` sit in SysV-volatile `RSI`/`RDI` — without
`sin-table.bin` the helper path ran and the binary segfaulted (exit 139 under
Rosetta; arm64 is unaffected). The call now puts its arguments in
`ARG0`/`ARG1`, and under SysV pushes `SEED` and `K_MAX` around the call
(two pushes, so the 16-byte alignment holds; Windows x64 doesn't push, since
there the callee preserves them and owns the 32-byte shadow space above the
return address). The Windows code bytes are unchanged.

It was checked on Windows by building the MEG JIT with the SysV argument
registers and calling both the generated block and `call_lfo` through
`__attribute__((sysv_abi))`, rendering without `sin-table.bin` so every LFO op
goes through the helper: the code before the fix dies with an access violation
(0xC0000005), the fix matches the interpreter byte for byte on effects, dense,
chord and lofi, and the same build with the two pushes removed does not — so
the test does see a clobbered `SEED`/`K_MAX`.

## The core needed one change

`timer_alloc` in `src/compat/mamecompat.h` called `machine().make_timer(...)`
while `running_machine` was still an incomplete type. GCC accepts that inside a
member template and defers the check to instantiation; **Apple Clang rejects it
at definition time**. The definition was moved below `running_machine`, the same
way MAME splits declaration from definition.

Two other spots were x86-specific and were made portable:

* `_mm_pause()` (x86 spin hint) → `smu2000::cpu_pause()` in
  `src/compat/platform.h`. It emits `PAUSE` on x86 and `YIELD` on arm64.
* `SetConsoleOutputCP(CP_UTF8)` → `smu2000::init_console_utf8()` in
  `src/compat/console.h`. A no-op on macOS, where terminals are UTF-8 already.

`src/render.cpp` and `src/statetest.cpp` only carried `#include <windows.h>`
without using anything from it; the include was dropped.

### Numeric fidelity

The audio is compared against MAME recordings, so the math has to be
reproducible. Apple Clang on arm64 may contract `a*b+c` into a fused
multiply-add, which baseline x86-64 (no FMA) cannot — so the port was checked
for it. Compiling the whole DSP path to assembly shows **zero fused operations**,
so the numbers match the Windows build without needing `-ffp-contract=off`.

State serialization (`src/state.h`) writes fixed-width types only, so the
platform differences in `long` do not reach the saved bytes.

## Platform layout

Shared logic stays in one place; only the OS edge is split.

| Concern | Windows | macOS |
|---|---|---|
| Audio output | `src/ui/audio_out.cpp` (WASAPI) | `src/ui/audio_apple.mm` (shared with iOS) + `audio_out_mac.cpp` (CoreAudio questions) |
| MIDI input | `src/ui/midi_in.cpp` (WinMM) | `src/ui/midi_in_apple.cpp` (CoreMIDI) |
| MIDI output | `src/ui/midi_out.cpp` (WinMM) | `src/ui/midi_out_apple.cpp` (CoreMIDI) |
| GDI subset | `compat/gdi.h` → `<windows.h>` | `compat/gdi.h` + `compat/gdi_mac.cpp` (CoreGraphics) |
| Panel / editor / effects drawing | `src/ui/{panel,editor,effects,layout,svg}.cpp` — **the same files** | ditto |
| Window, events, menus | `src/ui/window_win.h/.cpp` + `src/gui.cpp` (Win32) | `src/ui/window_mac.mm` + `src/ui/app_mac.h/.cpp` + `src/gui_mac.cpp` (AppKit) |
| Real-time MIDI file playback | `src/ui/player.cpp` (Win32 timers) | `src/ui/player.cpp` (`std::chrono`) |
| Settings and file lookup | `src/compat/paths.h` (`%LOCALAPPDATA%`) | `src/compat/paths.h` (`~/Library/Application Support`) |
| VST3 | `src/vst3/*` (`x86_64-win`) | `src/vst3/view_mac.mm` + `Contents/MacOS` |
| Audio Unit | — | `src/au/plugin.cpp` (`aumu`) |

`src/ui/audio_out.h` and `src/ui/midi_in.h` hold **both** class definitions under
`#if defined(__APPLE__)`. The public interface is identical on each side, so
`live` and the GUI are unaware of which implementation they get. The Windows
bodies are kept verbatim in the `#else` branch.

The macOS `audio_out` is a pimpl. Its constructor and destructor are declared in
the header and defined in the `.cpp`: with a `unique_ptr` member that is not
optional, because the compiler otherwise generates them where `impl` is still
incomplete.

### Comment language

The two conventions coexist, and the split is by **author, not by file**:

- Comments that came across from the existing Windows code **keep their original
  Japanese**. They are the author's prose; moving a block into `src/ui/engine.h`
  or `src/vst3/view_win.cpp` does not change who wrote it, so it is not
  retranslated. Searching a comment you remember from `gui.cpp` may now find it in
  one of those extracted files, unchanged.
- Comments the port adds are **English**: the whole of every new file, and any
  line written to explain a macOS path or a platform difference. Where a port note
  belongs beside an existing block the English line goes *below* the Japanese one
  rather than replacing it — `src/vst3/engine.cpp` and `src/ui/midi_in.h` show the
  shape.

User-facing strings are a separate question and were left alone: the panel, the
console messages and the error texts are still Japanese, so both platforms show
and print exactly the same thing. `--list`, the status line and the port menus on
macOS read identically to their Windows counterparts.

The split is checkable rather than a matter of taste. Searching this tree for
comment lines containing Japanese and looking each one up in `tarboh/S-MU2000`
leaves six that the other tree does not carry: four in `src/ui/engine.h` and two
in `src/vst3/view_win.cpp`. Those came across with the code when a file was split
— the engine struct left `gui.cpp`, the window procedure left `view.cpp` — and
they are Japanese for that reason and not because they were written here.

## How the audio port works

The central rule from [design.md](design.md) — **the synth owns no clock** — maps
onto CoreAudio almost directly. The Windows side runs a worker thread around
WASAPI; on macOS a `DefaultOutput` AudioUnit calls our render callback on its own
real-time HAL thread, so there is no thread to manage at all.

* Client format is 44100 Hz, 16-bit, stereo, interleaved — exactly what the
  synth generates. The unit converts for the device.
* `--latency` sets `kAudioDevicePropertyBufferFrameSize` on the default output
  device (clamped to the device's range). Not setting it leaves CoreAudio's
  default, often 512 frames.
* Real-time priority comes for free: the HAL thread is already real-time, so
  `mmcss()` reports whether the unit started.
* WASAPI exposes "frames still queued", which makes an underrun directly
  observable. CoreAudio does not, so `starved()` uses the same proxy `live`
  already tracked: a fill that took longer than the block it was making means
  the device would have run dry.

### MIDI input

CoreMIDI hands over whole packets, SysEx included, on its own thread. That
removes the Windows trap where SysEx silently disappears unless you pre-post
receive buffers to WinMM. The lock-free ring between the callback and the audio
thread is unchanged, because the audio thread still drains it a byte at a time.

`list()` and `open(i)` both walk `MIDIGetSource(i)` in order, so the index a
user passes to `--midi` matches the listing.

### MIDI output

The THRU path — anything the synth receives is echoed to MIDI OUT — is
`src/ui/midi_out_apple.cpp`. It keeps the same split as the Windows side, and it
matters more here: the audio thread has to hand a byte over without ever
blocking, and `MIDISend` can take a lock, so calling it from the render callback
would put an unpredictable wait inside the audio callback. The audio thread only
drops bytes into a ring; second thread sends.

The wake-up differs, because CoreMIDI has no event object to signal. A
`std::condition_variable` stands in for the Win32 event.

One detail worth knowing if the SysEx path is ever touched: CoreMIDI wants the
packet list in a buffer the caller owns, and a dump can be tens of kilobytes, so
the buffer is sized for the whole message and `MIDIPacketListAdd` lays it out.
The WinMM version instead prepares a header and waits for `MHDR_DONE`.

`list()` and `open(i)` walk `MIDIGetDestination(i)` in order.

## How the GUI port works

This is the part that could have been a rewrite, and deliberately is not.

### One drawing layer, two implementations

Every pixel of the panel, the editor page, the effects page and the SVG art goes
through about twenty GDI calls. Those are re-declared in `src/compat/gdi.h` —
which is nothing but `#include <windows.h>` on Windows — and implemented once
over CoreGraphics in `src/compat/gdi_mac.cpp`.

`draw.h`, `panel.h`, `layout.h` and `svg.h` include `compat/gdi.h` instead of
`<windows.h>`, and that is the **entire** change to the drawing code: four
include lines, no edits to `panel.cpp`, `editor.cpp`, `effects.cpp`,
`layout.cpp` or `svg.cpp`. A hand-ported drawing layer has to be compared by eye
to know it matches. With the surface pinned, the two panels agree by
construction.

What the shim reproduces, and why each matters:

* **No flipping.** Every context it hands out has its origin top-left with y
  running down, like GDI. A bitmap context starts bottom-left, so it is turned
  over once when it is made; a context from a flipped `NSView` already is that
  way and is left alone.
* **The half-pixel nudge.** GDI paints a 1-pixel line on exact pixel
  boundaries: a rule at y = 5 covers row 5. CoreGraphics centres the line on
y = 5 and would straddle rows 4 and 5. Odd pen widths are shifted by half a
  pixel, which is what keeps the LCD's tick marks and the editing grid crisp.
  Even widths already line up and are left alone.
* **Rectangles.** `[left, right) × [top, bottom)`, GDI's convention.
* **`ALTERNATE` fill rule** (even-odd), because the SVG art has holes in it.
* **`Arc`** is flattened to a polyline. It is used once, for the send-level
  fans, and the context here is y-down; sampling the angles is unambiguous where
  untangling CoreGraphics' flipped angle signs is not.
* **A default pen and brush.** A fresh DC starts with `BLACK_PEN` and
  `WHITE_BRUSH`, exactly as GDI's does, so a `Polygon` with no pen selected
  still gets its outline.

Text is the one place that cannot match exactly. `CreateFontA` is asked for
"Segoe UI", which macOS does not have, so that request is answered with the
system UI font. Glyph metrics differ slightly from the Windows build, which
means layout tuned around text via `panel.txt` may want a nudge. Everything else
— colours, geometry, line weight — is identical.

### The macOS window, and how it talks to the app

The three platforms have the same file layout — a window-system file, an app
class, and a main — and the macOS column is `src/ui/window_mac.mm` (the only
Objective-C++ file in the project), `src/ui/app_mac.h/.cpp` (`gui_app : public
ui::app`), and `src/gui_mac.cpp`. The app class carries the same per-platform
hooks as `win_app` and `linux_app` (dialogs, audio say-lines, window opening);
everything the events *mean* — what a press does, what a key opens, what a
drop plays — lives once in `ui::app`, and all three pumps call those verbs
directly (`mouse_down`, `key`, `context_menu`, `paint_main`, …).

There is one piece of machinery this platform needs and the others do not.
Cocoa defines `BOOL` and Quickdraw (pulled in through AppKit) typedefs the
name `Polygon` — the two names `compat/gdi.h` exists to declare. The shim
resolves it itself: `BOOL` is spelled `bool`, agreeing with `objc/objc.h`'s
typedef, and its own `Polygon` is renamed out of Quickdraw's way:

```
// So we don't collide with the old Quickdraw Polygon
#define Polygon GdiPolygon
```

The drawing code keeps writing `Polygon` — the macro rewrites the token —
and `window_mac.mm` imports Cocoa before the shim's headers, the same order
any Cocoa file uses, so one translation unit holds both.

The window is an `NSWindow` with a flipped `NSView`. Painting goes straight into
the view's `CGContext` wrapped by `smu_gdi_wrap_view_context()` and handed to
`ui::app::paint_main()`; AppKit already
double-buffers, so `gui.cpp`'s memory DC has no counterpart. A 33 ms timer drives
repaints, in common run loop modes so it keeps ticking while a menu is open.

The port picker is an `NSMenu` built from `ui::app::context_menu()` — the same
description the Windows and Linux popups render —
and choosing a MIDI file is an `NSOpenPanel`; the app asks for the latter by
name (`ui::open_midi_file_panel()`), so it never needs AppKit itself. The same
window services carry the one confirmation there is — `ui::confirm_modal()` — which is
used before the machine's settings are thrown away.

### Ports, settings and factory reset

The picker offers the same choices as the Windows one: MIDI IN A-D, the
machine's own **MIDI OUT**, and the two THRU ports. C and D (parts 33-64) exist
only over USB on the real machine, so the GUI starts with HOST SELECT = USB as
`gui.cpp` does, and `--host-midi` gives the DIN ports A and B only. The menu ids
and the `gui.ini` keys (`midi_in`, `midi_in_b`, `midi_in_c`, `midi_in_d`) are
gui.cpp's. The machine's MIDI OUT is what makes the
settings reachable from a librarian or editor over a virtual port. The titles
are word for word what `gui.cpp` shows, so the two platforms cannot drift: `OUT`
is what the firmware sends by itself, `THRU` what was received and echoed.

Every choice is remembered **by name**, not by index, because replugging a USB
device shifts the numbers. A port that would not open keeps the name it was
asked for, so a virtual port that is not running yet is not forgotten by the
next start; picking from the menu clears that, so a deliberate "unused" sticks.
The file is `gui.ini` in the per-user settings directory `compat/paths.h`
provides, and it carries the same keys the Windows build uses — including the
audio device and the VOLUME knob, which is analogue on the real machine and so
is not in the firmware's RAM.

`--factory` skips the whole file and boots blank; the equivalent menu entry
("工場出荷状態に戻す...") asks first and then calls
`ui::engine::factory_reset()`, which waits for the audio thread to let go of the
machine and reboots it. That takes tens of seconds, so it runs on its own thread
and the previous one is joined rather than left to overlap.

### Sharing the engine rather than copying it

The synth half of `gui.cpp` — load the ROMs, boot, and render blocks — moved to
`src/ui/engine.h` and both front ends use it. The point is not the boilerplate
but the routing in `fill()`: which port feeds which part, and what gets echoed
back out. Two copies of that are two things that can drift, and a drift there
would show up as the platforms sounding different. `gui.cpp` now pulls the name
in with a `using` declaration, so the rest of it reads as it always did.

Three smaller things became portable instead of being duplicated:
`src/compat/paths.h` (the executable directory and the settings directory),
`src/ui/player.cpp` (a `std::chrono` clock in place of
`QueryPerformanceCounter` and `Sleep`; `steady_clock` is QueryPerformanceCounter
underneath on Windows, and `timeBeginPeriod(1)` is kept behind `_WIN32`),
and `src/ui/png.cpp`, which turned out to be hand-rolled zlib and never needed
GDI+ at all.

## How the VST3 port works

### The bundle

A macOS VST3 is `S-MU2000.vst3/Contents/MacOS/S-MU2000` plus
`Contents/Info.plist` (`packaging/vst3-macos-Info.plist`) and `Contents/PkgInfo`.
The `Info.plist` is not decoration: nothing `dlopen`s a `.vst3` directory. The
host opens it with `CFBundle`, reads `CFBundleExecutable` to find the binary, and
calls `bundleEntry`. So the entry points in `src/vst3/plugin.cpp` are per
platform — `InitDll`/`ExitDll` on Windows, `bundleEntry`/`bundleExit` on macOS —
and both hand back the same factory.

It is linked with `-bundle`, not `-shared`, because that is what `CFBundle`
loads.

### The view

`view.cpp` used to own an `HWND`. It is now split the same way the GUI is: the
VST3 interface, the panel and the input semantics stay in `view.cpp`, and the
window itself moves behind `src/vst3/plug_window.h` — `view_win.cpp` for the
child `HWND`, `view_mac.mm` for an `NSView` added to whatever view the host hands
over in `attached()`.

`view.h` mentions no window system at all and holds the panel behind a pimpl,
so `view_mac.mm` can include it next to Cocoa; the shim's `BOOL`/`Polygon`
handling (see the window section) is what makes the two coexist in one
translation unit.

The subview is flipped, so the context AppKit hands to `drawRect` is already
top-left with y down and goes straight into `smu_gdi_wrap_view_context()`. No
backing store either, unlike the Windows side: AppKit double-buffers already.
There is no `WM_TIMER` to drive repaints, so the view runs its own 33 ms timer in
common run loop modes. Key codes are mapped to `plug_key` in the `.mm`
(honouring the same characters `gui_mac.cpp` does), and `view.cpp` maps
`plug_key` to `mu2000::button` in one place, so the two platforms cannot
disagree about what a key does.

The probe's `--view` mode needed the same treatment: `probe.h`'s Win32 host
window became `src/vst3/probe_host.h`, implemented by `probe_host_win.cpp` and
`probe_host_mac.mm`. The macOS one runs the real `[NSApp run]` loop, because the
plugin's repaint timer lives on the run loop and its editor does not animate
otherwise.

### The probe

`vst3probe` opens the module the way each platform does — `LoadLibrary` on
Windows, `CFBundle` + `bundleEntry` on macOS — and the timing calls
(`GetTickCount`, `Sleep`) became `std::chrono` and `std::this_thread`.

## How the Audio Unit port works

The AU reuses the VST3 engine unchanged. `src/au/plugin.cpp` is only the host
interface; `midi()`, `fill()`, the resampler, the ROM search, the boot thread
and the state packer are all the same code the VST3 plug-in runs.

An Audio Unit is registered rather than opened: `Info.plist`'s `AudioComponents`
array (`packaging/au-macos-Info.plist`) names the type (`aumu`, a MusicDevice),
the subtype (`SMU2`), the manufacturer (`Trbh`) and the `factoryFunction`
(`SMU2000AUFactory`). A mismatch between that file and the constants in
`plugin.cpp` means either the AU is invisible to hosts or it loads and offers
nothing. Apple's `AudioUnitSDK` is not vendored — for the same reason Steinberg's
`public.sdk` is not: the dispatch is a fixed table, so it is written out here
instead of pulling in a framework.

The pieces worth knowing about:

* **`AudioComponentPlugInInterface`** with `Open`/`Close`/`Lookup`. The object's
  interface must be its first member, because the host is handed `&iface` and
  hands the same pointer back as `self` for every method.
* **MIDI scheduling.** `MusicDeviceMIDIEvent` can be called from a thread that is
  not the audio thread, and the `engine`'s MIDI entry is audio-thread-only (it
  touches the pre-boot queue), so events are parked in a queue behind a mutex
  and drained inside `Render` with `try_lock` — if the lock is busy, the
  messages are picked up in the next block. The engine's own MIDI path is never
  called from anywhere but the render thread.
* **Offsets are honoured the same way the VST3 side does it**: `fill()` runs up to
  each event's offset, the event is injected, then `fill()` continues. That is
  what makes the AU and VST3 renders line up sample-for-sample rather than
  block-for-block.
* **Interleaved output** (one buffer holding two channels) is de-interleaved
  256 frames at a time into scratch buffers; the non-interleaved case, which is
  what a host normally asks for, writes straight into the host's buffers.
* **`ClassInfo`** carries the same `state_pack()` blob the VST3 plug-in stores,
  so the AU's state is ~330 KB rather than the 6 MB raw machine image. The
  output level rides alongside it as a `CFNumber`.
* **A restore that arrives before boot is parked, not dropped.** A host sets
  `ClassInfo` straight after `AudioComponentInstanceNew` — exactly when the boot
  thread is still running — and the machine cannot be written back until it has
  come up. `engine::load_state()` used to refuse while the state was `loading`,
  **in silence**, so the restore vanished and the unit came up at its defaults.
  (The output level still landed, because that is a plain member and not part of
  the machine — which is what made it look like it had worked.) Now the buffer
  is parked and `boot()` applies it at the end of boot, before it publishes
  `ready`, so a host that reads straight back does not see the old machine.
  `vst3/plugin.cpp`'s `setState` no longer needs its wait either.

  A timeout was the wrong shape twice over: boot measured **3.6 s** of wall
  clock on a cold instance here, so the 3 s wait the VST3 used lost the restore,
  and simply waiting longer would block the host instead. Parking has no timing
  assumption in it.

  It was pinned down with a scratch host (not in the tree yet): one instance
  boots and is then given a program change so its state is distinctive, another
  is handed that state *immediately* after `AudioComponentInstanceNew`, and the
  raw (unpacked) state bytes are compared. Matching the first exactly means the
  restore survived; matching the default boot state means it was dropped. Two
  plain boots are compared as well, so the boot state is known to be
  reproducible and the verdict cannot be an artefact of one run.

  The failure was intermittent, which is what gave the timeout away: with a 3 s
  wait it read "restored" on a warm instance and "DROPPED" on a cold one. The
  same scratch host is worth keeping — `aubprobe --torture` cannot see this,
  because it waits for the blob to grow before it sets anything.
* **Latency** is reported from `latency_samples()`, which is non-zero only when
  the host's rate is not 44100 (then the resampler adds the delay).
* **Parameters** are two, not the VST3 side's 2098. AU has no MIDI-CC-to-parameter
  convention like VST3's `IMidiMapping`, so there is nothing to map; MIDI arrives
  through the MusicDevice entry points instead, and the two parameters are what a
  host's generic panel can usefully show (output level, and whether the firmware
  has come up).

`make au` writes `build/S-MU2000.component`; `make install-au` copies it to
`~/Library/Audio/Plug-Ins/Components`, which is where `auval` and every DAW look.

### The editor

A host asks for an AU's own interface with `kAudioUnitProperty_CocoaUI` (Global),
which answers with a bundle URL and the name of a class inside implementing
`AUCocoaUIBase`; the host then calls that class's
`uiViewForAudioUnit:withSize:`. `src/au/editor.h` is the seam between the two
halves — plugin.cpp is plain C++ and answers the property, `editor_mac.mm` is
Objective-C++ and holds the class. That is the same two-half shape as
`src/ui/window_mac.mm`.

The view itself is not built here. `editor_mac.mm` asks
`smu2000::vst3::make_panel_view` (`src/vst3/panel_nsview.mm`) for it, and what
comes back is an `SMU2000PanelView` holding a `smu2000::vst3::plug_view` — the
panel the VST3 build already shows, not a second one. The AUv3 asks the same
function for the same view (`src/auv3/view_controller.mm`), so the three formats
have one editor between them rather than three that look alike. What is not
shared is the host interface around it: the AU needs an `NSView` and the VST3
wants an `IPlugView`, so each format keeps its own way of being asked and all
draw the same thing.

`make_panel_view`'s `owner` argument is the one thing that differs. The view
holds a strong reference to it for as long as it lives, because a host may let go
of the plug-in before the view it was handed (`auval` does) and the panel's
teardown reaches back into the engine. The AUv3 passes its `AUAudioUnit`; the
AUv2's engine is owned by an instance the host tears down after the view, so it
passes `nil`.

One detail is worth knowing before touching this. The `AudioUnit` a host passes
to the view factory is **not** the pointer the plug-in was given as `self` — the
wrapper is a small dispatch table and the two are different objects (measured,
not assumed), so the factory cannot cast one to the other. The engine is reached
through a private read-only property (`kEngineProperty`, 64000, in the range
Apple leaves to everyone else) instead, which travels the ordinary property
dispatch in plugin.cpp and so arrives with the instance already resolved.

One more thing the editor does for its host rather than for itself. A host that
keeps a plug-in's window in a panel which never takes key focus (Waveform does)
makes every click into it a "first mouse" click, and AppKit spends such a click
activating the window instead of handing it to the view: the panel draws, the
LCD animates, and no button ever fires. So the view answers `acceptsFirstMouse:`
YES — the click is delivered *and* the window activates — and it asks for first
responder where the window is finally known, in `viewDidMoveToWindow`, not in
`attached()`, which runs before the host has put the view anywhere and therefore
asks nil. Focus is only taken when the window itself owns it, so a host that
keeps another control in the same window keeps it.

Whether the events arrive at all is the part no host will tell you about, so the
view writes the first eight clicks to the engine's log with the three answers
that name the cases: whether the window was key, whether the click arrived on the
main thread, and which view it hit (a host that puts something over the panel
takes them all). A run with no such line at all is the other answer — the window
never became key and the click never got that far.

Waveform 14 is where this was measured, and what a healthy answer looks like:

```
クリック 1 回目: 窓 あり、key yes、主の糸 yes、当たった先 SMUPlugView
```

(the window took key focus, the click arrived on the main thread, and the view it
hit is the panel — three lines and the panel was live again.)

`aubprobe --torture` walks the host's path from C++ through the Objective-C
runtime and checks that a view comes back with a live subview in it, and `auval`
reports `VERIFYING CUSTOM UI / Cocoa Views Available: 1 / SMU2000AUViewFactory /
PASS` without being told where to look.

## Building

```
make   verify / boot / render / panel / statetest / blocktime / live / gui
       and both bundles: S-MU2000.vst3 and S-MU2000.component
```

The Makefile detects the platform: `OS=Windows_NT` → Windows, `uname -s` =
`Darwin` → macOS. The Windows recipes keep the same target names, flags and
libraries as before. The two Objective-C++ files get their own pattern rule,
because they have to be compiled with `-fobjc-arc`.

Architecture: by default everything builds for this machine's own slice
(`-arch` is not passed and the compiler default wins, so Apple Silicon Macs
build arm64-only). Three opt-ins:

```
make ARCH=arm64       # force one slice
make ARCH=x86_64      # force the Intel slice (Rosetta)
make UNIVERSAL=1      # both slices in every binary and bundle
```

A forced slice gets a build directory of its own automatically
(`build-arm64`, `build-x86_64`, `build-universal`), because object files of
different slices cannot be mixed: an `ARCH=x86_64 make` after a native build
used to fail in the link step with a message about which architecture the
`.o` files were. `BUILD=...` still overrides the choice, and a plain `make`
keeps using `build/`.

The two JITs (SH2 and MEG) each have an arm64 backend as well as the original
x86-64 one (`src/compat/a64asm.h` is the aarch64 emitter; `src/compat/exec_mem.h`
holds the mmap/mprotect layer). So the arm64 slice compiles them both in, and
Rosetta is no longer needed just to get the JITs. The same audio fingerprints
pass with the JITs on and off, and a render is byte-identical either way; the
arm64 build is now the fastest and steadiest configuration (doc/benchmarks.md
has the numbers). Under Rosetta the worst-case blocks are still far behind,
because Rosetta occasionally pauses for tens of milliseconds while it
translates newly generated code, so an audio path running under it wants a
large device buffer; the arm64 slice has no such spikes.

## Verifying

Without ROMs only the ROM-free checks can run:

```
make check
```

which exercises the SWP30 register file and the machine's random sequence.

CoreAudio can be checked on its own, without ROMs:

```
clang++ -std=c++20 -O2 -I src -I src/compat -x c++ -c -o /tmp/audiotest.o - <<'EOF'
#include "ui/audio_out.h"
...
EOF
clang++ /tmp/audiotest.o build/src/ui/audio_out_mac.o \
    -framework CoreAudio -framework AudioToolbox -framework AudioUnit \
    -framework CoreFoundation -o /tmp/audiotest
/tmp/audiotest
```

On this machine that reports `buffer_frames=1323` for `--latency 30` and pulls
about 13371 frames in 300 ms (expected ~13230), `starved=0`.

**Verifying the sound itself needs ROMs.** Dump them as described in
[doc/dump/](dump/) and point `live` at the directory. `live --list` shows MIDI
inputs (`MIDI 入力が見つからない` when there are none).

### Checking the drawing layer

`--shot` renders a page to a PNG with no window at all, which is the quickest
way to see the panel:

```
./build/gui --shot /tmp/panel.png --size 1400x360          # empty panel
./build/gui --shot /tmp/grid.png  --size 1400x360 --grid   # with the editing grid
./build/gui roms --boot --shot /tmp/boot.png               # with the firmware up
./build/gui --layout art/mame/panel.txt   --shot /tmp/art.png --size 1000x420
./build/gui --layout art/sample/panel.txt --shot /tmp/art.png --size 1000x420
```

The last two are worth running because the SVG art is the only thing that
exercises `PolyPolygon`. `art/mame/panel.txt` draws the whole MAME panel as one
SVG, and the art contains colours that are not in the built-in palette — so
finding `#404040` in the output, and *not* in a run without the art, proves the
SVG was parsed and filled rather than silently skipped.

There is also a direct check that the window path and the `--shot` path agree,
which is what makes the PNGs a valid stand-in for what the window shows: paint
the same panel twice, once into a DIB and once into a context that has already
been flipped the way AppKit flips it, and compare the bytes.

### What was checked, and what was not

Verified on arm64 against the working tree:

| Check | Result |
|---|---|
| `make` from clean | every tool + both bundles, no warnings |
| `make check` | random sequence `574a3af2 de214fbe 610c06da`, as before |
| `statetest` | packed state **332767** bytes, restore exact |
| `render` (Bhangra, XG) | **634.921** cycles/sample, 8583140 word writes, 0 byte writes — identical to before this step |
| `--shot` of the built-in panel | renders; palette matches (`#c4bdaa` face, `#d8cda5` keys) |
| `--shot` with `art/mame` and `art/sample` | SVG parsed and filled; art-only colour present |
| DIB path vs flipped view context | **0** differing bytes of 360000 |
| `gui` window | runs, CoreAudio real-time thread at 30 ms, stays up |
| Windows recipes | dry run unchanged (`-static`, `-lwinmm -lgdi32 …`) |

The render numbers matter most: they are the same as the pre-port build, so none
of this step disturbed the audio path.

**Not checked.** The window was confirmed to run and to paint identically to the
verified PNG path, but it was not looked at on screen, and the popup menus, the
file panel and keyboard/wheel input were not exercised interactively — that
needs someone at the machine. The editor and effects pages share every primitive
with the front page (they differ only in which controls they draw), but like the
front page they were only checked as PNGs, not clicked.

The plug-in view is in the same position: `vst3probe --view 6` reports
`createView` / `attached` / `removed` and stays up for six seconds with the panel
animating, the AU's probe drives the same window path, but neither plug-in's
window was looked at on screen.

### After closing the gaps

Run again from a clean build on arm64, with the ROMs in `roms/`:

| Check | Result |
|---|---|
| `make clean && make` | 0 errors, 0 warnings; every executable (blocktime now among them) + both bundles |
| `make check` | `574a3af2 de214fbe 610c06da`, unchanged |
| `make test` | verify + statetest (packs to **471918** bytes, restore exact) + **all 7 audio fingerprints** + the threaded check + `xg` 424 round-trips, 0 mismatches |
| `blocktime roms <xg midi> 512 3 1` | mean **4.677 ms** per 512-frame block (**40.3%** of real time), worst 5.81 ms, **0** of 259 blocks over |
| `live roms --nomidi --seconds 2 --dump-dev cap.wav --exclusive --audio Haut-parleurs` | hog mode taken, device matched by name, 2.1 s capture written (369564 bytes) |
| the same run | 2.1 s of audio in 0.77 s of CPU time (37.2%) |
| `gui` with `midi_out_mu=NoSuchVirtualPort` in `gui.ini` | reports `MIDI OUT: なし（「NoSuchVirtualPort」が見つからない…）` — the remembered name survived |
| the same run, after the settings write | `audio_out=` and `volume=` written back; `--factory` skipped the NVRAM load |
| `gui --shot` | does not touch `gui.ini` at all |
| `--shot` regression | face `#c4bdaa` and the art-only `#404040` still where they were |
| `make probe` | factory found, 1 class, 4194 parameters |
| `make au-probe` | opens, 2 parameters, latency 0 |
| `make check-au` | `OK: エディタ SMU2000PanelView が画面を作った（下位ビュー 1 枚）`, **0 problems** |
| `auval -v aumu SMU2 Trbh` | `VERIFYING CUSTOM UI / Cocoa Views Available: 1` … **AU VALIDATION SUCCEEDED** |

## Plug-ins

### VST3

```
make vst3                     write build/S-MU2000.vst3
make probe                    load it in a headless host and describe it
make install-vst3             copy to ~/Library/Audio/Plug-Ins/VST3

build/vst3probe build/S-MU2000.vst3                       describe
build/vst3probe build/S-MU2000.vst3 song.mid out.wav      render
build/vst3probe build/S-MU2000.vst3 --torture             abuse it
build/vst3probe build/S-MU2000.vst3 --view 20             show the panel
```

The ROMs are found through `S_MU2000_ROMS`, or from `roms.txt` next to the
bundle, or from `~/Library/Application Support/S-MU2000/roms`.

Checked on arm64:

| Check | Result |
|---|---|
| `vst3probe` | factory found, 1 class, `aumu`-equivalent audio module, 4194 parameters, 4192 MIDI mappings |
| `--torture` | initialize/terminate cycles, 22050–192000 Hz, zero-length buffers, 4 simultaneous instances — **0 problems** |
| state save/restore | **333273** bytes, round-trips |
| 4 instances at once | 2.13 s of audio in 3.19 s wall (37% CPU each) |
| render vs `render` | 0 of 2194 windows where the reference plays and the plug-in does not (100% agreement) |

### Audio Unit

```
make au                       write build/S-MU2000.component
make aubprobe                 (via make au-probe) describe it
make install-au               copy to ~/Library/Audio/Plug-Ins/Components
make check-au                 the torture test

build/aubprobe build/S-MU2000.component --list                 describe
build/aubprobe build/S-MU2000.component song.mid out.wav       render
build/aubprobe build/S-MU2000.component --torture              abuse it
auval -v aumu SMU2 Trbh                                        Apple's own check
```

Both `make probe` and `make au-probe` pass the **bundle directory**, not the
executable: the probes open the bundle with `CFBundle`, so handing them
`Contents/MacOS/S-MU2000` stops them with "cannot open bundle". They also
export `S_MU2000_ROMS=$(ROMS)` (`ROMS ?= roms`), because neither plug-in can
boot without the ROMs.

`aubprobe <bundle>` reads the bundle with `CFBundle` and registers its factory
with `AudioComponentRegister`, so it needs nothing installed. Passing `-`
instead of a path uses whatever is registered for `aumu/SMU2/Trbh` on the
machine — which is the path `auval` takes, so the two can be compared.

Checked on arm64:

| Check | Result |
|---|---|
| `aubprobe --list` | opens; `Output Level` 0–1 and `Status` 0–2; instrument name; latency 0 |
| `--torture` | 8 create/dispose without initialize, 3 initialize cycles, 22050–192000 Hz, zero-length render, property table, state round-trip, 4 instances, the editor, and (installed bundle) an out-of-process render in `AUHostingService` — **0 problems** |
| state save/restore | **333333** bytes (packed), round-trips |
| 4 instances at once | 2.13 s of audio in 3.17 s wall (37% CPU each) |
| render vs `render` | 0 of 2194 windows missing; envelope within ±10%, the same scatter the VST3 side shows |
| AU render vs VST3 render | aligned at shift 0.00 s, 0 windows missing |

The last line is the useful one: driven with identical MIDI at identical offsets,
the AU and the VST3 bundle produce the same music.

### `auval`

Apple's validator is stricter than either probe — it wants a specific set of
properties to answer, in specific scopes, with the right writability — and it
only sees components in the standard `Components` directories, so it needs
`make install-au` first. It now passes:

It is, however, an **in-process** host, which is why the element count below
got past it:

```
make install-au
auval -v aumu SMU2 Trbh
  ... AU VALIDATION SUCCEEDED.
```

Three things stood out while getting there, each worth knowing before changing
the AU again:

- **`sandboxSafe` has to be `false` in `Info.plist`.** With it set, `auval`
tries to open the component inside its own sandbox, and the ROM lookup — which
reads from a directory the user chose, not from the bundle — fails. The flag is
a promise the AU cannot keep here.
- **A missing property answer reads as a failure, not as "unimplemented".**  `auval` walks a fixed table of scopes and asks for each in turn; anything that
  comes back `kAudioUnitErr_InvalidProperty` is reported rather than skipped, so
  the wrapper has to answer the whole table.
- **The scope is part of the answer.** `kAudioUnitProperty_SupportedNumChannels`
  is documented as **Global**, and `DLSMusicDevice` — Apple's own `aumu` —
  refuses it for both Input and Output. `aubprobe` used to ask for it in Output,
  which accused a unit that answers exactly the way Apple's does. The probe now
  asks in Global, and additionally checks the refusals it and DLSMusicDevice
  share (`SupportedNumChannels`, `Latency` and `MaximumFramesPerSlice` for
  Output, `StreamFormat` for Input, `ClassInfo` for Output). When a scope
  question comes up, ask the reference: the pairs above were read off
  DLSMusicDevice rather than guessed.

### The A/D INPUT bus

Upstream's VST3 build has an `A/D Input` bus (the machine samples from it in
`engine::fill()`), and the AU has the same bus. It is stereo at element 0 on
the input scope, and the scope answers always: `SupportedNumChannels` says
`[2, 2]`, the stream format reads and sets at any rate (the input is resampled
at the output rate), and `SetRenderCallback` and `MakeConnection` are both
accepted. A graph connection is stored as the `AudioUnitConnection` it is,
never folded into the callback struct.

The one gate is `ElementCount` on the input scope: 0 by default, and while it
is 0 the engine is handed no input and the unit renders from MIDI alone. A
host that wants the bus writes `1`, which switches the pull on and feeds the
machine's A/D INPUT; writing `0` hands the bus back and drops the stored
callback.

Two contract details the pull keeps (both probed): the render's timestamp
travels with the pull (`auval`: `AU is not passing time stamp correctly`
otherwise), and a failing callback's error comes back out of `AURender` with
the block silenced rather than playing stale buffers.

Asking for the bus after the unit is initialised does not work in an
out-of-process host: its graph is built at initialise time, so the element is
missing from it and every render fails with `-10877` (`InvalidElement`). A
host adds the bus between `Uninitialize` and `Initialize`.

`aubprobe --torture` checks both states. Step 9 is the round trip in process:
ask for the bus, and the count, the format, the callback and the channel
matrix all answer, and the callback is pulled once per block; hand it back,
and the callback is dropped with the unit still rendering. Step 8 is the
out-of-process side: it instantiates the *installed* component with
`kAudioComponentInstantiation_LoadOutOfProcess` — the road a sandboxed host
takes, and the one place a counted-but-unconnected input element fails, since
that hosting layer builds its graph from the unit's bus counts and every
render then comes back `-10876` (`NoConnection`). The default unit renders
there, and a second instantiation that asks for the bus before initialising
renders with its input pulled (86 times over a second of 512-frame blocks).
Step 8 needs `make install-au` first, and makes the `S_MU2000_ROMS` path
absolute — the service inherits the environment but not the working directory.
(`auval` likewise only looks at `~/Library/Audio/Plug-Ins/Components`, so
`make install-au` first or its verdict is about the copy installed last time;
the probes are the other way round — `build/aubprobe` opens the bundle it is
handed.)

#### The real-time-safety harness

`auval -real-time-safety` — a deeper harness that renders on a real-time
thread under dtrace — no longer completes on current macOS (it hangs), so
plain `auval -v` is the validator that runs today. While it still worked it
taught one thing worth keeping: it built per-bus bookkeeping from the element
count while wiring whatever the input scope answered, so a unit whose scope
answered but whose bus was not counted made it crash inside itself. The
current design is untouched by that either way — the scope answers, the count
decides whether the engine is fed — and the harness is retired.

### Where the ROMs are looked for

The ROMs are Yamaha firmware the user supplies, so they cannot ship inside the
bundle. `find_roms()` in `src/vst3/engine.cpp` tries each of these in order and
takes the first directory holding `mu2000_flash.bin`:

| | where |
|---|---|
| 1 | `$S_MU2000_ROMS` |
| 2 | the bundle's `Contents/Resources`, then `Contents/Resources/roms` |
| 3 | next to the binary, and `<binary dir>/roms` |
| 4 | a `roms.txt` naming a directory, next to the binary or in `Contents/Resources` |
| 5 | `~/Library/Application Support/S-MU2000/roms`, that directory itself, and a `roms.txt` in it |
| 6 | `~/Documents/S-MU2000/roms` |
| 7 | `/Library/Application Support/S-MU2000/roms`, that directory itself, and a `roms.txt` in it |

Steps 5-7 are the macOS places a plug-in's own data belongs: the per-user
Application Support directory, Documents, and the **machine-wide** Application
Support directory. The per-user answers come first on purpose, so a user's own
copy beats a shared install. Steps 6 and 7 exist for the AU in particular: it is
one bundle in `Components`, used by every account on the machine, and it has no
window in which to be told a path. Step 7 is never written to — creating it
takes an installer with the rights to.

`S_MU2000_ROMS` is still the strongest answer, which is what the Makefile uses
(`ROMS ?= roms`). When nothing is found, the message and the log list every path
tried, so it is visible which one was expected.

`gui` is the exception: it takes the ROM directory as an argument and does not
search these. It is a program you run from a checkout, with somewhere to type.

## A note on the three Windows-only tools

`midisend`, `rec` and the old `--waveout` path in `live` are development aids
for comparing against a real MU2000 over a Windows audio stack. They are not
built on macOS yet. `live` on macOS uses CoreAudio unconditionally.
