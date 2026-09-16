# VST instrument (VST 2.4)

The Windows build can produce a legacy VST 2.4 instrument DLL. It uses the
same emulator engine, ROM lookup, state format, resampler, and panel view as
the VST3 and CLAP plugins.

```
make vsti
make vsti-probe
make install-vsti
```

The output is `build/S-MU2000.dll`. `vsti-probe` loads the DLL and checks its
instrument declaration, stereo output, MIDI input, state round-trip, and editor
attachment without requiring a plugin host.

With ROMs available, `build/vstiprobe.exe build/S-MU2000.dll --audio` also
waits through firmware boot and requires a MIDI note to produce real audio.

The plugin accepts MIDI and SysEx on MIDI IN A and exposes one automatable
`Output` parameter. VST 2 does not provide the two note buses used by the VST3
and CLAP builds, so MIDI IN B is not exposed by this wrapper.

ROM lookup is shared with the other plugin formats. Put a one-line `roms.txt`
beside `S-MU2000.dll`, or use `%LOCALAPPDATA%\S-MU2000\roms.txt`.

The source contains an independent minimal declaration of the public VST 2.4
binary interface. The discontinued VST2 SDK is neither required nor included.
