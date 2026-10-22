"""揺れながら周波数ごとに違う動きをするインサーション（フェイザー類・V-Flanger・Vintage Flanger・Tempo Flanger・Flanger 3・Symphonic・
Auto Wah・Vintage Auto Wah・Jump・Talking Modulator など）を、プリセットの組のまま録って、時間ごとの周波数応答 H(t, f) で持つ。
  python mr_fx_ltv.py list | record [--only 名前,…] | analyze
録り方: mr_fx_comp.py --wave 705 --long 8（ノイズ、音量 100、鍵 60）で dry と wet を 1 本ずつ。ノイズの波形は鳴らし始めから毎回同じ並びなので、
音の頭でそろえれば dry と wet の入口は同じ（0.1 秒おきの短い音どうしで相関 1.000 を確かめた）。テンポに合わせる型は音色のテンポを入れる。
読み方: 窓 4096 点（Hann）・ずらし 1024 点の STFT で、左右の出口ごとに、大きさ √(|W|² / |X|²)・位相は W·conj(X) の H を出す（どれも前後 1 枠ずつで均す）。
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
# 式のある型（Chorus 1〜5・Celeste 1〜4・Flanger 1・2）も、組のまま録ったものがあればそれを優先する（式は Chorus 1・Flanger 1 で測ったもの）
EXTRA_TYPES = {(5, 0x00), (5, 0x01), (5, 0x02), (5, 0x08), (5, 0x09), (5, 0x10), (5, 0x11), (5, 0x12), (5, 0x18), (6, 0x00), (6, 0x01)}
TYPES = TYPES | EXTRA_TYPES
TEMPO_TYPES = {(7, 0x50), (6, 0x50), (14, 0x30)}
# 入口の大きさで効き方が変わる型・歪みや雑音を含む組み合わせ（近似の組）は、相互スペクトルの平均で H を出し、較正もしない
# （エネルギーの比にすると歪みの倍音まで線形の利きに数え、Crying で +2.4dB、The Ripper で +6.7dB 大きく出た）
APPROX_TYPES = {(12, 0x10), (12, 0x11), (12, 0x12), (12, 0x30), (12, 0x31), (15, 0x78), (6, 0x60), (7, 0x60), (10, 0x50), (10, 0x51), (14, 0x10),
                (12, 0x01), (12, 0x02), (12, 0x40), (12, 0x41), (10, 0x40), (10, 0x41)}
# LoRes・Noisy・Attack LoFi（0D 10〜30）は、間引きの折り返しや雑音が入口と相関しないので、相互スペクトルでは取りこぼす（Step Dance で −9.8dB）。
# エネルギーの比と較正の側に置く（スペクトルの形は近似、音量は合う）


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
            if t in TYPES and (t not in EXTRA_TYPES or ins["param10"] > 1):
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
            pyy = np.apply_along_axis(lambda c: np.convolve(c, ker, "same"), 0, np.abs(Y) ** 2)
            # 大きさは出力と入力のエネルギーの比、位相は相互スペクトルから。W·conj(X) の平均だけだと、遅れが揺れる型（Symphonic）で
            # 位相のずれた分を取りこぼし、自分の録音でも 3.5dB 小さく戻った
            if (ins["type"] >> 7, ins["type"] & 0x7F) in APPROX_TYPES:
                Hs.append(nums / (dens + 1e-6 * dens.max()))
            else:
                coh = nums / (dens + 1e-6 * dens.max())
                mag = np.sqrt(pyy / (dens + 1e-6 * dens.max()))
                # 入口のエネルギーがほとんど無いビン（ノイズの波形の低い端など）では、出口の雑音を割って大きくなりすぎる（J-Pop の弦の低域が +34dB）。
                # 枠の中央値の 1/1000 より小さいビンは相互スペクトルの値を使う
                low = dens < 1e-3 * np.median(dens, axis=1, keepdims=True)
                # ある枠だけ入口の小さいビンで比が尖る（Celeste 3 で 4kHz より上に +34dB）ので、相互スペクトルの大きさの 4 倍（+12dB）で頭打ち
                mag = np.minimum(mag, 4 * np.abs(coh))
                Hs.append(np.where(low, coh, mag * np.exp(1j * np.angle(nums))))
        mag = np.abs(Hs[0]).mean(1) + np.abs(Hs[1]).mean(1)
        a = mag[4:] - mag[4:].mean()
        per = 0
        if np.std(a) > 1e-4 * np.mean(mag):
            ac = np.correlate(a, a, "full")[len(a) - 1:]
            ac /= ac[0]
            i = 8 + int(np.argmax(ac[8:len(ac) // 2]))
            per = i if ac[i] > 0.5 else 0
        # ビンごとに、時間方向の中央値の 4 倍（+12dB）で頭打ち（ある枠だけ尖る所がある。Celeste 3 で 4kHz より上に +34dB）
        clipped = []
        for H in Hs:
            lim = 4 * np.median(np.abs(H), axis=0, keepdims=True) + 1e-9
            a_ = np.abs(H)
            clipped.append(np.where(a_ > lim, H * (lim / np.maximum(a_, 1e-12)), H))
        Hs = clipped
        F = Hs[0].shape[0]
        # 自分の録音で較正: 録った dry を模型に通し、wet との音量の比を左右ごとに H に掛ける（重なった枠どうしの位相の打ち消しの分。
        # Symphonic で 1.4dB）
        n2 = min(m, int(7.5 * FS))
        ins2 = dict(ins)
        oL, oR = _apply_H([dL_[od:od + n2] for dL_ in (np.asarray(dL, dtype=np.float64), np.asarray(dR, dtype=np.float64))], Hs, 0, ins2)
        corr = []
        for o, w in ((oL, wL), (oR, wR)):
            a2, b2 = int(1.0 * FS), n2 - int(1.0 * FS)
            e1 = np.mean(o[a2:b2] ** 2)
            e2 = np.mean(w[a2:b2] ** 2)
            corr.append(np.sqrt(e2 / e1) if e1 > 1e-20 else 1.0)
        if (ins["type"] >> 7, ins["type"] & 0x7F) in APPROX_TYPES:
            corr = [1.0, 1.0]
        Hs = [H * c for H, c in zip(Hs, corr)]
        with open(os.path.join(OUT, k + ".ltv"), "wb") as f:
            f.write(("LTV n=%d hop=%d frames=%d period=%d\n" % (NW, HOP, F, per)).encode("ascii"))
            for i in range(F):
                for H in Hs:
                    f.write(np.stack([H[i].real, H[i].imag], axis=1).astype("<f4").tobytes())
        g = 20 * np.log10(np.sqrt(np.mean(np.abs(Hs[0]) ** 2)) + 1e-12)
        print("%s（%s）: 枠 %d・周期 %d 枠（%.2f 秒）・平均の利き %.1f dB・較正 %+.2f / %+.2f dB" % (k, ",".join(voices), F, per, per * HOP / FS, g,
              20 * np.log10(corr[0]), 20 * np.log10(corr[1])), flush=True)


def _apply_H(xs_in, Hs, per, ins):
    """H の並び（左右）を入口に掛ける（apply と同じ計算。較正に使う）"""
    H = np.stack(Hs, axis=1)
    return _run(xs_in[0], xs_in[1], H, per, ins)


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
    return _run(L, R, H, per, ins)


def _run(L, R, H, per, ins):
    F = H.shape[0]
    # 左右を別々に通す（フェイザー類・Symphonic 系の 7 音色で、左右を足すより実機に 2〜4dB 近い。49 音色の比べ）。Dynamic Filter だけは足す方が合う
    t = (ins["type"] >> 7, ins["type"] & 0x7F)
    stereo = t != (15, 0x78) and not (t in EXTRA_TYPES and t[0] == 5 and ins.get("param15", 1) == 0)    # コーラス系は Input Mode（15）が 0 なら mono
    xs = [np.asarray(L, dtype=np.float64), np.asarray(R, dtype=np.float64)]
    if not stereo:
        xs = [(xs[0] + xs[1]) / 2] * 2
    n = len(xs[0])
    # 頭の前に窓 1 枚ぶんの無音を足す（最初の枠では窓の重なりの合計がほぼ 0 で、割り算が暴れる。芯の音は 0 サンプル目から鳴るので、
    # J-Pop で頭の 0.1 秒が +14dB・最大値 207 になった）。出口はその分ずらして取る
    xps = [np.concatenate([np.zeros(NW), x, np.zeros(NW)]) for x in xs]
    xp = xps[0]
    w = np.hanning(NW + 1)[:NW]
    out = np.zeros((2, n + 3 * NW))
    norm = np.zeros(n + 3 * NW)
    nf = 1 + (len(xp) - NW) // HOP
    for i in range(nf):
        j = i if i < F or per <= 0 else (F - 1 - per) + (i - (F - 1 - per)) % per
        j = min(j, F - 1)
        for ch in range(2):
            X = np.fft.rfft(xps[ch][i * HOP:i * HOP + NW] * w)
            out[ch, i * HOP:i * HOP + NW] += np.fft.irfft(X * H[j, ch], NW) * w
        norm[i * HOP:i * HOP + NW] += w * w
    norm = np.where(norm > 1e-9, norm, 1)
    return out[0, NW:NW + n] / norm[NW:NW + n], out[1, NW:NW + n] / norm[NW:NW + n]


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
