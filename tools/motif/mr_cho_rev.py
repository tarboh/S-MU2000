"""コーラスからリバーブへの送り（40 02 00 の 0x24 Send Chorus To Reverb）を実機で測る（2026-10-28）。
  python mr_cho_rev.py 置き場 [--analyze-only]
ボイスモードの雛形（mr_ref.template、波形 704 のサイン）を 0.3 秒鳴らし、離してから 0.4〜1.5 秒（リバーブの尾だけ）の大きさを見る。
  rv<R>: リバーブへ直接 R（0x0B）で送る（コーラスへは送らない）。物差し
  cr<S>_ret<T>: コーラスへ 127 で送り（0x0C）、コーラスの Return を T、コーラス → リバーブを S にする（リバーブへは直接送らない）
マルチの編集バッファ（5F 00）を控え、終わったら戻す。送るのは Data List のパラメーターチェンジ・ダンプの要求と送り返し・CC・ノートだけ（保存しない）。"""
import os
import sys
import time

import numpy as np

import mr_midi
import mr_ref

FS = 44100


def takes():
    t = [("rv%03d" % r, r, 0, 96, 0) for r in (16, 32, 64, 127)]
    t += [("cr%03d_ret096" % s, 0, 127, 96, s) for s in (0, 16, 32, 64, 96, 127)]
    t += [("cr064_ret%03d" % ret, 0, 127, ret, 64) for ret in (32, 127)]
    t += [("cr127_cs064", 0, 64, 96, 127)]
    if FINE:
        t += [("cr%03d_ret096" % s, 0, 127, 96, s) for s in list(range(4, 128, 4)) + [127] if s not in (16, 32, 64, 96)]
        t += [("cr127_ret%03d" % ret, 0, 127, ret, 127) for ret in (0, 16, 32, 48, 64, 80, 112, 127)]
    return t


FINE = "--fine" in sys.argv


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
        r.el(0, 0x41, 0x26, 64)                   # ディケイ 2 を 64（離さなくても 0.3 秒で消える形にはしない。離して切る）
        r.cc(91, 127)
        r.cc(93, 127)
        for name, rs, cs, ret, s2r in takes():
            if os.path.exists(os.path.join(d, name + ".wav")):
                continue
            r.param(0x40, 0x00, 0x0B, rs)
            r.param(0x40, 0x00, 0x0C, cs)
            r.param(0x40, 0x02, 0x22, ret)
            r.param(0x40, 0x02, 0x24, s2r)
            time.sleep(0.3)
            r.all_off()
            time.sleep(0.5)
            r.one_note(name, note=60, vel=100, hold=0.3, tail=2.5, pre=0.3)
            print(name, flush=True)
    finally:
        r.all_off()
        r.mode(3)
        if saved:
            for m in saved:
                r.mo.sysex(bytes(m))
                time.sleep(0.03)
        r.close()
        mi.close()


def tail_db(p):
    L, R = mr_ref.read_wav(p)
    x = (np.asarray(L) ** 2 + np.asarray(R) ** 2) / 2
    a = int((0.3 + 0.3 + 0.4) * FS)
    return 10 * np.log10(x[a:a + int(1.1 * FS)].mean() + 1e-30)


def main():
    a = sys.argv[1:]
    d = a[0]
    os.makedirs(d, exist_ok=True)
    if "--analyze-only" not in a:
        record(d, a)
    ref = {}
    for name, rs, cs, ret, s2r in takes():
        p = os.path.join(d, name + ".wav")
        if os.path.exists(p):
            ref[name] = tail_db(p)
    for k, v in ref.items():
        print("%-14s 尾 %.1fdB" % (k, v))


if __name__ == "__main__":
    main()
