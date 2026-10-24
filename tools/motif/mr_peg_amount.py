"""PEG の効く量（高さ × 深さ → セント）を測り直す（2026-10-24。Ringvoxx のエレメント 1 は高さ +102・深さ 84 で、
実機はちょうど +12 半音、前の式 0.5282 × 高さ × (深さ − 64) では +10.8 半音）。
  python mr_peg_amount.py --dir 置き場 [--read]
雛形の音色（正弦波 704、鍵 60）で、PEG の高さを全部の段で L、時間を全部 0 にして、L × 深さを 1.2 秒鳴らし、周波数を読む。
送るのはパラメーターチェンジとノートだけ（保存しない）。終わったらマルチへ戻す。"""
import os
import sys
import time

import numpy as np

import mr_ref

FS = 44100
TAKES = [(L, d) for d in (68, 74, 84, 96, 127) for L in (16, 32, 64, 102, 127, -64, -102)]


def freq(path):
    L, _ = mr_ref.read_wav(path)
    x = np.asarray(L, dtype=np.float64)
    on = int(np.argmax(np.abs(x) > 0.01 * np.max(np.abs(x))))
    x = x[on + int(0.2 * FS):on + int(1.1 * FS)]
    N = len(x)
    X = np.fft.fft(x)
    X[N // 2 + 1:] = 0
    X[1:N // 2] *= 2
    ph = np.unwrap(np.angle(np.fft.ifft(X)))
    return (ph[-1] - ph[0]) / (2 * np.pi * (N - 1) / FS)


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    if "--read" in a:
        f0 = freq(os.path.join(d, "peg_ref.wav"))
        for L, dp in TAKES:
            f = freq(os.path.join(d, "peg_L%+04d_d%03d.wav" % (L, dp)))
            c = 1200 * np.log2(f / f0)
            dd = min(dp - 64, 64) if dp < 127 else 64
            LL = 128 if L >= 127 else L
            print("高さ %+4d 深さ %3d: %+8.1f セント（前の式 %+8.1f、1 あたり %.4f）" % (L, dp, c, 0.5282 * LL * dd, c / (LL * dd)))
        return
    os.makedirs(d, exist_ok=True)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)

    def lv(off, v):
        x = 128 + v
        r.el(0, 0x42, off, x >> 7, x & 0x7F)
    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(704)
        r.el(0, 0x43, 0x00, 21)
        for off in range(0x05, 0x0A):
            r.el(0, 0x42, off, 0)
        for off in (0x15, 0x16, 0x17, 0x1B):
            r.el(0, 0x42, off, 64 if off != 0x15 else 4)
        r.el(0, 0x42, 0x17, 64)                      # 高さのベロシティ感度 0
        time.sleep(0.05)
        if not os.path.exists(os.path.join(d, "peg_ref.wav")):
            r.one_note("peg_ref", note=60, vel=100, hold=1.2, tail=0.2, pre=0.2)
        for L, dp in TAKES:
            name = "peg_L%+04d_d%03d" % (L, dp)
            if os.path.exists(os.path.join(d, name + ".wav")):
                continue
            for i in range(5):
                lv(0x0A + 2 * i, L)
            r.el(0, 0x42, 0x14, dp)
            time.sleep(0.05)
            r.one_note(name, note=60, vel=100, hold=1.2, tail=0.2, pre=0.2)
            print(name, "録った", flush=True)
    finally:
        r.el(0, 0x42, 0x14, 64)
        r.all_off()
        r.mode(3)
        r.close()


if __name__ == "__main__":
    main()
