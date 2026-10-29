"""ロータリースピーカー（Dual Rotary Speaker 1〜3、08 70〜72）の速さの切り替えを録る。
  python mr_fx_rotary.py list | record [--only 名前,…]
組（mr_fx_mod.py の名前）ごとに、
  * Speed Control（16）を 1（fast）にした組を、mr_fx_mod.py と同じ録り方（正弦波 220Hz・2093Hz を 8 秒、ノイズ 4 秒）で録る。名前は p16 を替えた組の名前
  * 切り替え: 正弦波を 14 秒鳴らし、3 秒で fast、9 秒で slow に替える（cs<名前>_k<鍵>_sw_long_wet.wav）。ローター・ホーンの速さの移り変わりを読む
送るのはパラメーターチェンジ・ダンプの要求・ノートだけ（保存しない）。録った音は配らないこと。"""
import os
import subprocess
import sys

import numpy as np

import mr_fx_mod as M

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = M.OUT
ROT = {(8, 0x70), (8, 0x71), (8, 0x72)}


def sets():
    out = {}
    for k, (ins, tempo, voices) in M.needed().items():
        t = (ins["type"] >> 7, ins["type"] & 0x7F)
        if t in ROT:
            out[k] = (ins, voices)
    return out


def fast_key(ins):
    f = dict(ins)
    f["param16"] = 1
    return M.key_of(f, 0), f


def comp(t, key, st, tag, secs, extra=()):
    cmd = [sys.executable, os.path.join(HERE, "mr_fx_comp.py"), "--dir", OUT, "--type", "%d:%d" % t, "--key", str(key), "--set", st,
           "--tag", tag, "--long", str(secs), "--level", "100"] + list(extra)
    subprocess.run(cmd, check=True, cwd=HERE, stdout=subprocess.DEVNULL)


def record(only):
    for k, (ins, voices) in sets().items():
        if only and k not in only:
            continue
        t = (ins["type"] >> 7, ins["type"] & 0x7F)
        fk, f = fast_key(ins)
        if fk != k:
            st = ",".join("p%d=%d" % (i, f["param%d" % i]) for i in range(1, 17))
            for key, _ in M.KEYS:
                if not os.path.exists(os.path.join(OUT, "cs%s_k%d_long_wet.wav" % (fk, key))):
                    comp(t, key, st, "%s_k%d" % (fk, key), 8)
            if not os.path.exists(os.path.join(OUT, "cs%s_noise_long_wet.wav" % fk)):
                subprocess.run([sys.executable, os.path.join(HERE, "mr_fx_comp.py"), "--dir", OUT, "--type", "%d:%d" % t, "--key", "60", "--wave", "705",
                                "--set", st, "--tag", fk + "_noise", "--long", "4", "--level", "100"], check=True, cwd=HERE, stdout=subprocess.DEVNULL)
        st0 = ",".join("p%d=%d" % (i, ins["param%d" % i] if i != 16 else 0) for i in range(1, 17))
        if ins["param16"] != 0:
            # プリセットが fast の組は、slow の定常も録る（mr_fx_mod.py は組のまま = fast だけ録っている）
            s0 = dict(ins)
            s0["param16"] = 0
            sk = M.key_of(s0, 0)
            for key, _ in M.KEYS:
                if not os.path.exists(os.path.join(OUT, "cs%s_k%d_long_wet.wav" % (sk, key))):
                    comp(t, key, st0, "%s_k%d" % (sk, key), 8)
        for key, _ in M.KEYS:
            if not os.path.exists(os.path.join(OUT, "cs%s_k%d_sw_long_wet.wav" % (k, key))):
                comp(t, key, st0, "%s_k%d_sw" % (k, key), 14, ["--at", "3:p16=1,9:p16=0"])
        print(k, "録った（%s）" % ",".join(voices), flush=True)


