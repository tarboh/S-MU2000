"""エレメントの Key On Delay のテンポ同期（41 ee 12 = 1、長さ 41 ee 1C）の遅れを測る（2026-10-24。Willows のエレメント 3 が実機では鳴らない件）。
  python mr_keydelay_sync.py --dir 置き場 [--values 0,1,…] [--hold 6]
雛形の音色（波形 704 = 正弦波、エレメント 1 だけ）で、音色のテンポ（40 70 28）を 120 にして、長さの値ごとに鍵 60 を鳴らして録る。
値 6・10 はテンポ 60 でも録る（テンポに比例するか）。基準は同期を切った遅れ 0。音の頭は |x| > 1e-4 の所。
送るのはパラメーターチェンジとノートだけ（保存しない）。終わったらマルチへ戻す。"""
import os
import sys

import numpy as np

import mr_ref

FS = 44100


def onset(path):
    L, R = mr_ref.read_wav(path)
    x = np.abs(np.asarray(L)) + np.abs(np.asarray(R))
    return int(np.argmax(x > 1e-4)) if (x > 1e-4).any() else -1


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    vals = [int(v) for v in mr_ref.opt(a, "--values", ",".join(str(i) for i in range(20))).split(",")]
    hold = float(mr_ref.opt(a, "--hold", "6"))
    os.makedirs(d, exist_ok=True)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)

    def take(name):
        if not os.path.exists(os.path.join(d, name + ".wav")):
            r.record(name, 0.3 + hold + 0.5, [(0.3, lambda: r.note_on(60, 100)), (0.3 + hold, lambda: r.note_off(60))])

    def tempo(t):
        r.param(0x40, 0x70, 0x28, t >> 7, t & 0x7F)

    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(704)
        tempo(120)
        r.el(0, 0x41, 0x11, 0)
        r.el(0, 0x41, 0x12, 0)
        take("base")
        r.el(0, 0x41, 0x12, 1)
        for v in vals:
            r.el(0, 0x41, 0x1C, v)
            take("ks%03d_t120" % v)
        tempo(60)
        for v in (6, 10):
            r.el(0, 0x41, 0x1C, v)
            take("ks%03d_t060" % v)
    finally:
        r.el(0, 0x41, 0x12, 0)
        r.all_off()
        r.mode(3)
        r.close()
    base = onset(os.path.join(d, "base.wav"))
    for v in vals:
        o = onset(os.path.join(d, "ks%03d_t120.wav" % v))
        print("長さ %2d（テンポ 120）: 遅れ %s" % (v, "鳴らない" if o < 0 else "%.1f ms（%.3f 拍）" % ((o - base) / FS * 1000, (o - base) / FS * 2)))
    for v in (6, 10):
        o = onset(os.path.join(d, "ks%03d_t060.wav" % v))
        print("長さ %2d（テンポ 60）: 遅れ %s" % (v, "鳴らない" if o < 0 else "%.1f ms（%.3f 拍）" % ((o - base) / FS * 1000, (o - base) / FS)))


if __name__ == "__main__":
    main()
