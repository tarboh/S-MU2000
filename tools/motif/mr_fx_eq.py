"""mr_fx.py --block insa で録った線形のインサーション（EQ など）の周波数応答を読む（録音を読むだけで、実機には何も送らない）。
  python mr_fx_eq.py 名前…（_dry.wav・_wet.wav の前まで）

wet を dry で割った H(f) の大きさ（dB）を、1/3 オクターブの点（31.5Hz〜16kHz）で出す。インサーションは途中に入るので、H がそのまま応答。"""
import sys

import numpy as np

import mr_fx_fit as F

FS = 44100
POINTS = [31.5 * 2 ** (i / 3) for i in range(28)]


def response(base):
    dL, dR = F.load(base + "_dry.wav")
    wL, wR = F.load(base + "_wet.wav")
    od = max(0, int(np.argmax(np.abs(dL) > 1e-4)) - 100)
    ow = max(0, int(np.argmax(np.abs(wL) > 1e-4)) - 100)
    d, w = dL[od:od + 16384], wL[ow:ow + 16384]
    N = 1 << 16
    D, W = np.fft.rfft(d, N), np.fft.rfft(w, N)
    f = np.fft.rfftfreq(N, 1 / FS)
    P = np.abs(D) ** 2
    H = W * np.conj(D) / (P + 1e-6 * P.max())
    mag = 20 * np.log10(np.abs(H) + 1e-12)
    out = []
    for p in POINTS:
        m = (f > p / 2 ** (1 / 12)) & (f < p * 2 ** (1 / 12))
        out.append(float(np.median(mag[m])))
    return np.array(out)


def main():
    print("%-28s " % "Hz" + " ".join("%6.0f" % p for p in POINTS))
    for base in sys.argv[1:]:
        r = response(base)
        print("%-28s " % base.split("/")[-1].split("\\\\")[-1] + " ".join("%6.1f" % v for v in r))


if __name__ == "__main__":
    main()
