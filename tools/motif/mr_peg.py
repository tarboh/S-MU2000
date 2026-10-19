"""MOTIF-RACK ES のエレメントのピッチ EG（PEG）と音程の感度を測る録音（正弦波の波形 704、フィルター THRU）。
  python mr_peg.py --dir 置き場 [--part scale,time,lvel,kf,pvel]

  scale : 高さ 5 つを全部 x（−64・−16・+16・+64・+127）、深さ +8・+16・+32・+63 で鍵 60（pd???_l???.wav）。止まった音程のずれを見る
  time  : 深さ +32 で、ホールドの高さ +64 → アタック 0（アタックの時間 32・48・64・80、pa??.wav）、
          ホールド・アタック +64 → ディケイ 1 で 0（時間 48・64・80、pd1_??.wav）、ホールドの時間 32・64（ph??.wav）、
          リリース: 全部 0 でリリースの高さ +64・時間 48・64（pr??.wav）
  lvel  : 深さ +32・高さ全部 +64 で、高さのベロシティ感度 +16・+32・+63（カーブ 0）× ベロシティ 1・32・64・96・127（pl???_v???.wav）
  kf    : PEG なしで、音程のキーフォロー（42 ee 19）48・80・112・127 × 鍵 48・72（中心 60、pk???_k??.wav）
  pvel  : PEG なしで、音程のベロシティ感度（42 ee 02）80・96・127 × ベロシティ 1・64・127（pv???_v???.wav）
アドレスは 42 ee 00〜1D（mr_params.py）。高さは 2 バイト（128 が 0）。雛形の値（mr_ref.py）を基にし、測るもの以外は 0 にする。
もうあるものは飛ばす。数を出すのは mr_peg_fit.py。送るのはパラメーターチェンジとノートだけ（保存しない）。終わったらマルチへ戻す。"""
import os
import sys
import time

import mr_ref


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    parts = mr_ref.opt(a, "--part", "scale,time,lvel,kf,pvel").split(",")
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"),
                   mr_ref.opt(a, "--dev", "0"), d)

    def pit(off, *v):
        r.el(0, 0x42, off, *v)

    def lev(i, x):                                  # 高さ i（0 ホールド … 4 リリース）を x（−128..+127）に
        v = x + 128
        pit(0x0A + 2 * i, v >> 7, v & 0x7F)

    def take(name, hold=1.0, key=60, vel=100):
        if not os.path.exists(os.path.join(d, name + ".wav")):
            r.record(name, 0.3 + hold + 1.0, [(0.3, lambda: r.note_on(key, vel)), (0.3 + hold, lambda: r.note_off(key))])

    def base():
        for off in range(0x05, 0x0A):
            pit(off, 0)                             # 時間は全部 0
        for i in range(5):
            lev(i, 0)
        pit(0x14, 64)                               # 深さ 0
        pit(0x15, 4)
        pit(0x16, 64)
        pit(0x17, 64)
        pit(0x18, 0)
        pit(0x19, 96)                               # 音程のキーフォロー（96 がふつう）
        pit(0x1A, 60)
        pit(0x1B, 64)
        pit(0x1C, 60)
        pit(0x02, 64)
        r.el(0, 0x41, 0x27, 64)                     # AEG のリリースを 0.8 秒に（リリースの音程を見るため）
        time.sleep(0.05)

    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(704)
        r.el(0, 0x43, 0x00, 21)
        r.el(0, 0x43, 0x01, 1, 127)
        r.el(0, 0x43, 0x22, 64)
        base()
        if "scale" in parts:
            for dp in (72, 80, 96, 127):
                pit(0x14, dp)
                for x in (-64, -16, 16, 64, 127):
                    for i in range(5):
                        lev(i, x)
                    time.sleep(0.05)
                    take(("pd%03d_l%+04d" % (dp, x)).replace("+", "p").replace("-", "m"), 0.8)
            print("scale done", flush=True)
            base()
        if "time" in parts:
            pit(0x14, 96)
            lev(0, 64)
            for t in (32, 48, 64, 80):
                pit(0x06, t)
                time.sleep(0.05)
                take("pa%02d" % t, 2.5)
            pit(0x06, 0)
            lev(1, 64)
            for t in (48, 64, 80):
                pit(0x07, t)
                time.sleep(0.05)
                take("pd1_%02d" % t, 3.0)
            pit(0x07, 0)
            lev(1, 0)
            for h in (32, 64):
                pit(0x05, h)
                time.sleep(0.05)
                take("ph%02d" % h, 1.5)
            pit(0x05, 0)
            lev(0, 0)
            lev(4, 64)
            for t in (48, 64):
                pit(0x09, t)
                time.sleep(0.05)
                take("pr%02d" % t, 1.0)
            print("time done", flush=True)
            base()
        if "lvel" in parts:
            pit(0x14, 96)
            for i in range(5):
                lev(i, 64)
            for s in (80, 96, 127):
                pit(0x17, s)
                time.sleep(0.05)
                for v in (1, 32, 64, 96, 127):
                    take("pl%03d_v%03d" % (s, v), 0.8, 60, v)
            print("lvel done", flush=True)
            base()
        if "kf" in parts:
            for s in (48, 80, 112, 127):
                pit(0x19, s)
                time.sleep(0.05)
                for k in (48, 72):
                    take("pk%03d_k%02d" % (s, k), 0.8, k)
            print("kf done", flush=True)
            base()
        if "pvel" in parts:
            for s in (80, 96, 127):
                pit(0x02, s)
                time.sleep(0.05)
                for v in (1, 64, 127):
                    take("pv%03d_v%03d" % (s, v), 0.8, 60, v)
            print("pvel done", flush=True)
            base()
        r.el(0, 0x41, 0x27, 0)
        r.all_off()
        r.mode(3)
    finally:
        r.close()
    print("DONE", flush=True)


if __name__ == "__main__":
    main()
