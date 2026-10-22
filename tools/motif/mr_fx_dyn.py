"""入口の大きさで効き方が変わるインサーション（Touch Wah・Vintage Touch Wah・Touch Wah + Dist / Odrv。Dynamic Filter / Flanger / Phaser・
Multi Band Comp 1・2 は録ったが使わない）を、プリセットの組のまま、ノイズの音量を 4 段（エレメントの音量 127・96・64・32）にして録り、音量ごとの周波数応答で持つ。
  python mr_fx_dyn.py list | record [--only 名前,…] | analyze
録り方: mr_fx_comp.py --wave 705 --long 3 --level L（dry と wet を 1 本ずつ）。
読み方: 窓 4096・ずらし 1024 の STFT で、0.5〜2.8 秒の枠の W·conj(X) と |X|² を足し合わせて割った H(f)（左右）と、dry の 1 枠あたりの音量（dB）を段ごとに持つ。
芯（apply）: 入口の STFT の枠ごとに音量を測り、段の音量の間で H を補間して掛ける（その型の Attack / Release は見ていない。枠の長さ 93ms の動き）。
結果は capture/fxdyn/<名前>.dyn（1 行目「DYN n=4096 hop=1024 levels=K」、そのあと段ごとに float32 の音量 dB と complex64 の HL・HR 2049 点ずつ）。
送るのはパラメーターチェンジ・ダンプの要求・ノートだけ（保存しない）。録った音は配らないこと。"""
import glob
import json
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CAP = os.path.join(HERE, "../../roms/motif-rack-es/capture")
OUT = os.path.join(CAP, "fxdyn")
FS = 44100
NW, HOP = 4096, 1024
TYPES = {(12, 0x10), (12, 0x11), (12, 0x12), (12, 0x30), (12, 0x31)}
# Dynamic Filter / Flanger / Phaser・Multi Band Comp（15 78・06 60・07 60・0A 50・0A 51）も録ったが、枠ごとに音量で切り替えるこの形では
# 一つの音量で録った姿（mr_fx_ltv.py）より実機から離れた（49 音色の比べで Griffin −4.0 → −4.7dB、The Ripper −0.3 → −6.5dB）ので使わない
RECORDED_TYPES = TYPES | {(15, 0x78), (6, 0x60), (7, 0x60), (10, 0x50), (10, 0x51)}
LEVELS = (127, 96, 64, 32)


def key_of(ins):
    t = (ins["type"] >> 7, ins["type"] & 0x7F)
    ps = [ins["param%d" % i] for i in range(1, 17)]
    return "t%d_%d_%s" % (t[0], t[1], "_".join(str(v) for v in ps))


def needed():
    sets = {}
    for p in sorted(glob.glob(os.path.join(CAP, "voice_json", "*.json"))):
        v = os.path.basename(p)[:-5]
        js = json.load(open(p, encoding="utf-8"))
        for blk in ("insertion1", "insertion2"):
            ins = js[blk]
            t = (ins["type"] >> 7, ins["type"] & 0x7F)
            if t in RECORDED_TYPES and ins["param10"] > 1:
                sets.setdefault(key_of(ins), (ins, []))[1].append(v)
    return sets


def record(only):
    os.makedirs(OUT, exist_ok=True)
    for k, (ins, voices) in needed().items():
        if only and k not in only:
            continue
        t = (ins["type"] >> 7, ins["type"] & 0x7F)
        st = ",".join("p%d=%d" % (i, ins["param%d" % i]) for i in range(1, 17))
        for lv in LEVELS:
            tag = "%s_L%d" % (k, lv)
            if os.path.exists(os.path.join(OUT, "cs%s_long_wet.wav" % tag)):
                continue
            subprocess.run([sys.executable, os.path.join(HERE, "mr_fx_comp.py"), "--dir", OUT, "--type", "%d:%d" % t, "--key", "60", "--wave", "705",
                            "--set", st, "--tag", tag, "--long", "3", "--level", str(lv)], check=True, cwd=HERE, stdout=subprocess.DEVNULL)
        print(k, "録った（%s）" % ",".join(voices), flush=True)


def stft(x):
    w = np.hanning(NW + 1)[:NW]
    nf = 1 + (len(x) - NW) // HOP
    return np.stack([np.fft.rfft(x[i * HOP:i * HOP + NW] * w) for i in range(nf)])


def frame_db(X):
    return 10 * np.log10(np.mean(np.abs(X) ** 2, axis=-1) + 1e-30)


