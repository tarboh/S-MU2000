"""mr_velscan.py（または mr_vel.py）の録音から、ベロシティの層の境目を見立てる。
  python mr_velzones.py 置き場 [-o velzones.csv]

隣り合うベロシティの音を、出だしのずれを広く探した相関で比べる（大きさは比べない。音色の形だけ）。
相関が 0.97 を下回る所を層の境目とする。音の大きさの比（dB）も一緒に出す。"""
import csv
import os
import sys

import numpy as np

import mr_zones as Z


def segs(base):
    L, R = Z.read24(base + ".wav")
    x = L + R
    out = {}
    for l in open(base + ".txt"):
        v, t = l.split()
        s = int((float(t) - 0.03) * 44100)
        seg = x[s:s + int(0.45 * 44100)]
        hot = np.abs(seg) > 2e-4
        if hot.any():
            on = int(np.argmax(hot))
            out[int(v)] = seg[on:on + 8000]
    return out


def sim(a, b, m=4000):
    if len(a) < m + 2 * Z.W or len(b) < m:
        return 0.0
    return Z.xc(np.concatenate([np.zeros(Z.W), a[:m + 2 * Z.W]]), b[:m], Z.W)


def analyse(base):
    S = segs(base)
    vs = sorted(S)
    bounds, cs = [], []
    for v, v2 in zip(vs, vs[1:]):
        c = sim(S[v], S[v2])
        cs.append(c)
        if c < 0.97:
            bounds.append(v2)
    lv = {v: 20 * np.log10(np.sqrt(np.mean(S[v][:4000] ** 2)) + 1e-12) for v in vs}
    return vs, bounds, cs, lv


def main():
    a = sys.argv[1:]
    out = None
    if "-o" in a:
        i = a.index("-o")
        out = a[i + 1]
        del a[i:i + 2]
    d = a[0]
    names = sorted(f[:-4] for f in os.listdir(d) if f.endswith(".wav") and f[0] in "vw" and f[1:5].isdigit())
    rows = []
    for nmn in names:
        vs, bounds, cs, lv = analyse(os.path.join(d, nmn))
        rows.append([int(nmn[1:5]), len(bounds) + 1, " ".join(map(str, bounds)), " ".join("%.3f" % c for c in cs),
                     " ".join("%.1f" % (lv[v] - lv[vs[-1]]) for v in vs)])
        if not out:
            print("%4d 層 %d 境目 %s" % (rows[-1][0], rows[-1][1], rows[-1][2]))
    if out:
        with open(out, "w", newline="") as f:
            wr = csv.writer(f, lineterminator="\n")
            wr.writerow(["wave", "layers", "bounds", "adjacent_corr", "level_db_vs_top"])
            wr.writerows(rows)
        print("%d 波形" % len(rows))


if __name__ == "__main__":
    main()
