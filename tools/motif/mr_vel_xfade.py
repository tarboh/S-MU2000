"""エレメントの Velocity Cross Fade（41 ee 00 の 0x10）を実機で測る（2026-10-28）。
  python mr_vel_xfade.py 置き場 [--analyze-only]
ボイスモードの雛形（mr_ref.template、波形 704 のサイン、音量のベロシティ感度 0）で、エレメントのベロシティの範囲を 64〜127（下側）・1〜64（上側）にし、
クロスフェードの値を変えて、範囲の外のベロシティで鳴らした大きさを、範囲の端（64）で鳴らした大きさと比べる。
マルチの編集バッファ（5F 00）を控え、終わったら戻す。送るのは Data List のパラメーターチェンジ・ダンプの要求と送り返し・CC・ノートだけ（保存しない）。"""
import os
import sys
import time

import numpy as np

import mr_midi
import mr_ref

FS = 44100
XF = (0, 4, 8, 15, 32, 48, 64, 127)
LOWV = (64, 62, 60, 56, 48, 40, 32, 16, 1)
HIGHV = (64, 66, 70, 80, 96, 127)


def takes():
    t = []
    for x in XF:
        for v in LOWV:
            t.append(("lo_x%03d_v%03d" % (x, v), 64, 127, x, v))
    for x in (0, 15, 48):
        for v in HIGHV:
            t.append(("hi_x%03d_v%03d" % (x, v), 1, 64, x, v))
    return t


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
        cur = None
        for name, lo, hi, x, v in takes():
            if os.path.exists(os.path.join(d, name + ".wav")):
                continue
            if cur != (lo, hi, x):
                r.el(0, 0x41, 0x0E, lo)
                r.el(0, 0x41, 0x0F, hi)
                r.el(0, 0x41, 0x10, x)
                time.sleep(0.2)
                cur = (lo, hi, x)
            r.all_off()
            time.sleep(0.1)
            r.one_note(name, note=60, vel=v, hold=0.4, tail=0.2, pre=0.2)
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


def level(p):
    L, R = mr_ref.read_wav(p)
    x = (np.asarray(L) ** 2 + np.asarray(R) ** 2) / 2
    a = int(0.3 * FS)
    return 10 * np.log10(x[a:a + int(0.25 * FS)].mean() + 1e-30)


def main():
    a = sys.argv[1:]
    d = a[0]
    os.makedirs(d, exist_ok=True)
    if "--analyze-only" not in a:
        record(d, a)
    lv = {n: level(os.path.join(d, n + ".wav")) for n, *_ in takes() if os.path.exists(os.path.join(d, n + ".wav"))}
    print("下側（範囲 64〜127）: ベロシティ " + " ".join("%5d" % v for v in LOWV))
    for x in XF:
        b = lv["lo_x%03d_v064" % x]
        print("  クロスフェード %3d: " % x + " ".join("%5.1f" % (lv["lo_x%03d_v%03d" % (x, v)] - b) for v in LOWV))
    print("上側（範囲 1〜64）: ベロシティ " + " ".join("%5d" % v for v in HIGHV))
    for x in (0, 15, 48):
        b = lv["hi_x%03d_v064" % x]
        print("  クロスフェード %3d: " % x + " ".join("%5.1f" % (lv["hi_x%03d_v%03d" % (x, v)] - b) for v in HIGHV))


if __name__ == "__main__":
    main()
