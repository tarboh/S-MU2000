"""mr_lfo.py の録音から、エレメントの LFO の数を出す（録音を読むだけで、実機には何も送らない）。
  python mr_lfo_fit.py 置き場

正弦波（波形 704、鍵 60）の瞬間の周波数（ヒルベルト変換の位相の傾き）と音量を 5ms ごとに出し、
  速さ（Hz）: 揺れの FFT の山（放物線で補間）
  音程の深さ: 揺れの山から谷までの半分（セント）
  音量の深さ: 音量の山から谷まで（dB）
  ディレイ・フェード: 揺れの大きさが最後の 10%・90% に届くまでの、キーオンからの時間
  形: 一周を 64 に分けて平均した、揺れの形（-1..+1）"""
import os
import sys

import numpy as np

import mr_ref

FS = 44100
HOP = 220   # 5ms


def tracks(path):
    L, R = mr_ref.read_wav(path)
    x = np.asarray(L) + np.asarray(R)
    on = int(np.argmax(np.abs(x) > 1e-3))
    X = np.fft.fft(x)
    h = np.zeros(len(x))
    h[0] = 1
    h[1:(len(x) + 1) // 2] = 2
    if len(x) % 2 == 0:
        h[len(x) // 2] = 1
    a = np.fft.ifft(X * h)
    ph = np.unwrap(np.angle(a))
    amp = np.abs(a)
    # 鳴っているところだけ（キーオン後 0.05 秒から、音が消える 0.05 秒前まで）
    hot = np.nonzero(amp > amp.max() * 0.05)[0]
    s, e = on + int(0.05 * FS), hot[-1] - int(0.05 * FS)
    idx = np.arange(s, e - HOP, HOP)
    f = np.array([(ph[i + HOP] - ph[i]) / (2 * np.pi) * FS / HOP for i in idx])
    db = np.array([20 * np.log10(np.sqrt(np.mean(np.abs(a[i:i + HOP]) ** 2)) + 1e-12) for i in idx])
    t = (idx - on) / FS
    return t, f, db


def rate(y, dt=HOP / FS):
    y = y - np.mean(y)
    n = len(y)
    w = np.hanning(n)
    Y = np.abs(np.fft.rfft(y * w, 16 * n))
    k = int(np.argmax(Y[1:])) + 1
    if 1 <= k < len(Y) - 1:
        a, b, c = np.log(Y[k - 1] + 1e-12), np.log(Y[k] + 1e-12), np.log(Y[k + 1] + 1e-12)
        k = k + 0.5 * (a - c) / (a - 2 * b + c)
    return k / (16 * n * dt)


def cents(f):
    return 1200 * np.log2(f / np.median(f))


def onset_times(t, y, hz):
    # 揺れの大きさ（一周ぶんの山から谷）を時間で追い、最後の値の 10%・90% に届く時刻
    per = max(2, int(round(1 / hz / (HOP / FS))))
    env = np.array([np.ptp(y[i:i + per]) for i in range(0, len(y) - per)])
    full = np.median(env[-3 * per:])
    t10 = t[int(np.argmax(env > 0.1 * full))]
    t90 = t[int(np.argmax(env > 0.9 * full))]
    return t10, t90, full


def main():
    d = sys.argv[1]
    have = lambda n: os.path.exists(os.path.join(d, n + ".wav"))
    p = lambda n: os.path.join(d, n + ".wav")
    print("speed: 値 Hz")
    for sp in list(range(0, 64, 4)) + [63]:
        if have("ls%02d" % sp):
            t, f, db = tracks(p("ls%02d" % sp))
            c = cents(f)
            print("  %2d %8.3f  (深さ ±%.1f セント)" % (sp, rate(c), np.ptp(c[len(c) // 4:]) / 2))
    print("pitch depth: 値 ±セント")
    for dp in (16, 32, 64, 96, 127):
        if have("lp%03d" % dp):
            t, f, db = tracks(p("lp%03d" % dp))
            c = cents(f)
            print("  %3d %7.1f" % (dp, np.ptp(c[len(c) // 4:]) / 2))
    print("amp depth: 値 dB（山から谷）")
    for dp in (32, 64, 96, 127):
        if have("la%03d" % dp):
            t, f, db = tracks(p("la%03d" % dp))
            print("  %3d %7.2f  速さ %.3f Hz" % (dp, np.ptp(db[len(db) // 4:]), rate(db[len(db) // 4:])))
    print("wave: 形（一周を 16 に分けた平均、-1..+1）")
    for w in (0, 1, 2):
        if have("lw%d" % w):
            t, f, db = tracks(p("lw%d" % w))
            c = cents(f)
            hz = rate(c)
            ph = (t * hz) % 1.0
            # 一番低い所を位相 0 にそろえる
            bins = np.array([np.mean(c[(ph >= i / 64) & (ph < (i + 1) / 64)]) for i in range(64)])
            bins = np.roll(bins, -int(np.argmin(bins)))
            bins = (bins - bins.min()) / (np.ptp(bins) + 1e-12) * 2 - 1
            print("  %d %s" % (w, " ".join("%+.2f" % v for v in bins[::4])))
    for pre, vals, lab in (("ld", (0, 8, 16, 32, 48, 64, 96), "delay"), ("lfd", (0, 8, 16, 32, 48, 64, 80, 96), "fade")):
        print("%s: 値 10%% 90%% の時刻（秒）" % lab)
        for v in vals:
            if have("%s%03d" % (pre, v)):
                t, f, db = tracks(p("%s%03d" % (pre, v)))
                c = cents(f)
                t10, t90, full = onset_times(t, c, rate(c[len(c) // 2:]))
                print("  %3d %6.3f %6.3f  (±%.1f セント)" % (v, t10, t90, full / 2))
    print("sync: キーオン直後の音程のずれ（セント）")
    for s in (0, 1):
        row = []
        for i in (1, 2):
            if have("lk%d_%d" % (s, i)):
                t, f, db = tracks(p("lk%d_%d" % (s, i)))
                c = 1200 * np.log2(f / 261.6256)
                row.append("%+.1f" % np.mean(c[:4]))
        print("  %d %s" % (s, " ".join(row)))
    for w in (0, 2):
        if have("lk1w%d" % w):
            t, f, db = tracks(p("lk1w%d" % w))
            c = 1200 * np.log2(f / 261.6256)
            print("sync on・形 %d（速さ 16）: 最初の 0.3 秒の音程（セント、25ms ごと）: %s" % (w, " ".join("%+.0f" % v for v in c[:60:5])))
    if have("lpa"):
        t, f, db = tracks(p("lpa"))
        c = cents(f)
        c, db = c[len(c) // 4:], db[len(db) // 4:]
        # 音程と音量の揺れの相関（+1 なら音程が高い時に音量が大きい）
        print("音程と音量の揺れの相関: %+.2f" % np.corrcoef(c, db)[0, 1])


if __name__ == "__main__":
    main()
