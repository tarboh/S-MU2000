"""取ったサンプル（.smpx、44.1kHz の録音）から、実機が読んでいた元のサンプルの格子を見つけて組み直す（2026-10-28）。
  python mr_romgrid.py サンプルの置き場 [ファイル名 ...] [--list ファイル一覧] [--check] [--write]
実機（SWP50）は、元のサンプルを 4 点の 3 次ラグランジュ補間（SWP30 と同じ式、f0(t) = (t − t³)/6、f1(t) = t + (t² − t³)/2）で読む。
元のサンプルの速さが 44.1kHz より遅いと、読み出しの速さ fr の倍数のまわりに像が出て、音程を下げるほど像が下りてきて高い音として聞こえる
（Hard Kit の SdGate1 を RPN の Coarse −24 で鳴らすと、芯は 4kHz から上が実機より 5〜25dB 小さかった。mr_pitchdown.py・mr_interp_kink.py）。
録音は光のデジタル出力なので、「元のサンプルをラグランジュで 1 出力あたり s 進んで読んだもの」と見ると、s と出だしの端数を決めれば
連立一次式がほぼ 0 の残りで解ける（BdDistRM で −72.7dB）。
  1) 出だしの音の大きいところ 4096 サンプルで、s を 0.25〜0.98 から、像の形（fr/2 から上）の合い方で当たりをつける
  2) その近く ±0.6% を 0.01% 刻み・端数 10 通りで、時間の側の残りがいちばん小さい s を選ぶ
  3) 残りが −35dB より小さければ、録音全体を fr/2 で切ってラグランジュの通り方を戻し、元の格子（1 出力あたり s）に取り直す
--write で組み直したものを同じ名前で書く（元は サンプルの置き場/../smpx44k に移す）。頭の行に grid=rom s=… を足し、stretch を stretch / s にする
（芯は grid=rom のサンプルをラグランジュで読む）。ループのあるサンプルは今は扱わない（ループの長さが元の格子で整数になるかを見ていない）。
読むのは録音だけで、実機には何も送らない。サンプルはヤマハのものなので配らないこと。"""
import os
import sys

import numpy as np

FS = 44100


def read_smpx(p):
    d = open(p, "rb").read()
    nl = d.index(b"\n")
    head = d[:nl].decode()
    kv = dict(t.split("=", 1) for t in head.split()[1:])
    x = np.frombuffer(d[nl + 1:], "<f4").reshape(-1, 2).astype(float)
    return head, kv, x


def write_smpx(p, kv, x):
    keys = ["key", "loop_start", "loop_length", "stretch", "frames"] + [k for k in kv if k not in ("key", "loop_start", "loop_length", "stretch", "frames")]
    head = "SMPX " + " ".join("%s=%s" % (k, kv[k]) for k in keys) + "\n"
    with open(p, "wb") as f:
        f.write(head.encode())
        f.write(np.asarray(x, "<f4").tobytes())


def f0(t):
    return (t - t ** 3) / 6


def f1(t):
    return t + (t ** 2 - t ** 3) / 2


def read_lagrange(src, s, n, p0=0.0):
    pos = p0 + 1 + np.arange(n) * s
    i = np.floor(pos).astype(int)
    ok = i + 2 < len(src)
    i, p = i[ok], (pos - i)[ok]
    return -src[i - 1] * f0(1 - p) + src[i] * f1(1 - p) + src[i + 1] * f1(p) - src[i + 2] * f0(p)


_FS = np.linspace(0, 0.5, 2001)


def _lag_resp():
    t = np.linspace(-2, 2, 8001)
    a = np.abs(t)
    h = np.where(a < 1, (a ** 3 - 2 * a ** 2 - a + 2) / 2, np.where(a < 2, (-a ** 3 + 6 * a ** 2 - 11 * a + 6) / 6, 0))
    dt = t[1] - t[0]
    return np.array([(h * np.cos(2 * np.pi * ff * t)).sum() * dt for ff in _FS])


_HL = _lag_resp()


