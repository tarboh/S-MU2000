"""mr_capture.py で録った長い音を、まとめて「出だし + ループ」（または鳴らし切り）に切り出す。
  python mr_extract_all.py 録った置き場 出力の置き場

録った置き場の capture.csv を読み、まだ切り出していないものを順に処理する（もうある .npz は飛ばす）。
* ループ: mr_extract.py と同じ（相関 0.999 以上をループとみなす）
* 鳴らし切り: 離す前に音が消える（最後の 0.3 秒が出だしの山より 70dB 以上小さい）なら、消えるまでをそのまま持つ
* どちらでもない（揺れていてループが見つからない）ものは、離すまでをそのまま持ち、印を付ける
結果の一覧は出力の置き場の samples.csv に書く。録音を読むだけで、実機には何も送らない。"""
import csv
import os
import sys

import numpy as np

import mr_extract as X
import mr_zones as Z

FS = 44100
HOLD = 6.0


def process(path, hold=HOLD):
    L, R = Z.read24(path)
    x = L + R
    on = int(np.argmax(np.abs(x) > 2e-4))
    off = on + int(hold * FS) - int(0.05 * FS)
    peak = float(np.max(np.abs(x[on:off]))) + 1e-12
    endrms = float(np.sqrt(np.mean(x[off - int(0.3 * FS):off] ** 2))) + 1e-12
    if 20 * np.log10(endrms / peak) < -70:
        # 鳴らし切り: 出だしの山から -90dB を切る所まで
        env = np.abs(x[on:off])
        above = np.nonzero(env > peak * 10 ** (-90 / 20))[0]
        end = on + (int(above[-1]) + 1 if len(above) else 0)
        return dict(kind="oneshot", L=L[on:end], R=R[on:end], loop_start=-1, loop_length=0, corr=0.0, stretch=1.0)
    try:
        oL, oR, start, Li, Lf, rr, k = X.extract(L, R, hold_end=off)
    except Exception:
        rr = 0.0
    if rr >= 0.999:
        return dict(kind="loop", L=oL, R=oR, loop_start=start, loop_length=Li, corr=rr, stretch=k)
    return dict(kind="raw", L=L[on:off], R=R[on:off], loop_start=-1, loop_length=0, corr=rr, stretch=1.0)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    rows = list(csv.DictReader(open(os.path.join(src, "capture.csv"))))
    out_csv = os.path.join(dst, "samples.csv")
    have = set()
    if os.path.exists(out_csv):
        have = {r["file"] for r in csv.DictReader(open(out_csv))}
    new = not os.path.exists(out_csv)
    with open(out_csv, "a", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        if new:
            w.writerow(["wave", "key_lo", "key_hi", "key", "vel_lo", "vel_hi", "vel", "file", "kind", "length", "loop_start", "loop_length", "corr", "stretch"])
        n = 0
        for r in rows:
            name = r["file"].replace(".wav", ".npz")
            if name in have or not os.path.exists(os.path.join(src, r["file"])):
                continue
            extra = r.get(None) or []                       # 古い見出し（hold の欄が無い）のあとに足された行
            hold = r.get("hold") or (extra[0] if extra else None)
            s = process(os.path.join(src, r["file"]), float(hold or HOLD))
            np.savez_compressed(os.path.join(dst, name), L=s["L"].astype(np.float32), R=s["R"].astype(np.float32),
                                loop_start=s["loop_start"], loop_length=s["loop_length"], stretch=s["stretch"])
            w.writerow([r["wave"], r["key_lo"], r["key_hi"], r["key"], r["vel_lo"], r["vel_hi"], r["vel"], name, s["kind"],
                        len(s["L"]), s["loop_start"], s["loop_length"], "%.6f" % s["corr"], "%.7f" % s["stretch"]])
            f.flush()
            n += 1
    print("%d 本を切り出した" % n)


if __name__ == "__main__":
    main()
