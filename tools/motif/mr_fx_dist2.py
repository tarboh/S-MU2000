"""V-Distortion（09 10）を「線形の応答 h1 → 頭打ち → 後ろの応答 h2」に当てはめる（2026-10-27。録音を読むだけで、実機には何も送らない）。
  python mr_fx_dist2.py 組の名前（t9_16_… の形） [--write]
mr_fx_dist.py の Wiener 型（線形の応答 → 頭打ち → LPF）は、V-Distortion では LPF の欄が無いので 20kHz にしていた。強く歪む組（Drive 40 から上）で、
実機より 4〜8kHz が 10〜20dB 明るく、ギターの音色で外れていた。V-Distortion にはスピーカーの作り（p3）があり、頭打ちのあとに暗くする形が入っているらしい。
1. 小さいノイズ（_s64 があればそれ）の wet / dry から、全体の線形の応答 H = H1·H2
2. 正弦波（鍵 48、130.8Hz）の音量 127 の wet の奇数の倍音と、頭打ちのあとを素通しにした模型の倍音の比から |H2| を読む（基音で 1、帯ごとにならす）。
   H2 は最小位相、H1 = H / H2
3. c・p を、8 段の音量が合うように格子で探す（mr_fx_dist.py と同じ）。頭打ちの上下の違い b（入口を b × c だけずらし、ずらした分を引く）は、
   2 倍音（音量 127・48）が合うように選ぶ
--write で capture/fxds/<組>.dsx を書き直す（頭に post=タップ数・b=、h1 の左右のあとに h2 を足す）。前の形は <組>.dsx.wiener に控えておき、
正弦波 8 段の倍音（2〜12 次）と音量の外れが前の形より 0.5 小さいときだけ書き直す（歪みの小さい組では倍音が床に埋もれて h2 が読めない）。"""
import os
import sys

import numpy as np

import mr_fx_dist as D
import mr_fx_fit as F

FS = 44100
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "../../roms/motif-rack-es/capture/fxds")
F0 = 440.0 * 2 ** ((48 - 69) / 12)


