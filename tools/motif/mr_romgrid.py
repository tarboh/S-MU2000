"""取ったサンプル（.smpx、44.1kHz の録音）から、実機が読んでいた元のサンプルの格子を見つけて組み直す（2026-10-28）。
  python mr_romgrid.py サンプルの置き場 [ファイル名 ...] [--list ファイル一覧] [--jobs 8] [--write] [--redo]
実機（SWP50）は、元のサンプルを 4 点の 3 次ラグランジュ補間（SWP30 と同じ式、f0(t) = (t − t³)/6、f1(t) = t + (t² − t³)/2）で読む。
元のサンプルの速さが 44.1kHz より遅いと、読み出しの速さ fr の倍数のまわりに像が出て、音程を下げるほど像が下りてきて高い音として聞こえる
（Hard Kit の SdGate1 を RPN の Coarse −24 で鳴らすと、芯は 4kHz から上が実機より 5〜25dB 小さかった。mr_pitchdown.py・mr_interp_kink.py）。
録音は光のデジタル出力なので、「元のサンプルをラグランジュで 1 出力あたり s 進んで読んだもの」と見ると、s と出だしの端数を決めれば
連立一次式がほぼ 0 の残りで解ける（BdDistRM で −72.7dB）。
  1) 出だしの音の大きいところ 4096 サンプルで、s を 0.25〜0.98 から、像の形（fr/2 から上）の合い方で当たりをつける
  2) その近く ±0.6% を 0.01% 刻み・端数 10 通りで、時間の側の残りがいちばん小さい s を選ぶ
  3) 残りが −35dB より小さければ、録音全体から元の格子（1 出力あたり s）を解く
--write で組み直したものを同じ名前で書く（元は サンプルの置き場/../smpx44k に移す）。頭の行に grid=rom s=… start=… を足し、stretch を stretch / s にする
（start は録音の頭が来る元の格子の位置。芯はここから鳴らし始める）。--redo は start の無い前の組み直しを、s はそのままで解き直す
（芯は grid=rom のサンプルをラグランジュで読む）。ループのあるサンプルは、元の格子でループの長さが整数になるよう s をわずかに寄せ、
ループは巡回のまま解く。元の格子は、録音全体を「ラグランジュで読んだもの」として CG 法の最小二乗で解く（solve_rom。FFT で取り直す
to_rom は始めの値だけ。s > 0.5 だと 22.05kHz で折り返した像が元の帯に重なって残る）。6000 サンプルより短いものは窓を縮めて見る（800 サンプルから）。
読むのは録音だけで、実機には何も送らない。サンプルはヤマハのものなので配らないこと。"""
import os
import shutil
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


def solve_rom(y, s, P, x0=None, Ls=-1, Lr=0, iters=60, damp=1e-4):
    """y[j] = 元の格子を位置 1 + P + j·s でラグランジュで読んだもの、として元の格子 r を最小二乗で解く（CGLS）。
    FFT で取り直すと、fr/2 を超えて 22.05kHz で折り返した像が元の帯に重なって残る（読み戻すと −35〜−55dB の差）。
    Lr > 0 ならループ: 添え字 Ls + Lr 以上は Lr 戻す（ループは巡回のまま解ける）。
    両端の値は式でほとんど決まらず、0 へ寄せないと跳ねる（BdDistRM の最後の値が 0.23 になり、鳴らすと 12〜16kHz が 7dB 大きかった）。
    damp（A の大きさに対する比）で 0 へ寄せる。1e-3 だと読み戻しが −72 → −62dB に落ちる"""
    n = len(y)
    pos = 1 + P + np.arange(n) * s
    i = np.floor(pos).astype(np.int64)
    p = pos - i
    W = np.stack([-f0(1 - p), f1(1 - p), f1(p), -f0(p)])
    I = np.stack([i - 1, i, i + 1, i + 2])
    if Lr > 0:
        M = Ls + Lr
        I = np.where(I >= M, Ls + (I - Ls) % Lr, I)
    else:
        M = int(I.max()) + 1
    x = np.zeros(M) if x0 is None else np.resize(np.asarray(x0, float), M) * (np.arange(M) < len(x0))
    Ax = lambda v: (W * v[I]).sum(axis=0)
    ATy = lambda v: np.bincount(I.ravel(), (W * v).ravel(), M)
    lam = damp * np.bincount(I.ravel(), (W * W).ravel(), M).mean()
    r = y - Ax(x)
    g = ATy(r) - lam * x
    d = g.copy()
    gg = g @ g
    for _ in range(iters):
        q = Ax(d)
        a = gg / (q @ q + lam * (d @ d) + 1e-300)
        x += a * d
        r -= a * q
        g = ATy(r) - lam * x
        g2 = g @ g
        if g2 < 1e-20 * (y @ y + 1e-300):
            break
        d = g + (g2 / gg) * d
        gg = g2
    return x, 20 * np.log10(np.sqrt((r ** 2).mean() / ((y ** 2).mean() + 1e-30)) + 1e-30)


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


