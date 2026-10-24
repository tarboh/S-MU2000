"""カットオフのベロシティ感度（43 ee 03、64 が 0）を、大きい感度と弱いベロシティで測る（2026-10-24。Crazy Mod は感度 104 でベロシティ 40 が暗すぎる件）。
  python mr_fvel.py --dir 置き場 [--read]
雛形の音色（ノイズ 705、鍵 60）で、BEF12・カットオフ 160、感度 64・80・96・104・127 × ベロシティ 1・20・40・70・100・127。谷の目盛りの動きを読む。
送るのはパラメーターチェンジとノートだけ（保存しない）。終わったらマルチへ戻す。"""
import os
import sys
import time

import numpy as np

import mr_ref

FS = 44100
SENS = (64, 80, 96, 104, 127)
VELS = (1, 20, 40, 70, 100, 127)


def notch(path, thru):
    def spec(p):
        L, _ = mr_ref.read_wav(p)
        x = np.asarray(L, float)
        on = int(np.argmax(np.abs(x) > 1e-3 * np.max(np.abs(x))))
        x = x[on + int(0.2 * FS):on + int(1.2 * FS)]
        N = 8192
        fr = np.fft.rfftfreq(N, 1 / FS)
        P = np.zeros(len(fr))
        for i in range(0, len(x) - N, N // 2):
            P += np.abs(np.fft.rfft(x[i:i + N] * np.hanning(N))) ** 2
        return fr, P
    fr, P = spec(path)
    _, T = spec(thru)
    r = 10 * np.log10((P + 1e-20) / (T + 1e-20))
    g = np.exp(np.arange(np.log(100), np.log(10000), 0.01))
    rs = []
    for f in g:
        m = (fr > f / 2 ** (1 / 24)) & (fr < f * 2 ** (1 / 24))
        rs.append(r[m].mean() if m.any() else 99.0)
    return g[int(np.argmin(rs))]


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    if "--read" in a:
        base = {}
        for s in SENS:
            row = []
            for v in VELS:
                f = notch(os.path.join(d, "fv%03d_v%03d.wav" % (s, v)), os.path.join(d, "thru_v%03d.wav" % v))
                row.append(f)
            base[s] = row
        ref = base[64]
        for s in SENS:
            steps = [25.6 * np.log2(f / r) for f, r in zip(base[s], ref)]
            model = [0.0285 * abs(s - 64) * (v - 115) for v in VELS]
            print("感度 %3d: " % s + "  ".join("v%d %+6.1f（式 %+6.1f）" % (v, st, m) for v, st, m in zip(VELS, steps, model)))
        return
    os.makedirs(d, exist_ok=True)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(705)
        for off, v in ((0x05, 0), (0x06, 64), (0x22, 64), (0x2F, 64), (0x1D, 64)):
            r.el(0, 0x43, off, v)
        r.el(0, 0x43, 0x07, 0, 0)
        for i in range(4):
            r.el(0, 0x43, 0x27 + 2 * i, 1, 0)
        r.el(0, 0x43, 0x00, 21)
        r.el(0, 0x43, 0x03, 64)
        time.sleep(0.05)
        for v in VELS:
            if not os.path.exists(os.path.join(d, "thru_v%03d.wav" % v)):
                r.one_note("thru_v%03d" % v, note=60, vel=v, hold=1.2, tail=0.2, pre=0.2)
        r.el(0, 0x43, 0x00, 12)
        r.el(0, 0x43, 0x01, 1, 32)                       # 160
        for s in SENS:
            r.el(0, 0x43, 0x03, s)
            time.sleep(0.05)
            for v in VELS:
                name = "fv%03d_v%03d" % (s, v)
                if not os.path.exists(os.path.join(d, name + ".wav")):
                    r.one_note(name, note=60, vel=v, hold=1.2, tail=0.2, pre=0.2)
                print(name, "録った", flush=True)
    finally:
        r.all_off()
        r.mode(3)
        r.close()


if __name__ == "__main__":
    main()
