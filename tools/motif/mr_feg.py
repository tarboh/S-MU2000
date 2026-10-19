"""MOTIF-RACK ES のエレメントのフィルター EG（FEG）を測る録音（ノイズの波形 705、鍵 60、型 BEF12・カットオフ 128）。
  python mr_feg.py --dir 置き場 [--part level,time]

  level : 5 つの高さを全部 L にし（時間は全部 0）、高さ L × 深さ D を録る（lv±??_d???.wav）。谷が止まった位置で深さと高さの目盛りを見る
  time  : アタック（高さ −64 → +64）とディケイ 1（+64 → −64）の時間を変えて録る（ta???.wav・td???.wav）。谷の動きを短い区切りで追う
基準は THRU の音（thru.wav）。FEG のベロシティ感度と時間のキーフォローは 0 にする（雛形の音色では +63・+63・+16）。
もうあるものは飛ばす。数を出すのは別。送るのはパラメーターチェンジとノートだけ（保存しない）。終わったらマルチへ戻す。"""
import os
import sys
import time

import mr_ref

HOLD = 2.5


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    parts = mr_ref.opt(a, "--part", "level,time").split(",")
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"),
                   mr_ref.opt(a, "--dev", "0"), d)

    def take(name, hold=HOLD):
        if not os.path.exists(os.path.join(d, name + ".wav")):
            r.record(name, 0.4 + hold + 1.0, [(0.4, lambda: r.note_on(60, 100)), (0.4 + hold, lambda: r.note_off(60))])

    def lv(off, v):                                  # 高さ（-128..+127）
        x = 128 + v
        r.el(0, 0x43, off, x >> 7, x & 0x7F)

    def feg(times, levels, depth):
        for i, t in enumerate(times):
            r.el(0, 0x43, 0x0E + i, t)
        for i, v in enumerate(levels):
            lv(0x13 + 2 * i, v)
        r.el(0, 0x43, 0x1D, depth)

    def base():
        r.el(0, 0x43, 0x05, 0)
        r.el(0, 0x43, 0x06, 64)
        r.el(0, 0x43, 0x07, 0, 0)
        r.el(0, 0x43, 0x22, 64)
        r.el(0, 0x43, 0x2F, 64)
        r.el(0, 0x43, 0x03, 64)
        r.el(0, 0x43, 0x0A, 0, 0)
        r.el(0, 0x43, 0x0C, 1, 122)
        r.el(0, 0x43, 0x1F, 64)                     # FEG の時間のベロシティ感度 0
        r.el(0, 0x43, 0x20, 64)                     # FEG の高さのベロシティ感度 0
        r.el(0, 0x43, 0x30, 64)                     # FEG の時間のキーフォロー 0
        for i in range(4):
            r.el(0, 0x43, 0x27 + 2 * i, 1, 0)
        feg((0, 0, 0, 0, 0), (0, 0, 0, 0, 0), 64)
        r.el(0, 0x43, 0x00, 21)
        r.el(0, 0x43, 0x01, 1, 127)
        time.sleep(0.05)

    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(705)
        base()
        take("thru")
        r.el(0, 0x43, 0x00, 12)
        r.el(0, 0x43, 0x01, 1, 0)
        if "level" in parts:
            for L in (32, 64, 127, -64):
                for D in (0, 32, 64, 96, 127):
                    feg((0, 0, 0, 0, 0), (L,) * 5, D)
                    time.sleep(0.05)
                    take("lv%+04d_d%03d" % (L, D), hold=1.2)
            print("level done", flush=True)
        if "time" in parts:
            for T in (0, 32, 48, 64, 80, 96, 112):
                feg((0, T, 0, 0, 0), (-64, 64, 64, 64, 64), 127)       # アタック: -64 から +64 へ
                time.sleep(0.05)
                take("ta%03d" % T, hold=4.0)
            for T in (32, 48, 64, 80, 96, 112):
                feg((0, 0, T, 0, 0), (64, 64, -64, -64, -64), 127)     # ディケイ 1: +64 から -64 へ
                time.sleep(0.05)
                take("td%03d" % T, hold=4.0)
            print("time done", flush=True)
        base()
        r.all_off()
        r.mode(3)
    finally:
        r.close()
    print("DONE", flush=True)


if __name__ == "__main__":
    main()
