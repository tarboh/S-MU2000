# AUv3 (macOS)

An Audio Unit v3 whose ports are the real machine's jacks. The sound is
`src/vst3/engine.h` -- the same engine the VST3 and the AUv2 run -- and the
editor is `src/vst3/panel_nsview.mm`, the same panel. Only the host interface
differs.

| | |
|---|---|
| output 0 | MAIN OUT L/R (PHONES and DIGITAL OUT carry the same signal) |
| input 0 | A/D INPUT (AD1 on the left, AD2 on the right) |
| MIDI in | cables 0-3 = MIDI IN A/B/C/D (parts 1-16, 17-32, 33-48, 49-64) |
| MIDI out | MIDI OUT (SCI ch0 of the SH7043); the firmware's replies |

Identity: `aumu` / `MU2k` / `Trbh`, shown as **tarboh: MU2000**.

**That is deliberately not the AUv2's** `aumu` / `SMU2` / `Trbh`. Two components
with the same type, subtype and manufacturer are one component as far as the
system is concerned, so the AUv3 keeps the vendor code and takes its own
subtype. Both can be installed at once, and a session saved against one still
opens the one it was saved with.

## Building

```
make auv3                     build/S-MU2000.app, with the .appex inside
make auv3 AUV3_ROMS=roms      the same, with the ROMs built in (see below)
make install-auv3             copy to ~/Applications and register
make auval-auv3               Apple's validator against the registered one
make autest                   the in-process host, no .appex involved
```

An AUv3 is **only recognised as an .appex inside an application**, so a carrier
application is built around it (`src/auv3/main_app.mm`). It makes no sound.
Launching it once is what makes the system find the extension; after that it
appears in a DAW's instrument list.

### Registration needs the App Sandbox entitlement

macOS does not register an app extension that is not sandboxed. Signing without
`com.apple.security.app-sandbox` fails in a way that is very hard to read:
LaunchServices sees the bundle, `pluginkit` never lists it, and `pkd` logs
nothing at all. **The kind of certificate is irrelevant** -- ad-hoc registers
just as well, which is why `CODESIGN_ID` defaults to `-`.

### The ROMs have to be inside the bundle

Being sandboxed means the extension can read **only inside its own bundle**.
`$HOME` is redirected into a container, so neither
`~/Library/Application Support/S-MU2000` nor anywhere a `roms.txt` points is
reachable, and no environment variable set in a terminal survives the crossing
either.

```
make auv3 AUV3_ROMS=roms
```

copies them into `Contents/Resources/roms` inside the .appex, **before** it is
signed (adding them afterwards breaks the seal). The ROMs cannot be
redistributed, so nothing is copied in by default: the plug-in then loads and
plays silence, with the reason in the log.

## Settings

There is no window and no terminal, so the switches are a file:

```
<settings>/S-MU2000/plugin.ini

  Windows   %LOCALAPPDATA%\S-MU2000\
  macOS     ~/Library/Application Support/S-MU2000/
  AUv3      ~/Library/Containers/com.tarboh.smu2000.auv3/Data/
              Library/Application Support/S-MU2000/
```

| key | default | what it does |
|---|---|---|
| `threaded` | 1 | run the SWP30 slave on its own thread |
| `usb` | 1 | use the USB port (all four MIDI ins, 64 parts); `0` is DIN A/B only |

Both are read by `engine::boot` and apply to every plug-in format. Whichever was
used goes into `log.txt` at boot, so a setting can be checked rather than
believed. **Inside the sandbox the file is the only way in** -- an extension is
another process and inherits no environment.

## Four MIDI ports

`virtualMIDICableCount` is `mu2000::MIDI_PORTS`, not a literal, so the AUv3
widens with the machine rather than having to be edited alongside it.

A and B are the DIN jacks. **C and D exist only over USB** -- on real hardware
they go through the M37640 microcontroller, and the emulator reaches them the
same way (`mu2000::set_usb_host`, which the engine turns on by default). Nothing
in the AUv3 has to know that: it hands the engine a port number and the engine
decides which road it takes.

