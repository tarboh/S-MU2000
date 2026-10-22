"""歪み系のインサーション（Amp Simulator・Distortion など）を「線形の応答 → 頭打ち → LPF」（Wiener 型）に当てはめる
（録音を読むだけで、実機には何も送らない）。
  python mr_fx_dist.py 小さい音の名前（mr_fx.py --level 16 の _dry/_wet の前まで） 正弦波の置き場 タグ（mr_fx_comp.py --tag） [LPF の Hz]

1. 小さい音（エレメントの音量 16、入力 −47dBFS。8 と同じ応答になり、歪んでいない）の wet / dry から、線形の応答 h を出す
2. h から LPF（山のある 2 次、Q 1.6、測ったときの LPF の値の周波数）の分を割り戻し、正弦波 8 段（鍵 48）の dry に畳み込み、
   頭打ち x / (1 + |x / c|^p)^(1/p) と同じ LPF を通して、0.5〜0.9 秒の RMS が wet と合うように
   c・p を格子で探す。h の大きさは、鳴り続ける小さい正弦波（音量 16）の上げ幅に合わせる（50ms のノイズより 2dB ほど大きく出る）
3. 音量 127・48 で、倍音の大きさ（10 次まで）を模型と実機で並べる
初めは「頭打ち → 線形の応答」（Hammerstein 型）で当てはめたが、頭打ちの平らさと倍音の出方を同時に満たせなかった。
実機の倍音は方形波の 1/n（3・5・7・9 次が −10・−15・−18・−20dB）で、明るい応答は頭打ちの前にある。
結果は <小さい音の名前>_dist.npz（h の左右・c・p・LPF）に置く。"""
import os
import sys

import numpy as np

import mr_fx_fit as F

FS = 44100
LEVELS = (127, 112, 96, 80, 64, 48, 32, 16)


def linear_ir(base, n=8192):
    dL, _ = F.load(base + "_dry.wav")
    wL, wR = F.load(base + "_wet.wav")
    od = max(0, int(np.argmax(np.abs(dL) > 1e-6)) - 100)
    ow = max(0, int(np.argmax(np.abs(wL) > 1e-6)) - 100)
    hL = F.deconv(wL[ow:], dL[od:], n)
    hR = F.deconv(wR[ow:], dL[od:], n)
    return hL, hR


LPF_Q = 1.6     # Amp Simulator の LPF は山のある 2 次（値 40 と 58 の比で、1.6kHz に +3.9dB の山）


def lpf_resp(f, fc, q=LPF_Q):
    return 1 / (1 + 1j * (f / fc) / q - (f / fc) ** 2)


def lpf2(x, fc):
    N = 1 << int(np.ceil(np.log2(len(x) + 1)))
    f = np.fft.rfftfreq(N, 1 / FS)
    return np.fft.irfft(np.fft.rfft(x, N) * lpf_resp(f, fc), N)[:len(x)]


def unlpf(h, fc):
    """測った応答から LPF の分を割り戻す（頭打ちの前の形にする）"""
    N = 1 << int(np.ceil(np.log2(len(h) * 2)))
    f = np.fft.rfftfreq(N, 1 / FS)
    H = np.fft.rfft(h, N) / lpf_resp(f, fc)
    H[f > 18000] = 0
    return np.fft.irfft(H, N)[:len(h)]


def clip(x, c, p):
    return x / (1 + np.abs(x / c) ** p) ** (1 / p)


def harmonics(x, f0):
    N = len(x)
    X = np.abs(np.fft.rfft(x * np.hanning(N)))
    f = np.fft.rfftfreq(N, 1 / FS)
    out = []
    for k in range(1, 11):
        m = (f > f0 * k * 0.97) & (f < f0 * k * 1.03)
        out.append(20 * np.log10(X[m].max() + 1e-12) if m.any() else -200)
    return np.array(out)


def main():
    small, sdir, tag = sys.argv[1], sys.argv[2], sys.argv[3]
    fc = float(sys.argv[4]) if len(sys.argv) > 4 else 5000.0
    ref = int(sys.argv[5]) if len(sys.argv) > 5 else 16       # h の大きさを合わせる正弦波の段（小さいノイズを録った音量に近い段）
    hL, hR = linear_ir(small)
    hL, hR = unlpf(hL, fc), unlpf(hR, fc)          # 測った応答には LPF が入っているので割り戻し、頭打ちのあとで掛け直す
    takes = []
    for L in LEVELS:
        b = os.path.join(sdir, "cs%s_L%03d" % (tag, L))
        d, _ = F.load(b + "_dry.wav")
        w, _ = F.load(b + "_wet.wav")
        od = int(np.argmax(np.abs(d) > 1e-5))
        ow = int(np.argmax(np.abs(w) > 1e-5))
        takes.append((L, d[od:od + FS], w[ow:ow + FS]))
    sl = slice(int(0.5 * FS), int(0.9 * FS))
    rms = lambda x: 20 * np.log10(np.sqrt(np.mean(x[sl] ** 2)) + 1e-12)
    meas = np.array([rms(w) for _, _, w in takes])
    f0 = 440.0 * 2 ** ((48 - 69) / 12)
    seg = slice(int(0.5 * FS), int(0.5 * FS) + 16384)
    pre = [np.convolve(d, hL)[:len(d)] for _, d, _ in takes]
    ri = LEVELS.index(ref) if ref in LEVELS else len(LEVELS) - 1
    small_gain = meas[ri] - rms(pre[ri])
    k = 10 ** (small_gain / 20)
    pre = [x * k for x in pre]
    best = None
    for c in np.geomspace(0.01, 1.0, 50):
        for p in (2.0, 4.0, 8.0, 16.0, 32.0):
            outs = [lpf2(clip(x, c, p), fc) for x in pre]
            mod = np.array([rms(o) for o in outs])
            e1 = np.sqrt(np.mean((mod - meas) ** 2))
            if best is None or e1 < best[0]:
                best = (e1, c, p, mod, outs)
    err, c, p, mod, outs = best
    print("小さい音の上げ幅の直し %+.1f dB・頭打ち c = %.4f・硬さ p = %.0f・音量の差の二乗平均 %.2f dB" % (small_gain, c, p, err))
    for (L, d, w), mo, me in zip(takes, mod, meas):
        print("  音量 %3d: 入力 %.1f → 実機 %.1f・模型 %.1f dBFS" % (L, rms(d), me, mo))
    for i in (0, 5):
        hm = harmonics(takes[i][2][seg], f0)
        hmo = harmonics(outs[i][seg], f0)
        print("倍音（音量 %d、基音からの dB）実機:" % takes[i][0], " ".join("%.0f" % v for v in hm - hm[0]))
        print("                          模型:", " ".join("%.0f" % v for v in hmo - hmo[0]))
    np.savez_compressed(small + "_dist.npz", hL=(hL * k).astype(np.float32), hR=(hR * k).astype(np.float32), c=c, p=p, fc=fc)


if __name__ == "__main__":
    main()
