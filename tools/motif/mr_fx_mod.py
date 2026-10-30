"""音量や遅れを周期的に揺らすインサーション（Tremolo 08 00・Auto Pan 08 10・Auto Pan 2 08 11・Rotary 08 50〜・Dual Rotor 08 70〜72・
Slice 0E 50）を、プリセットの組のまま録って、「出口 = A(t) × 入口(t − d(t))」の A と d を左右それぞれ時間の並びで持つ。
  python mr_fx_mod.py list | record [--only 名前,…] | analyze
録り方: mr_fx_comp.py --long で、組の 16 個のパラメーター（と音色のテンポ）を入れ、正弦波（波形 704）の鍵 57（220Hz）と鍵 96（2093Hz）を
音量 100 で 8 秒ずつ、dry（スイッチを切る）と wet（入れる）。
読み方: 解析信号で、A(t) = |wet| / |dry|、d(t) = −（wet と dry の位相の差）/ (2πf)。dry と wet は別の録音なので、音の頭（|x| が 1e-4 を超える所）でそろえる。
220Hz の A・d を低い帯、2093Hz を高い帯とし、芯は 1kHz で 2 つに分けて（2 次のバターワースを 2 回、足すと平らになる組）それぞれに掛ける。
EQ の欄（6〜9 など）は 2 本の周波数の比に入っている（確かめていない所を足さない）。8 秒より長く鳴らすときは、最後の 1 周（A の自己相関の山）を繰り返す。
結果は capture/fxmod/<名前>.mod（1 行目「MOD rate=1000 frames=N period=P」、そのあと float32 の AL・dL・AR・dR（低い帯）・AL・dL・AR・dR（高い帯）を N 組、
d はサンプル）。送るのはパラメーターチェンジ・ダンプの要求・ノートだけ（保存しない）。録った音は配らないこと。"""
import glob
import json
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CAP = os.path.join(HERE, "../../roms/motif-rack-es/capture")
OUT = os.path.join(CAP, "fxmod")
FS = 44100
RATE = 1000
TYPES = {(8, 0x00), (8, 0x10), (8, 0x11), (8, 0x50), (8, 0x51), (8, 0x52), (8, 0x53), (8, 0x60), (8, 0x61), (8, 0x62), (8, 0x63),
         (8, 0x70), (8, 0x71), (8, 0x72), (14, 0x50)}
TEMPO_TYPES = {(14, 0x50)}
# 歪みを中に持つ Rotary（Dist / Odrv / Amp + Rotary、08 51〜53・61〜63）は、音量で効き方が変わる（RotoGtr の組で、音量 50 は 100 より低い帯 +1.7dB・
# 高い帯 +0.7dB）ので、音量 100・50・25 で録り、入口の包絡（50ms の RMS）で 3 つの出口を混ぜる。Dual Rotor は音量 50・100 で同じだった
LEVEL_TYPES = {(8, 0x51), (8, 0x52), (8, 0x53), (8, 0x61), (8, 0x62), (8, 0x63)}
LEVELS = (100, 25)          # 50 も録ったが、RotoGtr の組で 220Hz の遅れが半周期（逆の向き）ずれていて、混ぜると打ち消し合ったので使わない
KEYS = ((57, 220.0), (96, 440.0 * 2 ** ((96 - 69) / 12)))


def key_of(ins, tempo):
    t = (ins["type"] >> 7, ins["type"] & 0x7F)
    ps = [ins["param%d" % i] for i in range(1, 17)]
    return "t%d_%d_%s_%d" % (t[0], t[1], "_".join(str(v) for v in ps), tempo if t in TEMPO_TYPES else 0)


def needed():
    sys.path.insert(0, HERE)
    import mr_patch
    sets = {}
    for p in sorted(glob.glob(os.path.join(CAP, "voice_json", "*.json"))):
        v = os.path.basename(p)[:-5]
        js = json.load(open(p, encoding="utf-8"))
        for blk in ("insertion1", "insertion2"):
            ins = js[blk]
            t = (ins["type"] >> 7, ins["type"] & 0x7F)
            if t in TYPES:
                tempo = mr_patch.voice_tempo(p) if t in TEMPO_TYPES else 0
                sets.setdefault(key_of(ins, tempo), (ins, tempo, []))[2].append(v)
    return sets


