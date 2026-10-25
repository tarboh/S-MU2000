"""mr_layer_bounds.py で詰めた層の境目を、サンプルの一覧（samples/samples.csv・samples_drum/samples.csv）に入れる（2026-10-25）。
  python mr_layer_apply.py [capture の置き場]
境目 old を new に: vel_lo == old の行は new に、vel_hi == old − 1 の行は new − 1 にする。前の一覧は .bak に残す（もう .bak があれば残さない）。
パッチは作り直すこと（ふつうの音色は mr_patch.py、ドラムは mr_drum_patch.py）。"""
import csv
import os
import shutil
import sys


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    cap = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "..", "roms", "motif-rack-es", "capture")
    nb = {}
    for r in csv.DictReader(open(os.path.join(cap, "layerbounds", "layer_bounds.csv"))):
        if r["old"] != r["new"]:
            nb[(int(r["wave"]), int(r["old"]))] = int(r["new"])
    for sub in ("samples", "samples_drum"):
        p = os.path.join(cap, sub, "samples.csv")
        if not os.path.exists(p):
            continue
        if not os.path.exists(p + ".bak"):
            shutil.copy(p, p + ".bak")
        rows = list(csv.DictReader(open(p + ".bak")))
        cols = list(rows[0].keys())
        n = 0
        for r in rows:
            w = int(r["wave"])
            lo, hi = int(r["vel_lo"]), int(r["vel_hi"])
            if (w, lo) in nb:
                r["vel_lo"] = str(nb[(w, lo)])
                n += 1
            if (w, hi + 1) in nb:
                r["vel_hi"] = str(nb[(w, hi + 1)] - 1)
                n += 1
        with open(p, "w", newline="") as f:
            wr = csv.DictWriter(f, fieldnames=cols, lineterminator="\n")
            wr.writeheader()
            for r in rows:
                wr.writerow(r)
        print("%s: %d 欄を直した（境目 %d 個）" % (sub, n, len(nb)))


if __name__ == "__main__":
    main()
