"""高さが段ごとに変わる FEG で、高さのベロシティ感度（43 ee 20）の倍率が時間を通して同じかを測る（2026-10-24。Crazy Mod のベロシティ 40 が暗すぎる件）。
  python mr_feg_vel_dyn.py --dir 置き場 [--base 96]
雛形の音色（ノイズ 705、鍵 60）で、型 BEF12・カットオフ base、深さ 127、FEG を Crazy Mod のエレメント 1 と同じ
（時間 3・69・79・77・127、高さ −1・121・−39・−79・−45）にして 2 秒押さえる。感度 127・カーブ 2 でベロシティ 1・20・40・70・100・127、
基準に感度 64（倍率 1）でベロシティ 40・100。素通し（THRU）はベロシティごと。谷の動きを読むのは別（mr_feg_vel_dyn_read.py）。
送るのはパラメーターチェンジとノートだけ（保存しない）。終わったらマルチへ戻す。"""
import os
import sys
import time

import mr_ref

TIMES = (3, 69, 79, 77, 127)
LEVELS = (-1, 121, -39, -79, -45)
VELS = (1, 20, 40, 70, 100, 127)
HOLD = 2.0


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    base_c = int(mr_ref.opt(a, "--base", "96"))
    os.makedirs(d, exist_ok=True)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)

    def take(name, vel):
        if not os.path.exists(os.path.join(d, name + ".wav")):
            r.record(name, 0.4 + HOLD + 0.8, [(0.4, lambda: r.note_on(60, vel)), (0.4 + HOLD, lambda: r.note_off(60))])

    def lv(off, v):
        x = 128 + v
        r.el(0, 0x43, off, x >> 7, x & 0x7F)

    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(705)
        for off, v in ((0x05, 0), (0x06, 64), (0x22, 64), (0x2F, 64), (0x03, 64), (0x1F, 64), (0x30, 64)):
            r.el(0, 0x43, off, v)
        r.el(0, 0x43, 0x07, 0, 0)
        r.el(0, 0x43, 0x0A, 0, 0)
        r.el(0, 0x43, 0x0C, 1, 122)
        r.el(0, 0x43, 0x1E, 4)
        r.el(0, 0x43, 0x31, 60)
        for i in range(4):
            r.el(0, 0x43, 0x27 + 2 * i, 1, 0)
        # 素通し
        r.el(0, 0x43, 0x1D, 64)
        r.el(0, 0x43, 0x00, 21)
        r.el(0, 0x43, 0x01, 1, 127)
        time.sleep(0.05)
        for v in VELS:
            take("thru_v%03d" % v, v)
        # BEF12 と FEG
        r.el(0, 0x43, 0x00, 12)
        r.el(0, 0x43, 0x01, base_c >> 7, base_c & 0x7F)
        for i, t in enumerate(TIMES):
            r.el(0, 0x43, 0x0E + i, t)
        for i, v in enumerate(LEVELS):
            lv(0x13 + 2 * i, v)
        r.el(0, 0x43, 0x1D, 127)
        r.el(0, 0x43, 0x21, 2)
        for s, vs in ((64, (40, 100)), (127, VELS)):
            r.el(0, 0x43, 0x20, s)
            time.sleep(0.05)
            for v in vs:
                take("dv%03d_v%03d" % (s, v), v)
            print("感度", s, "録った", flush=True)
        # FEG なし（谷の基の位置）
        r.el(0, 0x43, 0x1D, 64)
        time.sleep(0.05)
        take("nofeg_v100", 100)
    finally:
        r.all_off()
        r.mode(3)
        r.close()


if __name__ == "__main__":
    main()
