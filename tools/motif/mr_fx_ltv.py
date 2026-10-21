"""揺れながら周波数ごとに違う動きをするインサーション（フェイザー類・V-Flanger・Vintage Flanger・Tempo Flanger・Flanger 3・Symphonic・
Auto Wah・Vintage Auto Wah・Jump・Talking Modulator など）を、プリセットの組のまま録って、時間ごとの周波数応答 H(t, f) で持つ。
  python mr_fx_ltv.py list | record [--only 名前,…] | analyze
録り方: mr_fx_comp.py --wave 705 --long 8（ノイズ、音量 100、鍵 60）で dry と wet を 1 本ずつ。ノイズの波形は鳴らし始めから毎回同じ並びなので、
音の頭でそろえれば dry と wet の入口は同じ（0.1 秒おきの短い音どうしで相関 1.000 を確かめた）。テンポに合わせる型は音色のテンポを入れる。
読み方: 窓 4096 点（Hann）・ずらし 1024 点の STFT で、H = W·conj(X) / (|X|² + 小さい値) を左右の出口ごとに出し、前後 1 枠ずつで均す。
8 秒より長く鳴らすときは、最後の 1 周（|H| の動きの自己相関の山）を繰り返す。結果は capture/fxltv/<名前>.ltv
（1 行目「LTV n=4096 hop=1024 frames=F period=P」、そのあと complex64（float32 の実部・虚部）の HL・HR を 2049 点ずつ、枠ごとに）。
芯は入口の STFT に H を掛けて重ね足しで戻す（入口は左右を足して半分）。送るのはパラメーターチェンジ・ダンプの要求・ノートだけ（保存しない）。"""
import glob
import json
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CAP = os.path.join(HERE, "../../roms/motif-rack-es/capture")
OUT = os.path.join(CAP, "fxltv")
FS = 44100
NW, HOP = 4096, 1024
TYPES = {(7, 0x00), (7, 0x08), (7, 0x20), (7, 0x21), (7, 0x30), (7, 0x31), (7, 0x50), (6, 0x08), (6, 0x10), (6, 0x20), (6, 0x21), (6, 0x22),
         (6, 0x50), (5, 0x20), (12, 0x00), (12, 0x20), (12, 0x21), (14, 0x30), (15, 0x40), (14, 0x20),
         # 入口の大きさで効き方が変わる型（Touch Wah・Vintage Touch Wah・Dynamic Filter / Flanger / Phaser・Multi Band Comp・Auto Synth）は、
         # ノイズ（音量 100）で録った「その大きさでの姿」で代える（近似）
         (12, 0x10), (12, 0x11), (12, 0x12), (12, 0x30), (12, 0x31), (15, 0x78), (6, 0x60), (7, 0x60), (10, 0x50), (10, 0x51), (14, 0x10),
         # 歪みや雑音を含む組み合わせ（Auto Wah + Dist / Odrv、Wah + Dist / Odrv + Delay、Comp + Dist / Odrv + Delay、LoRes・Noisy・Attack LoFi）も同じ近似
         (12, 0x01), (12, 0x02), (12, 0x40), (12, 0x41), (10, 0x40), (10, 0x41), (13, 0x10), (13, 0x20), (13, 0x30)}
TEMPO_TYPES = {(7, 0x50), (6, 0x50), (14, 0x30)}


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
        if os.path.exists(os.path.join(OUT, "cs%s_long_wet.wav" % k)):
            continue
        t = (ins["type"] >> 7, ins["type"] & 0x7F)
        st = ",".join("p%d=%d" % (i, ins["param%d" % i]) for i in range(1, 17))
        cmd = [sys.executable, os.path.join(HERE, "mr_fx_comp.py"), "--dir", OUT, "--type", "%d:%d" % t, "--key", "60", "--wave", "705",
               "--set", st, "--tag", k, "--long", "8", "--level", "100"]
        subprocess.run(cmd, check=True, cwd=HERE, stdout=subprocess.DEVNULL)
        print(k, "録った（%s）" % ",".join(voices), flush=True)


def stft(x):
    w = np.hanning(NW + 1)[:NW]
    nf = 1 + (len(x) - NW) // HOP
    return np.stack([np.fft.rfft(x[i * HOP:i * HOP + NW] * w) for i in range(nf)])


