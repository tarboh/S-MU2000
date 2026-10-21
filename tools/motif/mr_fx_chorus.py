"""mr_fx.py --train で録った揺れるエフェクト（コーラスなど）を、0.1 秒ごとの応答に分けて読む（録音を読むだけで、実機には何も送らない）。
  python mr_fx_chorus.py 名前（_dry.wav・_wet.wav の前まで） [探し始めの ms（既定 5。フランジャーは 0.1）]

dry の最初の 1 発（60ms）を型にして wet 全体を逆畳み込みすると、0.1 秒ごとに「dry の線 + エフェクトの線」が並ぶ。
dry の線の位置を 1 サンプル未満まで読んで差し引き、各回の 5〜60ms の中で（dry の線の裾を避ける）一番大きい線の遅れ（放物線で 1 サンプル未満まで）と高さを、
左右それぞれ出す。遅れの並びから、揺れの速さ（FFT の山）・深さ（5〜95% の幅。外れ値に強く）・真ん中の遅れを出す。"""
import sys

import numpy as np

import mr_fx_fit as F

FS = 44100


def main():
    base = sys.argv[1]
    t0 = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0     # dry の線は整数の位置の 1 本なので、手前を短くしても裾は拾わない
    dL, dR = F.load(base + "_dry.wav")
    wL, wR = F.load(base + "_wet.wav")
    od = int(np.argmax(np.abs(dL) > 1e-4))
    ow = int(np.argmax(np.abs(wL) > 1e-4))
    tmpl = dL[max(0, od - 100):od + int(0.06 * FS)]
    wL, wR = wL[max(0, ow - 100):], wR[max(0, ow - 100):]
    n = len(wL)
    hL = F.deconv(wL, tmpl, n)
    hR = F.deconv(wR, tmpl, n)
    # dry の線: 0.1 秒おき
    step = int(0.1 * FS)
    rows = []
    t = 0
    while t + step < n - int(0.2 * FS):
        a = max(0, t - 200)
        k = a + int(np.argmax(np.abs(hL[a:a + 400])))
        g = hL[k]
        res = []
        for h in (hL, hR):
            seg = h[k + max(2, int(t0 / 1000 * FS)):k + int(0.06 * FS)].copy()
            i = int(np.argmax(np.abs(seg)))
            if 0 < i < len(seg) - 1:
                y0, y1, y2 = seg[i - 1], seg[i], seg[i + 1]
                den = y0 - 2 * y1 + y2
                dt = 0.5 * (y0 - y2) / den if den != 0 else 0.0
            else:
                dt = 0.0
            res.append(((i + dt + max(2, int(t0 / 1000 * FS))) / FS * 1000, seg[i] / g))
        rows.append((k / FS, res, g))
        t = k + step
    gmax = max(abs(r[2]) for r in rows)
    rows = [r for r in rows if abs(r[2]) > 0.5 * gmax]   # 鳴らし終わったあと（dry の線が無い回）は捨てる
    tL = np.array([r[1][0][0] for r in rows])
    tR = np.array([r[1][1][0] for r in rows])
    gL = np.array([r[1][0][1] for r in rows])
    gR = np.array([r[1][1][1] for r in rows])
    times = np.array([r[0] for r in rows])
    print("回数 %d（%.1f 秒）" % (len(rows), times[-1] - times[0]))
    for name, d, g in (("L", tL, gL), ("R", tR, gR)):
        y = d - d.mean()
        Y = np.abs(np.fft.rfft(y * np.hanning(len(y)), 8 * len(y)))
        f = np.fft.rfftfreq(8 * len(y), 0.1)
        fk = f[1 + int(np.argmax(Y[1:]))]
        lo, hi = np.percentile(d, 5), np.percentile(d, 95)
        print("%s: 遅れ 真ん中 %.2fms・幅（5〜95%%）%.2f〜%.2fms・揺れ %.3fHz・線の高さ（中央値）%.3f" % (name, np.median(d), lo, hi, fk, np.median(np.abs(g))))
    print("時刻ごとの遅れ（ms、L/R）:", " ".join("%.1f/%.1f" % (a, b) for a, b in zip(tL[:30], tR[:30])))


if __name__ == "__main__":
    main()
