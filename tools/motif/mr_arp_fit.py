"""mr_arp.py で録ったアルペジオの出力から、型ごとの「押さえた音の数ごとのフレーズ」を作る（2026-10-25）。
  python mr_arp_fit.py 置き場 出力.txt [--types 0-1786] [--report]
押さえた音の数 n（1〜6・8）ごとに、1 周ぶんのノートを「拍の位置（1 拍 480）・長さ・ベロシティ・どの音か」で書く。
「どの音か」は、押さえた音を低い順に並べた順位と、そこからのオクターブ（r順位:オクターブ）。押さえた音に依らない音（ドラムのパターンなど）は
鍵の番号そのまま（a鍵）。押さえた音のどれでもない音は、いちばん低い音からの半音（s半音）。
Seq の UpOct などのように、周をまたいでも数え続ける型（walk）は、上り・下りなどの決まりとオクターブの数だけを書く。
出力はヤマハのデータから作ったものなので配らないこと（roms/ の下に置く）。"""
import json
import os
import statistics
import sys

PPQ = 480
TOL = 0.025                  # 同じ時刻とみなす幅（拍）。録った時刻の揺れは、組ごとのずれを除くと ±0.01 拍ほど


def events(probe, beat):
    """ノートオンとオフを組にして（拍, 鍵, ベロシティ, 長さ（拍、離すまで鳴っていたら None））。ほかに CC・PB を（拍, ステータス, d1, d2）"""
    on = {}
    notes, ctl = [], []
    for t, st, a, b in probe["events"]:
        t = t / beat
        if st & 0xF0 == 0x90 and b > 0:
            on.setdefault(a, []).append((t, b))
        elif st & 0xF0 == 0x80 or (st & 0xF0 == 0x90 and b == 0):
            if on.get(a):
                t0, v = on[a].pop(0)
                notes.append([t0, a, v, t - t0])
        elif st & 0xF0 in (0xB0, 0xE0):
            ctl.append([t, st & 0xF0, a, b])
    off = probe.get("off")
    for a, lst in on.items():
        for t0, v in lst:
            notes.append([t0, a, v, None])
    # 離した時刻まで鳴っていたノートは、長さが分からない（離したので切れた）
    if off is not None:
        for n in notes:
            if n[3] is not None and abs(n[0] + n[3] - off / beat) < 0.05:
                n[3] = None
    notes.sort(key=lambda n: (n[0], n[1], n[2]))
    return notes, ctl


def align(per_probe):
    """組ごとの始まりのずれ（アルペジオが内部の刻みで始まるため）を、ほかの組とそろうように除く。per_probe: {名前: [時刻...]}"""
    off = {p: 0.0 for p in per_probe}
    for _ in range(4):
        pts = sorted((t - off[p], p) for p, ts in per_probe.items() for t in ts)
        if not pts:
            break
        clusters, cur = [], [pts[0]]
        for x in pts[1:]:
            if x[0] - cur[-1][0] > TOL:
                clusters.append(cur)
                cur = [x]
            else:
                cur.append(x)
        clusters.append(cur)
        diff = {p: [] for p in per_probe}
        for c in clusters:
            m = statistics.median(t for t, _ in c)
            for t, p in c:
                diff[p].append(t - m)
        for p in off:
            if diff[p]:
                off[p] += statistics.median(diff[p])
    return off


def label(key, held):
    """鍵を、押さえた音（低い順）の順位とオクターブにする。同じ音名が 2 つあるとき（60 と 72）は、オクターブの小さいほう"""
    best = None
    for i, h in enumerate(held):
        if (key - h) % 12 == 0:
            o = (key - h) // 12
            if best is None or abs(o) < abs(best[1]):
                best = (i, o)
    return best


WALKS = ("up", "down", "updn_a", "updn_b")


def walk_index(kind, s, m):
    if kind == "up":
        return s % m
    if kind == "down":
        return m - 1 - s % m
    if kind == "updn_a":
        s %= 2 * m
        return s if s < m else 2 * m - 1 - s
    if m == 1:
        return 0
    s %= 2 * m - 2
    return s if s < m else 2 * m - 2 - s


def walk_list(held, lo, O, order):
    """歩く音の並び: 押さえた音をオクターブ lo〜lo+O−1 に広げたもの。order 0 は低い順、1 は音ごとにオクターブを先に並べる（Oct2Up など）"""
    if order == 0:
        return [h + 12 * o for o in range(lo, lo + O) for h in held]
    return [h + 12 * o for h in held for o in range(lo, lo + O)]


