"""mr_fx.py の録音（_dry と _wet）から、エフェクトのインパルス応答と数を出す（録音を読むだけで、実機には何も送らない）。
  python mr_fx_fit.py 置き場 [--save]

wet の左右を dry で割って（周波数ごと、小さい所は押さえる）インパルス応答 h を出す。dry の分は h のいちばん大きい所（遅れ 0）に
1 本の線として出るので、その位置を 1 サンプル未満まで読んで周波数ごとに差し引き、エフェクトだけの応答にする（2 本の録音は出だしでそろえてから割る）。--save なら <名前>_ir.npz（L・R・fs）に置く。
  level : エフェクトの応答の全エネルギー（dry の線を 0dB として）
  pre   : 応答のエネルギーが最大の 1/100（−20dB）を超える最初の時刻（ms）
  RT60  : オクターブの帯（125〜8kHz）ごとに、シュレーダーの逆積分の −5〜−35dB を直線で当てはめた残響時間（秒）
  corr  : 左右の応答の相関（0 なら左右がばらばら）"""
import glob
import json
import os
import sys

import numpy as np

import mr_ref

FS = 44100


def load(path):
    L, R = mr_ref.read_wav(path)
    return np.asarray(L, dtype=np.float64), np.asarray(R, dtype=np.float64)


def deconv(w, dsig, n, remove_dry=False):
    N = 1 << int(np.ceil(np.log2(len(w) + len(dsig))))
    W = np.fft.rfft(w, N)
    D = np.fft.rfft(dsig, N)
    P = np.abs(D) ** 2
    H = W * np.conj(D) / (P + 1e-4 * P.max())
    if remove_dry is not False:
        # dry の線を 1 サンプル未満の位置まで読み（放物線）、その遅れの線を周波数ごとに差し引く
        h = np.fft.irfft(H, N)
        t0 = int(np.argmax(np.abs(h[:4096]))) if remove_dry is True else remove_dry[0]
        y0, y1, y2 = h[t0 - 1], h[t0], h[t0 + 1]
        den = y0 - 2 * y1 + y2
        dt = 0.5 * (y0 - y2) / den if den != 0 else 0.0
        g = y1 - 0.25 * (y0 - y2) * dt
        if remove_dry is not True:
            dt, g = remove_dry[1], remove_dry[2]
        k = np.arange(len(H))
        H = H - g * np.exp(-2j * np.pi * k * (t0 + dt) / N) * (P / (P + 1e-4 * P.max()))
        return np.fft.irfft(H, N)[:n], (t0, dt, g)
    return np.fft.irfft(H, N)[:n]


def octave(h, fc):
    lo, hi = fc / np.sqrt(2), fc * np.sqrt(2)
    N = 1 << int(np.ceil(np.log2(len(h))))
    X = np.fft.rfft(h, N)
    f = np.fft.rfftfreq(N, 1 / FS)
    X[(f < lo) | (f > hi)] = 0
    return np.fft.irfft(X, N)[:len(h)]


def rt60(h):
    e = np.cumsum((h ** 2)[::-1])[::-1]
    e = 10 * np.log10(e / e[0] + 1e-30)
    i5 = int(np.argmax(e < -5))
    i35 = int(np.argmax(e < -35))
    if i35 <= i5 + 10:
        i25 = int(np.argmax(e < -25))
        if i25 <= i5 + 10:
            return float("nan")
        a, _ = np.polyfit(np.arange(i5, i25) / FS, e[i5:i25], 1)
        return -60 / a
    a, _ = np.polyfit(np.arange(i5, i35) / FS, e[i5:i35], 1)
    return -60 / a


def analyze(base, save=False, length=None):
    dL, dR = load(base + "_dry.wav")
    wL, wR = load(base + "_wet.wav")
    if length is None:
        length = len(wL) / FS - 1.0                # 録った長さいっぱい（鳴り始めまでと最後を除く）
    # 2 本の録音は鳴り始めの時刻が少し違うので、出だし（100 サンプル前）でそろえる。そろえないと dry の線が負の遅れ（輪の反対側）に回る
    od = max(0, int(np.argmax(np.abs(dL) > 1e-4)) - 100)
    ow = max(0, int(np.argmax(np.abs(wL) > 1e-4)) - 100)
    dry = dL[od:]                                  # dry は真ん中（左右同じ）
    wL, wR = wL[ow:], wR[ow:]
    n = int(length * FS)
    hL, (t0, dt, g) = deconv(wL, dry, n + 4096, True)
    hR, _ = deconv(wR, dry, n + 4096, (t0, dt, g))  # 左右で dry の位置は同じ（真ん中）
    hL, hR = hL[t0:t0 + n].copy(), hR[t0:t0 + n].copy()
    for h in (hL, hR):
        h[:3] = 0                                  # 差し引きの残り（頭の 3 サンプル、dry の 1 割ほど）。その後ろは −63dB より下
    if save:
        np.savez_compressed(base + "_ir.npz", L=hL.astype(np.float32), R=hR.astype(np.float32), fs=FS, dry_gain=g)
    hm = hL + hR
    en = hL ** 2 + hR ** 2
    env = np.convolve(en, np.ones(64) / 64, "same")
    pre = int(np.argmax(env > env.max() * 0.01)) / FS * 1000
    level = 10 * np.log10(en.sum() + 1e-30)
    corr = float(np.dot(hL, hR) / np.sqrt(np.dot(hL, hL) * np.dot(hR, hR) + 1e-30))
    rts = [rt60(octave(hm, fc)) for fc in (125, 250, 500, 1000, 2000, 4000, 8000)]
    return dict(level=level, pre=pre, rt=rts, corr=corr, dry_gain=float(g))


def main():
    d = sys.argv[1]
    save = "--save" in sys.argv
    print("%-18s %6s %6s %5s | RT60（秒） 125   250   500   1k    2k    4k    8k | params" % ("名前", "level", "pre", "corr"))
    for wet in sorted(glob.glob(os.path.join(d, "*_wet.wav"))):
        base = wet[:-8]
        name = os.path.basename(base)
        r = analyze(base, save)
        js = base + ".json"
        prm = ""
        if os.path.exists(js):
            j = json.load(open(js))
            prm = " ".join(str(j.get("param%d" % i, "")) for i in range(1, 17)) + " ret %s" % j.get("return", "")
        print("%-18s %6.1f %6.1f %5.2f |            " % (name, r["level"], r["pre"], r["corr"]) + " ".join("%5.2f" % v for v in r["rt"]) + " | " + prm)


if __name__ == "__main__":
    main()