def analyze():
    sys.path.insert(0, HERE)
    import mr_ref
    for k, (ins, voices) in needed().items():
        rows = []
        for lv in LEVELS:
            base = os.path.join(OUT, "cs%s_L%d_long" % (k, lv))
            if not os.path.exists(base + "_wet.wav"):
                break
            dL, dR = mr_ref.read_wav(base + "_dry.wav")
            wL, wR = mr_ref.read_wav(base + "_wet.wav")
            d = (np.asarray(dL, dtype=np.float64) + np.asarray(dR, dtype=np.float64)) / 2
            wL, wR = np.asarray(wL, dtype=np.float64), np.asarray(wR, dtype=np.float64)
            od = int(np.argmax(np.abs(d) > 1e-5))
            ow = int(np.argmax((np.abs(wL) + np.abs(wR)) > 1e-5))
            if abs(ow - od) > int(0.004 * FS):
                ow = od
            a, b = int(0.5 * FS), int(2.8 * FS)
            X = stft(d[od + a:od + b])
            P = np.sum(np.abs(X) ** 2, axis=0)
            Hs = [np.sum(stft(w[ow + a:ow + b]) * np.conj(X), axis=0) / (P + 1e-6 * P.max()) for w in (wL, wR)]
            rows.append((float(np.median(frame_db(X))), Hs[0], Hs[1]))
        if len(rows) < len(LEVELS):
            continue
        rows.sort(key=lambda r: r[0])
        with open(os.path.join(OUT, k + ".dyn"), "wb") as f:
            f.write(("DYN n=%d hop=%d levels=%d\n" % (NW, HOP, len(rows))).encode("ascii"))
            for db, HL, HR in rows:
                f.write(np.array([db], dtype="<f4").tobytes())
                for H in (HL, HR):
                    f.write(np.stack([H.real, H.imag], axis=1).astype("<f4").tobytes())
        g = [20 * np.log10(np.sqrt(np.mean(np.abs(r[1]) ** 2)) + 1e-12) for r in rows]
        print("%s（%s）: 入口 %s dB → 平均の利き %s dB" % (k, ",".join(voices), " ".join("%.1f" % r[0] for r in rows), " ".join("%+.1f" % x for x in g)), flush=True)


def apply(L, R, ins, cache={}):
    k = key_of(ins)
    p = os.path.join(OUT, k + ".dyn")
    if not os.path.exists(p):
        return None
    if k not in cache:
        with open(p, "rb") as f:
            head = dict(kv.split("=") for kv in f.readline().decode("ascii").split()[1:])
            K = int(head["levels"])
            nb = NW // 2 + 1
            dbs, Hs = [], []
            for _ in range(K):
                dbs.append(float(np.frombuffer(f.read(4), dtype="<f4")[0]))
                raw = np.frombuffer(f.read(2 * nb * 8), dtype="<f4").reshape(2, nb, 2)
                Hs.append(raw[..., 0] + 1j * raw[..., 1])
            cache[k] = (np.array(dbs), np.array(Hs))
    dbs, Hs = cache[k]
    xs = [np.asarray(L, dtype=np.float64), np.asarray(R, dtype=np.float64)]
    n = len(xs[0])
    mono = (xs[0] + xs[1]) / 2
    xps = [np.concatenate([x, np.zeros(NW)]) for x in xs]
    mp = np.concatenate([mono, np.zeros(NW)])
    w = np.hanning(NW + 1)[:NW]
    out = np.zeros((2, n + 2 * NW))
    norm = np.zeros(n + 2 * NW)
    nf = 1 + (len(mp) - NW) // HOP
    for i in range(nf):
        Xm = np.fft.rfft(mp[i * HOP:i * HOP + NW] * w)
        db = 10 * np.log10(np.mean(np.abs(Xm) ** 2) + 1e-30)
        # 段の音量の間で補間（外は端の段）
        j = int(np.clip(np.searchsorted(dbs, db, side="right") - 1, 0, len(dbs) - 2))
        u = float(np.clip((db - dbs[j]) / (dbs[j + 1] - dbs[j]), 0, 1))
        H = (1 - u) * Hs[j] + u * Hs[j + 1]
        for ch in range(2):
            X = np.fft.rfft(xps[ch][i * HOP:i * HOP + NW] * w)
            out[ch, i * HOP:i * HOP + NW] += np.fft.irfft(X * H[ch], NW) * w
        norm[i * HOP:i * HOP + NW] += w * w
    norm = np.where(norm > 1e-9, norm, 1)
    return out[0, :n] / norm[:n], out[1, :n] / norm[:n]


def main():
    a = sys.argv[1:]
    cmd = a[0] if a else "list"
    if cmd == "list":
        s = needed()
        for k, (ins, voices) in s.items():
            print(k, ",".join(voices))
        print("%d 組" % len(s))
    elif cmd == "record":
        record(set(a[a.index("--only") + 1].split(",")) if "--only" in a else None)
    elif cmd == "analyze":
        analyze()


if __name__ == "__main__":
    main()
