"""CC96・97（RPN の Data Increment・Decrement）と CC124・125（オムニ）を実機で測る（2026-10-28）。
  python mr_rpn_incdec.py 置き場 [--analyze-only]
ボイスモードの雛形（サイン）で、RPN を選んでから CC96・97 を送り、高さで効き目を読む。
  pb_inc3: RPN 0（ベンドの幅）を 2 にしてから CC96 を 3 回、ベンドを上いっぱい → 何半音上がるか
  pb_dec1: 2 にしてから CC97 を 1 回
  co_inc3: RPN 2（Coarse）を 64 にしてから CC96 を 3 回 → 何半音か
  fi_inc8: RPN 1（Fine）を 64・0 にしてから CC96 を 8 回 → 何セントか
  om124・om125: 鍵 60 を押さえたまま CC124・125 を送り、音が止まるか
マルチの編集バッファ（5F 00）を控え、終わったら戻す。送るのは Data List のパラメーターチェンジ・ダンプの要求と送り返し・CC・ピッチベンド・ノートだけ（保存しない）。"""
import os
import sys
import time

import numpy as np

import mr_midi
import mr_ref

FS = 44100


def rpn(r, num, msb, lsb=0):
    for c, v in ((101, 0), (100, num), (6, msb), (38, lsb)):
        r.cc(c, v)


def null(r):
    r.cc(101, 127)
    r.cc(100, 127)


def reset(r):
    rpn(r, 0, 2)
    rpn(r, 1, 64)
    rpn(r, 2, 64)
    null(r)
    r.mo.short(0xE0, 0, 64)


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
        r.param(0x40, 0x00, 0x05, 64 + 2)        # 音色のベンドの幅 +2（RPN で上書きする）
        r.param(0x40, 0x00, 0x06, 64 - 2)
        reset(r)
        time.sleep(0.2)
        r.one_note("ref", note=60, vel=100, hold=0.5, tail=0.1, pre=0.1)
        for name, num, start, cc, times, bend in (("pb_inc3", 0, 2, 96, 3, True), ("pb_dec1", 0, 2, 97, 1, True), ("co_inc3", 2, 64, 96, 3, False),
                                                  ("fi_inc8", 1, 64, 96, 8, False), ("fi_dec8", 1, 64, 97, 8, False)):
            reset(r)
            rpn(r, num, start)
            for _ in range(times):
                r.cc(cc, 0)
                time.sleep(0.02)
            null(r)
            if bend:
                r.mo.short(0xE0, 127, 127)
            time.sleep(0.1)
            r.one_note(name, note=60, vel=100, hold=0.5, tail=0.1, pre=0.1)
        reset(r)
        for name, cc in (("om124", 124), ("om125", 125), ("om123", 123)):
            r.all_off()
            time.sleep(0.1)
            r.record(name, 1.2, [(0.1, lambda: r.note_on(60, 100)), (0.5, lambda cc=cc: r.cc(cc, 0)), (1.0, lambda: r.note_off(60))])
        print("録った", flush=True)
    finally:
        r.all_off()
        reset(r)
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
    ref = f0(os.path.join(d, "ref.wav"))
    for n in ("pb_inc3", "pb_dec1", "co_inc3", "fi_inc8", "fi_dec8"):
        print("%-8s %+8.1f セント" % (n, 1200 * np.log2(f0(os.path.join(d, n + ".wav")) / ref)))
    for n in ("om123", "om124", "om125"):
        L, R = mr_ref.read_wav(os.path.join(d, n + ".wav"))
        x = (np.asarray(L) ** 2 + np.asarray(R) ** 2) / 2
        lv = 10 * np.log10(x[int(0.7 * FS):int(0.95 * FS)].mean() + 1e-30)
        print("%-8s 送ったあと（離す前）の大きさ %.1fdB" % (n, lv))


if __name__ == "__main__":
    main()
