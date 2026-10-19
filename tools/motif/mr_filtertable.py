"""mr_cutoff.py の録音（t??_c???.wav と thru.wav）から、フィルターの型ごとにカットオフの目盛り → 周波数の表を作る。
  python mr_filtertable.py 置き場 [置き場 …] [-o filter_table.csv]

型の形ごとに、測るものを変える（素通しの thru.wav との差のスペクトルで見る）。
  低域通過（0-5・14・19）: 低域（25〜40Hz）の高さから −3dB・−6dB 下がる点
  高域通過（6・7・15）    : 高域（15〜18kHz）の高さから、高いほうから見て −3dB・−6dB 下がる点
  帯域通過（8・10・11・16）: いちばん高い所（山の中心）と、そこから −3dB の幅
  帯域阻止（12・13・17）  : いちばん低い所（谷の中心）と、その深さ
録音を読むだけで、実機には何も送らない。"""
import csv
import math
import os
import re
import sys

import numpy as np

import mr_cutoff
import mr_ref

LP = {0, 1, 2, 3, 4, 5, 14, 19}
HP = {6, 7, 15}
BP = {8, 10, 11, 16}
BE = {12, 13, 17}


def response(d, name, f, ref):
    L, _ = mr_ref.read_wav(os.path.join(d, name))
    on = int(np.argmax(np.abs(np.asarray(L)) > 1e-4))
    p = mr_cutoff.smooth(mr_cutoff.spectrum(L[on + 4410:on + int((mr_cutoff.HOLD - 0.1) * 44100)]), f)
    return 10 * np.log10((p + 1e-30) / (ref + 1e-30))


def down_from_top(f, y, lvl):
    """高いほうから見て、初めて lvl を下回る点"""
    m = (f > 20) & (f < 20000)
    f, y = f[m], y[m]
    for i in range(len(y) - 1, 0, -1):
        if (y[i] - lvl) * (y[i - 1] - lvl) < 0:
            t = (lvl - y[i]) / (y[i - 1] - y[i])
            return math.exp(math.log(f[i]) + t * (math.log(f[i - 1]) - math.log(f[i])))
    return float("nan")


def measure(t, f, y):
    band = (f > 30) & (f < 19000)
    if t in LP:
        pas = float(np.median(y[(f > 25) & (f < 40)]))
        return pas, mr_cutoff.crossing(f, y, pas - 3), mr_cutoff.crossing(f, y, pas - 6), float("nan")
    if t in HP:
        pas = float(np.median(y[(f > 15000) & (f < 18000)]))
        return pas, down_from_top(f, y, pas - 3), down_from_top(f, y, pas - 6), float("nan")
    if t in BP:
        i = int(np.argmax(np.where(band, y, -1e9)))
        top = float(y[i])
        lo = hi = i
        while lo > 0 and y[lo] > top - 3:
            lo -= 1
        while hi < len(y) - 1 and y[hi] > top - 3:
            hi += 1
        return top, float(f[i]), float(f[lo]), float(f[hi])
    if t in BE:
        i = int(np.argmin(np.where(band, y, 1e9)))
        return float(y[i]), float(f[i]), float("nan"), float("nan")
    return float("nan"), float("nan"), float("nan"), float("nan")


def main():
    a = sys.argv[1:]
    out = "filter_table.csv"
    if "-o" in a:
        i = a.index("-o")
        out = a[i + 1]
        del a[i:i + 2]
    f = np.fft.rfftfreq(mr_cutoff.N, 1 / 44100)
    rows = []
    for d in a:
        L, _ = mr_ref.read_wav(os.path.join(d, "thru.wav"))
        on = int(np.argmax(np.abs(np.asarray(L)) > 1e-4))
        ref = mr_cutoff.smooth(mr_cutoff.spectrum(L[on + 4410:on + int((mr_cutoff.HOLD - 0.1) * 44100)]), f)
        for name in sorted(os.listdir(d)):
            m = re.match(r"t(\d\d)_c(\d\d\d)\.wav$", name)
            if not m:
                continue
            t, c = int(m.group(1)), int(m.group(2))
            v = measure(t, f, response(d, name, f, ref))
            rows.append([t, c] + [round(x, 2) for x in v])
        print(d, len(rows), flush=True)
    with open(out, "w", newline="\n") as fo:
        w = csv.writer(fo, lineterminator="\n")
        w.writerow(["type", "cutoff", "level_db", "f1", "f2", "f3"])
        w.writerows(rows)
    print("書いた: %s（%d 行）。列の意味: 低域・高域通過 = 通る所の高さ, −3dB, −6dB / 帯域通過 = 山の高さ, 中心, −3dB の下, 上 / 帯域阻止 = 谷の深さ, 中心" % (out, len(rows)))


if __name__ == "__main__":
    main()