def best_p0(y, s):
    """s を決めたときの、残りがいちばん小さい端数 p0"""
    grid = [(td_resid(y, s, q), q) for q in np.arange(0, 1, 0.125)]
    e, q = min(grid)
    q2, e2 = golden(lambda v: td_resid(y, s, v % 1.0), q - 0.125, q + 0.125, 14)
    return (e2, q2 % 1.0) if e2 < e else (e, q)


def polish(y, s0, ds):
    """s を s0 ± ds の中で、端数 p0 ごとに残りが最小になるよう連続に詰める（本当の格子なら残りは録音の雑音の底まで下がる）"""
    s, e = golden(lambda v: best_p0(y, v)[0], s0 - ds, s0 + ds, 22)
    e, q = best_p0(y, s)
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


def mono(kv, x):
    m = x.mean(axis=1)
    if int(kv.get("loop_start", "-1")) >= 0:
        m = m[:int(kv["loop_start"]) + int(kv["loop_length"])]
    return m


def window(m):
    """音の大きい 4096 サンプル（出だしの 64 サンプルは避ける）。短いサンプルは窓を縮める。返すのは窓の頭・幅と、詰める 600 サンプルの頭"""
    n = len(m)
    W = min(4096, n - 64) if n >= 6000 else n
    e = np.convolve(m ** 2, np.ones(W), "valid")
    st = int(np.argmax(e[64:])) + 64 if len(e) > 64 else 0
    return st, W, st + min(500, W - 600)


def analyze(path):
    head, kv, x = read_smpx(path)
    m = mono(kv, x)
    if len(m) < 800:
        return None
    st, W, yo = window(m)
    seg = m[st:st + W]
    cands = coarse_s(seg)
    if not cands:
        return None
    # 候補ごとに、同じ窓（600 サンプル）で s と端数を連続に詰めて、残りがいちばん下がるものを選ぶ。
    # 像の小さいサンプルでは、本当の s より大きい s でも大半を表せて残りが小さく出る（BdDistRM で本当の 0.8629 が −84.5dB、0.924 が −77.3dB）。
    # 本当の格子なら詰めるほど雑音の底まで下がり、違う s では途中で止まる
    y = m[yo:yo + 600]
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
    # 録音の頭からの端数（位置 j の出力は元の格子の 1 + P + j·s を読む）
    return {"s": s, "p0": p0, "P": (p0 - yo * s) % 1.0, "resid_db": 20 * np.log10(r + 1e-30), "spec_err": err, "kv": kv}


