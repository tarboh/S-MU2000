"""mr_drum_capture.py で録ったドラムの波形（鍵 60 + コースチューン）を、芯のサンプル（.npz）に切り出す（2026-10-25）。
  python mr_drum_extract.py [録った置き場（既定 capture/drumwave）] [出力の置き場（既定 capture/samples_drum）]
切り出し方は mr_extract_all.py と同じ（鳴り終わるものは鳴らし切り、鳴り続けるものはループ）。区間はその鍵だけ（key_lo = key_hi）。
一覧は出力の置き場の samples.csv（mr_extract_all.py と同じ欄）。もうある .npz は飛ばす。録音を読むだけで、実機には何も送らない。"""
import csv
import os
import sys

import numpy as np

import mr_extract_all as E

COLS = ["wave", "key_lo", "key_hi", "key", "vel_lo", "vel_hi", "vel", "file", "kind", "length", "loop_start", "loop_length", "corr", "stretch"]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    cap = os.path.join(here, "..", "..", "roms", "motif-rack-es", "capture")
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(cap, "drumwave")
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(cap, "samples_drum")
    os.makedirs(dst, exist_ok=True)
    out = os.path.join(dst, "samples.csv")
    have = {}
    if os.path.exists(out):
        for r in csv.DictReader(open(out)):
            have[r["file"]] = r
    rows = list(have.values())
    n = 0
    for r in csv.DictReader(open(os.path.join(src, "capture.csv"))):
        name = r["file"].replace(".wav", ".npz")
        if name in have or not os.path.exists(os.path.join(src, r["file"])):
            continue
        s = E.process(os.path.join(src, r["file"]), hold=float(r["hold"]))
        np.savez_compressed(os.path.join(dst, name), L=s["L"].astype(np.float32), R=s["R"].astype(np.float32),
                            loop_start=s["loop_start"], loop_length=s["loop_length"], stretch=s["stretch"])
        rows.append(dict(wave=r["wave"], key_lo=r["key"], key_hi=r["key"], key=r["key"], vel_lo=r["vel_lo"], vel_hi=r["vel_hi"], vel=r["vel"],
                         file=name, kind=s["kind"], length=len(s["L"]), loop_start=s["loop_start"], loop_length=s["loop_length"],
                         corr="%.6f" % s["corr"], stretch="%.7f" % s["stretch"]))
        n += 1
    with open(out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=COLS, lineterminator="\n")
        w.writeheader()
        for r in rows:
            w.writerow(r)
    print("切り出した %d 本（全部で %d 本）" % (n, len(rows)))


if __name__ == "__main__":
    main()
