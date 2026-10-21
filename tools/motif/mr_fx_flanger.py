"""mr_fx.py --train で録ったフランジャー（06 00 など）を読む（録音を読むだけで、実機には何も送らない）。
  python mr_fx_flanger.py 名前…（_dry.wav・_wet.wav の前まで）

mr_fx_chorus.py と同じく、dry の最初の 1 発を型にして wet を逆畳み込みし、0.1 秒ごとに応答を切り出す。
* 元の音の高さ: 遅れ 0 の線（整数の位置の 1 本）
* 揺れの線: 0.05〜15ms の一番大きい線の遅れ。並びに三角波（LFO の速さ・位相・下限・幅）を当てはめる
* 揺れの線のエネルギー: 遅れ 0 の線を除いた 0.05〜15ms（帰りが 0 のとき）
* 帰り: 2 回目の線（遅れの 2 倍の所）の高さ ÷ 1 回目の高さ（符号つき）"""
import sys

import numpy as np

import mr_fx_fit as F

FS = 44100


def rows(base):
    dL, dR = F.load(base + "_dry.wav")
    wL, wR = F.load(base + "_wet.wav")
    od = int(np.argmax(np.abs(dL) > 1e-4))
    ow = int(np.argmax(np.abs(wL) > 1e-4))
    tmpl = dL[max(0, od - 100):od + int(0.06 * FS)]
    wL, wR = wL[max(0, ow - 100):], wR[max(0, ow - 100):]
    n = len(wL)
    hs = (F.deconv(wL, tmpl, n), F.deconv(wR, tmpl, n))
    step = int(0.1 * FS)
    out = []
    t = 0
    # 1 回目の線の位置を dry の音の頭から決める（wet の頭は Dry/Wet 127 のとき遅れた線になる）
    k = int(np.argmax(np.abs(hs[0][:400]) + np.abs(hs[1][:400])))
    while k + int(0.08 * FS) < n - int(0.2 * FS):
        a = max(0, k - 200)      # 0.1 秒おきの音はサンプル単位ではそろわないので、毎回 dry の線を探し直す
        k = a + int(np.argmax(np.abs(hs[0][a:a + 400]) + np.abs(hs[1][a:a + 400])))
        row = []
        for h in hs:
            g = h[k]
            lo, hi = k + 2, k + int(0.015 * FS)
            seg = h[lo:hi]
            i = int(np.argmax(np.abs(seg)))
            dt = 0.0
            if 0 < i < len(seg) - 1:
                y0, y1, y2 = seg[i - 1], seg[i], seg[i + 1]
                den = y0 - 2 * y1 + y2
                dt = 0.5 * (y0 - y2) / den if den != 0 else 0.0
            d = (i + dt + 2) / FS * 1000
            e = float(np.sum(seg ** 2))
            # 2 回目の線: 遅れの 2 倍の前後 ±3 サンプルの、1 回目と同じ向きでの最大
            j = int(round(2 * (i + dt + 2)))
            s2 = h[k + j - 3:k + j + 4]
            v2 = s2[int(np.argmax(np.abs(s2)))] if len(s2) else 0.0
            row.append((g, d, seg[i], e, v2))
        out.append((k / FS, row))
        k += step
    return out


def tri_fit(times, d):
    best = None
    for hz in np.linspace(0.05, 5, 991):
        for ph in np.linspace(0, 1, 50, endpoint=False):
            p = (times * hz + ph) % 1.0
            tr = np.where(p < 0.5, 2 * p, 2 - 2 * p)
            A = np.stack([np.ones_like(tr), tr], 1)
            c, *_ = np.linalg.lstsq(A, d, rcond=None)
            e = np.sum((A @ c - d) ** 2)
            if best is None or e < best[0]:
                best = (e, hz, ph, c[0], c[1])
    return best


def main():
    for base in sys.argv[1:]:
        r = rows(base)
        gmax = max(abs(x[1][0][0]) + abs(x[1][0][2]) for x in r)
        r = [x for x in r if abs(x[1][0][0]) + abs(x[1][0][2]) > 0.3 * gmax]
        times = np.array([x[0] for x in r])
        print("== %s（%d 回）" % (base.replace("\\", "/").split("/")[-1], len(r)))
        for ch, nm in ((0, "L"), (1, "R")):
            g = np.array([x[1][ch][0] for x in r])
            d = np.array([x[1][ch][1] for x in r])
            v1 = np.array([x[1][ch][2] for x in r])
            e = np.array([x[1][ch][3] for x in r])
            v2 = np.array([x[1][ch][4] for x in r])
            err, hz, ph, lo, span = tri_fit(times, d)
            if span < 0:
                lo, span, ph = lo + span, -span, (ph + 0.5) % 1.0
            ok = d > 0.3    # 2 回目の線が 1 回目と重ならない回だけ
            fb = np.median(v2[ok] / v1[ok]) if ok.any() else float("nan")
            print("  %s: 元の音 %.3f・揺れ %.3fHz（位相 %.2f）・遅れ %.2f〜%.2fms・揺れの線のエネルギー %.3f（高さの中央値 %.3f）・2 回目 / 1 回目 %+.3f・当てはめの残り %.3fms" % (
                nm, np.median(g), hz, ph, lo, lo + span, np.median(e), np.median(np.abs(v1)), fb, np.sqrt(err / len(d))))


if __name__ == "__main__":
    main()
