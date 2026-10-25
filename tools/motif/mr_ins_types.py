"""インサーションの型の一覧（名前・番号・初期値）を書く（2026-10-25）。motif_play がパートのインサーションの型を選ぶのに使う。
  python mr_ins_types.py [capture の置き場]
名前と番号は Data List の Effect Type List（2 段組みが崩れているので、名前の並びと番号の並びを突き合わせた）。
初期値（パラメーター 1〜16）は、プリセット（とドラム）のパッチでその型を使っている設定のうち、いちばん多いもの。プリセットで使われていない型は書かない
（初期値が分からないので）。表に無い番号でプリセットが使っているもの（0A 08・0F 18）は、番号を名前にする。
出力は capture/ins_types.txt（1 行に 1 型: 型の番号（MSB × 128 + LSB）・名前・使っている数・パラメーター 1〜16）。
初期値はヤマハのデータから取ったものなので配らないこと。"""
import collections
import glob
import os
import re
import sys

NAMES = [
    ("No Effect", 0x00, 0x00),
    ("Hall 1", 0x01, 0x00), ("Hall 2", 0x01, 0x01), ("Room 1", 0x01, 0x10), ("Room 2", 0x01, 0x11), ("Room 3", 0x01, 0x12),
    ("Stage 1", 0x01, 0x20), ("Stage 2", 0x01, 0x21), ("Plate", 0x01, 0x30), ("White Room", 0x01, 0x40), ("Tunnel", 0x01, 0x41),
    ("Basement", 0x01, 0x42), ("Canyon", 0x01, 0x43), ("R3 Hall", 0x01, 0x02), ("R3 Room", 0x01, 0x13), ("R3 Plate", 0x01, 0x31),
    ("Large Hall", 0x01, 0x08), ("Medium Hall", 0x01, 0x09), ("Warm Room", 0x01, 0x18), ("Woody Room", 0x01, 0x19), ("Rich Plate", 0x01, 0x38),
    ("Delay L,C,R", 0x02, 0x00), ("Delay L,R", 0x02, 0x10), ("Delay L,R St", 0x02, 0x18), ("Echo", 0x02, 0x20), ("Cross Delay", 0x02, 0x30),
    ("Ctrl Delay 1", 0x02, 0x40), ("Ctrl Delay 2", 0x02, 0x48), ("TempDelay 1", 0x02, 0x50), ("TempDelay 2", 0x02, 0x58), ("TempoCrosDly", 0x02, 0x60),
    ("Early Ref 1", 0x03, 0x00), ("Early Ref 2", 0x03, 0x01), ("Gate Reverb", 0x03, 0x10), ("Reverse Gate", 0x03, 0x20),
    ("Karaoke 1", 0x04, 0x00), ("Karaoke 2", 0x04, 0x01), ("Karaoke 3", 0x04, 0x02),
    ("Chorus 1", 0x05, 0x00), ("Chorus 2", 0x05, 0x01), ("Chorus 3", 0x05, 0x02), ("Chorus 4", 0x05, 0x08), ("Chorus 5", 0x05, 0x09),
    ("Celeste 1", 0x05, 0x10), ("Celeste 2", 0x05, 0x11), ("Celeste 3", 0x05, 0x12), ("Celeste 4", 0x05, 0x18), ("Symphonic", 0x05, 0x20),
    ("Ensemble", 0x05, 0x30),
    ("Flanger 1", 0x06, 0x00), ("Flanger 2", 0x06, 0x01), ("Flanger 3", 0x06, 0x08), ("V-Flanger", 0x06, 0x10), ("VntgFlanger1", 0x06, 0x20),
    ("VntgFlanger2", 0x06, 0x21), ("VntgFlanger3", 0x06, 0x22), ("TempoFlanger", 0x06, 0x50), ("DynamFlanger", 0x06, 0x60),
    ("Phaser 1", 0x07, 0x00), ("Phaser 2", 0x07, 0x08), ("VntgPhsr1 St", 0x07, 0x20), ("VntgPhsr2 St", 0x07, 0x21), ("VntgPhsr1 Mn", 0x07, 0x30),
    ("VntgPhsr2 Mn", 0x07, 0x31), ("TempoPhaser", 0x07, 0x50), ("DynamPhaser", 0x07, 0x60),
    ("Tremolo", 0x08, 0x00), ("Auto Pan", 0x08, 0x10), ("Auto Pan 2", 0x08, 0x11), ("Rotary SP 1", 0x08, 0x50), ("Dist+RotSp1", 0x08, 0x51),
    ("Odrv+RotSp1", 0x08, 0x52), ("Amp +RotSp1", 0x08, 0x53), ("Rotary Sp 2", 0x08, 0x60), ("Dist+RotSp2", 0x08, 0x61), ("Odrv+RotSp2", 0x08, 0x62),
    ("Amp +RotSp2", 0x08, 0x63), ("Dual RotSp 1", 0x08, 0x70), ("Dual RotSp 2", 0x08, 0x71), ("Dual RotSp 3", 0x08, 0x72),
    ("Distortion", 0x09, 0x00), ("Comp + Dist", 0x09, 0x01), ("V-Distortion", 0x09, 0x10), ("Overdrive", 0x09, 0x20), ("AmpSimulator", 0x09, 0x30),
    ("AmpSim 2", 0x09, 0x31), ("Dist + Delay", 0x09, 0x40), ("Odrv + Delay", 0x09, 0x41),
    ("Compressor", 0x0A, 0x00), ("Noise Gate", 0x0A, 0x20), ("Cmp+Dist+Dly", 0x0A, 0x40), ("Cmp+Odrv+Dly", 0x0A, 0x41), ("MltBndComp 1", 0x0A, 0x50),
    ("MltBndComp 2", 0x0A, 0x51),
    ("PitchChange1", 0x0B, 0x00), ("PitchChange2", 0x0B, 0x01),
    ("Auto Wah", 0x0C, 0x00), ("A.Wah+Dist", 0x0C, 0x01), ("A.Wah+Odrv", 0x0C, 0x02), ("Vntg A.Wah1", 0x0C, 0x20), ("Vntg A.Wah2", 0x0C, 0x21),
    ("Touch Wah", 0x0C, 0x10), ("T.Wah+Dist", 0x0C, 0x11), ("T.Wah+Odrv", 0x0C, 0x12), ("Vntg T.Wah1", 0x0C, 0x30), ("Vntg T.Wah2", 0x0C, 0x31),
    ("Wah+Dist+Dly", 0x0C, 0x40), ("Wah+Odrv+Dly", 0x0C, 0x41),
    ("Lo-Fi", 0x0D, 0x00), ("LoResolution", 0x0D, 0x10), ("Noisy", 0x0D, 0x20), ("Attack LoFi", 0x0D, 0x30), ("D.Turntable", 0x0D, 0x50),
    ("D.Scratch", 0x0E, 0x00), ("Auto Synth", 0x0E, 0x10), ("Tech Mod", 0x0E, 0x20), ("Jump", 0x0E, 0x30), ("Isolator", 0x0E, 0x40), ("Slice", 0x0E, 0x50),
    ("3 Band EQ", 0x0F, 0x00), ("2 Band EQ", 0x0F, 0x01), ("Enhancer", 0x0F, 0x10), ("Voice Cancel", 0x0F, 0x20), ("Ambience", 0x0F, 0x30),
    ("Talking Mod", 0x0F, 0x40), ("Beat Change", 0x0F, 0x50), ("Ring Mod", 0x0F, 0x70), ("DynamRingMod", 0x0F, 0x71), ("DynamFilter", 0x0F, 0x78),
]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    cap = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "..", "roms", "motif-rack-es", "capture")
    sets = collections.defaultdict(collections.Counter)
    for f in glob.glob(os.path.join(cap, "patches_all", "*.patch")):
        t = open(f, encoding="utf-8", errors="replace").read()
        for k in ("insa", "insb"):
            m = re.search(r"\b%s=(\S+)" % k, t)
            if m:
                v = [int(x) for x in m.group(1).split(",")]
                sets[v[0]][tuple(v[1:17])] += 1
    name = {m << 7 | l: n for n, m, l in NAMES}
    order = [m << 7 | l for n, m, l in NAMES] + sorted(t for t in sets if t not in name)
    lines = ["# MOTIF-RACK ES insertion effect types: code name uses p1..p16 (defaults = the most common setting in the preset patches). Do not distribute."]
    n = 0
    for t in order:
        if t not in sets and t != 0:
            continue
        nm = name.get(t, "%02X %02X" % (t >> 7, t & 0x7F))
        p, c = sets[t].most_common(1)[0] if sets[t] else ((0,) * 16, 0)
        lines.append("%d %s %d %s" % (t, nm.replace(" ", "_"), sum(sets[t].values()), ",".join(str(x) for x in p)))
        n += 1
    open(os.path.join(cap, "ins_types.txt"), "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    print("インサーションの型 %d 個を書いた" % n)


if __name__ == "__main__":
    main()