NB = 512
FS = M.FS
LFO_HZ = [0.0, 0.04, 0.08, 0.13, 0.17, 0.21, 0.25, 0.29, 0.34, 0.38, 0.42, 0.46, 0.51, 0.55, 0.59, 0.63, 0.67, 0.72, 0.76, 0.8, 0.84, 0.88, 0.93, 0.97,
          1.01, 1.05, 1.09, 1.14, 1.18, 1.22, 1.26, 1.3, 1.35, 1.39, 1.43, 1.47, 1.51, 1.56, 1.6, 1.64, 1.68, 1.72, 1.77, 1.81, 1.85, 1.89, 1.94, 1.98,
          2.02, 2.06, 2.1, 2.15, 2.19, 2.23, 2.27, 2.31, 2.36, 2.4, 2.44, 2.48, 2.52, 2.57, 2.61, 2.65, 2.69, 2.78, 2.86, 2.94, 3.03, 3.11, 3.2, 3.28,
          3.37, 3.45, 3.53, 3.62, 3.7, 3.87, 4.04, 4.21, 4.37, 4.54, 4.71, 4.88, 5.05, 5.22, 5.38, 5.55, 5.72, 6.06, 6.39, 6.73, 7.07, 7.4, 7.74, 8.08,
          8.41, 8.75, 9.08, 9.42, 9.76, 10.1, 10.8, 11.4, 12.1, 12.8, 13.5, 14.1, 14.8, 15.5, 16.2, 16.8, 17.5, 18.2, 19.5, 20.9, 22.2, 23.6, 24.9,
          26.2, 27.6, 28.9, 30.3, 31.6, 33.0, 34.3, 37.0, 39.7]


def ratio(base, f0, t0=0.3, t1=7.8):
    """wet / dry の解析信号の比（左右、1ms ごとの複素数の平均）。音の頭でそろえ、t0〜t1 秒"""
    import mr_ref
    dL, _ = mr_ref.read_wav(base + "_long_dry.wav")
    wL, wR = mr_ref.read_wav(base + "_long_wet.wav")
    d, wL, wR = np.asarray(dL, dtype=np.float64), np.asarray(wL, dtype=np.float64), np.asarray(wR, dtype=np.float64)
    od = int(np.argmax(np.abs(d) > 1e-4))
    ow = int(np.argmax((np.abs(wL) + np.abs(wR)) > 1e-4))
    if abs(ow - od) > int(0.004 * FS):
        ow = od
    n = int(t1 * FS)
    d, wL, wR = d[od:od + n], wL[ow:ow + n], wR[ow:ow + n]
    m = min(len(d), len(wL), len(wR)) // 44 * 44
    ad = M.analytic(d[:m])
    ad = np.where(np.abs(ad) > 1e-6, ad, 1e-6)
    out = []
    for w in (wL[:m], wR[:m]):
        z = (M.analytic(w) / ad).reshape(-1, 44).mean(1)
        out.append(z[int(t0 * 1000):])
    return out, t0


K = 16          # 回る位置の周期関数の倍音の数


def design(pr, ph):
    """回る位置（周）の並びから、係数に掛ける列: DC・ローター ±1..K・ホーン ±1..K"""
    cols = [np.ones_like(pr, dtype=complex)]
    for n in range(1, K + 1):
        for p in (pr, ph):
            cols.append(np.exp(2j * np.pi * n * p))
            cols.append(np.exp(-2j * np.pi * n * p))
    return np.stack(cols, 1)


def lsq(z, t, fr, fh):
    X = design(t * fr, t * fh)
    c = np.linalg.lstsq(X, z, rcond=None)[0]
    e = z - X @ c
    return c, 10 * np.log10(np.mean(np.abs(e) ** 2) / np.mean(np.abs(z) ** 2))


