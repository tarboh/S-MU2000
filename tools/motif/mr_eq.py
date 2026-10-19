"""MOTIF-RACK ES のエレメントの EQ を測る録音（ノイズの波形 705、鍵 60、フィルターは THRU）。
  python mr_eq.py --dir 置き場

  L/H  : EQ1 の周波数（46-182）・EQ2 の周波数（131-241）を 16 刻み、ゲイン +12（lh1_f???.wav・lh2_f???.wav）。EQ1 のゲインも変える（lhg_g±??.wav）
  P.EQ : 周波数（83-251）を 16 刻み・ゲイン +12・レゾナンス 16（pq_f???.wav）、レゾナンス（pq_r??.wav）とゲイン（pq_g±??.wav）を変える
  Boost: 6・12・18（bo?.wav）
基準は EQ THRU の音（thru.wav）。もうあるものは飛ばす。数を出すのは別。
送るのはパラメーターチェンジとノートだけ（保存しない）。終わったら EQ を THRU に戻し、マルチへ戻す。"""
import os
import sys
import time

import mr_cutoff
import mr_ref

HOLD = mr_cutoff.HOLD


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"),
                   mr_ref.opt(a, "--dev", "0"), d)

    def take(name):
        if not os.path.exists(os.path.join(d, name + ".wav")):
            r.one_note(name, note=60, hold=HOLD, tail=0.3, pre=0.4)

    def eq(t, res=16, f1=100, g1=0, f2=200, g2=0):
        r.el(0, 0x44, 0x00, t)
        r.el(0, 0x44, 0x01, res)
        r.el(0, 0x44, 0x02, f1 >> 7, f1 & 0x7F)
        r.el(0, 0x44, 0x04, 0x40 + g1)
        r.el(0, 0x44, 0x05, f2 >> 7, f2 & 0x7F)
        r.el(0, 0x44, 0x07, 0x40 + g2)
        time.sleep(0.05)

    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(705)
        r.el(0, 0x43, 0x00, 21)                     # フィルターは THRU
        r.el(0, 0x43, 0x01, 1, 127)
        eq(5)
        take("thru")
        for f in range(46, 183, 16):
            eq(0, f1=f, g1=12, f2=200, g2=0)
            take("lh1_f%03d" % f)
        for f in range(131, 242, 16):
            eq(0, f1=100, g1=0, f2=f, g2=12)
            take("lh2_f%03d" % f)
        for g in (-32, -16, -8, 8, 16, 32):
            eq(0, f1=110, g1=g, f2=200, g2=0)
            take("lhg_g%+03d" % g)
        for f in range(83, 252, 16):
            eq(1, res=16, f1=f, g1=12)
            take("pq_f%03d" % f)
        for q in (0, 8, 16, 24, 31):
            eq(1, res=q, f1=163, g1=12)
            take("pq_r%02d" % q)
        for g in (-32, -16, -8, 8, 16, 32):
            eq(1, res=16, f1=163, g1=g)
            take("pq_g%+03d" % g)
        for t, nm in ((2, "6"), (3, "12"), (4, "18")):
            eq(t)
            take("bo%s" % nm)
        eq(5)
        r.all_off()
        r.mode(3)
    finally:
        r.close()
    print("DONE", flush=True)


if __name__ == "__main__":
    main()
