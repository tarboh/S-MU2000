"""帯域阻止の型（BEF12・BEF6・DualBEF）の録音から、谷の中心の周波数を細かく出す。
  python mr_notch.py 置き場の親 出力.csv [型 …]

置き場の親の下の cutoff_t12・cutoff_t13・cutoff_t17（mr_cutoff.py の録音）を読む。
65,536 点の FFT を対数周波数の格子に移して 1/48 オクターブでならし、いちばん低い所を放物線で細かくする。
この谷の中心は、型に共通の「カットオフの周波数」になる（doc/motif-rack-es-capture.md）。録音を読むだけで、実機には何も送らない。"""
import csv
import math
import os
import sys
from concurrent.futures import ProcessPoolExecutor

import numpy as np

import mr_cutoff
import mr_ref

N = 65536
f = np.fft.rfftfreq(N, 1 / 44100)


def spec(path):
    L, _ = mr_ref.read_wav(path)
    x = np.asarray(L)
    on = int(np.argmax(np.abs(x) > 1e-4))
    x = x[on + 4410:on + int((mr_cutoff.HOLD - 0.1) * 44100)]
    acc = np.zeros(N // 2 + 1)
    k = 0
    for s in range(0, len(x) - N, N // 4):
        acc += np.abs(np.fft.rfft(x[s:s + N] * np.hanning(N))) ** 2
        k += 1
    if k == 0:
        acc = np.abs(np.fft.rfft(x[:N] * np.hanning(len(x[:N])), N)) ** 2
    return acc


def job(args):
    d, t, c, ref = args
    y = 10 * np.log10((spec(os.path.join(d, "t%02d_c%03d.wav" % (t, c))) + 1e-30) / (ref + 1e-30))
    lf = np.linspace(math.log2(20), math.log2(20000), 2400)
    yl = np.interp(lf, np.log2(np.maximum(f, 1e-9)), y)
    w = 5
    ys = np.convolve(yl, np.ones(2 * w + 1) / (2 * w + 1), mode="same")
    i = int(np.argmin(ys[w:-w])) + w
    a, b, cc = ys[i - 1], ys[i], ys[i + 1]
    dd = 0.5 * (a - cc) / (a - 2 * b + cc) if (a - 2 * b + cc) != 0 else 0
    return t, c, 2 ** (lf[i] + dd * (lf[1] - lf[0])), float(b)


def main():
    root, out = sys.argv[1], sys.argv[2]
    types = [int(v) for v in sys.argv[3:]] or [12, 13, 17]
    rows = []
    for t in types:
        d = os.path.join(root, "cutoff_t%d" % t)
        ref = spec(os.path.join(d, "thru.wav"))
        with ProcessPoolExecutor(3) as ex:     # 多くするとメモリが足りなくなる
            rows += list(ex.map(job, [(d, t, c, ref) for c in range(256)]))
    with open(out, "w", newline="") as fo:
        w = csv.writer(fo, lineterminator="\n")
        w.writerow(["type", "cutoff", "notch_hz", "depth_db"])
        for r in rows:
            w.writerow([r[0], r[1], round(r[2], 2), round(r[3], 2)])
    print("%d 行" % len(rows))


if __name__ == "__main__":
    main()
