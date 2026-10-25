"""波形のベロシティの層の境目を、1 刻みまで実機で詰める（2026-10-25）。
  python mr_layer_bounds.py 置き場 [--capture 取ったものの置き場] [--waves 1317,972]
samples.csv の層の境目（vel_lo）は、mr_velscan.py のベロシティ（1・16・32・48・64・80・96・104・112・120・127）で見た「変わった所」で、
本当の境目はその 1 つ前の点との間にある（1317 は一覧 104、実機は 100。ドラムの鍵でも雛形でも同じ）。雛形（mr_capture.py と同じ、フィルター THRU）の鍵 60 で、
前の点（下の層）と一覧の境目（上の層）の音を録り、その間を二分探索する（どちらの層の音に、スペクトルの形が近いか）。
結果は 置き場/layer_bounds.csv（wave, old, new）。もうある波形は飛ばす。送るのは Data List のパラメーターチェンジとノートだけ（保存しない）。終わったらマルチへ戻す。"""
import csv
import os
import sys
import time

import numpy as np

import mr_drum_compare as C
import mr_ref

VELSCAN = (1, 16, 32, 48, 64, 80, 96, 104, 112, 120, 127)


def main():
    a = sys.argv[1:]
    d = a[0]
    here = os.path.dirname(os.path.abspath(__file__))
    cap = mr_ref.opt(a, "--capture", os.path.join(here, "..", "..", "roms", "motif-rack-es", "capture"))
    only = mr_ref.opt(a, "--waves")
    bounds = {}
    for s in csv.DictReader(open(os.path.join(cap, "samples", "samples.csv"))):
        if int(s["vel_lo"]) > 1:
            bounds.setdefault(int(s["wave"]), set()).add(int(s["vel_lo"]))
    waves = sorted(bounds) if not only else [int(w) for w in only.split(",")]
    os.makedirs(d, exist_ok=True)
    out = os.path.join(d, "layer_bounds.csv")
    done = set()
    if os.path.exists(out):
        done = {int(r["wave"]) for r in csv.DictReader(open(out))}
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)

    def shape(v, w):
        r.all_off()
        time.sleep(0.08)
        r.one_note("_lb", note=60, vel=v, hold=0.45, tail=0.15, pre=0.25)
        f = C.feats(os.path.join(d, "_lb.wav"), True)
        return f[2] - f[2].max()

    try:
        r.all_off()
        r.param(0x00, 0x00, 0x34, 0)
        r.mode(0)
        r.program(63, 0, 0)
        r.template(waves[0])
        r.el(0, 0x43, 0x00, 21)
        r.el(0, 0x43, 0x01, 1, 127)
        with open(out, "a", newline="") as f:
            wc = csv.writer(f, lineterminator="\n")
            if not done:
                wc.writerow(["wave", "old", "new"])
            for w in waves:
                if w in done:
                    continue
                r.el(0, 0x41, 0x03, w >> 7, w & 0x7F)
                time.sleep(0.15)
                for b in sorted(bounds[w]):
                    prev = max([v for v in VELSCAN if v < b] or [1])
                    lo_s, hi_s = shape(prev, w), shape(b, w)
                    if np.mean(np.abs(lo_s - hi_s)) < 0.5:
                        wc.writerow([w, b, b])            # 2 点で違いが見えない（見立てを変えない）
                        continue
                    lo, hi = prev, b                       # lo は下の層、hi は上の層
                    while hi - lo > 1:
                        m = (lo + hi) // 2
                        s = shape(m, w)
                        if np.mean(np.abs(s - hi_s)) < np.mean(np.abs(s - lo_s)):
                            hi = m
                        else:
                            lo = m
                    wc.writerow([w, b, hi])
                    print("波形 %d: 境目 %d → %d" % (w, b, hi), flush=True)
                f.flush()
    finally:
        r.all_off()
        r.mode(3)
        r.close()


if __name__ == "__main__":
    main()