def one(job):
    d, name, write = job[:3]
    redo = len(job) > 3 and job[3]
    p = os.path.join(d, name)
    bak = os.path.join(d, "..", "smpx44k")
    head, kv, x = read_smpx(p)
    was_rom = kv.get("grid") == "rom"
    if was_rom:
        # --redo: start の無い前の組み直し（FFT で取り直し、頭が 1 + 端数ずれていた）を、元の録音から s はそのままで解き直す
        if not redo or "start" in kv or not os.path.exists(os.path.join(bak, name)):
            return "%s: 組み直し済み" % name
        s = float(kv["s"])
        head, kv, x = read_smpx(os.path.join(bak, name))
        m = mono(kv, x)
        yo = window(m)[2]
        e, p0 = best_p0(m[yo:yo + 600], s)
        r = {"s": s, "P": (p0 - yo * s) % 1.0}
        msg = "%s: s = %.5f 解き直す（残り %.1fdB）" % (name, s, 20 * np.log10(e + 1e-30))
    else:
        r = analyze(p)
        if r is None:
            return "%s: 短すぎる・形が取れない" % name
        ok = r["resid_db"] < -35
        msg = "%s: s = %.5f（fr = %.1fHz）残り %.1fdB 像の差 %.1fdB %s" % (name, r["s"], r["s"] * FS, r["resid_db"], r["spec_err"],
                                                               "→ 組み直す" if ok and write else ("（合う）" if ok else "（そのまま）"))
        if not (ok and write):
            return msg
    ls, ll = int(kv.get("loop_start", "-1")), int(kv.get("loop_length", "0"))
    s, P = r["s"], r["P"]
    kv2 = dict(kv)
    if ls >= 0 and ll > 0:
        # ループの長さが元の格子で整数になるように s を寄せる（高さは stretch / s で保つ）。録音のループをもう 1 周足して、継ぎ目も式に入れる
        s = round(ll * s) / ll
        Lr = int(round(ll * s))
        Ls = int(np.ceil(1 + P + ls * s)) + 2
        sol = [solve_rom(np.concatenate([c[:ls + ll], c[ls:ls + ll]]), s, P, np.concatenate([[0.0], to_rom(c[:ls + ll], s)]), Ls, Lr)
               for c in (x[:, 0], x[:, 1])]
        kv2["loop_start"] = str(Ls)
        kv2["loop_length"] = str(Lr)
    else:
        sol = [solve_rom(c, s, P, np.concatenate([[0.0], to_rom(c, s)])) for c in (x[:, 0], x[:, 1])]
    (L, eL), (R, eR) = sol
    msg += " 読み戻し %.1fdB" % max(eL, eR)
    if max(eL, eR) > -45:                       # 窓では合っても全体で合わない（s が 1 に近く見分けにくいものなど）
        if was_rom:                             # 前に組み直したものは 44.1k の元へ戻す
            shutil.copy(os.path.join(bak, name), p)
            return msg + " → 合わないので元に戻した"
        return msg + " → 合わないのでそのまま"
    os.makedirs(bak, exist_ok=True)
    if not os.path.exists(os.path.join(bak, name)):
        os.replace(p, os.path.join(bak, name))
    kv2["stretch"] = "%.9f" % (float(kv.get("stretch", "1")) / s)
    kv2["frames"] = str(len(L))
    kv2["grid"] = "rom"
    kv2["s"] = "%.9f" % s
    kv2["start"] = "%.4f" % (1 + P)              # 録音の頭は元の格子の 1 + 端数の所（芯はここから鳴らし始める）
    write_smpx(p, kv2, np.stack([L, R], axis=1))
    return msg


def main():
    a = sys.argv[1:]
    d = a[0]
    files = [t for t in a[1:] if not t.startswith("--") and t.endswith(".smpx")]
    if "--list" in a:
        files += [l.strip() for l in open(a[a.index("--list") + 1], encoding="utf-8") if l.strip()]
    write = "--write" in a
    jobs = [(d, name, write, "--redo" in a) for name in files]
    if len(jobs) > 1:
        from multiprocessing import Pool
        with Pool(int(mr_opt(a, "--jobs", "8"))) as pool:
            for msg in pool.imap_unordered(one, jobs):
                print(msg, flush=True)
    else:
        for j in jobs:
            print(one(j), flush=True)


def mr_opt(a, name, default):
    return a[a.index(name) + 1] if name in a else default


if __name__ == "__main__":
    main()