def analyze():
    sys.path.insert(0, HERE)
    import mr_ref
    for k, (ins, tempo, voices) in needed().items():
        base = os.path.join(OUT, "cs%s_long" % k)
        if not os.path.exists(base + "_wet.wav"):
            continue
        dL, dR = mr_ref.read_wav(base + "_dry.wav")
        wL, wR = mr_ref.read_wav(base + "_wet.wav")
        d = (np.asarray(dL, dtype=np.float64) + np.asarray(dR, dtype=np.float64)) / 2
        wL, wR = np.asarray(wL, dtype=np.float64), np.asarray(wR, dtype=np.float64)
        od = int(np.argmax(np.abs(d) > 1e-4))
        ow = int(np.argmax((np.abs(wL) + np.abs(wR)) > 1e-4))
        if abs(ow - od) > int(0.004 * FS):
            ow = od
        n = int(8.0 * FS)
        d, wL, wR = d[od:od + n], wL[ow:ow + n], wR[ow:ow + n]
        m = min(len(d), len(wL))
        X = stft(d[:m])
        P = np.abs(X) ** 2
        ker = np.ones(3) / 3
        Hs = []
        for w in (wL[:m], wR[:m]):
            Y = stft(w)
            num = Y * np.conj(X)
            # 前後 1 枠ずつで均す（分子と分母を別々に）
            nums = np.apply_along_axis(lambda c: np.convolve(c, ker, "same"), 0, num)
            dens = np.apply_along_axis(lambda c: np.convolve(c, ker, "same"), 0, P)
            Hs.append(nums / (dens + 1e-6 * dens.max()))
        mag = np.abs(Hs[0]).mean(1) + np.abs(Hs[1]).mean(1)
        a = mag[4:] - mag[4:].mean()
        per = 0
        if np.std(a) > 1e-4 * np.mean(mag):
            ac = np.correlate(a, a, "full")[len(a) - 1:]
            ac /= ac[0]
            i = 8 + int(np.argmax(ac[8:len(ac) // 2]))
            per = i if ac[i] > 0.5 else 0
        F = Hs[0].shape[0]
        with open(os.path.join(OUT, k + ".ltv"), "wb") as f:
            f.write(("LTV n=%d hop=%d frames=%d period=%d\n" % (NW, HOP, F, per)).encode("ascii"))
            for i in range(F):
                for H in Hs:
                    f.write(np.stack([H[i].real, H[i].imag], axis=1).astype("<f4").tobytes())
        g = 20 * np.log10(np.sqrt(np.mean(np.abs(Hs[0]) ** 2)) + 1e-12)
        print("%s（%s）: 枠 %d・周期 %d 枠（%.2f 秒）・平均の利き %.1f dB" % (k, ",".join(voices), F, per, per * HOP / FS, g), flush=True)


def apply(L, R, ins, tempo, cache={}):
    k = key_of(ins, tempo)
    p = os.path.join(OUT, k + ".ltv")
    if not os.path.exists(p):
        return None
    if k not in cache:
        with open(p, "rb") as f:
            head = dict(kv.split("=") for kv in f.readline().decode("ascii").split()[1:])
            F, per = int(head["frames"]), int(head["period"])
            raw = np.frombuffer(f.read(), dtype="<f4").reshape(F, 2, NW // 2 + 1, 2)
            cache[k] = (raw[..., 0] + 1j * raw[..., 1], per)
    H, per = cache[k]
    F = H.shape[0]
    x = (np.asarray(L, dtype=np.float64) + np.asarray(R, dtype=np.float64)) / 2
    n = len(x)
    xp = np.concatenate([x, np.zeros(NW)])
    w = np.hanning(NW + 1)[:NW]
    out = np.zeros((2, n + 2 * NW))
    norm = np.zeros(n + 2 * NW)
    nf = 1 + (len(xp) - NW) // HOP
    for i in range(nf):
        j = i if i < F or per <= 0 else (F - 1 - per) + (i - (F - 1 - per)) % per
        j = min(j, F - 1)
        X = np.fft.rfft(xp[i * HOP:i * HOP + NW] * w)
        for ch in range(2):
            out[ch, i * HOP:i * HOP + NW] += np.fft.irfft(X * H[j, ch], NW) * w
        norm[i * HOP:i * HOP + NW] += w * w
    norm = np.where(norm > 1e-9, norm, 1)
    return out[0, :n] / norm[:n], out[1, :n] / norm[:n]


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
