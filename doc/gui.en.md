# The display

`gui.exe` and the VST3 display are **the same thing** (`src/ui/panel.*`). It
draws with GDI alone; from outside, one HDC is handed in. There are two
faces.

```
make            builds gui.exe along with the rest
build/gui.exe <rom folder> [--midi N] [--midi-b N]
              [--midiout N] [--midiout-b N]
              [--latency ms] [--size 1000x400] [--lcd]
build/gui.exe --list                      list MIDI inputs and outputs
build/gui.exe <rom> --boot --shot pic.png  write a picture without opening a window
build/gui.exe --shot pic.png --grid        picture only, no ROMs, with a grid overlaid
build/gui.exe --dump-layout panel.txt     write out the current layout
build/gui.exe <rom> --layout panel.txt    run with that layout (F5 reloads it)
build/gui.exe <rom> --play song.mid       run while playing a MIDI file
build/gui.exe <rom> --lcd                 show only the LCD, filling its own window
```

`--lcd` hides the panel, buttons and status line and shows just the LCD and
its bezel. The default window is 898x290. `--size WxH` and `--shot` can be
combined with it.

**The windows you operate from the PC are separate.** The list (F3,
right-click, `--list-window`) and the editor (F2, right-click, `--editor`).
See [doc/pc-editor.md](pc-editor.md).

**The panel artwork can be adjusted without redrawing it.** Positions and
colours live in a text file, `panel.txt`. The procedure is in
[doc/panel-editing.md](panel-editing.md).

## SmartMedia

Left- or right-click the **card slot** (bottom left) for a menu. The top
three entries are SmartMedia.

* `Create a new SmartMedia and insert it` → 16 MB / 32 MB / 64 MB / 128 MB. Choose where to save it and a card file (`.img`) is created and inserted. It comes formatted the same way `UTIL → CARD → Format` leaves a card, so it can be saved to right away. While a card is in, its edge shows in the panel's slot
* `Insert SmartMedia...` inserts a card file made earlier
* `Eject SmartMedia` writes back to the file, then ejects

SAMPLING's SAVE / LOAD (ALL+SEQ and so on) and UTIL's CARD work. What the
firmware writes is flushed to the file every two seconds, on eject and when
the GUI closes (changed blocks only). The inserted card is remembered in
gui.ini and inserted again at the next start.

The file holds the raw layout of the SmartMedia's NAND (512-byte pages plus
16 spare bytes). Move files in and out from the PC with `tools/smcard.py`:

```bash
python tools/smcard.py ls smartmedia.img
```

```bash
python tools/smcard.py put smartmedia.img drums.wav
```

```bash
python tools/smcard.py get smartmedia.img TAKE001.WAV
```

There are also `info` (capacity and free space) and `rm` (delete). A WAV or
AIFF you put in can be loaded in the unit with SAMPLING → LOAD → WAV, and
what SAMPLING → SAVE → WAV writes can be taken out with `get`. Names are 8.3
only (long names are never created). **Do not modify a card while the GUI
has it inserted** (the GUI writes back every two seconds, so one side's
writes are lost). Eject, change, reinsert.

`render` also has `--card card.img` (written back at the end).

The route from sampling to playing the recording as a MIDI voice is in
[sampling.md](sampling.md).

## Playing a MIDI file

The lower half of the same menu.

* `Play MIDI file...` chooses a file and plays it
* `Stop` stops and silences anything still ringing (All Note Off and damper release)