# つまみ（コントロールセット）が揺れの深さを動かす組は、深さを DEPTH_GRID に替えた組も録る（record --var・analyze --var）。
# 芯は録った組の値の両隣の 2 つを、深さで混ぜる。揺れの LFO は鍵を押しても始めに戻らないので、替えた組の揺れの位相は、元の組（深さ 0 なら深さ 127 の組）にそろえる
DEPTH = {(8, 0x00): 2, (8, 0x10): 2, (8, 0x11): 2}     # Tremolo の AM Depth、Auto Pan・Auto Pan 2 の L/R Depth
DEPTH_GRID = (0, 64, 127)                  # 00_028 の組で 0・32・64・96・127 を録り、A がほぼ深さにまっすぐだったので、残りは 3 点


def variants():
    """{替えた組の名前: (ins, tempo, [元の組の名前])}。プリセットの音色でつまみが深さを動かす組だけ"""
    import re
    out = {}
    for k, (ins, tempo, voices) in needed().items():
        t = (ins["type"] >> 7, ins["type"] & 0x7F)
        p = DEPTH.get(t)
        if not p:
            continue
        routed = False
        for v in voices:
            f = os.path.join(CAP, "patches_all", v + ".patch")
            if not os.path.exists(f):
                continue
            s = open(f, encoding="utf-8", errors="replace").read()
            m = re.search(r"^ctrl sets=(\S+)", s, re.M)
            for cs in (m.group(1).split(",") if m else []):
                src, dst, dep = map(int, cs.split(":"))
                if dep != 64 and dst in (4 + p - 1, 20 + p - 1):
                    w = "insa" if dst < 20 else "insb"
                    mm = re.search(r"\b%s=(\d+)" % w, s)
                    routed |= bool(mm) and int(mm.group(1)) == ins["type"]
        if not routed:
            continue
        for val in (0, 32, 64, 96, 127):              # DEPTH_GRID の外でも、もう録ってある深さは使う（芯はこの 5 つを探す）
            if val != ins["param%d" % p]:
                v2 = dict(ins)
                v2["param%d" % p] = val
                k2 = key_of(v2, tempo)
                if val in DEPTH_GRID or os.path.exists(os.path.join(OUT, "cs%s_k%d_long_wet.wav" % (k2, KEYS[-1][0]))):
                    out[k2] = (v2, tempo, [k])
    return out


def record(only, var=False):
    os.makedirs(OUT, exist_ok=True)
    for k, (ins, tempo, voices) in (variants() if var else needed()).items():
        if only and k not in only:
            continue
        t = (ins["type"] >> 7, ins["type"] & 0x7F)
        st = ",".join("p%d=%d" % (i, ins["param%d" % i]) for i in range(1, 17))
        for lv in (LEVELS if t in LEVEL_TYPES else (100,)):
            for key, _ in KEYS:
                tag = "%s_k%d" % (k, key) + ("" if lv == 100 else "_L%d" % lv)
                if os.path.exists(os.path.join(OUT, "cs%s_long_wet.wav" % tag)):
                    continue
                cmd = [sys.executable, os.path.join(HERE, "mr_fx_comp.py"), "--dir", OUT, "--type", "%d:%d" % t, "--key", str(key),
                       "--set", st, "--tag", tag, "--long", "8", "--level", str(lv)]
                subprocess.run(cmd, check=True, cwd=HERE, stdout=subprocess.DEVNULL)
        if not var and not os.path.exists(os.path.join(OUT, "cs%s_noise_long_wet.wav" % k)):
            # 周波数の形（EQ・アンプの色・帯の分け方）を、ノイズの時間平均の応答で取る
            subprocess.run([sys.executable, os.path.join(HERE, "mr_fx_comp.py"), "--dir", OUT, "--type", "%d:%d" % t, "--key", "60", "--wave", "705",
                            "--set", st, "--tag", k + "_noise", "--long", "4", "--level", "100"], check=True, cwd=HERE, stdout=subprocess.DEVNULL)
        print(k, "録った（%s）" % ",".join(voices), flush=True)


