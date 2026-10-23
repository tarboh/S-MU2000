"""mr_clfo_dest.py の録音を読む（録音を読むだけ）。
  python mr_clfo_dest_read.py 置き場
コモン LFO が +1（鳴り始めから 0.5〜2.3 秒）と −1（2.9〜4.9 秒）に止まった所で、
  fmd : ノイズの谷の周波数 → LFO なしの録音（カットオフ 32〜224）の谷から、カットオフの目盛りに直す
  reso: カットオフ近くの山の高さ（100〜200Hz の平均との差）→ レゾナンスの目盛りに直す
  pan : 左右の dB
  lspd: エレメント LFO の揺れの速さ（Hz）
を出す。"""
import os
import sys

import numpy as np

import mr_clfo_read
import mr_ref

FS = 44100
WIN = ((0.5, 2.3), (2.9, 4.9))


def load(d, n):
    L, R = mr_ref.read_wav(os.path.join(d, n + ".wav"))
    L, R = np.asarray(L, dtype=np.float64), np.asarray(R, dtype=np.float64)
    on = int(np.argmax(np.abs(L) + np.abs(R) > 1e-4))
    return L[on:], R[on:]


def spectrum(x, t0, t1):
    seg = x[int(t0 * FS):int(t1 * FS)]
    N = 4096
    fr = np.fft.rfftfreq(N, 1 / FS)
    P = np.zeros(len(fr))
    for i in range(0, len(seg) - N, N // 2):
        P += np.abs(np.fft.rfft(seg[i:i + N] * np.hanning(N))) ** 2
    # 1/6 オクターブでならす
    S = np.array([P[(fr > f / 2 ** (1 / 12)) & (fr < f * 2 ** (1 / 12))].mean() if f > 30 else 0 for f in fr[:1600]])
    return fr[:1600], 10 * np.log10(S + 1e-20)


def notch(x, t0, t1):
    fr, s = spectrum(x, t0, t1)
    m = (fr > 60) & (fr < 16000)
    return fr[m][np.argmin(s[m])]


def peak(x, t0, t1):
    fr, s = spectrum(x, t0, t1)
    ref = s[(fr > 100) & (fr < 200)].mean()
    m = (fr > 200) & (fr < 4000)
    return s[m].max() - ref


def to_units(v, xs, ys):
    """値 v を、目盛り xs で測った ys から目盛りに直す（ys は単調）"""
    ys, xs = np.asarray(ys), np.asarray(xs)
    o = np.argsort(ys)
    return float(np.interp(v, ys[o], xs[o]))


def main():
    d = sys.argv[1]
    cs = (32, 64, 96, 128, 160, 192, 224)
    nf = [np.log2(notch(load(d, "st_fmd_c%03d" % c)[0], 0.5, 4.9)) for c in cs]
    print("BEF12 の谷:", " ".join("%d:%.0fHz" % (c, 2 ** f) for c, f in zip(cs, nf)))
    for dp in ("004", "008", "016", "032", "032h", "064", "127"):
        L, _ = load(d, "fmd" + dp)
        u = [to_units(np.log2(notch(L, *w)), cs, nf) for w in WIN]
        print("fmd%-4s +1: %.1f  −1: %.1f  （128 から %+.1f・%+.1f）" % (dp, u[0], u[1], u[0] - 128, u[1] - 128))
    qs = (0, 16, 32, 48, 64, 80, 96, 112, 127)
    pk = [peak(load(d, "st_reso_q%03d" % q)[0], 0.5, 4.9) for q in qs]
    print("LPF24D の山:", " ".join("%d:%.1fdB" % (q, p) for q, p in zip(qs, pk)))
    for n, b in (("reso016", 64), ("reso032", 64), ("reso064", 64), ("reso127", 64), ("reso032_b032", 32), ("reso032_b096", 96)):
        L, _ = load(d, n)
        p = [peak(L, *w) for w in WIN]
        u = [to_units(v, qs, pk) for v in p]
        print("%-12s +1: %.1fdB=%.1f  −1: %.1fdB=%.1f  （%d から %+.1f・%+.1f）" % (n, p[0], u[0], p[1], u[1], b, u[0] - b, u[1] - b))
    for dp in ("008", "016", "024", "032", "048", "064", "064h", "096", "127"):
        L, R = load(d, "pan" + dp)
        out = []
        for w in WIN:
            a, b = int(w[0] * FS), int(w[1] * FS)
            out.append("L %.1f R %.1f" % (10 * np.log10(np.mean(L[a:b] ** 2)), 10 * np.log10(np.mean(R[a:b] ** 2))))
        print("pan%-4s +1: %s  −1: %s" % (dp, out[0], out[1]))
    for n in ("lspdS002", "lspdS008", "lspdS016", "lspdS032"):
        c, _ = mr_clfo_read.series(os.path.join(d, n + ".wav"))
        hz = []
        for w in ((0.6, 5.0), (5.8, 10.3)):
            v = c[int(w[0] * 1000):int(w[1] * 1000)]
            v = v - v.mean()
            Y = np.abs(np.fft.rfft(v * np.hanning(len(v)), 1 << 16))
            fr = np.fft.rfftfreq(1 << 16, 1e-3)
            hz.append(fr[1 + int(np.argmax(Y[1:]))])
        print("%-8s +1: %.3fHz  −1: %.3fHz" % (n, hz[0], hz[1]))


if __name__ == "__main__":
    main()
