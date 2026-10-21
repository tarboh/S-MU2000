"""Lo-Fi（0D 00）の線形の部分を読む（録音を読むだけで、実機には何も送らない）。録音は mr_fx.py --block insa --types 13:0 --level 16 で
capture/fxlf/ に置いたもの（lin = 標本化 44.1kHz・語長 127・利き 0dB・Pre-LPF 20kHz・Q 1.0・型 thru）。
  python mr_fx_lofi.py [置き場]
1. Pre-LPF の形: 山のある 2 次（角は表 3、Q = Resonance / 10）を、アナログの式と双一次変換の式で当てはめて比べる
2. Filter Type 1〜5 の応答を、lin との比（複素数）から 4096 点の応答にして fxlf/lofi_type<n>.ir（float32 の並び）に書く"""
import os
import sys

import numpy as np

import mr_fx_eq as E
import mr_fx_fit as F

FS = 44100
T3 = [20, 22, 25, 28, 32, 36, 40, 45, 50, 56, 63, 70, 80, 90, 100, 110, 125, 140, 160, 180, 200, 225, 250, 280, 315, 355, 400, 450,
      500, 560, 630, 700, 800, 900, 1000, 1100, 1200, 1400, 1600, 1800, 2000, 2200, 2500, 2800, 3200, 3600, 4000, 4500, 5000, 5600,
      6300, 7000, 8000, 9000, 10000, 11000, 12000, 14000, 16000, 18000, 20000]


def complex_resp(base, N=1 << 16):
    dL, _ = F.load(base + "_dry.wav")
    wL, _ = F.load(base + "_wet.wav")
    od = int(np.argmax(np.abs(dL) > 1e-5))
    d = dL[od - 200:od + 16384]
    w = wL[od - 200:od + 16384]
    D, W = np.fft.rfft(d, N), np.fft.rfft(w, N)
    P = np.abs(D) ** 2
    return W * np.conj(D) / (P + 1e-6 * P.max()), np.fft.rfftfreq(N, 1 / FS)


def lpf(f, fc, q, bil):
    if bil:
        s = 1j * np.tan(np.pi * np.minimum(f, FS * 0.4999) / FS) / np.tan(np.pi * fc / FS)
    else:
        s = 1j * f / fc
    return 1 / (s * s + s / q + 1)


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "../../roms/motif-rack-es/capture/fxlf"
    b = lambda t: os.path.join(d, "insa_t13_0_" + t)
    P = np.array(E.POINTS)
    lin = E.response(b("lin"))
    for tag, fcv, qv in (("lp40", 40, 10), ("lp40r60", 40, 60), ("lp40r120", 40, 120), ("lp50r120", 50, 120)):
        m = E.response(b(tag)) - lin
        for bil in (False, True):
            md = 20 * np.log10(np.abs(lpf(P, T3[fcv], qv / 10.0, bil)))
            ok = P < 16000
            print("%-9s %s: 差の二乗平均 %.2f dB  最大 %.2f" % (tag, "双一次" if bil else "アナログ", np.sqrt(np.mean((md - m)[ok] ** 2)), np.max(np.abs(md - m)[ok])))
    H0, f = complex_resp(b("lin"))
    for n in range(1, 6):
        H, _ = complex_resp(b("ft%d" % n))
        R = H / np.where(np.abs(H0) > 1e-3, H0, 1)
        # 録音の頭の揺れで応答の頭がずれる（負の側へ回り込むこともある）ので、山を 64 点目に置いてから 4096 点で切る
        h = np.fft.irfft(R)
        h = np.roll(h, 64 - int(np.argmax(np.abs(h))))
        hh = h[:4096].copy()
        hh[4000:] *= np.linspace(1, 0, 96)
        hh.astype("<f4").tofile(os.path.join(d, "lofi_type%d.ir" % n))
        Rm = np.fft.rfft(hh, len(f) * 2 - 2)
        k = [np.argmin(np.abs(f - p)) for p in (63, 250, 1000, 4000, 10000)]
        print("型 %d: 63・250・1k・4k・10kHz で 測った %s / 4096 点 %s dB" % (n, " ".join("%.1f" % (20 * np.log10(abs(R[i]))) for i in k),
                                                                   " ".join("%.1f" % (20 * np.log10(abs(Rm[i]))) for i in k)))


if __name__ == "__main__":
    main()