* `Play ports 3 and 4 on top of A and B` / `Do not play ports 3 and 4` control how files with three or four ports are handled (below)
* `Lighten heavy MIDI: thin pitch bends, drop Roland display data (unlike the real unit)` is off by default. The MU2000 firmware works through only about
  1,500 pitch bends a second; a denser stream falls behind and catches up later, on the real unit as here. When on, the
  player sends at most one bend every 4 ms per channel (the latest value), and the latest value before any note on that
  channel. It also leaves out Roland SC display messages (`F0 41 dd 45 12 …`, text and bitmaps for the SC's screen): the
  MU2000 does not use them, and songs that animate the SC screen send over 10 KB/s of them, more than the MU2000's USB
  input takes. Remembered in gui.ini

**Dropping a MIDI file on a window** also plays it (the panel window, or the
editor, list and insertion-settings windows). If several are dropped, only
the first plays. A song already playing is stopped and the new one started.

Each track's destination follows the SMF port meta event (`FF 21`), or a
device-name meta event (`FF 09`) of `A`-`D` / `Port 1`-`Port 4`. Port 1 is
MIDI IN A (parts 1-16), port 2 is B (17-32), port 3 is C (33-48), port 4 is
D (49-64). **To use ports C and D, start with the USB port** (see "Choosing
MIDI ports" below). When the USB port is not in use, ports 3 and 4 of a
three- or four-port song are by default played on top of A and B (parts on
the same channel mix). Choosing `Do not play ports 3 and 4` plays ports 1 and
2 only. The choice is remembered in gui.ini.

To play from the start:

```bash
build/gui.exe <rom folder> --play song.mid
```

The destination is the same route as the editor's controls (the `ui::bridge`
ring), so touching the panel does not interfere. **This is the one place that
has a clock**, but it is natural for the sender of a score to hold the clock;
it is in the same position as an external sequencer plugged into the real
unit with a MIDI cable. The tone generator advances, as ever, only as far as
the audio device asks.

### The MIDI player window

Open it with `Player` on the bar at the top of the window (or `--player-window`). It keeps a playlist and can pause,
seek, step between songs and loop (issue #124). `Play MIDI file...` in the menu, files dropped on a window and `--play`
all add to this list before playing, so whichever way a song starts it can be controlled here.

* **The list**: `Add files...` picks any number of files (or type a path in the box below and press `Add`).
  Double-click a row to play it from the top, drag a row to reorder, `x` removes it, `Clear the list` removes all.
  The song's own name is shown when the file has one (the first track-name event `FF 03` of the first track; port
  names such as `PartA` are not taken as a title; Shift_JIS titles are converted). Otherwise the file name
* `Prev` / `Next`: the previous or next song. `Prev` returns to the top of the current song when it is more than
  two seconds in
* `Pause` / `Play`: pausing silences what is sounding and waits in place
* **Loop**: `No loop` (play the list once and stop), `Loop the list`, `Loop one song`, `Shuffle` (random order,
  shuffled again each time round). There is a one-second gap between songs
* **The position slider**: drag and release to jump there. A jump first sends **only the settings** up to that point
  (SysEx, bank and program changes, RPN/NRPN and data entry all in order; other controllers, pitch bend and channel
  pressure only as their last value; no notes), then waits until the tone generator has read them (counted at
  10,000 bytes a second for the USB port and 3,125 for DIN) before playing on. Songs with a lot of SysEx take a
  moment; `catching up the settings...` is shown meanwhile
* **Bar, beat and tempo** are counted from the song's time signatures (`FF 58`) and tempo changes (`FF 51`)

The plugins (VST3 and so on) do not have this window; there the DAW plays the song.

## Choosing MIDI ports

**Choose ports while running** from the panel's `MIDI IN A` menu or Settings → MIDI.
The MIDI IN menu also has GM, GS / TG300B and XG resets for the emulator, plus Panic.

Right-click anywhere to open the list (F3), the editor (F2), or Settings.
Settings → Emulation contains Play effects in C++ and Play without the firmware.

The **POWER menu** has Restart and **Factory reset...**. After confirmation,
factory reset discards the remembered settings (work RAM, `src/nvram.h`)
and powers up again. The start-up argument `--factory` does the same.

### Fast start-up (a snapshot after boot)

The firmware's boot takes about eight seconds of audio time and close to a
second of real time. **The fully booted state is kept and loaded next time**
(`src/bootcache.h`). It lives in `%LOCALAPPDATA%\S-MU2000\boot\<key>.bin`,
where the key is made from the firmware, the work RAM used at boot, the wave
ROM and the state-format version. If any of those change the key changes,
so a stale snapshot is never read. Delete it and the next start makes it
again.

**The restored state is bit-identical to booting all the way.** Rendering the
same MIDI both ways gives WAVs that match byte for byte
(`render --bootcache`).

The unit's DIN sockets are **two MIDI IN-A, IN-B, OUT and THRU**. Over USB
only there are also **IN C and D** (parts 33-64) and **OUT B onward**. Here
we expose the **four IN ports A-D** and the **OUT** the MU2000 itself sends
on, each chosen separately (THRU A / B are this side's outlets that pass
received data on).

| | Contents |
|---|---|
| MIDI IN A | receives here to play **parts 1-16**. Where Domino and the like connect ([doc/domino.md](domino.md)) |
| MIDI IN B | likewise **parts 17-32**. For Domino's MIDI OUT B and so on |
| MIDI IN C | **parts 33-48**. A USB-only port on the hardware |
| MIDI IN D | **parts 49-64**. Likewise USB-only |
| MIDI OUT | **what the MU2000 itself sends**. The transmit line of SCI channel 0, the same as the unit's OUT socket. Replies to XG requests and dump requests come out here. Passed through loopMIDI to an external editor, it can be read and written from outside |
| MIDI THRU A | passes what A received **straight out**. Control changes and SysEx from the display's controls go out with it |
| MIDI THRU B | passes what B received out. Not mixed with A, so a tone generator connected outside keeps its part assignment intact |

### A/D INPUT (audio for sampling)

Open the panel's `A/D INPUT` menu or Settings → Audio to choose the
**recording device** (WASAPI shared mode).
Left is AD1, right is AD2. Anything other than 44,100 Hz is converted
internally. The chosen name is remembered as `audio_in` in `gui.ini` and
opened at the next start. The default is "not used". It feeds sampling
(SAMPLING → REC) and the A/D parts. The A/D part volume defaults to 0, so
raise it when you want to hear the input.

**When comparing with the real unit**, connect MIDI THRU A to the unit's
first port and MIDI THRU B to its second. The same data from the sequencer
reaches the same parts here and on the hardware.

Choices are remembered **by name** in `%LOCALAPPDATA%\S-MU2000\gui.ini`.
Remembering by number would connect to a different device after a USB device
is replugged (replugging the MU2000 really does reorder the list).
`--midi` / `--midi-b` / `--midi-c` / `--midi-d` / `--midi-e` / `--midiout` (THRU A) /
`--midiout-b` (THRU B) / `--midiout-mu` (OUT) take precedence when given.

**A port that could not be opened at start-up keeps its remembered name.**
Forgetting to launch loopMIDI, or not having the device plugged in, should
not lose the port you chose. Choosing again from the menu replaces it.

### Audio output (the PHONES jack)

Left- or right-click the `PHONES` jack on the panel for the audio output
menu, with playback device, stream sample rate and Limit output peaks controls.
These are also in Settings → Audio. The digital/analog choice is remembered
as `output` (`digital` / `analog`) in `gui.ini`. The default is digital.

| Output | Contents |
|---|---|
| Digital | the same as the unit's S/PDIF. The DC that some DPCM samples carry (A2 and A5 of Alto Sax Legato, for instance) comes out as it is. Recorded from the unit's S/PDIF and compared, even the DC level matches to 0.1 LSB (issue #3) |
| Analog | meant as LINE OUT / PHONES: DC is removed after the DAC (before the VOLUME knob) with a first-order high-pass (`src/analog_out.h`). Right after a sound with DC is released, a slow swing appears as what was removed comes back |

**The analog cut-off (3 Hz) is a placeholder.** The unit's analog output has
not been recorded yet; once measured, set `analog_out::CUTOFF_HZ` to match.

### When a port does not respond, or MIDI floods

**Opening a port is given up after two seconds.** If the port's owner is
hung, Windows's "open" call never returns. In practice, after Reason crashed
holding a loopMIDI port, the GUI appeared to stall part way through
start-up. It now prints "not responding (loopMIDI or a driver may be hung)"
and moves on. Relaunching loopMIDI or replugging the device fixes it.

**THRU never sends more than twice a MIDI cable's rate (6,250 bytes per
second).** A MIDI loop between a DAW and loopMIDI multiplies messages
without limit; passing that through THRU to the real unit hung the unit
too. The excess is dropped by whole messages and "too much MIDI, dropped" is
printed once a second. When you see that, suspect a loop. The MU2000's
receive buffer also has a cap (4 MB), but that is a brake against loops and
real playing never reaches it. It used to drop at 65,536 bytes, and turning
a wheel continuously in a DAW dropped note-offs and left notes ringing. The
real unit loses nothing at the same volume over USB; it just processes it
all late (issue #18, `src/mu2000.h`).

### Ports C and D (parts 33-64)

IN C and D are USB-only on the hardware, received by the M37640
microcontroller on the USB side. Its ROM has not been dumped, but **the SH-2
sees only two addresses and two interrupts**, so a stand-in was put there to
make them pass ([doc/dump/usb.md](dump/usb.md)).

The firmware **passes C and D only when HOST SELECT is USB**, so **the GUI,
VST3 and CLAP all start with the USB port by default** (the same as
connecting the unit to a PC). To return to the DIN ports A and B only, use
`--host-midi` for the GUI or write `usb=0` in `plugin.ini` for the plug-ins.
**It is decided before boot**, so restart after changing it.

With the USB port, **A and B go through the USB side too**. Just as the DIN
sockets go quiet when the unit's HOST SELECT is USB, the MU2000's MIDI OUT
also comes out on the USB side (here it is available from the same "MIDI
OUT" port, so there is no visible difference). Byte delivery changes from
DIN's 31,250 bps to the unit's USB speed (10,000 bytes/s).

With loopMIDI, **create one virtual cable per port** and assign each. In a
DAW, the plug-in exposes **four MIDI input buses** (`MIDI In A (Part 1-16)`
to `MIDI In D (Part 49-64)`), so choose the destination bus per track.

The bottom line of the window shows the ports currently connected.

Windows's MIDI output API can block, so **it is never called from the audio
thread**. The audio thread only pushes bytes onto a ring; another thread
takes them off and sends (`src/ui/midi_out.*`).

## The panel face

The unit's front panel. **Laid out from measurements of photographs of the
unit** (A/D INPUT, VOLUME and power on the left; the LCD and 18 voice
category buttons in the centre; PLAY/EDIT/UTIL/EFFECT/SAMPLING/SEQ, a 3×3
button grid and the big dial on the right).

| Item | Contents |
|---|---|
| LCD | a real HD44780. Glyphs come from `roms/hd44780u_b04.bin`. **2 rows × 20 columns of text plus a segment area for pictograms** (below) |
| LEDs | six for PLAY / EDIT / UTIL / EFFECT / SAMPLING / SEQ and four for MU / PLG-1 to 3, lit as the firmware lights them. The six double as push buttons |
| Buttons | 35, wired as MAME's `mu500` input ports |
| Big dial | **turn with the wheel, or grab and move up and down**. It drives the unit's rotary encoder (port A bits 17/16) directly. Same effect as VALUE −/+ (below) |
| VOLUME | **the final level, applied outside the tone generator**. Sends no MIDI. Grab and move (vertically or horizontally), or use the wheel over it. In VST3 it is the same value as the Output parameter |

The keyboard works too (the same assignments as MAME).

```
A=PLAY  E=EDIT  U=UTIL  F=EFFECT  S=MUTE/SOLO  [ ]=PART−/+
−  ==VALUE−/+  BackSpace=EXIT  Enter=ENTER  , .=SELECT◀▶
Q=SEQ  Z=AUDITION  X=SELECT  M=SAMPLING/MODE
```

The LCD shows exactly what the firmware writes. After boot it shows the play
screen, something like `◀000▶001 GrandP #01`.

### How the window divides

**DDRAM columns are not simply laid out in one row.** Which column becomes
what was worked out by pressing buttons and following the changes.

| DDRAM | Where it appears |
|---|---|
| 0-8 (both rows) | the level meters on the upper face; two bars per cell, 18 bars (A1 A2 and 1-16) |
| 9-16 (both rows) | eight text columns on the upper face; row 1 is the voice name, row 2 is `▶000◀001` |
| row 0, 17-18 | **lower face, left**: the part number, "01" |
| row 1, 17-19 | **to its right**: "A01" |
| 20-23 (both rows) | **lower face**: the instrument picture, one image across both rows |
| row 0, 19 | unknown; not yet worked out |

This was determined by watching columns 17-18 step `01`→`02` on PART+, only
columns 9-16 and the row-2 number move on VALUE+, and columns 9-16 change
from `GrandP #` to `GrandPno` on a GS reset (TG300B voice names use all eight
characters).

The upper face has **no gap between the two rows**: the meter bars extend
across both rows, and a gap would break them. Columns are one dot apart.

The lower face is arranged differently from the upper.

* the five characters of "01" and "A01" are spaced 1, 2, 1, 1 dots apart from the left (the 2-dot gap is where the blocks meet)
* half a dot on, the instrument picture. **Only this area has finer, non-square dots**: a 20-wide by 16-high image squeezed into one text row's height
* to its right, the VOL / EXP / PAN / REV / CHO / VAR / KEY segments
* at the far right, four small `▶` marks. The top one has no label; the rest are XG / GS (TG300B) / PERFORM.
  The labels beside the window are drawn at these marks' heights (`modes.x` in panel.txt sets only their x)

**Column 23 is not a picture but 64 bits that switch fixed-shape segments
on and off.** The pan knob, the reverb/chorus/variation fans, the note-shift
seven-segment, the `MIC` `LINE` `BANK` `PGM#` `A1` `A2` `1`-`32` labels and
the `▼` cursor are all driven by these. The addresses are measured and
written in [doc/lcd-segments.md](lcd-segments.md).

The scale numbers are part numbers, so they sit **directly beneath each
bar**.

The labels printed under the window are placed at the same positions.

### Level meters and indicators

The firmware draws the level meters in **the left nine cells**. **Each cell
holds two bars**, and **each bar is two dots wide**. The character code is

```
0x7f + 9 × (dots in the left bar) + (dots in the right bar)     (each 0-8)
```

The upper and lower rows use **the same table**. Bars extend across both
rows: the lower row fills to 8, then the upper row grows from 1. A side that
has not reached the upper row is 0 there. A cell with both at 0 holds a
space (`0x20`).

Silent parts still show one dot (both sides 1, code `0x89`). That is the
row of dots you see along the bottom of the window.

9 cells × 2 bars = 18 bars for `A1 A2 1 2 … 16`. The left half of a cell is
parts 1, 3, 5, …, the right half parts 2, 4, 6, …. **Depending on the display
mode, the right half can show channel B (tracks 17-32) instead.**

#### How this was worked out

The LCD's glyph images were not obtainable, so the codes the firmware
writes were measured: play parts 1 and 2 only, sweep CC7 from 0 to 127 and
read DDRAM.

| Part 1 | Part 2 | Upper row | Lower row | Reading |
|---|---|---|---|---|
| 0 | 0 | `20` | `89` | (1,1), one dot each |
| 48 | 0 | `20` | `bf` | (7,1) |
| 0 | 48 | `20` | `8f` | (1,7) |
| 48 | 48 | `20` | `c5` | (7,7) |
| 127 | 0 | `c7` | `cf` | lower (8,8), upper (8,0) |
| 127 | 64 | `c8` | `cf` | upper (8,1) |
| 127 | 127 | `cf` | `cf` | both (8,8) |
| 16 | 127 | `87` | `a2` | lower (3,8), upper (0,8) |

Left and right add cleanly, and the same table serves the upper row.
`0x80`-`0x88` are part of the same table (left 0, right 1-8); the earlier
belief that they were **a single full-width bar** was wrong. The base is
`0x7f`, not `0x89`, and the bars had been drawn one dot too low.

The glyph images belong in the CGROM, but only an ASCII substitute font was
to hand, so **they are generated from the rule above**. If the real CGROM
(`hd44780u_b04.bin` from MAME's `mulcd.zip`) is present it takes precedence.

The pan, reverb and other indicators occupy the rightmost 2 rows × 4 cells,
and those are **images the firmware writes to CGRAM**, so they are real from
the start.

To see them, write a picture while a song plays.

```
build/gui.exe roms --mid song.mid 60 --shot panel.png --size 2200x880
```

### The dial and VALUE −/+ are the same thing

On the unit, the big dial and the VALUE −/+ buttons do the same job. The
entry points are quite different (the dial is port A, the buttons are the
switch matrix) but they land in the same place in the firmware. Measurement
agrees.

| Action | Volume value |
|---|---|
| nothing | 100 |
| tap VALUE+ three times | 103 |
| dial +3 clicks | 103 |
| dial +1 click | 101 |

Auto-repeat on a long press is done by the firmware itself (+1 at 0.3 s,
+5 at 1 s, +18 at 2 s, accelerating). This side merely holds the unit's
switch down; nothing is built in.

The display treats them the same. **The wheel acts as the dial on every
face**, and pressing VALUE −/+ turns the drawn dial too.

**The dial can also be grabbed and moved up and down.** Hold the panel dial
and move up for +, down for − (the same direction as the wheel). One click
per 4 pixels on the panel artwork's scale (25 per 100 pixels at a window
width of 1000), scaling with the window. The drawn dial turns 15° per click,
as with the wheel (`panel::dial_follow`, src/ui/editor.cpp).

Unlike a long press, **the firmware does not accelerate the dial**. Sending
20 clicks at once advances no more than 20, and while changing voices some
are dropped (the same as spinning the unit's dial fast).

### Inside the dial

MAME does not drive this (`pa_r()` just returns 0xffff). Only a wiring note
survived, so the rule was found by measurement.

  * the firmware reads port A every 2.5 ms (400 Hz)
  * **a rising edge on bit 17 is one click; holding bit 16 reverses it**
  * it is not plain quadrature
  * `0xffff` contains neither bit 16 nor 17; the value MAME returned means "dial at rest"

## The editor face

A face modelled on the SOL2 XG editor. Choose one part and move its
controls.

**The tone generator is untouched. Only MIDI is sent.** Controls send XG
parameter changes (multi part `08 pp xx`). It is the same as sending to the
hardware, so it never contradicts what the panel did.

There are 32 parts. The upper two rows are port A (1-16), the lower two
port B (17-32).

| Row | Controls |
|---|---|
| 1 | Volume / Pan / Dry / Reverb / Chorus / Variation |
| 2 | Cutoff / Resonance / Attack / Decay / Release / Vib Rate |
| 3 | Vib Depth / Vib Delay / Note Shift / Detune / Bank / Program |

Drag controls up and down, or use the wheel. `XG Reset` sends XG System On
(`F0 43 10 4C 00 00 7E 00 F7`).

**Values are not remembered by the display; they are read back from the
tone generator** (the parameter layer, [doc/params.md](params.md)). While the
face is open, the selected part's block is read once a second, so values
changed from the panel or by a song show up as they are. Values not yet read
show as `--`. The Expression, Porta and Modulation controls that used to be
here were removed because the firmware has no way to be asked for them
(they cannot be read back).

### Confirming it works

Set Program to 19 in the editor and return to the panel face: the LCD
changes to `◀000▶019 RockOrg#01`. The loop display → bridge → tone generator
→ firmware → LCD is visible.

## The effects face

Reverb / chorus / variation (the XG system side) and the MU2000's **two
insertion blocks**. The addresses were confirmed by measurement; the list
and the method are in [doc/effects.md](effects.md).

Choose the type with `<` `>` or the wheel. Values are read back from the
tone generator as on the editor face. A type not in the face's table (one
chosen on the panel) shows as a number, `TYPE 49-00`. "Resend this screen"
was no longer needed and was removed.

## How it is built

```
ui::panel     layout, drawing, hit-testing; face switching lives here too
ui::bridge    between the display and the tone generator: buttons are atomic
              bits, MIDI is a ring, the LCD copy is a seqlock. No locks
ui::driver    the audio-thread side: buttons to the tone generator, MIDI to
              the tone generator, the copy to the display
```

The tone generator is driven by the audio thread, so the display must never
touch it directly. The only meeting point is the bridge.

## Fixing the look without opening a window

`--shot` writes a PNG. Use it where a window cannot be opened (automated
checks, bug reports).

```
build/gui.exe roms --boot --shot panel.png --size 1000x400
```