def to_rom(y, s):
    """44.1k の録音 y（1 出力あたり元の格子を s 進んだもの）→ 元の格子の値。fr/2 で切り、ラグランジュの通り方 H を戻して取り直す"""
    N = len(y)
    pad = 1 << int(np.ceil(np.log2(N + 256)))
    Y = np.fft.rfft(y, pad)
    f = np.fft.rfftfreq(pad, 1.0)            # 出力 1 サンプルあたりの周波数
    u = f / s                                 # 元の格子での周波数
    H = np.interp(u, _FS, _HL)
    Y = np.where(u < 0.5 * 0.995, Y / np.maximum(H, 0.2), 0)
    M = int(round(pad * s))
    rom = np.fft.irfft(Y[:M // 2 + 1], M) * M / pad
    return rom[:int(np.ceil(N * s)) + 4]


def logspec(y):
    w = np.hanning(len(y))
    X = np.abs(np.fft.rfft(y * w)) ** 2
    k = 15
    return 10 * np.log10(np.convolve(X, np.ones(k) / k, "same") + 1e-30)


def coarse_s(seg):
    """像の形で s の当たりをつける: 候補の s で seg を元の格子に組み直し、ラグランジュで読み直したものと、fr/2 から上の形を比べる"""
    S = logspec(seg)
    n = len(seg)
    f = np.fft.rfftfreq(n, 1.0)
    best = []
    for s in np.exp(np.arange(np.log(0.25), np.log(0.985), 0.0025)):
        rom = to_rom(seg, s)
        z = read_lagrange(np.concatenate([[0.0], rom]), s, n)
        if len(z) < n:
            continue
        Z = logspec(z)
        sel = (f > 0.5 * s * 1.02) & (f < 0.49) & (S > S.max() - 90)
        if sel.sum() < 20:
            continue
        best.append((np.abs(Z[sel] - S[sel]).mean(), s))
    best.sort()
    out = []
    for err, s in best:
        if all(abs(s / t - 1) > 0.015 for _, t in out):
            out.append((err, s))
        if len(out) == 5:
            break
    return out


def td_resid(y, s, p0):
    pos = p0 + np.arange(len(y)) * s
    i = np.floor(pos).astype(int)
    p = pos - i
    M = i[-1] + 4
    A = np.zeros((len(y), M))
    r = np.arange(len(y))
    A[r, i] = -f0(1 - p)
    A[r, i + 1] = f1(1 - p)
    A[r, i + 2] = f1(p)
    A[r, i + 3] = -f0(p)
    G = A.T @ A
    G[np.diag_indices(M)] += 1e-12 * np.trace(G) / M
    sol = np.linalg.solve(G, A.T @ y)
    e = y - A @ sol
    return np.sqrt((e ** 2).mean() / ((y ** 2).mean() + 1e-30))


def golden(fn, a, b, n):
    g = (np.sqrt(5) - 1) / 2
    c, d = b - g * (b - a), a + g * (b - a)
    fc, fd = fn(c), fn(d)
    for _ in range(n):
        if fc < fd:
            b, d, fd = d, c, fc
            c = b - g * (b - a)
            fc = fn(c)
        else:
            a, c, fc = c, d, fd
            d = a + g * (b - a)
            fd = fn(d)
    return (c, fc) if fc < fd else (d, fd)


def polish(y, s0, ds):
    """s を s0 ± ds の中で、端数 p0 ごとに残りが最小になるよう連続に詰める（本当の格子なら残りは録音の雑音の底まで下がる）"""
    def best_p0(s):
        grid = [(td_resid(y, s, q), q) for q in np.arange(0, 1, 0.125)]
        e, q = min(grid)
        q2, e2 = golden(lambda v: td_resid(y, s, v % 1.0), q - 0.125, q + 0.125, 14)
        return (e2, q2 % 1.0) if e2 < e else (e, q)
    s, e = golden(lambda v: best_p0(v)[0], s0 - ds, s0 + ds, 22)
    e, q = best_p0(s)
    return s, q, e


def fine_s(y, s0, span=0.006, step=0.0001, nph=10):
    best = None
    for s in s0 * (1 + np.arange(-span, span + step / 2, step)):
        for p0 in np.arange(0, 1, 1.0 / nph):
            e = td_resid(y, s, p0)
            if best is None or e < best[0]:
                best = (e, s, p0)
    e, s, p0 = best
    # 端数を細かく
    for p in np.arange(p0 - 0.09, p0 + 0.1, 0.01):
        e2 = td_resid(y, s, p % 1.0)
        if e2 < e:
            e, p0 = e2, p % 1.0
    return s, p0, e


def analyze(path):
    head, kv, x = read_smpx(path)
    m = x.mean(axis=1)
    n = len(m)
    if n < 6000:
        return None
    # 音の大きい 4096 サンプル（出だしの 64 サンプルは避ける）
    e = np.convolve(m ** 2, np.ones(4096), "valid")
    st = int(np.argmax(e[64:])) + 64 if len(e) > 64 else 0
    seg = m[st:st + 4096]
    cands = coarse_s(seg)
    if not cands:
        return None
    # 候補ごとに、同じ窓（600 サンプル）で s と端数を連続に詰めて、残りがいちばん下がるものを選ぶ。
    # 像の小さいサンプルでは、本当の s より大きい s でも大半を表せて残りが小さく出る（BdDistRM で本当の 0.8629 が −84.5dB、0.924 が −77.3dB）。
    # 本当の格子なら詰めるほど雑音の底まで下がり、違う s では途中で止まる
    y = m[st + 500:st + 1100]
    fin = []
    for err, s in cands:
        # 短い窓（200）で ±0.8% を 0.03% 刻みに見て（この窓なら本当の s のまわり ±0.06% は残りが下がる）、よい 2 つを長い窓で詰める
        near = []
        for sv in s * (1 + np.arange(-0.008, 0.0081, 0.0003)):
            e, q = min((td_resid(y[:200], sv, q), q) for q in np.arange(0, 1, 1 / 6))
            near.append((e, sv))
        near.sort()
        for e, sv in near[:2]:
            sf, p0, r2 = polish(y, sv, sv * 0.0004)
            fin.append((r2, sf, p0, err))
    fin.sort()
    r, s, p0, err = fin[0]
    return {"s": s, "p0": p0, "resid_db": 20 * np.log10(r + 1e-30), "spec_err": err, "kv": kv}


def one(job):
    d, name, write = job
    p = os.path.join(d, name)
    head, kv, x = read_smpx(p)
    if kv.get("grid") == "rom":
        return "%s: 組み直し済み" % name
    if int(kv.get("loop_start", "-1")) >= 0:
        return "%s: ループあり（そのまま）" % name
    r = analyze(p)
    if r is None:
        return "%s: 短すぎる・形が取れない" % name
    ok = r["resid_db"] < -35
    msg = "%s: s = %.5f（fr = %.1fHz）残り %.1fdB 像の差 %.1fdB %s" % (name, r["s"], r["s"] * FS, r["resid_db"], r["spec_err"],
                                                           "→ 組み直す" if ok and write else ("（合う）" if ok else "（そのまま）"))
    if ok and write:
        s = r["s"]
        L = to_rom(x[:, 0], s)
        R = to_rom(x[:, 1], s)
        bak = os.path.join(d, "..", "smpx44k")
        os.makedirs(bak, exist_ok=True)
        if not os.path.exists(os.path.join(bak, name)):
            os.replace(p, os.path.join(bak, name))
        kv2 = dict(kv)
        kv2["stretch"] = "%.9f" % (float(kv.get("stretch", "1")) / s)
        kv2["frames"] = str(len(L))
        kv2["grid"] = "rom"
        kv2["s"] = "%.6f" % s
        write_smpx(p, kv2, np.stack([L, R], axis=1))
    return msg


def main():
    a = sys.argv[1:]
    d = a[0]
    files = [t for t in a[1:] if not t.startswith("--") and t.endswith(".smpx")]
    if "--list" in a:
        files += [l.strip() for l in open(a[a.index("--list") + 1], encoding="utf-8") if l.strip()]
    write = "--write" in a
    jobs = [(d, name, write) for name in files]
    if len(jobs) > 1:
        from multiprocessing import Pool
        with Pool(int(mr_opt(a, "--jobs", "8"))) as pool:
            for msg in pool.imap(one, jobs):
                print(msg, flush=True)
    else:
        for j in jobs:
            print(one(j), flush=True)


def mr_opt(a, name, default):
    return a[a.index(name) + 1] if name in a else default


if __name__ == "__main__":
    main()
