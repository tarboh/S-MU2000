"""音量のベロシティ感度（41 ee 21）とカーブ（41 ee 22）の、強いベロシティの側（x = 感度 ×（127 − v）が小さい所）を測る（2026-10-25。
プリセットのベロシティ 127 が、ベロシティ 100 を基準にすると芯で 0.3〜0.8dB 大きい件）。
  python mr_lvel_top.py --dir 置き場
雛形の音色（正弦波 704、鍵 60）で、感度 64（基準）・72・80・88・96 × カーブ 0〜4 × ベロシティ 70・85・100・110・120・127 を 0.6 秒鳴らす。
送るのはパラメーターチェンジとノートだけ（保存しない）。終わったらマルチへ戻す。"""
import os
import sys
import time

import numpy as np

import mr_ref

SENS = (72, 80, 88, 96)
VELS = (70, 85, 100, 110, 120, 127)


def level(path):
    L, R = mr_ref.read_wav(path)
    x = np.asarray(L, float)
    on = int(np.argmax(np.abs(x) > 1e-4))
    seg = x[on + int(0.15 * 44100):on + int(0.55 * 44100)]
    return 10 * np.log10(np.mean(seg ** 2) + 1e-20)


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    os.makedirs(d, exist_ok=True)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)

    def take(name, vel):
        if not os.path.exists(os.path.join(d, name + ".wav")):
            r.record(name, 0.3 + 0.6 + 0.4, [(0.3, lambda: r.note_on(60, vel)), (0.9, lambda: r.note_off(60))])

    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(704)
        r.el(0, 0x41, 0x21, 64)
        time.sleep(0.05)
        for v in VELS:
            take("ref_v%03d" % v, v)
        for s in SENS:
            r.el(0, 0x41, 0x21, s)
            for c in range(5):
                r.el(0, 0x41, 0x22, c)
                time.sleep(0.05)
                for v in VELS:
                    take("lt_s%03d_c%d_v%03d" % (s, c, v), v)
            print("感度", s, "録った", flush=True)
    finally:
        r.el(0, 0x41, 0x21, 64)
        r.el(0, 0x41, 0x22, 0)
        r.all_off()
        r.mode(3)
        r.close()
    ref = {v: level(os.path.join(d, "ref_v%03d.wav" % v)) for v in VELS}
    for s in SENS:
        for c in range(5):
            print("感度 %+d カーブ %d: %s" % (s - 64, c, " ".join("v%d %+.2f" % (v, level(os.path.join(d, "lt_s%03d_c%d_v%03d.wav" % (s, c, v))) - ref[v]) for v in VELS)))


if __name__ == "__main__":
    main()
