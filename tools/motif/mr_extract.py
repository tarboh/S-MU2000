"""mr_long.py で長く録った 1 音から、「出だし + ループ 1 回ぶん」を切り出す（サンプリング方式のソフトシンセ用）。
  python mr_extract.py 録音.wav [--out 切り出し.npz] [--check]

1. 出だし（音が 2e-4 を超える所）を決める
2. 伸ばした所（出だしから 3 秒〜離すまで）の自己相関で、ループの長さ L（0.02 秒〜「伸ばした所の半分」か 8 秒、小数つき）を決める
3. 出だしの後で「L 先とほぼ同じ形」が始まる最初の所をループの始まり s0 にする
4. ループを整数の長さに合わせるため、全体をわずかに伸び縮みさせる（音程のずれはループ 0.5 秒で 0.1 セント程度）
出力は左右の配列・ループの始まりと長さ（npz）。--check で、切り出しを 8 秒ぶん鳴らして録音と比べる。録音を読むだけ。"""
import sys

import numpy as np

import mr_zones as Z

FS = 44100


def loop_length(seg):
    n = len(seg)
    F = np.fft.rfft(seg, 2 * n)
    ac = np.fft.irfft(F * np.conj(F))[:n]
    cs = np.cumsum(seg ** 2)
    tail = np.cumsum(seg[::-1] ** 2)[::-1]
    lags = np.arange(int(0.02 * FS), min(int(8.0 * FS), n // 2))      # 低い鍵ほどループが長いので、録音の長さに合わせて 8 秒まで探す
    r = ac[lags] / np.sqrt(cs[n - 1 - lags] * tail[lags])
    i = int(np.argmax(r))
    # いちばん短い「ほぼ同じ高さの山」を選ぶ（倍の長さの山を取らない）
    best = r[i]
    for j in np.argsort(-r)[:20]:
        if r[j] > best - 2e-5 and lags[j] < lags[i]:
            i = j
    L = lags[i]
    # 放物線で小数まで
    y0, y1, y2 = r[i - 1], r[i], r[i + 1]
    d = 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2) if (y0 - 2 * y1 + y2) != 0 else 0
    return L + d, float(r[i])


def extract(L, R, hold_end=None):
    x = L + R
    on = int(np.argmax(np.abs(x) > 2e-4))
    end = hold_end or (len(x) - int(3.2 * FS))
    seg = x[on + 3 * FS:end]
    Lf, rr = loop_length(seg)
    Li = int(round(Lf))
    # ループの始まり: 出だしから 20ms ごとに、[s, s+Li) と [s+Lf, s+Lf+Li) を比べる
    s0 = None
    for s in range(on + int(0.02 * FS), end - 2 * Li, int(0.005 * FS)):
        a = x[s:s + Li]
        b = np.interp(np.arange(Li) + s + Lf, np.arange(len(x)), x)
        c = float(np.dot(a, b) / np.sqrt(np.dot(a, a) * np.dot(b, b) + 1e-30))
        if c > rr - 2e-4:
            s0 = s
            break
    if s0 is None:
        s0 = on + 3 * FS
    # 伸び縮み: 長さ Lf を Li にそろえる
    k = Lf / Li
    n = int((s0 - on + Li) / k) + 2
    t = on + np.arange(n) * k
    outL = np.interp(t, np.arange(len(L)), L)
    outR = np.interp(t, np.arange(len(R)), R)
    start = int(round((s0 - on) / k))
    return outL, outR, start, Li, Lf, rr, k


def play(sig, start, length, n):
    out = np.empty(n)
    m = start + length
    if n <= m:
        return sig[:n].copy()
    out[:m] = sig[:m]
    i = m
    while i < n:
        c = min(length, n - i)
        out[i:i + c] = sig[start:start + c]
        i += c
    return out


def main():
    a = sys.argv[1:]
    path = a[0]
    out = a[a.index("--out") + 1] if "--out" in a else None
    L, R = Z.read24(path)
    oL, oR, start, Li, Lf, rr, k = extract(L, R)
    print("ループ: 長さ %.2f サンプル（%.4f 秒、相関 %.6f）→ %d に合わせた（伸び縮み %.6f 倍 = %+.2f セント）、始まり %d（%.3f 秒）"
          % (Lf, Lf / FS, rr, Li, k, 1200 * np.log2(k), start, start / FS))
    if out:
        np.savez_compressed(out, L=oL.astype(np.float32), R=oR.astype(np.float32), loop_start=start, loop_length=Li, stretch=k)
    if "--check" in a:
        x = L + R
        on = int(np.argmax(np.abs(x) > 2e-4))
        n = int(7.5 * FS)
        y = play(oL + oR, start, Li, int(n / k))
        y = np.interp(np.arange(n) / k, np.arange(len(y)), y)       # 伸び縮みを戻す
        ref = x[on:on + n]
        for t0 in (0.0, 0.5, 1.0, 2.0, 4.0, 7.0):
            s, e = int(t0 * FS), int(t0 * FS) + 4096
            c = float(np.dot(y[s:e], ref[s:e]) / np.sqrt(np.dot(y[s:e], y[s:e]) * np.dot(ref[s:e], ref[s:e]) + 1e-30))
            print("  %.1f 秒: 録音との相関 %.5f" % (t0, c))


if __name__ == "__main__":
    main()
