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


def record(only):
    os.makedirs(OUT, exist_ok=True)
    for k, (ins, tempo, voices) in needed().items():
        if only and k not in only:
            continue
        t = (ins["type"] >> 7, ins["type"] & 0x7F)
        st = ",".join("p%d=%d" % (i, ins["param%d" % i]) for i in range(1, 17))
        for key, _ in KEYS:
            tag = "%s_k%d" % (k, key)
            if os.path.exists(os.path.join(OUT, "cs%s_long_wet.wav" % tag)):
                continue
            cmd = [sys.executable, os.path.join(HERE, "mr_fx_comp.py"), "--dir", OUT, "--type", "%d:%d" % t, "--key", str(key),
                   "--set", st, "--tag", tag, "--long", "8", "--level", "100"]
            subprocess.run(cmd, check=True, cwd=HERE, stdout=subprocess.DEVNULL)
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


def analyze():
    for k, (ins, tempo, voices) in needed().items():
        bases = [os.path.join(OUT, "cs%s_k%d" % (k, key)) for key, _ in KEYS]
        if not all(os.path.exists(b + "_long_wet.wav") for b in bases):
            continue
        bands = [read_pair(b, f0) for b, (_, f0) in zip(bases, KEYS)]
        n = min(len(x) for band in bands for x in band)
        cols = [x[:n] for band in bands for x in band]
        P = period_of(cols[0]) or period_of(cols[2])      # Auto Pan は左右が逆に動いて足すと一定なので、片側ずつ見る
        with open(os.path.join(OUT, k + ".mod"), "wb") as f:
            f.write(("MOD rate=%d frames=%d period=%d\n" % (RATE, n, P)).encode("ascii"))
            f.write(np.stack(cols, axis=1).astype("<f4").tobytes())
        lo, hi = bands
        print("%s（%s）: 周期 %d ms・低い帯 A 左 %.2f〜%.2f 右 %.2f〜%.2f・d 左 %.1f〜%.1f・高い帯 A 左 %.2f〜%.2f・d 左 %.1f〜%.1f サンプル" % (
            k, ",".join(voices), P, lo[0][200:].min(), lo[0][200:].max(), lo[2][200:].min(), lo[2][200:].max(),
            lo[1][200:].min(), lo[1][200:].max(), hi[0][200:].min(), hi[0][200:].max(), hi[1][200:].min(), hi[1][200:].max()), flush=True)


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


def apply(L, R, ins, tempo, cache={}):
    k = key_of(ins, tempo)
    p = os.path.join(OUT, k + ".mod")
    if not os.path.exists(p):
        return None
    if k not in cache:
        with open(p, "rb") as f:
            head = f.readline().decode("ascii").split()
            P = int(head[3].split("=")[1])
            cache[k] = (np.frombuffer(f.read(), dtype="<f4").reshape(-1, 8).astype(np.float64), P)
    cols, P = cache[k]
    # 左右を別々に通す（Tremolo・Slice が実機と 3〜4dB 合うようになった。49 音色の比べ）。Rotary・Dual Rotor はスピーカーへ足して送る形なので左右を足す
    t = (ins["type"] >> 7, ins["type"] & 0x7F)
    stereo = not (t[0] == 8 and 0x50 <= t[1] <= 0x72)
    xs = [np.asarray(L, dtype=np.float64), np.asarray(R, dtype=np.float64)]
    if not stereo:
        xs = [(xs[0] + xs[1]) / 2] * 2
    n = len(xs[0])
    t = np.arange(n)
    out = [np.zeros(n), np.zeros(n)]
    bl = [split(x) for x in xs]
    He = eq_response(ins, np.array([KEYS[0][1], KEYS[1][1]]))
    norm = [1.0, 1.0] if He is None else [max(1e-3, abs(He[0])), max(1e-3, abs(He[1]))]
    for b in range(2):
        for ch in range(2):
            xb = bl[ch][b]
            A = series(cols[:, b * 4 + ch * 2], P, n) / norm[b]
            d = series(cols[:, b * 4 + ch * 2 + 1], P, n)
            out[ch] += A * np.interp(t - d, t, xb, left=0.0)
    N = 1 << int(np.ceil(np.log2(n + 1)))
    He = eq_response(ins, np.fft.rfftfreq(N, 1 / FS))
    if He is not None:
        out = [np.fft.irfft(np.fft.rfft(o, N) * He, N)[:n] for o in out]
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
        record(set(a[a.index("--only") + 1].split(",")) if "--only" in a else None)
    elif cmd == "analyze":
        analyze()


if __name__ == "__main__":
    main()
