# S-MU2000

## [日本語版はこちら](README.ja.md)

A software tone generator modeled on the Yamaha MU2000, designed to be played as a plug-in inside a DAW.

**[Try it out in your browser!](https://tarboh.github.io/S-MU2000/) _Note: needs ROMs extracted from your own MU2000_.**

![Screenshot](doc/mu_screenshot.png)

**Current state: runs as VST3 / CLAP (Windows, [Linux](#building-on-linux)) and VST3 / Audio Unit (macOS), with a hardware-style front-panel screen plus a mouse-and-keyboard editor.**


This project is developed in the open, work-in-progress and all. On X, follow `#S_MU2000`.

> An unofficial project, not affiliated with Yamaha. Yamaha, MU2000 and XG are trademarks of Yamaha Corporation.

## Hardware-derived data: do not distribute or post

**This repository publishes only source code — the emulator and the ROM-extraction tools.**
It contains no Yamaha ROMs, waveform data, or firmware, in the repository or in any release,
and it never will. The ROMs needed to run it must be **extracted by each user from their own MU2000**.

Publishing source code and sharing data taken from the hardware are **two separate matters**;
this project does only the former. Users are asked to do the following:

* Do not redistribute dumped wave ROM or program ROM images anywhere, including GitHub.
* Do not post hardware-derived ROM images in Issues, Pull Requests, Discussions, Releases,
  or attachments. Hashes, logs, MIDI files, and recordings are enough for bug reports.
* Do not distribute custom firmware created during extraction, modified firmware images,
  or custom `.ydl` files (dumper tools, etc.). All of them contain genuine Yamaha firmware.
* Obtain Yamaha's updater (`mu2r1_uw.zip`) from Yamaha's own download page.

These hardware-derived files are handled separately from the S-MU2000 source code and are
not covered by "Origins and license" below.

The stock firmware runs as-is and receives MIDI to produce sound. Sound quality is checked by
recording the real hardware over S/PDIF and comparing directly, band by band in 1/3-octave steps:

* XG's 128 GM voices plus 1225 other XG voices (at C2, C4, C6). At C4, 1109 of the 1225 voices
  match within 1.5 dB. Of the remaining 116, 60 vary by about the same amount when the same
  hardware unit is simply re-recorded.
* 100 performances in Performance mode. Mean band difference 0.61 dB
  (hardware-to-hardware repeatability is 0.22 dB).
* All system reverb/chorus types, variation run as a send, controllers,
  per-drum-instrument NRPN, multi EQ, and voice stealing past 128 voices.

What still mismatches, and how it was investigated, is recorded in [doc/todo.md](doc/todo.md).

A JIT (translating SH2 and MEG instructions to machine code) is included; on a test song with
all 16 parts playing continuously, CPU usage is about 22% of real time
(Ryzen 7 9700X, average per block). The LCD shows exactly what the firmware draws,
and the buttons and dial are clickable.

## What this is

It emulates the MU2000's internals (SH7043 CPU + two SWP30 tone-generator chips) in software
and runs the genuine firmware unmodified. Because it is an emulator, both the voices and the
behavior come from the real hardware.

MAME can also sound the MU2000, but MAME keeps its own clock and tries to catch up with real
time, so driving it from a DAW or an external sequencer accumulates delay until it falls apart
(measured: average speed stuck below 100% despite 177% of spare processing power).
A soft synth is driven by the host's audio callback, so this problem cannot occur in principle.

Details are in [doc/design.md](doc/design.md). What remains is listed in priority order in
[doc/todo.md](doc/todo.md).

## ROMs

**No ROMs are included.** You must extract them from your own MU2000.
Handling of dumped data follows
["Hardware-derived data: do not distribute or post"](#hardware-derived-data-do-not-distribute-or-post) above.

| ROM         | Contents             |
|-------------|----------------------|
| Program ROM | 4 MB (unit firmware) |
| Wave ROM    | 32 MB (voice data)   |

**Extraction steps and tools are in [doc/dump/](doc/dump/).**

Place the following in the `rom` directory:

| File                           | Contents                                |
|--------------------------------|-----------------------------------------|
| `mu2000_flash.bin`             | 4 MB program ROM (as seen from the CPU) |
| `dump/xv364a0.ic49` and 3 more | Wave ROM, 8 MB × 4                      |
| `standin/sin-table.bin`        | 64 KB sine table used by the MEG        |

The GUI and the plug-ins look for this directory in the same places (the `S_MU2000_ROMS` environment
variable, the settings folder's `roms` or a `roms.txt` there naming the folder, next to the program, …).
**Starting the GUI without a folder** (double-clicking it, say) searches those places, and if nothing is
found it asks you to choose the folder and writes it to `roms.txt` in the settings folder, so from then on
the GUI and the plug-ins both find the ROMs.

* You **don't need to dump the program ROM**. It can be reconstructed from Yamaha's published
  updater (`mu2r1_uw.zip`). The contents are a MIDI file of Flash-write SysEx messages as-is,
  which reassemble to the SHA1 registered in MAME.
* The wave ROM takes **about 36 minutes over a single USB cable**. No disassembly and no MIDI
  interface needed. A home-built dumper is written only into the unit's firmware area and reads
  data via the SWP30's wave direct access, streaming it out over USB. The downloader is untouched,
  so the official updater restores everything at any time (though rewriting firmware is at your own risk).

A spare MIDI-based path also exists, and dumps from both paths have been verified
byte-identical.

## Getting Started

See [doc/manual.en.md](doc/manual.en.md).
Japanese: [doc/manual.md](doc/manual.md)

It explains:
- What you need.
- How to build the project.
- How to play some songs.
- How to use the GUI.
- How to set it up with a DAW.


### Command Syntax

```
build/gui.exe    <rom directory> [--midi number] [--midi-b/-c/-d number]  Play with a hardware-style panel
                 [--host-midi]                      Receive on DIN ports (A and B only) instead of USB
                 [--play song.mid]                  Play a MIDI file
                 [--exclusive] [--audio name] [--latency ms]  Audio output (see "Latency" below)
                 [--editor] [--list-window]         Start with the PC-operated windows open
                 [--factory] [--nomidi] [--fast-midi]
build/gui.exe    <rom directory> --lcd              Play with an LCD-only screen
build/gui.exe    --list                             List MIDI inputs/outputs and audio outputs
build/live.exe   <rom directory> [--midi number] [--fast-midi]  Play MIDI input with no screen
build/live.exe   --list                             List MIDI inputs
build/render.exe <rom directory> <MIDI> <output wav> [seconds]  Render a file to WAV
                 [--reset gm|gs|xg]                 Explicitly insert a reset at the start
                 [--usb]                            Start on USB ports, routing song ports 1-4 to A-D
                 [--fast-midi]                      Deliver MIDI as fast as the firmware can read
                 [--card image.img] [--adc-in input.wav]  Insert SmartMedia / feed A/D INPUT
                 [--sample-rate Hz] [--bit-depth 8|16|24|32]  Output format (default 44100 Hz, 16-bit; 32 is float)
                 [--gain x] [--normalize]           Scale the level / use the full range
                                                    Run a tool with no arguments for the full list of options;
                                                    add -jp for messages in Japanese
build/panel.exe  <rom directory> [--keys "play,edit"] [--list]  Drive the panel as text only
build/boot.exe   <rom directory> [cycles]           Boot check
build/statetest.exe <rom directory> [MIDI]          Check that state save/restore is exact
build/blocktime.exe <rom> <MIDI> <frames> [sec] [reps]  Measure time per block
build/midisend.exe <MIDI file> [--port number]      Stream to a MIDI output in real time
build/rec.exe    --list                             List audio inputs
build/rec.exe    <number> <wav> <sec> [--send <number> <MIDI>]  Record the real hardware
```

### Tips

**To play it from an external sequencer such as Domino, see
[doc/domino.md](doc/domino.md).** All you need is one virtual MIDI cable (loopMIDI).
`gui.exe` lets you pick inputs and outputs **while running, from the screen** —
press the panel's `MIDI IN A` jack, or right-click and choose Settings → MIDI.

Inputs are **4 ports, A–D** (parts 1–16, 17–32, 33–48, 49–64). It starts in the same shape as
the real hardware with HOST SELECT set to USB, so C and D — USB-only on the hardware — work too.
With `--host-midi` it starts on the DIN ports (A and B, 32 parts). Each port also has a selectable
THRU output. From a single input, sending the cable message `F5 nn` (nn = 1–4) routes subsequent
messages to ports A–D (same convention as TO HOST on the MU128 and similar units.
The real MU2000 ignores `F5` sent over USB. [doc/dump/usb.md](doc/dump/usb.md)).
Selections are remembered in `%LOCALAPPDATA%\S-MU2000\gui.ini`. The panel VOLUME knob position
lives there too (on the hardware it is an analog knob, outside firmware RAM).

Besides the panel there are mouse-and-keyboard windows (list, editor, insertion setup,
part voices; F2 / F3 or right-click. [doc/pc-editor.md](doc/pc-editor.md)).
The SmartMedia slot and sampling A/D INPUT also work ([doc/gui.en.md](doc/gui.en.md)).
MIDI files can be dropped onto the window or played with `--play`.

**MU2000 settings survive a power cycle.** The same battery-backed RAM as the real hardware is
saved by `gui` and `live` on exit under `%LOCALAPPDATA%\S-MU2000\nvram\` and reused at the next
launch. Utility settings as well as values like the XG master volume persist
(because the genuine firmware is written that way). Plug-ins (VST3 / CLAP / AU) only **read**
this area: when inserted, they start from settings made in gui / live (changes made inside the
plug-in stay in the DAW project). To restore factory state, start with `--factory`, open
the panel's POWER menu and choose Factory reset, or just delete the files.

Screen contents are described in [doc/gui.en.md](doc/gui.en.md). There are 3 faces.
**The panel artwork can be fixed without redrawing** — positions and colors are factored out
into a text file, `panel.txt` ([doc/panel-editing.md](doc/panel-editing.md)).

* **Panel** … the physical front panel (LCD, 35 buttons, large dial)
* **Editor** … SOL2-style: per-part filter, EG, effect sends, voices
* **Effects** … reverb / chorus / variation plus 2 insertion systems
  (addresses verified by measurement. [doc/effects.md](doc/effects.md))

The plug-in screen is the same.

`live` advances the tone generator only by what the audio device requests. It keeps no clock of
its own, so it never drifts against external sync (this is where MAME fell apart).

**Output is opened in whatever format the device reports.** On hardware reporting 48000 Hz,
conversion from 44100 is done with our own sinc resampler (never through the Windows converter).

### Using it in a DAW

Build steps and ROM placement: [doc/vst3.en.md](doc/vst3.en.md). Per-DAW notes:
[doc/reason.md](doc/reason.md) (Reason), [doc/sonar.md](doc/sonar.md) (Cakewalk Sonar).

**VST3.** Installed in `%LOCALAPPDATA%\Programs\Common\VST3` (per-user) or
`C:\Program Files\Common Files\VST3` (all users). ROMs can't be bundled, so write their
location on one line in the bundle's `Contents/Resources/roms.txt`.
Inputs are 4 lines matching MIDI IN A–D on the hardware (64 parts). On hosts like Cubase that
handle MIDI program changes through the `IUnitInfo` voice list, voices still switch per part.

**Voice and effect settings stay in the project, and part volume / filter / EG / EQ and master EQ
are exposed as named parameters for automation** (VST3 / CLAP; values touched on screen propagate
to the host). [doc/automation.md](doc/automation.md).

```
make vst3             Bundle appears in build/S-MU2000.vst3/
make install-vst3     Copy it to the VST3 location
make probe            Load and sound it without a DAW
build/vst3probe.exe <DLL inside the bundle> --torture
                      Try every hostile host call pattern (DLL is at Contents/x86_64-win/S-MU2000.vst3)
```

**CLAP** (verified on Windows. `make clap` for macOS also exists but hasn't been tried in a macOS
host yet). Same DSP as the VST3 build; MIDI is received as a byte stream. Note input is also
4 ports, A–D. Installed in `C:\Program Files\Common Files\CLAP` (all users) or
`%LOCALAPPDATA%\Programs\Common\CLAP` (per-user). Write the ROM location on one line in
`roms.txt` next to `S-MU2000.clap`, or in `%LOCALAPPDATA%\S-MU2000\roms.txt`.

```
make clap             Builds build/S-MU2000.clap
make install-clap     Copy it to the CLAP location
build/clapprobe.exe build/S-MU2000.clap <MIDI> <output wav>
                      Sound it without a DAW (ROM location can also come from S_MU2000_ROMS)
```

Plug-ins also start on USB ports (A–D) by default. To return to DIN ports (A and B),
write `usb=0` in `%LOCALAPPDATA%\S-MU2000\plugin.ini`.

**macOS** (Apple silicon) can build VST3 and Audio Unit (AUv2, `aumu`).
`make` builds the tools and both bundles. Details: [doc/porting-macos.md](doc/porting-macos.md).

## Standalone settings

Open **Settings** from the toolbar, any quick menu, or `--settings`.
General selects English or Japanese. Audio selects playback and recording devices,
sample rate, buffer size, latency, output channels, resampler, volume, DC filtering,
and peak limiting. Changes apply immediately; failed changes restore the previous
stream. Device lists refresh automatically.

Emulation contains C++ effects, firmware bypass, and MIDI-file lightening.
Hover over each option for details. The existing WASAPI, CoreAudio, and ALSA
backends are used; this adds no DAC or circuit simulation.

Windows builds can also enable DirectSound and installed ASIO drivers with
`make ASIO=1`, or CMake with `-DASIO=ON`. This adds a Driver selector and an
ASIO control-panel button to Audio settings. Only this opt-in build needs CMake 3.18+ and downloads the ASIO
SDK. The default build uses neither PortAudio nor the SDK. See
[build notes](third_party/portaudio/S-MU2000.md) for details.

## Latency

**Measured 117 ms → 16 ms end to end** (MIDI in to sound out, measured from waveforms
side by side with the hardware). Usable for live playing.

| End-to-end, measured      | Before  | Now       |
|---------------------------|---------|-----------|
| Exclusive (`--exclusive`) | 117 ms  | **16 ms** |
| Shared mode               | ~110 ms | **50 ms** |

50 ms in shared mode is about the slap-back delay of a hand clap in a large room. Unless you play
side by side with the hardware, shared mode is still playable.

Three things matter:

**1. `--exclusive` (take over the device)**

Bypasses the Windows mixer. On the same machine the difference is this large.

The table below is "how much is written but not yet heard" from our side. The end-to-end figures
above add the interface and air on top of this.

|                                  | Time to sound (written but unheard)             |
|----------------------------------|-------------------------------------------------|
| Shared, `--latency 20` (default) | 48 ms                                           |
| Shared, `--latency 10`           | **38 ms** (floor for shared)                    |
| Exclusive, `--latency 10`        | **11.6 ms** (exactly one period, nothing extra) |

In exchange, other apps can't make sound while it plays.

**There is no going below 38 ms in shared mode.** 10 ms of that is our own buffering (can't go
below one period), and the remaining ~28 ms sits in the Windows mixer and driver.
Neither the `IAudioClient3` low-latency path (441 frames fixed on this machine, same as default)
nor `--raw` (skipping the engine's signal processing) helped.

For shared mode, use `--latency 10`. **Thicker buffering doesn't make it safer**
(the period sets the deadline, so 20 ms and 10 ms have the same headroom).
The only difference is whether one wholly missed signal survives.

**2. Name the audio output (`--audio`)**

Windows' "default playback device" changes on its own (it actually moved while touching settings).
Check names with `--list` and specify with **part of the name**, e.g. `--audio "Analog (3+4)"`.
Names — not numbers — are remembered for the same reason as MIDI ports.
When started from the screen it is remembered in `gui.ini`, so you only set it once.

**3. Interface-side settings**

These can't be touched from S-MU2000. On RME, in Fireface USB Settings:

* **Buffer Size** … 1024 samples costs 21 ms. **Bring it down to 256.**
* **Set sample rate to 44100** … the MU2000 only runs at 44100. Setting the card to 44100 too
  removes our conversion entirely (reported as `no conversion (stays at 44100)`).
  Comparing against the hardware over S/PDIF is meaningless unless rates match in the first place.

### Choosing `--latency`

**The target buffering length.** In exclusive mode it is **rounded to the device unit
(a power-of-two frame count)**. Pro interfaces run at 128 / 256 / 512 / 1024, and a length that
doesn't divide evenly comes out choppy (this actually happened with 480 frames).

|                            | Period        | Worst-case engine | Result              |
|----------------------------|---------------|-------------------|---------------------|
| `--exclusive --latency 10` | 11.6 ms (512) | 5.4 ms            | No dropouts         |
| `--exclusive --latency 5`  | 5.8 ms (256)  | 3.4 ms            | Occasionally misses |

Headroom must stay longer than the engine's worst single block or sound breaks up. If "missed
deadline" reports grow, lengthen `--latency`. Single-block time can be measured with
`build/blocktime.exe`.

The latency figures and tables in this section were measured before the JIT (2026-09-13) and have
not been re-measured yet. The engine has since gotten faster: running the 16-part simultaneous
test song `dense` in 512-frame blocks averages 2.5 ms with a 7.2 ms worst case
(2026-09-17, Ryzen 7 9700X).

## Building

Also consider reading [doc/manual.en.md](doc/manual.en.md).

Windows expects MSYS2 / MinGW-w64 g++, macOS expects Apple clang++, Linux expects g++.
C++20 is required. `make test` runs the regression tests ([doc/testing.md](doc/testing.md)).
Machines without ROMs still run the subset that needs no ROMs.

### Building on Linux

> **On the Linux screen and plug-ins (PR [#33](https://github.com/tarboh/S-MU2000/pull/33))**
> The Linux `gui`, VST3, and CLAP were contributed by spessasus, and the author — who doesn't use
> Linux — **cannot test, support, or take responsibility for them**. Use them at your own risk.
> Bug reports and fixes from Linux users via issues and PRs are welcome.

On Debian/Ubuntu install the first line, on Arch the second:

```
sudo apt install build-essential libasound2-dev libcairo2-dev libfontconfig-dev libsdl3-dev
sudo pacman -Sy base-devel alsa-lib cairo fontconfig sdl3
```

`make` builds the full toolset plus `gui`, VST3, and CLAP under `build-linux/`. ROMs go in
`roms/` (same contents as ["ROMs"](#roms)).

```
build-linux/gui roms                       Play with a hardware-style panel (English UI; F2/F3 for PC windows)
build-linux/gui roms --boot --shot out.png Render just the screen artwork to a file
make vst3 / make clap                      DAW plug-in builds (generic screen)
make probe                                 Load and sound plug-ins without a DAW
make test                                  Regression tests (need ROMs + numpy; Debian: python3-numpy, Arch: python-numpy)
```

VST3 / CLAP find their ROMs via the `S_MU2000_ROMS` environment variable or a `roms.txt` next
to the bundle (for installs under `~/.vst3` / `~/.clap`, use `~/.local/share/S-MU2000/roms.txt`).
Latency, MIDI selection, and other usage match the Windows build.
Details: [doc/linux.md](doc/linux.md) (tools and `live`) and
[doc/porting-linux-gui.md](doc/porting-linux-gui.md) (screen).
The Windows exes are statically linked so they **don't depend on MSYS2 DLLs**
(left dynamically linked, they silently exit when started from plain PowerShell).

The SH2 and MEG JITs cover both x86-64 and arm64. `midisend` and `rec` (tools for comparing
against the hardware) are Windows-only. For the macOS build, see [doc/porting-macos.md](doc/porting-macos.md).

## Translations

The window, menu and tooltip texts are plain files under [`locale/`](locale/), one folder per language
(English and Japanese so far). Adding or improving a language needs no programming, and you can try your
texts in the program without building it: see [locale/README.md](locale/README.md).

## Origins and license

The core chip implementations are **taken from MAME**. MAME as a whole is GPL, but every
individual device implementation used here is **BSD-3-Clause**, which permits reuse.

| Import                     | Copyright                                           |
|----------------------------|-----------------------------------------------------|
| `src/mame/sound/swp30.*`   | MAME `src/devices/sound/swp30.*` — Olivier Galibert |
| `src/mame/cpu/sh*`         | MAME `src/devices/cpu/sh/`                          |
| `src/mame/machine/sci4.*`  | MAME `src/devices/machine/sci4.*`                   |
| `src/mame/video/hd44780.*` | MAME `src/devices/video/hd44780.*` — Sandro Ronco   |
| `src/mame/ymmu2000.cpp`    | MAME `src/mame/yamaha/ymmu2000.cpp`                 |

`src/mame/cpu/sh*` is by Olivier Galibert and David Haywood; the panel artwork
(`art/mame/`) is by hap and Felipe Sanches (CC0-1.0). Changes on our side are marked `S-MU2000:`.

Imports track MAME 0.289 equivalent (master 2026-09-06, commit `1fb001f9`).
MAME itself is not linked (because of the GPL). VST3 uses only the interface definitions (MIT);
the SDK proper, dual-licensed GPLv3 + Steinberg proprietary, is not used.

Bundled attributions are collected in [NOTICE.txt](NOTICE.txt).

The VST3 interface definitions (`third_party/vst3/pluginterfaces`) are Steinberg's but
distributed as **MIT**. The GPLv3 `public.sdk` is not used, so the entire plug-in foundation
lives inside this repository. Details: [third_party/vst3/README.md](third_party/vst3/README.md).

The CLAP headers (`third_party/clap`, Alexandre BIQUE, **MIT**) are also imported unmodified.
Details: [third_party/clap/README.md](third_party/clap/README.md).

The PC editor windows in gui.exe are drawn with Dear ImGui (`third_party/imgui`, Omar Cornut,
**MIT**), imported unmodified.

This repository's own code is BSD-3-Clause.

## Contributions upstream

Bugs found in MAME's SWP30 while voicing the MU2000 are fixed against hardware measurements and
sent back to MAME. Everything found, and whether it was sent, is collected in
[doc/upstream.md](doc/upstream.md).

Merged (as of 2026-09-17):

| PR                                                               | Contents                                                                                                                                  |
|------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------|
| [mamedev/mame#16075](https://github.com/mamedev/mame/pull/16075) | Loop-length mask width (voices changing one after another on long tones) and pitch clamp (pitch sticking at maximum 150 ms after note-on) |
| [mamedev/mame#16115](https://github.com/mamedev/mame/pull/16115) | iir2 register order, missing DPCM accumulation                                                                                            |
| [mamedev/mame#16140](https://github.com/mamedev/mame/pull/16140) | Made the reverb-RAM enable register readable                                                                                              |
| [mamedev/mame#16141](https://github.com/mamedev/mame/pull/16141) | 3 places where the MEG DRC disagreed with the interpreter                                                                                 |
| [mamedev/mame#16142](https://github.com/mamedev/mame/pull/16142) | Absolute addressing for MEG memory reads                                                                                                  |
| [mamedev/mame#16143](https://github.com/mamedev/mame/pull/16143) | Sample interpolation order for reverse playback                                                                                           |

Under review: [#16144](https://github.com/mamedev/mame/pull/16144) (truncation of the voice-volume
product) · [#16150](https://github.com/mamedev/mame/pull/16150) (DPCM fraction) ·
[#16151](https://github.com/mamedev/mame/pull/16151) (MEG ALU edge cases found via distortion
effects) · [#16154](https://github.com/mamedev/mame/pull/16154) (R/W of chopped reverb-RAM partitions).