def refine(z, t, fr, fh):
    """ローターとホーンの速さを、残りがいちばん小さくなるように詰める（座標ごとの黄金分割、±1%）"""
    for _ in range(3):
        for which in (0, 1):
            c = fr if which == 0 else fh
            lo, hi = c * 0.99, c * 1.01
            for _ in range(30):
                m1, m2 = lo + (hi - lo) * 0.382, lo + (hi - lo) * 0.618
                e1 = lsq(z, t, m1, fh)[1] if which == 0 else lsq(z, t, fr, m1)[1]
                e2 = lsq(z, t, m2, fh)[1] if which == 0 else lsq(z, t, fr, m2)[1]
                if e1 < e2:
                    hi = m2
                else:
                    lo = m1
            if which == 0:
                fr = (lo + hi) / 2
            else:
                fh = (lo + hi) / 2
    return fr, fh


def comp_values(c, which, n=NB):
    """係数のうち、ローター（which 0）かホーン（1）の成分の、回る位置 n 点での複素の値。DC は帯の主な方（dc が真のとき）に入れる"""
    ph = np.arange(n) / n
    v = np.zeros(n, complex)
    i = 1
    for m in range(1, K + 1):
        for w in (0, 1):
            if w == which:
                v += c[i] * np.exp(2j * np.pi * m * ph) + c[i + 1] * np.exp(-2j * np.pi * m * ph)
            i += 2
    return v


def rotate(c, dr, dh):
    """係数を回る位置 dr・dh（周）だけ進める（φ → φ + d）"""
    c = c.copy()
    i = 1
    for m in range(1, K + 1):
        for d in (dr, dh):
            c[i] *= np.exp(2j * np.pi * m * d)
            c[i + 1] *= np.exp(-2j * np.pi * m * d)
            i += 2
    return c


def align(cs, cf, which):
    """速いの成分を遅いの成分にいちばん重なるように回す量（周、細かく 1/1024）。大きさの並びの相関（遅れは速さで変わるので見ない）"""
    a = np.abs(comp_values(cs, which, 1024))
    a = a - a.mean()
    bb = np.abs(comp_values(cf, which, 1024))
    bb = bb - bb.mean()
    best = (-1e9, 0.0)
    for d in np.arange(0, 1, 1 / 1024):
        b = np.roll(bb, -int(round(d * 1024)))
        q = np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b) + 1e-12)
        if q > best[0]:
            best = (q, d)
    return best[1], best[0]


def to_ad(v, f0, ref=None):
    """成分の複素の値の並び（回る位置）を、大きさ A と遅れ d（サンプル）に。位相は区間をまたいでつなぎ、周期のずれは ref（あれば）か中央値で決める"""
    a = np.abs(v)
    d = -np.unwrap(np.angle(v)) / (2 * np.pi * f0) * FS
    per = FS / f0
    target = np.median(d) if ref is None else np.median(ref)
    d -= per * np.round((np.median(d) - target) / per) if ref is not None else per * np.round(np.median(d) / per)
    return a, d


