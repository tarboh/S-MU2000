"""MOTIF-RACK ES のエレメントのフィルターの、残りのパラメーターを測る録音（ノイズの波形 705、鍵 60）。
  python mr_fparams.py --dir 置き場 [--part dist,gain,vel]

  dist : Dual 系の型（14-17）・カットオフ 128 で、Distance（0x0A、0-255）を 16 刻みで録る（d??_v???.wav）
  gain : 型 LPF6・カットオフ 255 で、Filter Gain（0x0C、0-255）を 32 刻みで録る（g???.wav）
  vel  : 型 BEF12・カットオフ 128 で、カットオフのベロシティ感度（0x03、64 が 0）× ベロシティを録る（vs???_v???.wav）
基準は THRU の音（thru.wav、ベロシティ 100）と、ベロシティごとの THRU（thru_v???.wav）。
条件は mr_cutoff.py と同じ（レゾナンス 0、キーフォロー 0、スケーリング 0、HPF 0）。もうあるものは飛ばす。数を出すのは別。
送るのはパラメーターチェンジとノートだけ。ボイスモードの今の音色を作り変える（保存しない）。終わったらマルチへ戻す。"""
import os
import sys
import time

import mr_cutoff
import mr_ref

HOLD = mr_cutoff.HOLD
VELS = (1, 32, 64, 100, 127)
SENS = (0, 16, 32, 48, 64, 80, 96, 112, 127)


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    parts = mr_ref.opt(a, "--part", "dist,gain,vel").split(",")
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"),
                   mr_ref.opt(a, "--dev", "0"), d)

    def take(name, vel=100):
        if not os.path.exists(os.path.join(d, name + ".wav")):
            r.record(name, 0.4 + HOLD + 0.3, [(0.4, lambda: r.note_on(60, vel)), (0.4 + HOLD, lambda: r.note_off(60))])

    def base():
        r.el(0, 0x43, 0x05, 0)
        r.el(0, 0x43, 0x07, 0, 0)
        r.el(0, 0x43, 0x22, 64)
        r.el(0, 0x43, 0x2F, 64)
        r.el(0, 0x43, 0x03, 64)                     # カットオフのベロシティ感度 0
        r.el(0, 0x43, 0x0A, 0, 0)                   # Distance 0（雛形の値）
        r.el(0, 0x43, 0x0C, 1, 122)                 # Filter Gain 250（雛形の値）
        for off in (0x27, 0x29, 0x2B, 0x2D):
            r.el(0, 0x43, off, 1, 0)
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
        if "dist" in parts:
            r.el(0, 0x43, 0x01, 1, 0)               # カットオフ 128
            for t in (14, 15, 16, 17):
                r.el(0, 0x43, 0x00, t)
                for v in list(range(0, 256, 16)) + [255]:
                    r.el(0, 0x43, 0x0A, v >> 7, v & 0x7F)
                    time.sleep(0.05)
                    take("d%02d_v%03d" % (t, v))
            r.el(0, 0x43, 0x0A, 0, 0)
            print("dist done", flush=True)
            base()
        if "gain" in parts:
            r.el(0, 0x43, 0x00, 5)
            for v in list(range(0, 256, 32)) + [255]:
                r.el(0, 0x43, 0x0C, v >> 7, v & 0x7F)
                time.sleep(0.05)
                take("g%03d" % v)
            print("gain done", flush=True)
            base()
        if "vel" in parts:
            for v in VELS:
                take("thru_v%03d" % v, v)
            r.el(0, 0x43, 0x00, 12)
            r.el(0, 0x43, 0x01, 1, 0)
            for s in SENS:
                r.el(0, 0x43, 0x03, s)
                time.sleep(0.05)
                for v in VELS:
                    take("vs%03d_v%03d" % (s, v), v)
            print("vel done", flush=True)
            base()
        r.all_off()
        r.mode(3)
    finally:
        r.close()
    print("DONE", flush=True)


if __name__ == "__main__":
    main()
