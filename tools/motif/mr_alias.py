"""波形の下録り（mr_sweep.py）から、同じサンプルを共有している波形を見つける（サンプリング方式の録り直しを省くため）。
  python mr_alias.py [置き場（capture）]

区間の境目が同じ波形どうしで、鍵 36・60・84 の出だし 8000 サンプルを左右とも比べ、相関 0.99999 以上・音量の差 1% 以内なら同じとする。
結果は wave_alias.csv（wave, same_as）。録音を読むだけで、実機には何も送らない。"""
import csv
import os
import sys

import numpy as np

import mr_zones as Z

C = sys.argv[1] if len(sys.argv) > 1 else "../../roms/motif-rack-es/capture"
KEYS = (36, 60, 84)


def segs(w):
    base = os.path.join(C, "wave", "w%04d" % w)
    L, R = Z.read24(base + ".wav")
    marks = dict((int(a), float(b)) for a, b in (l.split() for l in open(base + ".txt")))
    out = {}
    for k in KEYS:
        s = int((marks[k] - 0.03) * 44100)
        seg, segR = L[s:s + int(0.45 * 44100)], R[s:s + int(0.45 * 44100)]
        hot = np.abs(seg) > 2e-4
        on = int(np.argmax(hot)) if hot.any() else 0
        out[k] = (seg[on:on + 8000], segR[on:on + 8000])
    return out


def same(a, b):
    for k in KEYS:
        for ch in (0, 1):
            x, y = a[k][ch], b[k][ch]
            if len(x) < 8000 or len(y) < 8000 or Z.corr(x, y, 7000) < 0.99999:
                return False
            if abs(np.sqrt(np.mean(x ** 2)) / (np.sqrt(np.mean(y ** 2)) + 1e-12) - 1) > 0.01:
                return False
    return True


def main():
    z = {int(r["wave"]): r for r in csv.DictReader(open(os.path.join(C, "zones_all.csv")))}
    groups = {}
    for w, r in z.items():
        groups.setdefault(r["boundaries"], []).append(w)
    alias = {}
    for ws in groups.values():
        if len(ws) < 2:
            continue
        ws = sorted(ws)
        cache = {w: segs(w) for w in ws}
        for i, w in enumerate(ws):
            if w in alias:
                continue
            for v in ws[i + 1:]:
                if v not in alias and same(cache[w], cache[v]):
                    alias[v] = w
    with open(os.path.join(C, "wave_alias.csv"), "w", newline="") as f:
        wr = csv.writer(f, lineterminator="\n")
        wr.writerow(["wave", "same_as"])
        for v, w in sorted(alias.items()):
            wr.writerow([v, w])
    print("同じ音の波形 %d 本" % len(alias))


if __name__ == "__main__":
    main()
