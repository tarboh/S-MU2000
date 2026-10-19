"""mr_peg.py の録音から、PEG と音程の感度の数を出す（録音を読むだけで、実機には何も送らない）。
  python mr_peg_fit.py 置き場

音程は mr_lfo_fit.tracks（ヒルベルト変換の位相の傾き、5ms ごと）。基準は PEG の高さが全部 0 の録音（pr48 の押さえている間）。"""
import os
import sys

import numpy as np

import mr_lfo_fit as T


def main():
    d = sys.argv[1]
    p = lambda n: os.path.join(d, n + ".wav")
    have = lambda n: os.path.exists(p(n))
    t, f, db = T.tracks(p("pr48"))
    ref = float(np.median(f[(t > 0.2) & (t < 0.8)]))
    cents = lambda f_: 1200 * np.log2(f_ / ref)
    print("基準 %.3f Hz" % ref)

    def still(n, t0=0.3, t1=0.7):
        t, f, db = T.tracks(p(n))
        return float(np.median(cents(f[(t > t0) & (t < t1)])))

    print("scale: 深さ / 高さ −64 −16 +16 +64 +127 の音程（セント）")
    for dp in (72, 80, 96, 127):
        row = []
        for x in (-64, -16, 16, 64, 127):
            n = ("pd%03d_l%+04d" % (dp, x)).replace("+", "p").replace("-", "m")
            row.append(still(n) if have(n) else np.nan)
        print("  %+3d %s" % (dp - 64, " ".join("%8.1f" % v for v in row)))

    def path(n, step=0.05, upto=3.0):
        t, f, db = T.tracks(p(n))
        c = cents(f)
        return " ".join("%.0f" % np.median(c[(t >= a) & (t < a + step)]) for a in np.arange(0.0, upto, step) if ((t >= a) & (t < a + step)).any())

    def crossing(n, frac=0.5):
        # 音程が始め（最初の 20ms）から終わり（最後の 0.2 秒）へ frac だけ動いた時刻
        t, f, db = T.tracks(p(n))
        c = cents(f)
        a, b = np.median(c[:4]), np.median(c[-40:])
        i = int(np.argmax((c - a) * np.sign(b - a) >= frac * abs(b - a)))
        return t[i], a, b

    if have("pa32"):
        print("time: 50ms ごとの音程（セント）")
        for n in ["pa32", "pa48", "pa64", "pa80", "pd1_48", "pd1_64", "pd1_80", "ph32", "ph64"]:
            if have(n):
                tm, a, b = crossing(n)
                t90, _, _ = crossing(n, 0.9)
                print("  %-7s 50%% %.3f 秒・90%% %.3f 秒（%.0f → %.0f）| %s" % (n, tm, t90, a, b, path(n, 0.05, 1.6)))
        for n in ("pr48", "pr64"):
            if have(n):
                t, f, db = T.tracks(p(n))
                c = cents(f)
                print("  %-7s 離した後 50ms ごと: %s" % (n, " ".join("%.0f" % np.median(c[(t >= a) & (t < a + 0.05)]) for a in np.arange(0.95, 1.9, 0.05) if ((t >= a) & (t < a + 0.05)).any())))
    if have("pl127_v127"):
        print("lvel: 感度 / ベロシティ 1 32 64 96 127 の音程（セント）")
        for s in (80, 96, 127):
            print("  %+3d %s" % (s - 64, " ".join("%8.1f" % still("pl%03d_v%03d" % (s, v)) for v in (1, 32, 64, 96, 127))))
    if have("pk127_k72"):
        print("kf: 値 / 鍵 48 72 の音程（鍵 60 からのセント。ふつうなら −1200・+1200）")
        for s in (48, 80, 112, 127):
            print("  %3d %s" % (s, " ".join("%8.1f" % (still("pk%03d_k%02d" % (s, k)) - 0) for k in (48, 72))))
    if have("pv127_v127"):
        print("pvel: 値 / ベロシティ 1 64 127 の音程（セント）")
        for s in (80, 96, 127):
            print("  %3d %s" % (s, " ".join("%8.1f" % still("pv%03d_v%03d" % (s, v)) for v in (1, 64, 127))))


if __name__ == "__main__":
    main()
