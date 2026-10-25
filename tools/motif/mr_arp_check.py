"""芯のアルペジオ（arp_dump、sampler_arp.h）の出すノートを、実機の録音（mr_arp.py の置き場）と比べる（2026-10-25）。
  python mr_arp_check.py 置き場 表.txt arp_dump.exe [--types 0-1786] [--probes n1,n3] [--show]
組（押さえた鍵）ごとに、1 周 + 1 拍のノートオンを（鍵、時刻 ±0.04 拍）で突き合わせ、
合った割合（実機の音のうち芯も出した割合・芯の音のうち実機も出した割合）、時刻・ベロシティ・長さの差を出す。乱れ（Rndm）の型は飛ばす。"""
import json
import os
import statistics
import subprocess
import sys

import mr_arp_fit as F


def hw_notes(j, g, L):
    beat = 60.0 / j["tempo"]
    raw = {p: F.events(pr, beat)[0] for p, pr in j["probes"].items()}
    off = F.align({p: [n[0] for n in raw[p] if n[0] < L - 0.05] for p in raw})
    out = {}
    for p, ns in raw.items():
        out[p] = [(n[0] - off[p] - g, n[1], n[2], n[3]) for n in ns if n[0] - off[p] - g < L + 0.9]
    return out


def core_notes(exe, prog, typ, keys, vel, beats):
    r = subprocess.run([exe, prog, str(typ), ",".join(str(k) for k in keys), str(vel), "%g" % beats, "120", "3"],
                       capture_output=True, text=True)
    res = []
    for line in r.stdout.split("\n"):
        f = line.split()
        if len(f) == 4:
            res.append((float(f[0]), int(f[1]), int(f[2]), None if float(f[3]) < 0 else float(f[3])))
    return res


def match(a, b, tol=0.04):
    """a・b: [(拍, 鍵, ベロシティ, 長さ)]。鍵が同じで時刻の近いものを組にする"""
    used = set()
    pairs = []
    for i, x in enumerate(a):
        best = None
        for k, y in enumerate(b):
            if k in used or y[1] != x[1]:
                continue
            d = abs(y[0] - x[0])
            if d < tol and (best is None or d < best[0]):
                best = (d, k)
        if best:
            used.add(best[1])
            pairs.append((x, b[best[1]]))
    return pairs


def main():
    a = sys.argv[1:]
    d, prog, exe = a[0], a[1], a[2]
    lo, hi = 0, 1786
    if "--types" in a:
        lo, hi = (int(x) for x in a[a.index("--types") + 1].split("-"))
    probes = a[a.index("--probes") + 1].split(",") if "--probes" in a else None
    files = {int(f[1:5]): os.path.join(d, f) for f in os.listdir(d) if f.endswith(".json")}
    js = {t: json.load(open(p, encoding="utf-8")) for t, p in sorted(files.items()) if lo <= t <= hi}
    g = 0.0
    for line in open(prog, encoding="utf-8"):
        if line.startswith("# delay="):
            g = float(line[8:])
    tot = {"hw": 0, "core": 0, "pair": 0}
    dts, dvs, dgs = [], [], []
    worst = []
    for t, j in js.items():
        info = j["info"]
        if "Rndm" in info["name"]:
            continue
        L = info["measures"] * info["beats"] * 4.0 / info["unit"]
        hw = hw_notes(j, g, L)
        th = tc = tp = 0
        for p, ns in hw.items():
            if probes and p not in probes:
                continue
            pr = j["probes"][p]
            # 頭の 1 拍は鍵の届き方で欠けることがあるので、2 周目の頭（L〜L+0.9）までを比べ、1 周目の頭は除く
            h = [n for n in ns if 0.95 <= n[0] < L + 0.9]
            c = [n for n in core_notes(exe, prog, t, pr["keys"], pr["vel"], L + 1) if 0.95 <= n[0] < L + 0.9]
            pairs = match(h, c)
            th += len(h)
            tc += len(c)
            tp += len(pairs)
            for x, y in pairs:
                dts.append(y[0] - x[0])
                dvs.append(y[2] - x[2])
                if x[3] is not None and y[3] is not None and x[0] + x[3] < L + 0.85:
                    dgs.append(y[3] - x[3])
            if "--show" in a and (len(pairs) < len(h) or len(pairs) < len(c)):
                print("  %d %s 実機 %s" % (t, p, " ".join("%.2f:%d" % (n[0], n[1]) for n in h[:20])))
                print("  %d %s 芯   %s" % (t, p, " ".join("%.2f:%d" % (n[0], n[1]) for n in c[:20])))
        tot["hw"] += th
        tot["core"] += tc
        tot["pair"] += tp
        r = min(tp / th if th else 1, tp / tc if tc else 1)
        worst.append((r, t, info["cat"], info["name"], th, tc, tp))
    worst.sort()
    for r, t, cat, name, th, tc, tp in worst[:25]:
        print("%5.3f 型 %d %s %s 実機 %d 芯 %d 合った %d" % (r, t, cat, name, th, tc, tp))
    print("型 %d: 実機の音 %d・芯の音 %d・合った %d（実機の %.4f・芯の %.4f）" % (
        len(worst), tot["hw"], tot["core"], tot["pair"], tot["pair"] / max(1, tot["hw"]), tot["pair"] / max(1, tot["core"])))
    if dts:
        print("時刻の差 中央 %+.4f 拍・|差| の 95%% %.4f 拍" % (statistics.median(dts), sorted(abs(x) for x in dts)[int(len(dts) * 0.95)]))
        print("ベロシティが同じ %.4f" % (sum(1 for x in dvs if x == 0) / len(dvs)))
    if dgs:
        print("長さの差 |差| の中央 %.4f 拍・95%% %.4f 拍" % (statistics.median(abs(x) for x in dgs), sorted(abs(x) for x in dgs)[int(len(dgs) * 0.95)]))


if __name__ == "__main__":
    main()