Saying fewer cables than there are does not degrade gracefully. A host believes
what it is told, never uses the higher cables, and parts 17-64 simply cannot be
reached; some file-playing hosts instead open *one instrument per port*, booting
one MU2000 each. `build/autest` plays a different voice down each cable and
reports the peak per port, so all four landing on part 1 would show up as four
identical numbers rather than passing quietly.

## The boot snapshot

The firmware takes one to two seconds of real time to come up, and it comes up
the same way every time. So it is photographed once and restored after that.
The key is the program ROM's contents, the NVRAM at boot, and how long it was
run; change any of those and the snapshot is remade. A snapshot in an older
save format is refused by `load_state` and remade too.

The default stops at `midi_ready`. **Running past it does not change what
sounds:** booting for 8 seconds and for 30 and then playing the same piece
differed by 0.0-0.1% rms per second, with the same voices in the same order.
Stopping there also means the plug-in starts from **the same state `render`
does**, so a disagreement between the two is never about where they started.

`make auv3 AUV3_ROMS=roms` bakes a snapshot into the bundle as well, so even the
first insert comes up in milliseconds. It is made with an **empty `HOME`**,
because a sandboxed plug-in has no NVRAM of its own and the key would otherwise
not match.

## Waiting for the boot

`allocateRenderResources` waits for the machine to come up (the AUv2 does the
same in `Initialize`). Neither is the realtime thread, so waiting there is
allowed, and not waiting is worse than it sounds: every block would be silence
until the machine was up, and the MIDI arriving meanwhile would only pile up. In
a host that renders faster than realtime, that silence becomes the first
ten-odd seconds of the song, notes and all.

## The MIDI throttle, and why there is not one

The real MIDI IN is a 31250bps serial line: one byte is ten bits, 320us. A DAW
does not know that and will hand over dozens of program changes stamped at the
same instant. This branch carried a throttle that held them in a queue of its
own and released one byte at a time as the line cleared. **It has been removed**,
and this section is here so the idea is not reinvented from scratch.

It was measured first. Rendering the same file with it off and on gave
bit-identical output, for sixteen program changes plus a chord all stamped at
t=0, and for an XG SYSTEM ON immediately followed by the same burst -- the case
it was written for. Instrumenting the two branches confirmed each run really did
take the path it claimed.

The reason is that `mu2000::midi_in` **already** clocks the line at 31250bps:
bytes handed to it wait in the machine's own queue and the SCI shifts them out
at line rate regardless. The throttle only changed *which* queue a burst waited
in, and both hold 65536 bytes before dropping. It would matter under
`set_fast_midi(true)`, which hands bytes over without the line -- but no plug-in
turns that on.

There is a second reason not to bring it back, and it is now the decisive one.
MIDI IN C and D reach the machine over USB rather than a DIN line, and the USB
port is the default. The gate read `midi_queued(port)`, which is the **DIN**
queue; for a USB port that is always empty, so the throttle would have released
one byte per sample -- 44100 B/s -- and throttled nothing at all while looking
like it did.

## Checking it

`build/autest` registers the audio unit into its own process with
`registerSubclass:`, so the ports, the sound and the MIDI in both directions can
all be checked without the system registering anything:

```
build/autest                          the ports, both MIDI ins, MIDI out, A/D
build/autest --system                 the registered .appex, out of process
build/autest --smf song.mid out.wav   play a file through the plug-in
build/autest --state out.bin          write the booted state and stop
```

Rendering the same file twice and comparing the two WAVs is how the throttle
above was shown to make no difference; the same method works for anything else
whose effect is claimed rather than measured.

It renders at 48000Hz on purpose, so the resampler is always exercised.

`auval -v aumu MU2k Trbh` passes: 22050-192000Hz, 64-4096 frames, sliced
render, MIDI, all of it.

## Files

```
src/auv3/audio_unit.{h,mm}   the AUAudioUnit: buses, render, state
src/auv3/factory.mm          the principal class: AUAudioUnitFactory and the
                             editor's AUViewController in one
src/auv3/main_app.mm         the carrier application
src/auv3/autest.mm           the in-process host above
src/vst3/panel_nsview.{h,mm} the panel in an NSView, shared with the AUv2
src/ui/midi_split.h          cuts the MIDI OUT byte stream back into messages
packaging/auv3-*.plist       what the system registers
packaging/auv3-*.entitlements the sandbox and JIT entitlements
```
