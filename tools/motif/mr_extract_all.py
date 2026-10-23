"""mr_capture.py で録った長い音を、まとめて「出だし + ループ」（または鳴らし切り）に切り出す。
  python mr_extract_all.py 録った置き場 出力の置き場 [--redo-raw] [--redo-list 一覧] [--no-xfade]

録った置き場の capture.csv を読み、まだ切り出していないものを順に処理する（もうある .npz は飛ばす）。
* ループ: mr_extract.py と同じ（相関 0.998 以上をループとみなす。小さい音の高い鍵は 0.998〜0.999 に来る）
* 鳴らし切り: 離す前に音が消える（最後の 0.3 秒が出だしの山より 70dB 以上小さい）なら、消えるまでをそのまま持つ
* どちらでもない（揺れていてループが見つからない）ものは、離す直前をクロスフェードでつないだループにする（xfade_loop）。
  相関 0.9 以上なら周期の整数倍の長さ（0.5 秒以上）で位相をそろえて直線につなぎ（xfade）、それより下は雑音として 0.5 秒をエネルギーを
  保つ形でつなぐ（xnoise）。--no-xfade なら前のように、離すまでをそのまま持つ（raw）
結果の一覧は出力の置き場の samples.csv に書く。録音を読むだけで、実機には何も送らない。"""
import csv
import os
import sys

import numpy as np

import mr_extract as X
import mr_zones as Z

FS = 44100
HOLD = 6.0


def process(path, hold=HOLD, xfade=True):
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
    Lf = 0.0
    try:
        oL, oR, start, Li, Lf, rr, k = X.extract(L, R, hold_end=off)
    except Exception:
        rr = 0.0
    if rr >= 0.998:
        return dict(kind="loop", L=oL, R=oR, loop_start=start, loop_length=Li, corr=rr, stretch=k)
    if not xfade:
        return dict(kind="raw", L=L[on:off], R=R[on:off], loop_start=-1, loop_length=0, corr=rr, stretch=1.0)
    # 相関が足りない: 離す直前の 0.5 秒以上をクロスフェードでつないだループにする
    return xfade_loop(L, R, on, off, Lf if rr >= XF_PITCHED else 0.0, rr)


XF_PITCHED = 0.9        # これ以上の相関なら、周期の整数倍の長さで位相をそろえてつなぐ（下は雑音として扱う）
XF_SECONDS = 0.5        # ループの長さの下限
XF_MAX = 0.15           # クロスフェードの長さの上限（秒。ループの 1/3 まで）


def xfade_loop(L, R, on, off, Lf, rr):
    """離す直前を、継ぎ目をクロスフェードで焼き込んだループにする。Lf（周期、サンプル）があれば、その整数倍の長さにして
    伸び縮みで整数にそろえ、直線のクロスフェード（位相がそろうので打ち消し合わない）。無ければ 0.5 秒・エネルギーを保つクロスフェード"""
    if Lf > 0:
        Lm = Lf * int(np.ceil(XF_SECONDS * FS / Lf))
    else:
        Lm = XF_SECONDS * FS
    Li = int(round(Lm))
    k = Lm / Li
    n = int((off - on) / k)
    t = on + np.arange(n) * k
    oL = np.interp(t, np.arange(len(L)), L)
    oR = np.interp(t, np.arange(len(R)), R)
    end = n - int(0.02 * FS)
    X_ = min(Li // 3, int(XF_MAX * FS))
    start = end - Li
    if start < X_ + int(0.05 * FS):
        return dict(kind="raw", L=L[on:off], R=R[on:off], loop_start=-1, loop_length=0, corr=rr, stretch=1.0)
    a = np.linspace(0.0, 1.0, X_)
    fa, fb = (1 - a, a) if Lf > 0 else (np.cos(a * np.pi / 2), np.sin(a * np.pi / 2))
    for o in (oL, oR):
        tail = o[start + Li - X_:start + Li].copy()
        pre = o[start - X_:start]
        o[start + Li - X_:start + Li] = tail * fa + pre * fb
    return dict(kind="xfade" if Lf > 0 else "xnoise", L=oL[:start + Li], R=oR[:start + Li], loop_start=start, loop_length=Li,
                corr=rr, stretch=k)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    if "--redo-raw" in sys.argv:
        # 「そのまま」にしたものを消して、もう一度切り出す（線や探し方を変えたあと）
        path = os.path.join(dst, "samples.csv")
        keep = []
        for r in csv.DictReader(open(path)):
            if r["kind"] == "raw":
                try:
                    os.remove(os.path.join(dst, r["file"]))
                except OSError:
                    pass
            else:
                keep.append(r)
        with open(path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(keep[0].keys()) if keep else [], lineterminator="\n")
            w.writeheader()
            w.writerows(keep)
    if "--redo-list" in sys.argv:
        # 一覧（1 行に 1 つ .npz の名前）にあるものを消して、もう一度切り出す（ループの探し方を変えたあと。mr_loop_check.py で見つけたもの）
        redo = {l.strip() for l in open(sys.argv[sys.argv.index("--redo-list") + 1]) if l.strip()}
        path = os.path.join(dst, "samples.csv")
        keep = []
        for r in csv.DictReader(open(path)):
            if r["file"] in redo:
                try:
                    os.remove(os.path.join(dst, r["file"]))
                except OSError:
                    pass
            else:
                keep.append(r)
        with open(path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(keep[0].keys()), lineterminator="\n")
            w.writeheader()
            w.writerows(keep)
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
            s = process(os.path.join(src, r["file"]), float(hold or HOLD), "--no-xfade" not in sys.argv)
            np.savez_compressed(os.path.join(dst, name), L=s["L"].astype(np.float32), R=s["R"].astype(np.float32),
                                loop_start=s["loop_start"], loop_length=s["loop_length"], stretch=s["stretch"])
            w.writerow([r["wave"], r["key_lo"], r["key_hi"], r["key"], r["vel_lo"], r["vel_hi"], r["vel"], name, s["kind"],
                        len(s["L"]), s["loop_start"], s["loop_length"], "%.6f" % s["corr"], "%.7f" % s["stretch"]])
            f.flush()
            n += 1
    print("%d 本を切り出した" % n)


if __name__ == "__main__":
    main()
