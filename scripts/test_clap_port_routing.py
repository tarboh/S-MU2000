"""CLAP port-routing regression for #59 and #258 (Windows, with local ROMs)."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
import wave


def write_midi(path, ports):
    # SMF format 1, 480 ticks per quarter note. The port prefix makes the
    # identical note on each track an intentional, independently routed event.
    tracks = []
    for port in ports:
        events = bytes((
            0, 0xFF, 0x21, 1, port,
            0x83, 0x60, 0x90, 60, 100,
            0x83, 0x60, 0x80, 60, 64,
            0, 0xFF, 0x2F, 0,
        ))
        tracks.append(b"MTrk" + len(events).to_bytes(4, "big") + events)
    path.write_bytes(b"MThd\0\0\0\x06\0\x01" + len(tracks).to_bytes(2, "big") + b"\x01\xe0" + b"".join(tracks))


def render(root, args, label, fixture, flags, expected):
    profile = root / label
    profile.mkdir()
    wav = profile / "render.wav"
    env = dict(os.environ, LOCALAPPDATA=str(profile), S_MU2000_ROMS=str(args.roms.resolve()))
    result = subprocess.run(
        [args.probe.resolve(), args.plugin.resolve(), fixture, wav, "--tail", "0.5", *flags],
        cwd=profile, env=env, capture_output=True, timeout=90,
    )
    if result.returncode:
        raise RuntimeError(result.stdout.decode("utf-8", "replace") + result.stderr.decode("utf-8", "replace"))
    log = (profile / "S-MU2000" / "log.txt").read_text(encoding="utf-8")
    summary = re.search(
        r"MIDI のノートオン \[A (\d+) / B (\d+) / C (\d+) / D (\d+).*?"
        r"CLAP のノートオン \[A (\d+) / B (\d+) / C (\d+) / D (\d+)", log
    )
    if summary is None:
        raise AssertionError(f"{label}: no port summary in log:\n{log}")
    numbers = tuple(map(int, summary.groups()))
    actual = tuple(raw + note for raw, note in zip(numbers[:4], numbers[4:]))
    if actual != expected:
        raise AssertionError(f"{label}: expected {expected}, got {actual}")
    print(f"{label}: {actual}", flush=True)
    with wave.open(str(wav), "rb") as audio:
        return audio.readframes(audio.getnframes())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("plugin", type=Path)
    parser.add_argument("roms", type=Path)
    args = parser.parse_args()
    single = (1, 0, 0, 0)
    four = (1, 1, 1, 1)
    with tempfile.TemporaryDirectory(prefix="smu-clap-port-test-") as directory:
        root = Path(directory)
        independent, a_only = root / "independent.mid", root / "a-only.mid"
        write_midi(independent, range(4))
        write_midi(a_only, [0])
        render(root, args, "independent-raw-midi", independent, ["--host-name", "SoundCraft"], four)
        render(root, args, "independent-clap-notes", independent, ["--clap-notes", "--host-name", "SoundCraft"], four)
        reference = render(root, args, "a-only", a_only, [], single)
        for index, name in enumerate(("REAPER", "reaper", "REAPER (x64)", "Cockos REAPER")):
            folded = render(root, args, f"reaper-name-{index}", a_only, ["--broadcast", "--host-name", name], single)
            if folded != reference:
                raise AssertionError(f"{name}: folded audio differs from the single-port control")
        for label, flags in (
            ("soundcraft-fanout", ["--host-name", "SoundCraft"]),
            ("empty-name", ["--host-name", ""]),
            ("null-name", ["--null-host-name"]),
            ("unrelated-name", ["--host-name", "GrimReaper"]),
        ):
            render(root, args, label, a_only, ["--broadcast", *flags], four)


if __name__ == "__main__":
    main()
