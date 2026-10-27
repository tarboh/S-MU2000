"""エレメントの音程のキーフォロー（42 ee 00 の 0x19）を細かく実機で測る（2026-10-28）。
  python mr_pitch_kf.py 置き場 [--analyze-only]
前の測り（値 48・80・96・112・127 の 5 点、doc/motif-rack-es-capture.md）は間を直線でつないでいたが、90 で実機は 1 鍵 90 セント、芯は 77.5 セントだった。
ボイスモードの雛形（mr_ref.template、波形 704 のサイン、中心の鍵 60）で、値ごとに鍵 48 と 72 を鳴らし、1 鍵あたりのセントを読む。
マルチの編集バッファ（5F 00）を控え、終わったら戻す。送るのは Data List のパラメーターチェンジ・ダンプの要求と送り返し・CC・ノートだけ（保存しない）。"""
import os
import sys
import time

import numpy as np

import mr_midi
import mr_ref

FS = 44100
VALS = sorted(set(list(range(64, 128, 2)) + [127] + list(range(0, 64, 4)) + [62, 63]))


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
        r.program(63, 0, 0)
        r.template(704)
        r.el(0, 0x42, 0x1A, 60)                  # 音程のキーフォローの中心の鍵 60
        for v in VALS:
            r.el(0, 0x42, 0x19, v)
            time.sleep(0.15)
            for k in (48, 72):
                nm = "kf%03d_k%03d" % (v, k)
                if os.path.exists(os.path.join(d, nm + ".wav")):
                    continue
                r.all_off()
                time.sleep(0.05)
                r.one_note(nm, note=k, vel=100, hold=0.5, tail=0.1, pre=0.1)
        print("録った", flush=True)
    finally:
        r.all_off()
        r.mode(3)
        if saved:
            for m in saved:
                r.mo.sysex(bytes(m))
                time.sleep(0.03)
        r.close()
        mi.close()


def f0(p):
    L, R = mr_ref.read_wav(p)
    m = (np.asarray(L) + np.asarray(R)) / 2
    a = np.abs(m)
    on = int(np.argmax(a > a.max() * 0.1))
    y = m[on + int(0.08 * FS):on + int(0.45 * FS)]
    n = 1 << 18
    Y = np.abs(np.fft.rfft(y * np.hanning(len(y)), n))
    f = np.fft.rfftfreq(n, 1 / FS)
    Y[f < 10] = 0
    i = int(np.argmax(Y))
    al, be, ga = np.log(Y[i - 1] + 1e-30), np.log(Y[i] + 1e-30), np.log(Y[i + 1] + 1e-30)
    return (i + 0.5 * (al - ga) / (al - 2 * be + ga)) * FS / n


def main():
    a = sys.argv[1:]
    d = a[0]
    os.makedirs(d, exist_ok=True)
    if "--analyze-only" not in a:
        record(d, a)
    out = []
    for v in VALS:
        lo, hi = os.path.join(d, "kf%03d_k048.wav" % v), os.path.join(d, "kf%03d_k072.wav" % v)
        if os.path.exists(lo) and os.path.exists(hi):
            c = 1200 * np.log2(f0(hi) / f0(lo)) / 24
            out.append("%3d %+7.2f" % (v, c))
    print("値 1 鍵あたりのセント")
    print("\n".join(out))
    open(os.path.join(d, "pitch_kf.txt"), "w", encoding="utf-8").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
