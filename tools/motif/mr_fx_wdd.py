"""Wah+Dist+Dly（0C 40）の歪みの部分を .dsx に書く（2026-10-27。録音は capture/fxwdd、実機には何も送らない）。
  python mr_fx_wdd.py 組の名前（s0 など。capture/fxwdd/<名前>.dsx は mr_fx_dist2.py --out で当てはめたもの） 組のパラメーター 1〜16（, で区切る）
Wah Sensitive 0 で録った組の h1 には、止まったワウ（Wah Cutoff の山、Q = Resonance / 10 の帯域通過）が入っている。芯はワウを頭打ちの前で動かすので、
h1 をその帯域通過で割った「ワウの無い前の応答」にして、capture/fxds/t12_64_<p1〜p16>.dsx に書く（Delay mix p3・Dry/Wet p10・Wah Sensitive p11 は
芯で掛けるので、名前では 0・127・0 にそろえる）。帯域通過は芯（sampler_fx.h の touch_wah）と同じ TPT の SVF にインパルスを通して作る。"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CAP = os.path.join(HERE, "../../roms/motif-rack-es/capture")
FS = 44100
# Wah Cutoff（p12）→ 山の周波数（Hz）。感度 0・Drive 0 のノイズで、p12 を 8 おきに録って当てはめた（Q 3 の帯域通過で、外れは 0.1〜0.4dB、
# 104 から上は 0.3〜1.2dB）。sampler_fx.h の WDD_FC と同じ
WDD_P = [0, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120, 127]
WDD_FC = [12.1, 29.1, 59.6, 109.6, 186.3, 297.9, 453.9, 666.9, 948.9, 1314.9, 1785.5, 2380.7, 3127.3, 4058.4, 5214.8, 6673.6, 8256.2]


def bpf_ir(p12, q, n):
    fc = np.exp(np.interp(p12, WDD_P, np.log(WDD_FC)))
    g = np.tan(np.pi * min(fc, 20000) / FS)
    k = 1 / q
    a1 = 1 / (1 + g * (g + k))
    ic1 = ic2 = 0.0
    y = np.zeros(n)
    for i in range(n):
        v = 1.0 if i == 0 else 0.0
        v3 = v - ic2
        v1 = a1 * ic1 + g * a1 * v3
        v2 = ic2 + g * v1
        ic1 = 2 * v1 - ic1
        ic2 = 2 * v2 - ic2
        y[i] = k * v1
    return y


def main():
    name = sys.argv[1]
    ps = [int(v) for v in sys.argv[2].split(",")]
    src = os.path.join(CAP, "fxwdd", name + ".dsx")
    with open(src, "rb") as fo:
        head = fo.readline().decode()
        hd = dict(x.split("=") for x in head.split()[1:])
        n = int(hd["frames"])
        h = np.frombuffer(fo.read(n * 8), "<f4").reshape(-1, 2).astype(float)
        post = np.frombuffer(fo.read(int(hd.get("post", 0)) * 4), "<f4")
    N = 8192
    f = np.fft.rfftfreq(N, 1 / FS)
    W0 = np.fft.rfft(bpf_ir(ps[11], ps[12] / 10.0, N))
    W0[f < 25] = W0[np.argmin(np.abs(f - 25))]        # 25Hz より下は割り戻さない（帯域通過は直流で 0）
    pre = [np.fft.irfft(np.fft.rfft(h[:, c], N) / W0, N)[:4096] for c in range(2)]
    q = list(ps)
    q[2], q[9], q[10] = 0, 127, 0
    key = "t12_64_" + "_".join(str(v) for v in q)
    out = os.path.join(CAP, "fxds", key + ".dsx")
    head = "DSX frames=%d c=%s p=%s fc=20000.0 drive=0 post=%d b=%s\n" % (4096, hd["c"], hd["p"], len(post), hd.get("b", "0"))
    with open(out, "wb") as fo:
        fo.write(head.encode("ascii"))
        fo.write(np.stack(pre, axis=1).astype("<f4").tobytes())
        fo.write(post.astype("<f4").tobytes())
    print("書いた", out)


if __name__ == "__main__":
    main()
