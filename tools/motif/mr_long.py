"""MOTIF-RACK ES の波形を、長く伸ばして録る（サンプリング方式のソフトシンセ用の下調べ。ループとリリースを見る）。
  python mr_long.py --dir 置き場 --waves 3,704,326,125 [--keys 60] [--hold 8] [--vel 100]

雛形の音色（mr_ref.py）で、フィルターは THRU・キーフォロー 0 にして、波形ごと・鍵ごとに 1 音を長く鳴らして録る（L??_w????_k???_v???.wav）。
もうあるものは飛ばす。送るのはパラメーターチェンジとノートだけ（保存しない）。終わったらマルチへ戻す。"""
import os
import sys
import time

import mr_ref
import mr_sweep


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    waves = [int(v) for v in mr_ref.opt(a, "--waves", "3,704,326,125").split(",")]
    keys = [int(v) for v in mr_ref.opt(a, "--keys", "60").split(",")]
    hold = float(mr_ref.opt(a, "--hold", "8"))
    vel = int(mr_ref.opt(a, "--vel", "100"))
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"),
                   mr_ref.opt(a, "--dev", "0"), d)
    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(waves[0])
        r.el(0, 0x43, 0x00, 21)                     # フィルター THRU
        r.el(0, 0x43, 0x01, 1, 127)
        r.el(0, 0x43, 0x22, 64)                     # キーフォロー 0
        r.el(0, 0x43, 0x06, 64)                     # レゾナンスのベロシティ感度 0
        for w in waves:
            r.el(0, 0x41, 0x03, w >> 7, w & 0x7F)
            time.sleep(0.15)
            for k in keys:
                base = os.path.join(d, "L_w%04d_k%03d_v%03d" % (w, k, vel))
                if os.path.exists(base + ".wav"):
                    continue
                L, R = r.record("_tmp", 0.4 + hold + 3.0, [(0.4, lambda k=k: r.note_on(k, vel)), (0.4 + hold, lambda k=k: r.note_off(k))])
                mr_sweep.write_wav24(base + ".wav", L, R)
                os.remove(os.path.join(d, "_tmp.wav"))
                print("wave %d key %d done" % (w, k), flush=True)
        r.all_off()
        r.mode(3)
    finally:
        r.close()
    print("DONE", flush=True)


if __name__ == "__main__":
    main()
