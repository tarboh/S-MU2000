"""MOTIF-RACK ES のエレメントのフィルターの、カットオフのスケーリングとレゾナンスのベロシティ感度を測る録音（ノイズの波形 705）。
  python mr_fscale.py --dir 置き場 [--part scale,rvel]

  scale : 型 BEF12・カットオフ 128 で、ブレークポイントを 36・48・72・84、ずらしを A = (+40, −40, +40, −40)、B = (+64, +64, +64, +64) にして、
          鍵 24〜96 を 3 刻みで録る（sA_k??.wav・sB_k??.wav）。鍵ごとの素通しは thru_k??.wav
  rvel  : 型 BPF6・カットオフ 128・レゾナンス 32 で、レゾナンスのベロシティ感度（0x06、64 が 0）× ベロシティを鍵 60 で録る（rv???_v???.wav）。
          ベロシティごとの素通しは thru_v???.wav
条件は mr_cutoff.py と同じ（キーフォロー 0、HPF 0）。終わったらスケーリングを雛形の値（27・48・72・84、ずらし 0）に戻す。
もうあるものは飛ばす。数を出すのは別。送るのはパラメーターチェンジとノートだけ（保存しない）。終わったらマルチへ戻す。"""
import os
import sys
import time

import mr_cutoff
import mr_ref

HOLD = mr_cutoff.HOLD
KEYS = tuple(range(24, 97, 3))
VELS = (1, 32, 64, 100, 127)
RSENS = (0, 32, 64, 96, 127)


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    parts = mr_ref.opt(a, "--part", "scale,rvel").split(",")
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"),
                   mr_ref.opt(a, "--dev", "0"), d)

    def take(name, key=60, vel=100):
        if not os.path.exists(os.path.join(d, name + ".wav")):
            r.record(name, 0.4 + HOLD + 0.3, [(0.4, lambda: r.note_on(key, vel)), (0.4 + HOLD, lambda: r.note_off(key))])

    def scaling(bps, offs):
        for i, (bp, of) in enumerate(zip(bps, offs)):
            r.el(0, 0x43, 0x23 + i, bp)
            v = 128 + of
            r.el(0, 0x43, 0x27 + 2 * i, v >> 7, v & 0x7F)

    def base():
        r.el(0, 0x43, 0x05, 0)
        r.el(0, 0x43, 0x06, 64)                     # レゾナンスのベロシティ感度 0
        r.el(0, 0x43, 0x07, 0, 0)
        r.el(0, 0x43, 0x22, 64)
        r.el(0, 0x43, 0x2F, 64)
        r.el(0, 0x43, 0x03, 64)
        r.el(0, 0x43, 0x0A, 0, 0)
        r.el(0, 0x43, 0x0C, 1, 122)
        scaling((27, 48, 72, 84), (0, 0, 0, 0))     # 雛形の値
        r.el(0, 0x43, 0x00, 21)
        r.el(0, 0x43, 0x01, 1, 127)
        time.sleep(0.05)

    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(705)
        base()
        if "scale" in parts:
            for k in KEYS:
                take("thru_k%02d" % k, k)
            r.el(0, 0x43, 0x00, 12)
            r.el(0, 0x43, 0x01, 1, 0)
            for name, offs in (("A", (40, -40, 40, -40)), ("B", (64, 64, 64, 64))):
                scaling((36, 48, 72, 84), offs)
                time.sleep(0.05)
                for k in KEYS:
                    take("s%s_k%02d" % (name, k), k)
            print("scale done", flush=True)
            base()
        if "rvel" in parts:
            for v in VELS:
                take("thru_v%03d" % v, 60, v)
            r.el(0, 0x43, 0x00, 11)
            r.el(0, 0x43, 0x01, 1, 0)
            r.el(0, 0x43, 0x05, 32)
            for s in RSENS:
                r.el(0, 0x43, 0x06, s)
                time.sleep(0.05)
                for v in VELS:
                    take("rv%03d_v%03d" % (s, v), 60, v)
            print("rvel done", flush=True)
            base()
        r.all_off()
        r.mode(3)
    finally:
        r.close()
    print("DONE", flush=True)


if __name__ == "__main__":
    main()
