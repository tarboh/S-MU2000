"""mr_arp.py で録ったアルペジオの出力を、拍の並びに直して見る（2026-10-25）。
  python mr_arp_read.py 置き場 [型の番号,…] [--probes c1,cmaj]
1 周（型の長さ）の中のノートを「拍（1/480 に丸める）:鍵:ベロシティ:長さ（拍）」で出す。録音を読むだけ。"""
import json
import os
import sys

LAT = 0.0025      # 鍵を送ってから最初の音が出るまで（2〜3ms）


def notes(probe, tempo, length_beats):
    """ノートオンとオフを組にして、（拍, 鍵, ベロシティ, 長さ）の並び（1 周目だけ）"""
    beat = 60.0 / tempo
    on = {}
    out = []
    for t, st, a, b in probe["events"]:
        if st & 0xF0 == 0x90 and b > 0:
            on.setdefault(a, []).append((t, b))
        elif st & 0xF0 == 0x80 or (st & 0xF0 == 0x90 and b == 0):
            if on.get(a):
                t0, v = on[a].pop(0)
                out.append((t0, a, v, t - t0))
    for a, lst in on.items():
        for t0, v in lst:
            out.append((t0, a, v, None))
    res = []
    for t0, a, v, g in sorted(out):
        b = round((t0 - LAT) / beat * 480) / 480
        if b >= length_beats - 1e-6:
            continue
        res.append((b, a, v, None if g is None else round(g / beat * 480) / 480))
    return res


def length_beats(info):
    return info["measures"] * info["beats"] * 4.0 / info["unit"]


def main():
    a = sys.argv[1:]
    d = a[0]
    probes = None
    if "--probes" in a:
        probes = a[a.index("--probes") + 1].split(",")
    ts = [int(x) for x in a[1].split(",")] if len(a) > 1 and not a[1].startswith("--") else sorted(int(f[1:5]) for f in os.listdir(d) if f.endswith(".json"))
    for t in ts:
        p = os.path.join(d, "t%04d.json" % t)
        if not os.path.exists(p):
            continue
        j = json.load(open(p, encoding="utf-8"))
        info = j["info"]
        L = length_beats(info)
        print("型 %d %s %03d %s（%g 拍）" % (t, info["cat"], info["num"], info["name"], L))
        for name, pr in j["probes"].items():
            if probes and name not in probes:
                continue
            ns = notes(pr, j["tempo"], L)
            print("  %-9s %s" % (name, " ".join("%g:%d:%d" % (b, k, v) for b, k, v, g in ns[:24])))


if __name__ == "__main__":
    main()
