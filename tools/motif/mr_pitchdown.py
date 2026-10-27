"""音程を下げて鳴らしたときの高い側（補間の像）を実機で測る（2026-10-28）。
  python mr_pitchdown.py 置き場 [--analyze-only]
Hard Kit（PREDR:035）の鍵 26（SdGate1）と鍵 33（BdDistRM）を、鍵のインサーションを切って、RPN の Coarse Tune を 0・−6・−12・−18・−24 にして鳴らす。
読み出しの速さ fr の倍数に谷が並ぶなら、谷は Coarse に付いて 1 オクターブで半分に動く。
ボイスモードで鳴らし、終わったら RPN を 0 に戻してマルチ（3）にし、控えたマルチの編集バッファ（5F 00）を送り返す。
送るのは Data List のパラメーターチェンジ・ダンプの要求と送り返し・CC（RPN）・ノートだけ（保存しない）。録った音は配らないこと。"""
import os
import sys
import time

import numpy as np

import mr_midi
import mr_ref

FS = 44100
KEYS = (26, 33)
COARSE = (0, -6, -12, -18, -24)


def rpn(r, num, msb):
    for c, v in ((101, 0), (100, num), (6, msb), (38, 0), (101, 127), (100, 127)):
        r.cc(c, v)


def record(d, a):
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
    mi = mr_midi.MidiIn(mr_ref.opt(a, "--midi-in", "MOTIF-RACK ES-1"))
    saved = None
    try:
        r.all_off()
        r.param(0x00, 0x00, 0x34, 0)
        r.mode(3)
        saved = mr_midi.request_block(mi, r.mo, 0, 0x5F, 0x00)
        if not saved:
            raise SystemExit("マルチの編集バッファを控えられない（ダンプが返らない）ので止める")
        open(os.path.join(d, "saved_5F.syx"), "wb").write(b"".join(bytes(m) for m in saved))
        r.mode(0)
        r.program(63, 32, 34)
        r.param(0x46, 0x70, 0x16, 0)
        for k in KEYS:
            r.param(0x47, k - 24, 0x11, 0)
        for c, v in ((7, 127), (11, 127), (10, 64), (91, 0), (93, 0), (74, 64), (71, 64)):
            r.cc(c, v)
        rpn(r, 1, 64)
        time.sleep(0.5)
        for ct in COARSE:
            rpn(r, 2, 64 + ct)
            time.sleep(0.2)
            for k in KEYS:
                name = "k%03d_c%+03d" % (k, ct)
                if os.path.exists(os.path.join(d, name + ".wav")):
                    continue
                r.all_off()
                time.sleep(0.15)
                r.one_note(name, note=k, vel=100, hold=1.2, tail=0.4, pre=0.3)
                print(name, flush=True)
    finally:
        r.all_off()
        rpn(r, 2, 64)
        rpn(r, 1, 64)
        r.mode(3)
        for m in saved or []:
            r.mo.sysex(bytes(m))
            time.sleep(0.03)
        r.close()
        mi.close()


def spec(p, n=FS // 2):
    L, R = mr_ref.read_wav(p)
    x = (np.asarray(L) + np.asarray(R)) / 2
    on = int(np.argmax(np.abs(x) > np.abs(x).max() * 0.02))
    y = x[on:on + n]
    X = np.abs(np.fft.rfft(y * np.hanning(len(y)), FS)) ** 2
    return np.convolve(X, np.ones(60) / 60, "same")


def main():
    a = sys.argv[1:]
    d = a[0]
    os.makedirs(d, exist_ok=True)
    if "--analyze-only" not in a:
        record(d, a)
    f = np.arange(FS // 2 + 1)
    for k in KEYS:
        print("鍵 %d（500Hz ごとの平均、いちばん大きいところから dB）" % k)
        print("       " + " ".join("%4d" % (lo // 100) for lo in range(1000, 22000, 500)))
        for ct in COARSE:
            X = spec(os.path.join(d, "k%03d_c%+03d.wav" % (k, ct)))
            ref = 10 * np.log10(X.max())
            row = [10 * np.log10(X[lo:lo + 500].mean() + 1e-30) - ref for lo in range(1000, 22000, 500)]
            sel = (f > 2500) & (f < 21000)
            mins = []
            for lo in range(2500, 21000, 250):
                pass
            print("  %+3d %s" % (ct, " ".join("%4.0f" % v for v in row)))


if __name__ == "__main__":
    main()