def analytic(x):
    N = len(x)
    X = np.fft.fft(x)
    X[N // 2 + 1:] = 0
    X[1:N // 2] *= 2
    return np.fft.ifft(X)


def read_pair(base, f0):
    sys.path.insert(0, HERE)
    import mr_ref
    dL, _ = mr_ref.read_wav(base + "_long_dry.wav")
    wL, wR = mr_ref.read_wav(base + "_long_wet.wav")
    d, wL, wR = np.asarray(dL, dtype=np.float64), np.asarray(wL, dtype=np.float64), np.asarray(wR, dtype=np.float64)
    od = int(np.argmax(np.abs(d) > 1e-4))
    ow = int(np.argmax((np.abs(wL) + np.abs(wR)) > 1e-4))
    if abs(ow - od) > int(0.004 * FS):            # 出口が頭で黙っている型（Slice など）は、録音の頭どうしでそろえる
        ow = od
    n = int(7.8 * FS)                             # 8 秒で離すので、その手前まで（離したあとは dry が小さくなって割り算が暴れる）
    d, wL, wR = d[od:od + n], wL[ow:ow + n], wR[ow:ow + n]
    m = min(len(d), len(wL))
    ad = analytic(d[:m])
    out = []
    for w in (wL[:m], wR[:m]):
        z = analytic(w) / np.where(np.abs(ad) > 1e-6, ad, 1e-6)
        A = np.abs(z)
        # 振幅がほぼ 0 の所（Auto Pan の片側など）は位相が決まらないので、直前の位相を保ってからつなぐ
        ph0 = np.angle(z)
        ok = A > 0.05 * np.percentile(A, 99)
        idx = np.where(ok, np.arange(len(A)), 0)
        np.maximum.accumulate(idx, out=idx)
        ph = np.unwrap(ph0[idx])
        dl = -ph / (2 * np.pi * f0) * FS
        per = FS / f0                              # 位相のつなぎの始まりで 1 周期ずれることがあるので、中央値を ±半周期に入れる
        dl -= per * np.round(np.median(dl) / per)
        # 1kHz へ間引く（1ms ごとの平均）
        k = FS // RATE
        mm = len(A) // k * k
        out.append(A[:mm].reshape(-1, k).mean(1))
        out.append(dl[:mm].reshape(-1, k).mean(1))
    return out


def period_of(A):
    a = A[200:] - np.mean(A[200:])
    if np.std(a) < 0.01 * np.mean(A[200:]):      # 揺れが 1% 未満なら周期なし（細かいさざ波を周期と取り違えない）
        return 0
    ac = np.correlate(a, a, "full")[len(a) - 1:]
    ac /= ac[0]
    lo = 100                                     # 100ms より短い山は細かいさざ波（Dual Rotor で 30ms を拾った）
    i = lo + int(np.argmax(ac[lo:len(ac) // 2]))
    return i if ac[i] > 0.5 and i > lo else 4000  # 周期が見えない（範囲の端に当たったときも）ときは最後の 4 秒を繰り返す


def dry_db(base):
    """dry の正弦波の音量（RMS、dB）。音量ごとに録った組で、入口の包絡と比べる目盛り"""
    sys.path.insert(0, HERE)
    import mr_ref
    d = np.asarray(mr_ref.read_wav(base + "_long_dry.wav")[0], dtype=np.float64)
    od = int(np.argmax(np.abs(d) > 1e-4))
    return 10 * np.log10(np.mean(d[od + FS:od + 7 * FS] ** 2) + 1e-20)


NW = 4096


def avg_response(k):
    """ノイズの時間平均の応答の大きさ（左右、2049 点、1/6 オクターブで均す）"""
    sys.path.insert(0, HERE)
    import mr_ref
    base = os.path.join(OUT, "cs%s_noise_long" % k)
    dL, dR = mr_ref.read_wav(base + "_dry.wav")
    wL, wR = mr_ref.read_wav(base + "_wet.wav")
    d = (np.asarray(dL, dtype=np.float64) + np.asarray(dR, dtype=np.float64)) / 2
    od = int(np.argmax(np.abs(d) > 1e-4))
    w = np.hanning(NW + 1)[:NW]
    a, b = od + int(0.5 * FS), od + int(3.8 * FS)
    nf = (b - a - NW) // (NW // 4)
    X = np.stack([np.fft.rfft(d[a + i * NW // 4:a + i * NW // 4 + NW] * w) for i in range(nf)])
    Pxx = np.sum(np.abs(X) ** 2, axis=0)
    f = np.fft.rfftfreq(NW, 1 / FS)
    out = []
    for wch in (wL, wR):
        y = np.asarray(wch, dtype=np.float64)
        Y = np.stack([np.fft.rfft(y[a + i * NW // 4:a + i * NW // 4 + NW] * w) for i in range(nf)])
        Pyy = np.sum(np.abs(Y) ** 2, axis=0)
        mag = np.sqrt(Pyy / (Pxx + 1e-9 * Pxx.max()))
        sm = np.empty_like(mag)
        for i, fi in enumerate(f):
            lo, hi = fi / 2 ** (1 / 12), fi * 2 ** (1 / 12)
            m = (f >= lo) & (f <= hi)
            sm[i] = np.sqrt(np.mean(mag[m] ** 2)) if m.any() else mag[i]
        out.append(sm)
    return np.stack(out)


def analyze():
    for k, (ins, tempo, voices) in needed().items():
        if os.path.exists(os.path.join(OUT, "cs%s_noise_long_wet.wav" % k)):
            avg_response(k).astype("<f4").tofile(os.path.join(OUT, k + ".modeq"))
        t = (ins["type"] >> 7, ins["type"] & 0x7F)
        for lv in (LEVELS if t in LEVEL_TYPES else (100,)):
            suf = "" if lv == 100 else "_L%d" % lv
            bases = [os.path.join(OUT, "cs%s_k%d%s" % (k, key, suf)) for key, _ in KEYS]
            if not all(os.path.exists(b + "_long_wet.wav") for b in bases):
                continue
            bands = [read_pair(b, f0) for b, (_, f0) in zip(bases, KEYS)]
            n = min(len(x) for band in bands for x in band)
            cols = [x[:n] for band in bands for x in band]
            P = period_of(cols[0]) or period_of(cols[2])      # Auto Pan は左右が逆に動いて足すと一定なので、片側ずつ見る
            with open(os.path.join(OUT, k + suf + ".mod"), "wb") as f:
                f.write(("MOD rate=%d frames=%d period=%d indb=%.2f\n" % (RATE, n, P, dry_db(bases[0]))).encode("ascii"))
                f.write(np.stack(cols, axis=1).astype("<f4").tobytes())
            lo, hi = bands
            print("%s（%s）: 周期 %d ms・低い帯 A 左 %.2f〜%.2f 右 %.2f〜%.2f・d 左 %.1f〜%.1f・高い帯 A 左 %.2f〜%.2f・d 左 %.1f〜%.1f サンプル" % (
                k + suf, ",".join(voices), P, lo[0][200:].min(), lo[0][200:].max(), lo[2][200:].min(), lo[2][200:].max(),
                lo[1][200:].min(), lo[1][200:].max(), hi[0][200:].min(), hi[0][200:].max(), hi[1][200:].min(), hi[1][200:].max()), flush=True)


def analyze_var():
    """替えた組の .mod を作る。揺れの位相は、元の組（深さ 0 なら深さ 127 の組）の A に、周期の中でずらしていちばん重なる所でそろえる"""
    base_sets = needed()
    groups = {}
    for k2, (ins2, tempo, base) in variants().items():
        groups.setdefault(base[0], []).append(k2)

    def cols_of(key):
        bases = [os.path.join(OUT, "cs%s_k%d" % (key, kk)) for kk, _ in KEYS]
        if not all(os.path.exists(b + "_long_wet.wav") for b in bases):
            return None
        bands = [read_pair(b, f0) for b, (_, f0) in zip(bases, KEYS)]
        n = min(len(x) for band in bands for x in band)
        return [x[:n] for band in bands for x in band], bases

    for k, ks in groups.items():
        ins, tempo, voices = base_sets[k]
        p = DEPTH[(ins["type"] >> 7, ins["type"] & 0x7F)]
        ref_key = k if ins["param%d" % p] > 0 else next((x for x in ks if x.split("_")[1 + p] == "127"), None)
        ref = cols_of(ref_key) if ref_key else None
        for k2 in ks:
            got = cols_of(k2)
            if got is None:
                continue
            cols, bases = got
            P = period_of(cols[0]) or period_of(cols[2])
            lag = 0
            if ref is not None and k2 != ref_key and 0 < P < 4000:
                a0, a1 = 1000, 6000
                best = -1e9
                for L in range(0, P):
                    q = sum(np.corrcoef(ref[0][c][a0:a1], cols[c][a0 + L:a1 + L])[0, 1] for c in (0, 2, 4, 6) if np.std(cols[c][a0:a1]) > 1e-6)
                    if q > best:
                        best, lag = q, L
                cols = [x[lag:] for x in cols]
            # d は録るたびに一定の量ずれる（音の頭の見つけ方と、A が 0 になる所の位相の半周期の取り違え。深さ 0・32〜109・127 で −23・−1.8・+100 サンプルなど）。
            # 揺れの中の d の動きは小さい（0.01〜0.16 サンプル）ので、中央値を元の組の .mod にそろえる（つまみを動かし始めても遅れが跳ばない）
            bm = os.path.join(OUT, k + ".mod")
            if os.path.exists(bm):
                with open(bm, "rb") as f:
                    f.readline()
                    b = np.frombuffer(f.read(), dtype="<f4").reshape(-1, 8)
                for c in (1, 3, 5, 7):
                    cols[c] = cols[c] - np.median(cols[c][1000:]) + np.median(b[1000:, c])
            with open(os.path.join(OUT, k2 + ".mod"), "wb") as f:
                f.write(("MOD rate=%d frames=%d period=%d indb=%.2f\n" % (RATE, len(cols[0]), P, dry_db(bases[0]))).encode("ascii"))
                f.write(np.stack(cols, axis=1).astype("<f4").tobytes())
            print("%s（%s の深さ %s）: 周期 %d ms・ずらし %d ms・低い帯 A 左 %.2f〜%.2f 右 %.2f〜%.2f" % (
                k2, k, k2.split("_")[1 + p], P, lag, cols[0][200:].min(), cols[0][200:].max(), cols[2][200:].min(), cols[2][200:].max()), flush=True)


def split(x):
    """1kHz で 2 つに分ける（2 次のバターワースを 2 回 = Linkwitz-Riley、足すと全域通過）"""
    N = 1 << int(np.ceil(np.log2(len(x) + 1)))
    f = np.fft.rfftfreq(N, 1 / FS)
    s = 1j * np.tan(np.pi * np.minimum(f, FS * 0.4999) / FS) / np.tan(np.pi * 1000.0 / FS)
    lp = (1 / (s * s + np.sqrt(2) * s + 1)) ** 2
    hp = (s * s / (s * s + np.sqrt(2) * s + 1)) ** 2
    X = np.fft.rfft(x, N)
    return np.fft.irfft(X * lp, N)[:len(x)], np.fft.irfft(X * hp, N)[:len(x)]


def series(col, P, n):
    """1kHz の並びを、鳴らす長さ n サンプルへ（8 秒より先は最後の 1 周を繰り返す）"""
    t = np.arange(n) / FS * RATE
    m = len(col)
    if P > 0:
        t = np.where(t < m - 1, t, (m - 1 - P) + np.mod(t - (m - 1 - P), P))
    else:
        t = np.minimum(t, m - 1)
    return np.interp(t, np.arange(m), col)


def eq_response(ins, f):
    """揺れる型の EQ。録った A には 220Hz・2093Hz での EQ の大きさが入っているので、割り戻してから出口に EQ 全体を掛ける
    （Phaser Vin の Auto Pan 2 で、中域の +6dB を 2 点では拾えず 5dB 小さく出た）。
    Tremolo・Auto Pan・Auto Pan 2・Rotary: 6〜9 低・高（2 Band EQ の形）と 11〜13 中。Dual Rotor: 11〜14 低・高。Slice: 13〜16 低・高。ほかは無し"""
    import mr_fx_apply as A
    t = (ins["type"] >> 7, ins["type"] & 0x7F)
    if t in ((8, 0x00), (8, 0x10), (8, 0x11), (8, 0x50), (8, 0x60)):
        base, mid = 6, True
    elif t in ((8, 0x70), (8, 0x71), (8, 0x72)):
        base, mid = 11, False
    elif t == (14, 0x50):
        base, mid = 13, False
    else:
        return None
    q = dict(param1=ins["param%d" % base], param2=ins["param%d" % (base + 1)], param3=ins["param%d" % (base + 2)], param4=ins["param%d" % (base + 3)])
    H = A.eq2_response(q, f)
    if mid:
        H = H * A.eq3_response(dict(param1=64, param5=64, param6=0, param7=0, param2=ins["param11"], param3=ins["param12"], param4=ins["param13"]), f)
    return H


def load_mod(path, cache={}):
    if path not in cache:
        with open(path, "rb") as f:
            head = dict(kv.split("=") for kv in f.readline().decode("ascii").split()[1:])
            cache[path] = (np.frombuffer(f.read(), dtype="<f4").reshape(-1, 8).astype(np.float64), int(head["period"]),
                           float(head.get("indb", "nan")))
    return cache[path]


def shape_of(ins, tempo):
    """周波数の形: ノイズで録った時間平均の応答（左右）があればそれ、無ければパラメーターの EQ（左右同じ）。(f の点 → 大きさ) を返す関数か None"""
    k = key_of(ins, tempo)
    p = os.path.join(OUT, k + ".modeq")
    if os.path.exists(p):
        m = np.fromfile(p, dtype="<f4").astype(np.float64).reshape(2, -1)
        fg = np.fft.rfftfreq(NW, 1 / FS)
        return lambda f, ch: np.interp(f, fg, m[ch])
    def eqf(f, ch):
        H = eq_response(ins, f)
        return None if H is None else H
    return eqf if eq_response(ins, np.array([1000.0])) is not None else None


def _run(xs, ins, cols, P, shape=None):
    n = len(xs[0])
    t = np.arange(n)
    out = [np.zeros(n), np.zeros(n)]
    bl = [split(x) for x in xs]
    for b in range(2):
        for ch in range(2):
            xb = bl[ch][b]
            nb = 1.0 if shape is None else max(1e-3, float(np.abs(shape(np.array([KEYS[b][1]]), ch))[0]))
            A = series(cols[:, b * 4 + ch * 2], P, n) / nb
            d = series(cols[:, b * 4 + ch * 2 + 1], P, n)
            out[ch] += A * np.interp(t - d, t, xb, left=0.0)
    return out


def env_db(x):
    """入口の包絡: 50ms の RMS（dB）"""
    w = int(0.05 * FS)
    c = np.concatenate([[0.0], np.cumsum(x * x)])
    i = np.arange(len(x))
    lo, hi = np.maximum(0, i - w // 2), np.minimum(len(x), i + w // 2)
    return 10 * np.log10((c[hi] - c[lo]) / w + 1e-20)


def apply(L, R, ins, tempo):
    k = key_of(ins, tempo)
    p = os.path.join(OUT, k + ".mod")
    if not os.path.exists(p):
        return None
    # 左右を別々に通す（Tremolo・Slice が実機と 3〜4dB 合うようになった。49 音色の比べ）。Rotary・Dual Rotor はスピーカーへ足して送る形なので左右を足す
    t = (ins["type"] >> 7, ins["type"] & 0x7F)
    stereo = not (t[0] == 8 and 0x50 <= t[1] <= 0x72)
    xs = [np.asarray(L, dtype=np.float64), np.asarray(R, dtype=np.float64)]
    if not stereo:
        xs = [(xs[0] + xs[1]) / 2] * 2
    n = len(xs[0])
    files = [p] + [os.path.join(OUT, "%s_L%d.mod" % (k, lv)) for lv in LEVELS[1:]]
    files = [f for f in files if os.path.exists(f)]
    if t in LEVEL_TYPES and len(files) > 1:
        sets = sorted((load_mod(f) for f in files), key=lambda z: z[2])
        dbs = np.array([z[2] for z in sets])
        shape = shape_of(ins, tempo)
        outs = [_run(xs, ins, c, P, shape) for c, P, _ in sets]
        e = env_db((xs[0] + xs[1]) / 2)
        j = np.clip(np.searchsorted(dbs, e, side="right") - 1, 0, len(dbs) - 2)
        u = np.clip((e - dbs[j]) / (dbs[j + 1] - dbs[j]), 0, 1)
        out = [np.zeros(n), np.zeros(n)]
        for i in range(len(dbs)):
            wgt = np.where(j == i, 1 - u, 0.0) + np.where(j + 1 == i, u, 0.0)
            for ch in range(2):
                out[ch] += wgt * outs[i][ch]
    else:
        shape = shape_of(ins, tempo)
        cols, P, _ = load_mod(p)
        out = _run(xs, ins, cols, P, shape)
    if shape is not None:
        N = 1 << int(np.ceil(np.log2(n + 1)))
        f = np.fft.rfftfreq(N, 1 / FS)
        out = [np.fft.irfft(np.fft.rfft(o, N) * shape(f, ch), N)[:n] for ch, o in enumerate(out)]
    return out[0], out[1]


def main():
    a = sys.argv[1:]
    cmd = a[0] if a else "list"
    if cmd == "list":
        s = needed()
        for k, (ins, tempo, voices) in s.items():
            print(k, ",".join(voices))
        print("%d 組" % len(s))
    elif cmd == "record":
        record(set(a[a.index("--only") + 1].split(",")) if "--only" in a else None, "--var" in a)
    elif cmd == "analyze":
        analyze_var() if "--var" in a else analyze()
    elif cmd == "listvar":
        s = variants()
        for k, (ins, tempo, base) in s.items():
            print(k, base[0])
        print("%d 組" % len(s))


if __name__ == "__main__":
    main()