def analyze(only):
    for k, (ins, voices) in sets().items():
        if only and k not in only:
            continue
        fk, _ = fast_key(ins)
        s0 = dict(ins)
        s0["param16"] = 0
        names = {"slow": M.key_of(s0, 0), "fast": fk}
        fit = {}
        ok = True
        for sp, kk in names.items():
            pr, ph = (1, 2) if sp == "slow" else (3, 4)
            for bd, (key, f0) in enumerate(M.KEYS):
                base = os.path.join(OUT, "cs%s_k%d" % (kk, key))
                if not os.path.exists(base + "_long_wet.wav"):
                    ok = False
                    break
                zs, t0 = ratio(base, f0)
                t = t0 + np.arange(len(zs[0])) / 1000.0
                fr, fh = refine(zs[0], t, LFO_HZ[ins["param%d" % pr]] * 0.9967, LFO_HZ[ins["param%d" % ph]] * 0.9967)
                cs, es = [], []
                for z in zs:
                    c, e = lsq(z, t, fr, fh)
                    cs.append(c)
                    es.append(e)
                fit[(sp, bd)] = (fr, fh, cs, es, f0)
        if not ok:
            print(k, "録音が足りない", flush=True)
            continue
        # 速さは 2 つの帯で詰めた値の平均（ローターは低い帯、ホーンは高い帯の方が確か。重みを付ける）
        spd = {}
        for sp in ("slow", "fast"):
            spd[sp] = (0.8 * fit[(sp, 0)][0] + 0.2 * fit[(sp, 1)][0], 0.2 * fit[(sp, 0)][1] + 0.8 * fit[(sp, 1)][1])
        # 速いの係数を、遅いの形に重なるように回す（ローターは低い帯の左、ホーンは高い帯の左で決め、両方の帯に掛ける）
        dr, qr = align(fit[("slow", 0)][2][0], fit[("fast", 0)][2][0], 0)
        dh, qh = align(fit[("slow", 1)][2][0], fit[("fast", 1)][2][0], 1)
        tabs = []          # [帯][速さ][左右][成分] → (A, d)
        dmin = 0.0
        for bd in range(2):
            f0 = M.KEYS[bd][1]
            per_band = []
            slow_ad = None
            for sp in ("slow", "fast"):
                cs = fit[(sp, bd)][2]
                if sp == "fast":
                    cs = [rotate(c, dr, dh) for c in cs]
                per_sp = []
                for ch in range(2):
                    per_ch = [None, None]
                    # 帯の主な成分（低い帯はローター、高い帯はホーン。DC もこちら）から。弱い方の成分の周期のずれは主な方にそろえる
                    # （大きさがほぼ 0 で遅れが決まらず、離れた所に置くと先読みが増える）。速いは遅いの同じ成分にそろえる
                    for comp in (bd, 1 - bd):
                        v = comp_values(cs[ch], comp)
                        if comp == bd:
                            v = v + cs[ch][0]
                        ref = slow_ad[ch][comp][1] if sp == "fast" else (None if comp == bd else per_ch[bd][1])
                        a, d = to_ad(v, f0, ref)
                        per_ch[comp] = (a, d)
                        dmin = min(dmin, float(d.min()))
                    per_sp.append(per_ch)
                if sp == "slow":
                    slow_ad = per_sp
                per_band.append(per_sp)
            tabs.append(per_band)
        with open(os.path.join(OUT, names["slow"] + ".rot"), "wb") as fo:       # 名前は Speed Control を slow にした組（芯の rot_key）
            fo.write(("ROT nb=%d rotor_slow=%.6f horn_slow=%.6f rotor_fast=%.6f horn_fast=%.6f\n" % (
                NB, spd["slow"][0], spd["slow"][1], spd["fast"][0], spd["fast"][1])).encode("ascii"))
            # 並び: 帯（低・高）× 速さ（遅い・速い）× 左右 × 成分（ローター・ホーン）× (A, d) × NB（float32）
            for per_band in tabs:
                for per_sp in per_band:
                    for per_ch in per_sp:
                        for a, d in per_ch:
                            fo.write(np.asarray(a, "<f4").tobytes())
                            fo.write(np.asarray(d, "<f4").tobytes())
        es = [fit[(sp, bd)][3] for sp in ("slow", "fast") for bd in range(2)]
        print("%s（%s）: ローター %.4f / %.4f Hz・ホーン %.4f / %.4f Hz（表 %.2f / %.2f・%.2f / %.2f）・残り %s dB・形のそろい %.2f / %.2f・d の最小 %.1f" % (
            k, ",".join(voices), spd["slow"][0], spd["fast"][0], spd["slow"][1], spd["fast"][1], LFO_HZ[ins["param1"]], LFO_HZ[ins["param3"]],
            LFO_HZ[ins["param2"]], LFO_HZ[ins["param4"]], " ".join("%.0f" % max(e) for e in es), qr, qh, dmin), flush=True)


def main():
    a = sys.argv[1:]
    cmd = a[0] if a else "list"
    only = set(a[a.index("--only") + 1].split(",")) if "--only" in a else None
    if cmd == "list":
        for k, (ins, voices) in sets().items():
            print(k, ",".join(voices))
    elif cmd == "record":
        record(only)
    elif cmd == "analyze":
        analyze(only)


if __name__ == "__main__":
    main()
