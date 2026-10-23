"""エレメントのパン（41 ee 08）とスケーリングパン（41 ee 0B、64 が 0）を測る。
  python mr_pan.py --dir 置き場
雛形の音色（正弦波）で、(1) パン 1・16・32・48・64・80・96・112・127（鍵 60）、(2) スケーリングパンの深さ 127・96・32 で鍵 36・48・60・72・84・96 を鳴らし、
左右の RMS の比を出す。送るのはパラメーターチェンジ・ダンプの要求・ノートだけ（保存しない）。終わったらマルチへ戻す。"""
import os
import sys

import numpy as np

import mr_ref

FS = 44100


def lr(path):
    L, R = mr_ref.read_wav(path)
    L, R = np.asarray(L), np.asarray(R)
    on = int(np.argmax(np.abs(L) + np.abs(R) > 1e-4))
    s = slice(on + int(0.2 * FS), on + int(0.8 * FS))
    return 10 * np.log10(np.mean(L[s] ** 2) + 1e-20), 10 * np.log10(np.mean(R[s] ** 2) + 1e-20)


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    os.makedirs(d, exist_ok=True)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
    takes = [("pan%03d" % p, p, 64, 60) for p in (1, 16, 32, 48, 64, 80, 96, 112, 127)]
    takes += [("sc%03d_k%02d" % (s, k), 64, s, k) for s in (127, 96, 32) for k in (36, 48, 60, 72, 84, 96)]
    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(704)
        for name, pan, sc, key in takes:
            if os.path.exists(os.path.join(d, name + ".wav")):
                continue
            r.el(0, 0x41, 0x08, pan)
            r.el(0, 0x41, 0x0B, sc)
            r.record(name, 1.3, [(0.3, lambda k=key: r.note_on(k, 100)), (1.1, lambda k=key: r.note_off(k))])
    finally:
        r.el(0, 0x41, 0x08, 64)
        r.el(0, 0x41, 0x0B, 64)
        r.all_off()
        r.mode(3)
        r.close()
    for name, pan, sc, key in takes:
        l, rr = lr(os.path.join(d, name + ".wav"))
        print("%-12s パン %3d 深さ %3d 鍵 %2d: 左 %.1f 右 %.1f dB（右 − 左 %+.1f）" % (name, pan, sc, key, l, rr, rr - l))


if __name__ == "__main__":
    main()
