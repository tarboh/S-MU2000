"""ふつうの音色のエレメントで、ステレオの波形のパンの効き方を実機で測る（2026-10-25）。
  python mr_pan_stereo.py 置き場 [--wave 1023] [--key 60]
雛形（mr_ref.template、フィルター THRU）にステレオの波形を入れ、エレメントのパン（41 00 08）を 1・16・32・48・64・80・96・112・127 にして鳴らして録る。
ドラムの鍵では、左の音を（パン − 63）、右の音を（パン + 63）の位置に置く形だった（mr_drum_cal.py）。ふつうの音色でも同じかを見る。
送るのは Data List のパラメーターチェンジとノートだけ（保存しない）。終わったらマルチへ戻す。"""
import sys
import time

import numpy as np

import mr_ref


def main():
    a = sys.argv[1:]
    d = a[0]
    wave = int(mr_ref.opt(a, "--wave", "1023"))
    key = int(mr_ref.opt(a, "--key", "60"))
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
    res = {}
    try:
        r.all_off()
        r.param(0x00, 0x00, 0x34, 0)
        r.mode(0)
        r.program(63, 0, 0)
        r.template(wave)
        r.el(0, 0x43, 0x00, 21)
        r.el(0, 0x43, 0x01, 1, 127)
        for p in (64, 1, 16, 32, 48, 80, 96, 112, 127):
            r.el(0, 0x41, 0x08, p)
            time.sleep(0.15)
            L, R = r.one_note("pan_w%04d_p%03d" % (wave, p), note=key, vel=100, hold=0.8, tail=0.3, pre=0.3)
            L, R = np.asarray(L)[int(0.32 * 44100):int(1.0 * 44100)], np.asarray(R)[int(0.32 * 44100):int(1.0 * 44100)]
            res[p] = (10 * np.log10((L ** 2).mean() + 1e-30), 10 * np.log10((R ** 2).mean() + 1e-30))
            r.all_off()
            time.sleep(0.1)
        for p in sorted(res):
            print("パン %3d: 左 %+.2f 右 %+.2f（中央から）" % (p, res[p][0] - res[64][0], res[p][1] - res[64][1]), flush=True)
    finally:
        r.all_off()
        r.mode(3)
        r.close()


if __name__ == "__main__":
    main()
