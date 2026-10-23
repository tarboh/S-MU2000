"""音色のコモン LFO（40 06 00）を測る。
  python mr_clfo.py --dir 置き場 [--only 名前,…]
雛形の音色（正弦波、エレメント 1 だけ）のエレメント 1 を LFO の箱 1 に入れ（41 ee 1A = 1）、コモン LFO の箱 1 の行き先と深さ・速さ・波形を変えて
鍵 69 を 4 秒鳴らす。PMD は瞬時の周波数（セント）、AMD は振幅（dB）の揺れを 1ms ごとに読む。
送るのはパラメーターチェンジ・ダンプの要求・ノートだけ（保存しない）。終わったらマルチへ戻す。"""
import os
import sys

import numpy as np

import mr_ref

FS = 44100
# 名前: コモン LFO の欄（番地: 値）、エレメント 1 の箱 1 の割合（44 e 51）
BASE = {0x00: 0, 0x01: 0, 0x02: 32, 0x04: 0, 0x05: 0, 0x06: 0, 0x07: 127, 0x08: 0, 0x09: 1, 0x0A: 1, 0x0B: 64, 0x0D: 0, 0x0E: 0, 0x10: 0, 0x11: 0}
TAKES = []
for s in (0, 4, 8, 16, 24, 32, 40, 48, 56, 63):
    TAKES.append(("spd%02d" % s, {0x02: s}, 127))
for d in (16, 32, 64, 96, 127):
    TAKES.append(("pmd%03d" % d, {0x0B: d}, 127))
for r_ in (32, 64, 96):
    TAKES.append(("ratio%03d" % r_, {0x0B: 64}, r_))
for d in (16, 32, 64, 127):
    TAKES.append(("amd%03d" % d, {0x0A: 0, 0x0B: d}, 127))
for w in range(12):
    TAKES.append(("wave%02d" % w, {0x01: w, 0x02: 16}, 127))
for w in range(12):
    TAKES.append(("w40_%02d" % w, {0x01: w, 0x02: 40}, 127))       # 1.6Hz で形と始まりの位相を見る
for s_ in (8, 16):
    TAKES.append(("slow%02d" % s_, {0x02: s_}, 127))
for dl in (16, 32, 64, 96, 127):
    TAKES.append(("dly%03d" % dl, {0x02: 48, 0x05: dl}, 127))
for fi in (16, 32, 64, 96, 127):
    TAKES.append(("fin%03d" % fi, {0x02: 48, 0x06: fi}, 127))
for d in (32, 64, 96, 113, 127):                                   # ユーザー波形 2 段（64・0）で AMD・PMD の深さ
    TAKES.append(("uamd%03d" % d, {0x01: 12, 0x02: 16, 0x0A: 0, 0x0B: d, 0x13: 0, 0x15: 64, 0x16: 0}, 127))
for lv in (64, 96):                                                # エレメントの音量（41 e 20）を下げると AMD の効きが変わるか
    for d in (64, 96):
        TAKES.append(("uamd%03d_l%03d" % (d, lv), {0x01: 12, 0x02: 16, 0x0A: 0, 0x0B: d, 0x13: 0, 0x15: 64, 0x16: 0}, 127, {0x20: lv}))
for d in (32, 64):
    TAKES.append(("upmd%03d" % d, {0x01: 12, 0x02: 16, 0x0B: d, 0x13: 0, 0x15: 64, 0x16: 0}, 127))
for hd, fo in ((32, 32), (32, 96), (64, 64)):
    TAKES.append(("hold%03d_fo%03d" % (hd, fo), {0x02: 48, 0x07: hd, 0x08: fo}, 127))


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    only = mr_ref.opt(a, "--only")
    os.makedirs(d, exist_ok=True)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(704)
        r.el(0, 0x41, 0x1A, 0, 1)                 # LFO の箱 1 に入れる
        for take in TAKES:
            name, ch, ratio = take[:3]
            els = take[3] if len(take) > 3 else {}
            if only and name not in only.split(","):
                continue
            if os.path.exists(os.path.join(d, name + ".wav")):
                continue
            prm = dict(BASE)
            prm.update(ch)
            for off, v in prm.items():
                r.param(0x40, 0x06, off, v)
            r.el(0, 0x44, 0x51, ratio)
            for off, v in els.items():
                r.el(0, 0x41, off, v)
            hold = 12.0 if name[:4] in ("slow", "dly1", "fin1", "hold") or name in ("dly096", "fin096") else 4.2
            r.record(name, 0.3 + hold + 0.3, [(0.3, lambda: r.note_on(69, 100)), (0.3 + hold, lambda: r.note_off(69))])
            for off in els:
                r.el(0, 0x41, off, 127)
            print(name, "録った", flush=True)
    finally:
        r.all_off()
        r.mode(3)
        r.close()


if __name__ == "__main__":
    main()
