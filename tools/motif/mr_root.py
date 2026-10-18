"""mr_sweep.py の下録りから、波形の元の鍵（チップの読む速さが 1 になる鍵。端数つき）を出す。
  python mr_root.py 置き場 波形 [鍵の下 上]

元の鍵より下の鍵では、出力のスペクトルに補間の像が「読む速さ × 44,100Hz」おきにくり返して出る。
その間隔を対数スペクトルの自己相関で測り、鍵 n の元の鍵 = n − 12 log2(間隔 / 44100) とする。
音程の無い音（ドラム・効果音）でよく効く。2026-10-18 に、高い鍵の折り返しが読む速さ 3 倍で起きることをこれで確かめた。"""
import os
import sys

import numpy as np

import mr_zones as Z

NF = 8192


def logspec(x):
    k = (len(x) - NF) // (NF // 4)
    if k < 1:
        return None
    w = np.hanning(NF)
    P = sum(np.abs(np.fft.rfft(x[i * NF // 4:i * NF // 4 + NF] * w)) ** 2 for i in range(k + 1)) / (k + 1)
    return np.log(P + 1e-20)


def spacing(ls, lo_hz, hi_hz):
    """対数スペクトルのくり返しの間隔（Hz）と、その所の自己相関"""
    df = 44100 / NF
    v = ls - np.convolve(ls, np.ones(301) / 301, mode="same")  # ゆるい形を引く
    v = v[50:-50]
    ac = np.correlate(v, v, mode="full")[len(v) - 1:]
    ac /= ac[0]
    a, b = int(lo_hz / df), int(hi_hz / df)
    i = a + int(np.argmax(ac[a:b]))
    d = 0.0
    if 1 <= i < len(ac) - 1:
        y0, y1, y2 = ac[i - 1], ac[i], ac[i + 1]
        if y0 - 2 * y1 + y2 != 0:
            d = 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2)  # 放物線で細かく
    return (i + d) * df, ac[i]


def run(base, lo, hi):
    L, R = Z.read24(base + ".wav")
    x = L + R
    marks = [(int(a), float(b)) for a, b in (l.split() for l in open(base + ".txt"))]
    out = []
    for n, t in marks:
        if not lo <= n <= hi:
            continue
        s = int((t + 0.01) * 44100)
        ls = logspec(x[s:s + int(0.38 * 44100)])
        if ls is not None:
            out.append((n, ls))
    return out


def main():
    a = sys.argv[1:]
    d, w = a[0], int(a[1])
    lo, hi = (int(a[2]), int(a[3])) if len(a) > 3 else (0, 40)
    roots = []
    for n, ls in run(os.path.join(d, "w%04d" % w), lo, hi):
        sp, c = spacing(ls, 800, 15000)
        root = n - 12 * np.log2(sp / 44100)
        roots.append((c, root))
        print("%3d 間隔 %8.1f Hz（相関 %.2f）  元の鍵 %.2f" % (n, sp, c, root))
    good = [r for c, r in roots if c > 0.3]
    if good:
        print("元の鍵 %.2f（%d 鍵の中央値）" % (np.median(good), len(good)))


if __name__ == "__main__":
    main()
