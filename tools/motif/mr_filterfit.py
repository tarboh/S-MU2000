"""エレメントのフィルターの模型（src/motif/swp50_filter.h と同じ形）を、mr_cutoff.py の録音と突き合わせる。
  python mr_filterfit.py curves 置き場の親 curves.npz     … 全部の型・目盛りの特性の曲線を取り出す（時間がかかる）
  python mr_filterfit.py check  curves.npz                … 目盛り g を LPF24D から当てはめ、型ごとの誤差を範囲ごとに出す

置き場の親の下に cutoff_full（型 0）と cutoff_t1 〜 cutoff_t19 がある前提。録音を読むだけで、実機には何も送らない。"""
import os
import sys
from concurrent.futures import ProcessPoolExecutor

import numpy as np

import mr_cutoff
import mr_ref

FS = 44100.0
N = 16384
LF = np.linspace(np.log2(25), np.log2(20000), 480)     # 1/48 オクターブ
DIRS = {0: "cutoff_full"}
DIRS.update({t: "cutoff_t%d" % t for t in (1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17, 19)})


# ---- 曲線を取り出す ----
def _spec(path):
    L, _ = mr_ref.read_wav(path)
    x = np.asarray(L)
    on = int(np.argmax(np.abs(x) > 1e-4))
    return mr_cutoff.spectrum(x[on + 4410:on + int((mr_cutoff.HOLD - 0.1) * 44100)])


def _tolog(p):
    f = np.fft.rfftfreq(N, 1 / FS)
    fl = np.log2(np.maximum(f, 1e-9))
    half = (LF[1] - LF[0]) / 2
    out = np.empty(len(LF))
    for i, l in enumerate(LF):
        m = (fl >= l - half) & (fl < l + half)
        out[i] = p[m].mean() if m.any() else np.interp(l, fl, p)
    return out


def _job(a):
    d, t, c = a
    return t, c, _tolog(_spec(os.path.join(d, "t%02d_c%03d.wav" % (t, c))))


def curves(root, out):
    refs = {t: _tolog(_spec(os.path.join(root, d, "thru.wav"))) for t, d in DIRS.items()}
    jobs = [(os.path.join(root, d), t, c) for t, d in DIRS.items() for c in range(256)]
    with ProcessPoolExecutor(3) as ex:       # 多くするとメモリが足りなくなる
        res = list(ex.map(_job, jobs, chunksize=8))
    Y = np.full((22, 256, len(LF)), np.nan)
    for t, c, p in res:
        Y[t, c] = 10 * np.log10((p + 1e-30) / (refs[t] + 1e-30))
    np.savez_compressed(out, Y=Y, LF=LF)


# ---- 模型（swp50_filter.h と同じ） ----
F = 2 ** LF
Z1 = np.exp(-1j * 2 * np.pi * np.minimum(F, FS / 2) / FS)


def lp1(g):
    return g / (1 - (1 - g) * Z1)


def sv(g, q, i):
    g = min(g, 0.82)
    a = 1 - Z1
    hp = 1 / (1 + Z1 * g * g / (a * a) + q * Z1 * g / a)
    bp = g * hp / a
    lp = g * bp / a
    return (lp, bp, hp, lp + hp)[i]


MODEL = {
    0: ("LPF24D", lambda g: sv(g, 2, 0) ** 2), 1: ("LPF24A", lambda g: sv(g, 1.414, 0) ** 2),
    2: ("LPF18", lambda g: sv(g, 2, 0) * lp1(g)), 3: ("LPF18s", lambda g: lp1(g) * lp1(min(0.999, g * 5.657))),
    4: ("LPF12", lambda g: sv(g, 2, 0)), 5: ("LPF6", lambda g: lp1(g)),
    6: ("HPF24D", lambda g: sv(g, 2, 2) ** 2), 7: ("HPF12", lambda g: sv(g, 2, 2)),
    8: ("BPF12D", lambda g: sv(g, 2, 1) ** 2), 10: ("BPFw", lambda g: sv(g, 1, 1) ** 2), 11: ("BPF6", lambda g: sv(g, 2, 1)),
    12: ("BEF12", lambda g: sv(g, 2, 3) ** 2), 13: ("BEF6", lambda g: sv(g, 2, 3)), 14: ("DualLPF", lambda g: sv(g, 2, 0)),
    15: ("DualHPF", lambda g: 1 + sv(g, 2, 2)), 16: ("DualBPF", lambda g: sv(g, 2, 1) + 0.707 * sv(g * 2 ** -7.5, 1.414, 1)),
    17: ("DualBEF", lambda g: sv(g, 2, 3)), 19: ("LPF12+BPF6", lambda g: sv(g, 2, 0)),
}


def fit_g(Y):
    """LPF24D の曲線に 1 次の低域通過 4 段を絶対の高さで当てはめ、目盛りごとの g を出す"""
    g = np.zeros(256)
    cand = np.exp(np.linspace(np.log(0.0005), np.log(0.999), 4000))
    H = np.array([80 * np.log10(np.abs(lp1(x))) for x in cand])
    for c in range(256):
        y = Y[0, c]
        m = (F > 25) & (F < 18000) & (y > -60) & np.isfinite(y)
        if m.sum() < 20:
            g[c] = np.nan
            continue
        e = ((H[:, m] - y[m]) ** 2).mean(axis=1)
        g[c] = cand[int(np.argmin(e))]
    return g


def check(npz):
    Y = np.load(npz)["Y"]
    g = fit_g(Y)
    bands = [(60, 100), (100, 140), (140, 180), (180, 210), (210, 226)]
    print("型           " + "  ".join("%3d-%3d" % b for b in bands) + "  （dB の RMS。通る所の高さのずれを除いたもの）")
    for t, (nm, fn) in MODEL.items():
        row = []
        for a, b in bands:
            es = []
            for c in range(a, b, 2):
                y = Y[t, c]
                h = 20 * np.log10(np.abs(fn(g[c])) + 1e-12)
                m = (F > 30) & (F < 16000) & (y > -45) & np.isfinite(y)
                d = y[m] - h[m]
                d -= np.median(d)
                es.append(np.sqrt(np.mean(d * d)))
            row.append(np.mean(es))
        print("%2d %-10s " % (t, nm) + "  ".join("%7.2f" % v for v in row))


if __name__ == "__main__":
    if sys.argv[1] == "curves":
        curves(sys.argv[2], sys.argv[3])
    else:
        check(sys.argv[2])
