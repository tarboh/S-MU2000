"""mr_clfo.py の録音を読む（録音を読むだけ）。
  python mr_clfo_read.py 置き場 [名前…]
瞬時の周波数（セント、440Hz から）と振幅（dB）を 1ms ごとに出し、0.3 秒から先の揺れの速さ（FFT の山）・幅（5〜95% の点の差の半分）・形の手がかり
（1 周の平均の形を 8 点）を出す。"""
import glob
import os
import sys

import numpy as np

import mr_ref

FS = 44100


def series(path):
    L, R = mr_ref.read_wav(path)
    x = np.asarray(L, dtype=np.float64)
    on = int(np.argmax(np.abs(x) > 1e-4))
    x = x[on:on + int((11.8 if 'slow' in path else 4.0) * FS)]
    N = len(x)
    X = np.fft.fft(x)
    X[N // 2 + 1:] = 0
    X[1:N // 2] *= 2
    a = np.fft.ifft(X)
    f = np.diff(np.unwrap(np.angle(a))) * FS / (2 * np.pi)
    k = FS // 1000
    m = (len(f) // k) * k
    cents = 1200 * np.log2(np.maximum(f[:m].reshape(-1, k).mean(1), 1) / 440.0)
    amp = 20 * np.log10(np.abs(a[:m]).reshape(-1, k).mean(1) + 1e-12)
    return cents, amp


def describe(v):
    v = v[300:]
    y = v - v.mean()
    Y = np.abs(np.fft.rfft(y * np.hanning(len(y)), 1 << 18))
    fr = np.fft.rfftfreq(1 << 18, 1e-3)
    i = 1 + int(np.argmax(Y[1:]))
    hz = fr[i]
    lo, hi = np.percentile(v, 5), np.percentile(v, 95)
    shape = ""
    if hz > 0.2:
        P = 1000.0 / hz
        ph = (np.arange(len(v)) % P) / P
        shape = " ".join("%+.1f" % (np.mean(y[(ph >= j / 8) & (ph < (j + 1) / 8)])) for j in range(8))
    return hz, (hi - lo) / 2, shape


def main():
    d = sys.argv[1]
    names = sys.argv[2:] or [os.path.basename(p)[:-4] for p in sorted(glob.glob(os.path.join(d, "*.wav")))]
    for n in names:
        c, a = series(os.path.join(d, n + ".wav"))
        hz, w, sh = describe(c)
        hza, wa, sha = describe(a)
        print("%-9s 音程: %.3fHz ±%.1f セント（形 %s） | 振幅: %.3fHz ±%.2f dB（形 %s）" % (n, hz, w, sh, hza, wa, sha))


if __name__ == "__main__":
    main()
