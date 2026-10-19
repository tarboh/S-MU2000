"""mr_aeg.py の録音から、音量の数を出す（録音を読むだけで、実機には何も送らない）。
  python mr_aeg_fit.py 置き場

  level : AEG の高さ L ごとの音量（L=127 からの dB）
  vel   : 感度・カーブ・ベロシティごとの音量（ベロシティ 127 からの dB）
  kf    : キーフォローの感度・鍵ごとの音量（鍵 60 からの dB）
  time  : 時間のベロシティ感度・キーフォローごとに、ディケイ 1 で 127→64 に落ちきるまでの時間（秒）"""
import os
import sys

import numpy as np

import mr_ref

FS = 44100


def env(path, win=0.01):
    L, R = mr_ref.read_wav(path)
    x = np.asarray(L) + np.asarray(R)
    n = int(win * FS)
    m = len(x) // n
    e = np.sqrt(np.mean(x[:m * n].reshape(m, n) ** 2, axis=1)) + 1e-12
    return 20 * np.log10(e)


def sustain_db(path, t0=0.3 + 0.15, t1=0.3 + 0.6):
    e = env(path)
    return float(np.median(e[int(t0 / 0.01):int(t1 / 0.01)]))


def main():
    d = sys.argv[1]
    have = lambda n: os.path.exists(os.path.join(d, n + ".wav"))
    if have("al127"):
        ref = sustain_db(os.path.join(d, "al127.wav"))
        print("level: L, dB")
        for L in (127, 120, 112, 104, 96, 88, 80, 64, 48, 32, 16, 8, 1, 0):
            if have("al%03d" % L):
                print("  %3d %8.2f" % (L, sustain_db(os.path.join(d, "al%03d.wav" % L)) - ref))
    vels = [1] + list(range(8, 128, 8)) + [127]
    combos = [(s, 0) for s in (0, 16, 32, 48, 80, 96, 112, 127)] + [(127, c) for c in (1, 2, 3, 4)]
    if have("av127_c0_v127"):
        print("vel: 感度,カーブ / ベロシティ " + " ".join("%5d" % v for v in vels))
        for s, c in combos:
            row = []
            for v in vels:
                n = "av%03d_c%d_v%03d" % (s, c, v)
                row.append(sustain_db(os.path.join(d, n + ".wav"), 0.4, 0.9) if have(n) else np.nan)
            ref = row[-1]
            print("  %3d,%d " % (s, c) + " ".join("%6.1f" % (x - ref) for x in row))
    if have("ak127_k60"):
        print("kf: 感度 / 鍵 36 48 60 72 84")
        for s in (0, 32, 96, 127):
            row = [sustain_db(os.path.join(d, "ak%03d_k%02d.wav" % (s, k)), 0.4, 0.9) for k in (36, 48, 60, 72, 84)]
            print("  %3d " % s + " ".join("%6.2f" % (x - row[2]) for x in row))

    def fall_time(n):
        # (アタックが終わってディケイ 1 が始まるまで, ディケイ 1 の長さ) 秒
        e = env(os.path.join(d, n + ".wav"))
        on = int(np.argmax(e > -60))
        top = float(np.median(e[on + 3:on + 8]))
        tail = float(np.median(e[int(3.0 / 0.01):int(3.25 / 0.01)]))
        i0 = on + 3 + int(np.argmax(e[on + 3:] < top - 0.5))
        i1 = on + 3 + int(np.argmax(e[on + 3:] < tail + 0.5))
        return (i0 - on) * 0.01, (i1 - i0) * 0.01

    if have("at064_v064"):
        print("time vel: 感度 / ベロシティ 1 64 127 の、アタックの長さ+ディケイ 1 の長さ（秒）")
        for s in (64, 80, 96):
            print("  %3d " % s + " ".join("%5.2f+%5.2f" % fall_time("at%03d_v%03d" % (s, v)) for v in (1, 64, 127)))
    if have("ak2127_k60"):
        print("time kf: 感度 / 鍵 36 60 84")
        for s in (96, 127):
            print("  %3d " % s + " ".join("%5.2f+%5.2f" % fall_time("ak2%03d_k%02d" % (s, k)) for k in (36, 60, 84)))


    def line_fit(n, t_tail0, t_tail1, frac=(0.2, 0.8)):
        # 下がる段を dB の直線に当てはめ、(始まり, 長さ) 秒。長さは頭から尻まで動く時間
        e = env(os.path.join(d, n + ".wav"), 0.002)
        on = int(np.argmax(e > -60))
        top = float(np.median(e[on + 5:on + 15]))
        tail = float(np.median(e[int(t_tail0 / 0.002):int(t_tail1 / 0.002)]))
        hi, lo = top - (top - tail) * frac[0], top - (top - tail) * frac[1]
        i = on + 5 + int(np.argmax(e[on + 5:] < hi))
        j = i + int(np.argmax(e[i:] < lo))
        if j - i < 3:
            return (i - on) * 0.002, (j - i) * 0.002 / (frac[1] - frac[0]), top - tail
        tt = np.arange(i, j) * 0.002
        a, b = np.polyfit(tt, e[i:j], 1)
        return (top - b) / a - on * 0.002, (top - tail) / -a, top - tail

    if have("bt072_v016"):
        print("time vel（ディケイ 1 = 80）: 感度 / ベロシティ 16 32 64 96 127 の、ディケイ 1 の長さ（秒）")
        for s in (72, 80, 88, 96, 112):
            print("  %3d " % s + " ".join("%6.3f" % line_fit("bt%03d_v%03d" % (s, v), 4.0, 4.25)[1] for v in (16, 32, 64, 96, 127)))
    if have("bk032_k36"):
        print("time kf（ディケイ 1 = 80）: 感度 / 鍵 36 48 72 84 の、ディケイ 1 の長さ（秒）")
        for s in (32, 80, 96, 127):
            print("  %3d " % s + " ".join("%6.3f" % line_fit("bk%03d_k%02d" % (s, k), 4.0, 4.25)[1] for k in (36, 48, 72, 84)))
    if have("sd096"):
        print("shape: ディケイ 1（64）で 127 → L の、始まり・長さ（秒）・落ちた dB")
        for L in (96, 32, 8):
            print("  %3d %6.3f %6.3f %6.1f" % ((L,) + line_fit("sd%03d" % L, 3.0, 3.25)))
        e = env(os.path.join(d, "sd000.wav"), 0.05)
        print("  L=0 の 50ms ごと:", " ".join("%.0f" % v for v in e[6:40]))
    if have("sa032"):
        print("attack: 初め 0 → 127 の 50ms ごとの dB（値 32・48・64・80）")
        for t in (32, 48, 64, 80):
            e = env(os.path.join(d, "sa%03d.wav" % t), 0.05)
            print("  %2d %s" % (t, " ".join("%.0f" % v for v in e[5:50])))


if __name__ == "__main__":
    main()