def path_seq(segs, n):
    """道のり（オクターブと端 b/t の組を結んだ区間の並び）を、オクターブ 0 の最低音を 0 とした順位の並びにする。各区間は両端を含む"""
    out = []
    for o1, e1, o2, e2 in segs:
        a = o1 * n + (n - 1 if e1 == "t" else 0)
        b = o2 * n + (n - 1 if e2 == "t" else 0)
        out += list(range(a, b + 1)) if b >= a else list(range(a, b - 1, -1))
    return out


def walk_note(v, held, s):
    """声部 v =（kind, lo, O, order, rev, start）の s 歩目の鍵。start 0 はオクターブ 0 のいちばん低い音、1 はいちばん高い音から歩き始める。
    v が（"path", 区間...）なら、その道のりを繰り返す。（"rand", lo, O）は乱れなので None"""
    if v[0] == "rand":
        return None
    if v[0] == "path":
        n = len(held)
        q = path_seq(v[1:], n)
        i = q[s % len(q)]
        return held[i % n] + 12 * (i // n)
    kind, lo, O, order, rev, start = v
    E = walk_list(held, lo, O, order)
    n = len(held)
    oc = min(max(0, lo), lo + O - 1)        # 歩き始めるオクターブ（範囲に 0 が入っていなければ、0 に近い端）
    if order == 0:
        i0 = (oc - lo) * n + (n - 1 if start else 0)
    else:
        i0 = (n - 1 if start else 0) * O + (oc - lo)
    if not 0 <= i0 < len(E):
        return None
    if rev:
        E = E[::-1]
        i0 = len(E) - 1 - i0
    # 始めの位置が、その歩き方の何歩目にあたるかを探して、そこから s 歩進める
    m = len(E)
    for k in range(2 * m):
        if walk_index(kind, k, m) == i0:
            return E[walk_index(kind, k + s, m)]
    return None


# 決まりで探せない型（名前で決める）。乱れの声部は（"rand", 最低のオクターブ, オクターブの数）
NAMED = {
    "RndmOct1": [("rand", 0, 1)], "RndmOct2": [("rand", 0, 2)], "RndmOct4": [("rand", 0, 4)],
    "Up&Rndm1": [("up", 0, 1, 0, 0, 0), ("rand", 1, 1)], "Up&Rndm2": [("up", 0, 1, 0, 0, 0), ("rand", 1, 2)],
    "Up&Rndm4": [("up", 0, 1, 0, 0, 0), ("rand", 1, 4)],
    # 下の声部はオクターブ 0 を上ってから 2 オクターブ下まで下り、上の声部は 1 オクターブ上を下ってから 3 オクターブ上まで上る
    "Up&Down4": [("path", (0, "b", 0, "t"), (0, "t", -2, "b")), ("path", (1, "t", 1, "b"), (1, "b", 3, "t"))],
}


def score_voices(voices, steps):
    """声部の組で、歩みの何割が説明できるか（乱れの声部は、鳴った音が範囲に入っているかで見る）"""
    hit = tot = 0
    for held, st in steps.values():
        for s, ks in enumerate(st):
            tot += 1
            want = [walk_note(v, held, s) for v in voices if v[0] != "rand"]
            rest = list(ks)
            ok = True
            for w in want:
                if w in rest:
                    rest.remove(w)
                else:
                    ok = False
            for v in voices:
                if v[0] == "rand":
                    E = walk_list(held, v[1], v[2], 0)
                    hitr = [k for k in rest if k in E]
                    if hitr:
                        rest.remove(hitr[0])
                    else:
                        ok = False
            hit += ok and not rest
    return hit / tot if tot else 0


def fit_walk(steps):
    """steps: {組: (押さえた音, [その歩みで鳴った鍵の組...])}。声部を、上り・下り・上下 × オクターブの範囲 × 並べ方 × 向き × 始めの位置から探し、
    全部の歩みを（90% 以上）説明できる組み合わせを返す"""
    import itertools
    cands = []
    for kind in WALKS:
        for O in (1, 2, 3, 4):
            for lo in range(-O, 2):
                for order in ((0, 1) if O > 1 else (0,)):
                    for rev in (0, 1):
                        for start in (0, 1):
                            v = (kind, lo, O, order, rev, start)
                            hit = tot = 0
                            for held, st in steps.values():
                                for s, ks in enumerate(st):
                                    tot += 1
                                    hit += walk_note(v, held, s) in ks
                            if tot and hit >= 0.9 * tot:
                                cands.append((hit, v))
    cands.sort(reverse=True)
    V = statistics.mode(len(ks) for _, st in steps.values() for ks in st)     # 声部の数（頭の乱れで増えた歩みは数えない）
    best = None
    for combo in itertools.combinations([v for _, v in cands[:24]], V):
        hit = tot = 0
        for held, st in steps.values():
            for s, ks in enumerate(st):
                tot += 1
                hit += sorted(walk_note(v, held, s) for v in combo) == sorted(ks)
        if best is None or hit > best[0]:
            best = (hit, tot, combo)
    if best and best[0] >= 0.9 * best[1]:
        return list(best[2]), best[0] / best[1]
    return None, best[0] / best[1] if best else 0


def fit_type(j, g):
    info = j["info"]
    beat = 60.0 / j["tempo"]
    L = info["measures"] * info["beats"] * 4.0 / info["unit"]
    raw = {}
    for name, pr in j["probes"].items():
        raw[name] = events(pr, beat)
    off = align({p: [n[0] for n in raw[p][0] if n[0] < L - 0.05] for p in raw})
    for p in raw:
        for n in raw[p][0]:
            n[0] -= off[p] + g
        for c in raw[p][1]:
            c[0] -= off[p] + g
    out = {"info": info, "len": int(round(L * PPQ)), "lists": {}, "ctl": None, "kind": "table"}
    # 周をまたいで数え続けるか: 2 周目の頭の 1 拍が 1 周目の頭と違う（音の数 2 以上の組で）
    walk = 0
    total = 0
    for p, (ns, _) in raw.items():
        held = sorted(set(j["probes"][p]["keys"]))
        if len(held) < 2:
            continue
        a = [n[1] for n in ns if -0.1 <= n[0] < 0.95]
        b = [n[1] for n in ns if L - 0.05 <= n[0] < L + 0.95]
        if a and b:
            total += 1
            if a != b and not set(a) < set(b):
                walk += 1
    # Seq の 001〜025（UpOct〜Up&Rndm）は、音の数によっては 1 周がちょうど割り切れて同じに見えるので、いつも歩みとして当てはめる
    if (total and walk * 2 > total) or (info["cat"] == "Seq" and info["num"] <= 25):
        steps = {}
        for p, (ns, _) in raw.items():
            pr = j["probes"][p]
            if pr["vel"] != 100:
                continue
            st = []
            for n in ns:
                if not -0.1 <= n[0] < L + 0.95:
                    continue
                if st and n[0] - st[-1][0] < TOL * 2:
                    st[-1][1].append(n[1])
                else:
                    st.append((n[0], [n[1]]))
            steps[p] = (sorted(set(pr["keys"])), [sorted(k) for _, k in st])
        if info["name"] in NAMED:
            voices = NAMED[info["name"]]
            score = score_voices(voices, steps)
        else:
            voices, score = fit_walk(steps)
        ns, _ = raw["n1"]
        rhythm = []
        for n in ns:
            if -0.1 <= n[0] < L - 0.05 and not (rhythm and n[0] * PPQ - rhythm[-1][0] < TOL * 2 * PPQ):
                rhythm.append((int(round(max(0, n[0]) * PPQ)), None if n[3] is None else int(round(n[3] * PPQ)), n[2]))
        if voices:
            out["kind"] = "walk"
            out["walk"] = voices
            out["score"] = score
            out["rhythm"] = rhythm
            return out
        # 歩き方が見つからなければ、表の型として 1 周ぶんを持つ（周をまたいだ続きは合わないが、1 周目は録ったとおりに鳴る）
        out["walk_fail"] = score
    # n1 と d1（2 半音上）を比べ、押さえた音に依らない音（a）を決める
    fixed = []
    if "n1" in raw and "d1" in raw:
        d1 = [n for n in raw["d1"][0] if -0.1 <= n[0] < L + 0.95]
        for n in raw["n1"][0]:
            if any(abs(m[0] - n[0]) < TOL * 2 and m[1] == n[1] for m in d1) and not any(abs(m[0] - n[0]) < TOL * 2 and m[1] == n[1] + 2 for m in d1):
                fixed.append((n[0], n[1]))
    for p, (ns, ctl) in raw.items():
        pr = j["probes"][p]
        held = sorted(set(pr["keys"]))
        cyc = [n for n in ns if -0.1 <= n[0] < L - 0.05]
        nxt = [[n[0] - L] + n[1:] for n in ns if L - 0.05 <= n[0] < L + 0.95]
        # 頭の 1 拍は、鍵が届く前に始まって音が欠けることがある。2 周目の頭が 1 周目を含むなら、そちらを使う
        head = [n for n in cyc if n[0] < 0.95]
        if nxt and set(n[1] for n in head) < set(n[1] for n in nxt):
            cyc = [n for n in cyc if n[0] >= 0.95] + nxt
            cyc.sort()
        lst = []
        for t, k, v, gt in cyc:
            tick = max(0, int(round(t * PPQ)))
            gate = None if gt is None else max(1, int(round(gt * PPQ)))
            if any(abs(t - ft) < TOL * 2 and k == fk for ft, fk in fixed):
                spec = "a%d" % k
            else:
                lb = label(k, held)
                spec = "r%d:%d" % lb if lb else "s%d" % (k - held[0])
            lst.append((tick, gate, v, spec))
        out["lists"][p] = {"keys": pr["keys"], "vel": pr["vel"], "notes": lst}
        if ctl and p == "n1":
            out["ctl"] = [(max(0, int(round(c[0] * PPQ))), c[1], c[2], c[3]) for c in ctl if -0.1 <= c[0] < L - 0.05]
    # 離すまで鳴っていたノートの長さは、ほかの組の同じ位置のノートから借りる（無ければ次の周の頭まで）
    gates = {}
    for p, d in out["lists"].items():
        for tick, gate, v, spec in d["notes"]:
            if gate is not None:
                gates.setdefault(tick // 20, []).append(gate)
    for p, d in out["lists"].items():
        d["notes"] = [(tick, gate if gate is not None else int(statistics.median(gates.get(tick // 20, [out["len"] - tick]))), v, spec)
                      for tick, gate, v, spec in d["notes"]]
    return out


def write(outs, path, g):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# MOTIF-RACK ES arpeggio phrases measured from the hardware's arpeggio MIDI output (mr_arp_fit.py). Do not distribute.\n")
        f.write("# delay=%.5f\n" % g)
        for t, o in sorted(outs.items()):
            info = o["info"]
            f.write("type %d %s %s len=%d kind=%s" % (t, info["cat"], info["name"].replace(" ", "_"), o["len"], o["kind"]))
            if o["kind"] in ("walk", "walk?"):
                f.write(" voices=%s\n" % ";".join(",".join(str(x) if not isinstance(x, tuple) else "%d%s%d%s" % x for x in v)
                                                  for v in (o["walk"] or [])))
                for tick, gate, v in o["rhythm"]:
                    f.write("e %d %d %d\n" % (tick, gate if gate else 60, v))
                continue
            f.write("\n")
            for p, d in o["lists"].items():
                f.write("list %s keys=%s vel=%d\n" % (p, ",".join(str(k) for k in d["keys"]), d["vel"]))
                for tick, gate, v, spec in d["notes"]:
                    f.write("e %d %d %d %s\n" % (tick, gate, v, spec))
            if o["ctl"]:
                for tick, st, a, b in o["ctl"]:
                    f.write("c %d %d %d %d\n" % (tick, st, a, b))


def main():
    a = sys.argv[1:]
    d, path = a[0], a[1]
    lo, hi = 0, 1786
    if "--types" in a:
        lo, hi = (int(x) for x in a[a.index("--types") + 1].split("-"))
    files = {int(f[1:5]): os.path.join(d, f) for f in os.listdir(d) if f.endswith(".json")}
    js = {t: json.load(open(p, encoding="utf-8")) for t, p in sorted(files.items()) if lo <= t <= hi}
    # 全体の遅れ（鍵から最初の音まで）は、機械的な Seq の UpOct1 などで測る（型ごとに測ると、わざと遅らせた型まで戻してしまう）
    g = 0.0
    ref = [json.load(open(files[t], encoding="utf-8")) for t in range(0, 6) if t in files]
    if ref:
        ds = []
        for j in ref:
            beat = 60.0 / j["tempo"]
            raw = {p: events(pr, beat)[0] for p, pr in j["probes"].items()}
            off = align({p: [n[0] for n in raw[p]] for p in raw})
            ds += [n[0] - off[p] - round((n[0] - off[p]) * 4) / 4 for p in raw for n in raw[p]]
        g = statistics.median(ds)
    print("全体の遅れ %.4f 拍" % g)
    outs = {}
    for t, j in js.items():
        outs[t] = fit_type(j, g)
        if "--report" in a:
            o = outs[t]
            print(t, o["info"]["cat"], o["info"]["name"], o["kind"], o.get("walk", ""), o.get("score", ""),
                  "s" if any(e[3].startswith("s") for d in o["lists"].values() for e in d["notes"]) else "",
                  "a" if any(e[3].startswith("a") for d in o["lists"].values() for e in d["notes"]) else "")
    write(outs, path, g)


if __name__ == "__main__":
    main()
