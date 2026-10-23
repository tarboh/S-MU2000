"""エレメントのファインチューン（42 ee 01、64 が 0）の目盛りを測る（2026-10-24。Nice Bell の ±5 で、実機の音程の差が芯の半分だった件）。
  python mr_fine.py --dir 置き場 [--wave 704] [--keys 69,48] [--dense]（--dense は −20〜+20 を 1 つずつ）
雛形の音色（正弦波）で、ファイン −64・−32・−16・−8・−5・−2・−1・0・+1・+2・+5・+8・+16・+32・+63 を、鍵 69 と 48 で 1.5 秒鳴らす。
読むのは mr_fine.py --read 置き場（瞬時の周波数の平均、0 との差のセント）。送るのはパラメーターチェンジとノートだけ（保存しない）。"""
import os
import sys
import time

import numpy as np

import mr_ref

FS = 44100
FINES = (-64, -32, -16, -8, -5, -2, -1, 0, 1, 2, 5, 8, 16, 32, 63)
DENSE = tuple(range(-20, 21))
KEYS = (69, 48)


def opts(a):
    global FINES
    if "--dense" in a:
        a.remove("--dense")
        FINES = DENSE
    w = int(mr_ref.opt(a, "--wave", "704"))
    keys = tuple(int(k) for k in mr_ref.opt(a, "--keys", "69,48").split(","))
    return w, keys, "" if w == 704 else "w%04d_" % w


def freq(path):
    L, _ = mr_ref.read_wav(path)
    x = np.asarray(L, dtype=np.float64)
    on = int(np.argmax(np.abs(x) > 0.01 * np.max(np.abs(x))))
    x = x[on + int(0.2 * FS):on + int(1.4 * FS)]
    N = len(x)
    X = np.fft.fft(x)
    X[N // 2 + 1:] = 0
    X[1:N // 2] *= 2
    ph = np.unwrap(np.angle(np.fft.ifft(X)))
    return (ph[-1] - ph[0]) / (2 * np.pi * (N - 1) / FS)


def main():
    a = sys.argv[1:]
    w, keys, pre = opts(a)
    if "--read" in a:
        d = a[a.index("--read") + 1]
        for k in keys:
            f0 = freq(os.path.join(d, pre + "fine_k%03d_p000.wav" % k))
            print("鍵 %d（0 で %.4f Hz）" % (k, f0))
            for f in FINES:
                hz = freq(os.path.join(d, pre + "fine_k%03d_%s%03d.wav" % (k, "m" if f < 0 else "p", abs(f))))
                c = 1200 * np.log2(hz / f0)
                print("  %+4d: %+8.3f セント（1 あたり %s）" % (f, c, "%.4f" % (c / f) if f else "-"))
        return
    d = mr_ref.opt(a, "--dir", ".")
    os.makedirs(d, exist_ok=True)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"),
                   mr_ref.opt(a, "--dev", "0"), d)
    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(w)
        r.el(0, 0x43, 0x00, 21)
        for f in FINES:
            r.el(0, 0x42, 0x01, 64 + f if f < 64 else 127)
            time.sleep(0.05)
            for k in keys:
                name = pre + "fine_k%03d_%s%03d" % (k, "m" if f < 0 else "p", abs(f))
                if not os.path.exists(os.path.join(d, name + ".wav")):
                    r.one_note(name, note=k, vel=100, hold=1.5, tail=0.2, pre=0.2)
                print(name, "録った", flush=True)
    finally:
        r.el(0, 0x42, 0x01, 64)
        r.all_off()
        r.mode(3)
        r.close()


if __name__ == "__main__":
    main()
