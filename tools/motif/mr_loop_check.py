"""切り出したループが早すぎないか調べる（録音を読むだけ）。
  python mr_loop_check.py 録った置き場 切り出しの置き場 [出力.csv]
samples.csv の「loop」の行ごとに、ループの区間の音量と、生の録音の離す直前 0.3 秒の音量を比べる。
減っていく音で形だけそろった所をループにしてしまうと（相関は大きさを見ない）、ループのほうが大きい。差（dB）を書く。"""
import csv
import os
import sys

import numpy as np

import mr_zones as Z

FS = 44100
HOLD = 6.0


def main():
    src, sdir = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else os.path.join(sdir, "loop_check.csv")
    rows = [r for r in csv.DictReader(open(os.path.join(sdir, "samples.csv"))) if r["kind"] == "loop"]
    done = set()
    if os.path.exists(out):
        done = {r["file"] for r in csv.DictReader(open(out))}
    f = open(out, "a", newline="")
    w = csv.writer(f)
    if not done:
        w.writerow(["file", "loop_db", "end_db", "gap_db"])
    for i, r in enumerate(rows):
        if r["file"] in done:
            continue
        raw = os.path.join(src, r["file"].replace(".npz", ".wav"))
        if not os.path.exists(raw):
            continue
        z = np.load(os.path.join(sdir, r["file"]))
        y = z["L"] + z["R"]
        s, n = int(r["loop_start"]), int(r["loop_length"])
        ld = 10 * np.log10(np.mean(y[s:s + n].astype(np.float64) ** 2) + 1e-20)
        L, R = Z.read24(raw)
        x = L + R
        on = int(np.argmax(np.abs(x) > 2e-4))
        off = on + int(HOLD * FS) - int(0.05 * FS)
        ed = 10 * np.log10(np.mean(x[off - int(0.3 * FS):off] ** 2) + 1e-20)
        w.writerow([r["file"], "%.2f" % ld, "%.2f" % ed, "%.2f" % (ld - ed)])
        if i % 200 == 0:
            f.flush()
            print(i, len(rows), flush=True)
    f.close()


if __name__ == "__main__":
    main()