def minphase(mag, n):
    """大きさ（rfft の点、n 点の FFT）から最小位相のインパルス応答"""
    lm = np.log(np.maximum(mag, 1e-6))
    c = np.fft.irfft(lm, n)
    w = np.zeros(n)
    w[0] = 1
    w[1:n // 2] = 2
    w[n // 2] = 1
    return np.fft.irfft(np.exp(np.fft.rfft(c * w, n)), n)


def harm_amp(x, nh):
    N = len(x)
    X = np.abs(np.fft.rfft(x * np.hanning(N)))
    f = np.fft.rfftfreq(N, 1 / FS)
    return np.array([X[(f > F0 * k - 20) & (f < F0 * k + 20)].max() for k in range(1, nh + 1)])


def main():
    a = sys.argv[1:]
    k = a[0]
    tag = k
    small = os.path.join(OUT, "insa_%s_s64" % k)
    if not os.path.exists(small + "_wet.wav"):
        small = os.path.join(OUT, "insa_" + k)
    hL, hR = D.linear_ir(small)
    takes = []
    for L in D.LEVELS:
        b = os.path.join(OUT, "cs%s_L%03d" % (tag, L))
        d, _ = F.load(b + "_dry.wav")
        w, _ = F.load(b + "_wet.wav")
        od = int(np.argmax(np.abs(d) > 1e-5))
        ow = int(np.argmax(np.abs(w) > 1e-5))
        takes.append((L, d[od:od + FS], w[ow:ow + FS]))
    sl = slice(int(0.5 * FS), int(0.9 * FS))
    rms = lambda x: 20 * np.log10(np.sqrt(np.mean(x[sl] ** 2)) + 1e-12)
    meas = np.array([rms(w) for _, _, w in takes])
    seg = slice(int(0.5 * FS), int(0.5 * FS) + 16384)
    nh = int(18000 / F0)
    N = 8192
    f = np.fft.rfftfreq(N, 1 / FS)
    HL = np.fft.rfft(hL, N)
    HR = np.fft.rfft(hR, N)
    H2 = np.ones(len(f))
    c, p, bb = 0.1, 8.0, 0.0
    clipb = lambda x, c, p, b: D.clip(x + b * c, c, p) - D.clip(b * c, c, p)     # 頭打ちの上下の違い（b × c だけずらして、ずらした分を引く）
    for it in range(5):
        h2 = minphase(H2, N)[:2048]
        h1L = np.fft.irfft(HL / np.fft.rfft(h2, N), N)[:4096]
        # 大きさを小さい正弦波（音量 16）に合わせる
        pre = [np.convolve(d, h1L)[:len(d)] for _, d, _ in takes]
        post = lambda x: np.convolve(x, h2)[:len(x)]
        small_gain = meas[-1] - rms(post(pre[-1]))
        g = 10 ** (small_gain / 20)
        pre = [x * g for x in pre]
        best = None
        for cc in np.geomspace(0.01, 10.0, 60):
            for pp in (2.0, 4.0, 8.0, 16.0):
                mod = np.array([rms(post(clipb(x, cc, pp, bb))) for x in pre[:6:2]])
                e = np.sqrt(np.mean((mod - meas[:6:2]) ** 2))
                if best is None or e < best[0]:
                    best = (e, cc, pp)
        _, c, p = best
        # 2 倍音（音量 127・48）が合うように b を選ぶ
        tb = None
        for b in (0.0, 0.01, 0.02, 0.05, 0.1, 0.2, 0.3, 0.5):
            e = 0
            for i in (0, 5):
                hw2 = harm_amp(takes[i][2][seg], 2)
                mo2 = harm_amp(post(clipb(pre[i], c, p, b))[seg], 2)
                e += (20 * np.log10(hw2[1] / hw2[0]) - 20 * np.log10(mo2[1] / mo2[0] + 1e-9)) ** 2
            if tb is None or e < tb[0]:
                tb = (e, b)
        bb = tb[1]
        # 音量 127 の倍音の比で |H2| を直す
        hw = harm_amp(takes[0][2][seg], nh)
        mo = harm_amp(post(clipb(pre[0], c, p, bb))[seg], nh)
        r = hw / np.maximum(mo, 1e-9)
        r /= r[0]
        fk = F0 * np.arange(1, nh + 1)
        ok = (hw > hw.max() * 1e-3) & (np.arange(1, nh + 1) % 2 == 1)     # 偶数の倍音は頭打ちの上下の違いで決まるので、奇数だけで読む（床に近いものは使わない）
        if ok.sum() < 4:
            break                                                          # 歪みが小さくて読めない
        # 対数の周波数の格子（1/12 オクターブ）で 1 オクターブの幅にならしてから戻す。細かい谷を割り戻すと、
        # その帯だけ頭打ちの前で大きく持ち上がり、正弦波（130Hz）では見えないのにギターの音で歪み過ぎた
        lg = np.log2(F0) + np.arange(0, int(12 * np.log2(18000 / F0)) + 1) / 12
        lv = np.interp(lg, np.log2(fk[ok]), np.log(r[ok]))
        lv = np.convolve(np.pad(lv, 6, mode="edge"), np.ones(13) / 13, mode="valid")
        lr = np.interp(np.log2(np.maximum(f, F0)), lg, lv)
        H2 = H2 * np.exp(lr)
        H2[f > 18000] = H2[f <= 18000][-1]
        H2 = np.clip(H2, 10 ** (-40 / 20), 10 ** (12 / 20))                # 頭打ちの前の持ち上げを 40dB までにする
        mod = np.array([rms(post(clipb(x, c, p, bb))) for x in pre])
        print("回 %d: c %.4f p %.0f b %.2f 音量の差の二乗平均 %.2f dB・倍音の比（2〜12 次、dB）%s" % (
            it, c, p, bb, np.sqrt(np.mean((mod - meas) ** 2)), " ".join("%+.0f" % v for v in 20 * np.log10(r[1:12]))))
    h2 = minphase(H2, N)[:2048]
    H2c = np.fft.rfft(h2, N)
    h1L = np.fft.irfft(HL / H2c, N)[:4096] * g
    h1R = np.fft.irfft(HR / H2c, N)[:4096] * g
    post = lambda x: np.convolve(x, h2)[:len(x)]
    for i in (0, 3, 5):
        x = np.convolve(takes[i][1], h1L)[:FS]
        hm = harm_amp(takes[i][2][seg], 12)
        hmo = harm_amp(post(clipb(x, c, p, bb))[seg], 12)
        print("倍音（音量 %d、基音からの dB）実機:" % takes[i][0], " ".join("%.0f" % v for v in 20 * np.log10(hm / hm[0])))
        print("                          模型:", " ".join("%.0f" % v for v in 20 * np.log10(hmo / hmo[0])))
    print("|H2|（dB）:", " ".join("%d:%+.0f" % (fq, 20 * np.log10(np.interp(fq, f, H2))) for fq in (130, 500, 1000, 2000, 4000, 6000, 8000, 12000, 16000)))
    # 前の形（.dsx.wiener か今の .dsx: 線形の応答 → 頭打ち → LPF）と、正弦波 8 段の倍音（2〜12 次、実機の床より上）と音量で比べる
    def score(model):
        e = []
        for i, (L, d, w) in enumerate(takes):
            if i % 2:
                continue
            y = model(d)
            hm = 20 * np.log10(harm_amp(w[seg], 12) + 1e-12)
            hmo = 20 * np.log10(harm_amp(y[seg], 12) + 1e-12)
            # 基音から 60dB より下は 60dB にそろえる（床の下の違いは数えない）
            e.extend(list(np.abs(np.maximum(hm - hm[0], -60) - np.maximum(hmo - hmo[0], -60))[1:]))
            e.append(abs(rms(w) - rms(y)) * 3)
        return float(np.mean(e))
    old_path = os.path.join(OUT, k + ".dsx.wiener")
    if not os.path.exists(old_path):
        old_path = os.path.join(OUT, k + ".dsx")
    with open(old_path, "rb") as fo:
        hd = dict(x.split("=") for x in fo.readline().decode().split()[1:])
        nfr = int(hd["frames"])
        ow = np.frombuffer(fo.read(nfr * 8), "<f4").reshape(-1, 2)
    oc, op, ofc = float(hd["c"]), float(hd["p"]), float(hd["fc"])
    s_old = score(lambda d: D.lpf2(D.clip(np.convolve(d, ow[:, 0])[:len(d)], oc, op), ofc))
    s_new = score(lambda d: post(clipb(np.convolve(d, h1L)[:len(d)], c, p, bb)))
    use = s_new < s_old - 0.5
    # Comp+Dist（09 01）は中のコンプの動きが正弦波では見えず、ギターの音色で悪くなった（01_052 1.6 → 9.6dB、00_035 0.6 → 4.5dB）ので前の形のまま。
    # _post_skip.txt の組は、実機で録った音色（capture/presets_gt）で前の形のほうが合ったもの
    skip = os.path.join(OUT, "_post_skip.txt")
    if k.startswith("t9_1_") or (os.path.exists(skip) and k in open(skip).read().split()):
        use = False
    print("倍音と音量の外れ（前の形 %.2f・頭打ちのあとの応答 %.2f）→ %s" % (s_old, s_new, "書き直す" if use else "前の形のまま"))
    if "--write" in a and not use:
        if os.path.exists(old_path) and old_path.endswith(".wiener"):
            with open(old_path, "rb") as fi, open(os.path.join(OUT, k + ".dsx"), "wb") as fo:
                fo.write(fi.read())
    if "--write" in a and use:
        head = "DSX frames=%d c=%.6f p=%.3f fc=%.1f drive=0 post=%d b=%.4f\n" % (len(h1L), c, p, 20000.0, len(h2), bb)
        with open(os.path.join(OUT, k + ".dsx"), "wb") as fo:
            fo.write(head.encode("ascii"))
            fo.write(np.stack([h1L, h1R], axis=1).astype("<f4").tobytes())
            fo.write(h2.astype("<f4").tobytes())
        print("書いた", k + ".dsx")


if __name__ == "__main__":
    main()
