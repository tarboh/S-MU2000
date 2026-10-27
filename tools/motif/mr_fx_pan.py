"""音色のリバーブ・コーラスのパン（40 01 00 の 0x23・40 02 00 の 0x23）の効き方を実機で測る（2026-10-28）。
  python mr_fx_pan.py 置き場 [--analyze-only]
ボイスモードの雛形（サイン）を 0.3 秒鳴らす。リバーブは直接の送り 127・コーラスへは送らず、離してから 0.4〜1.5 秒の尾の左右の大きさ。
コーラスはコーラスへ 127・リバーブへは送らず、Return 127、鳴っている間の左右（元の音はパン中央なので、wet だけを見るため、元の音の大きさを
Return 0 の組から引く）。パンは 1・32・64・96・127。マルチの編集バッファ（5F 00）を控え、終わったら戻す。
送るのは Data List のパラメーターチェンジ・ダンプの要求と送り返し・CC・ノートだけ（保存しない）。"""
import os
import sys
import time

import numpy as np

import mr_midi
import mr_ref

FS = 44100
PANS = (1, 32, 64, 96, 127)


def takes():
    t = [("rv_p%03d" % p, "rv", p, 127) for p in PANS]
    t += [("ch_p%03d" % p, "ch", p, 127) for p in PANS]
    t += [("ch_dry", "ch", 64, 0)]
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
        r.cc(91, 127)
        r.cc(93, 127)
        for name, which, pan, ret in takes():
            if os.path.exists(os.path.join(d, name + ".wav")):
                continue
            if which == "rv":
                r.param(0x40, 0x00, 0x0B, 127)
                r.param(0x40, 0x00, 0x0C, 0)
                r.param(0x40, 0x01, 0x23, pan)
                r.param(0x40, 0x01, 0x22, 96)
            else:
                r.param(0x40, 0x00, 0x0B, 0)
                r.param(0x40, 0x00, 0x0C, 127)
                r.param(0x40, 0x02, 0x24, 0)
                r.param(0x40, 0x02, 0x23, pan)
                r.param(0x40, 0x02, 0x22, ret)
            time.sleep(0.3)
            r.all_off()
            time.sleep(0.5)
            r.one_note(name, note=60, vel=100, hold=0.3 if which == "rv" else 1.0, tail=2.0, pre=0.3)
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


def lr(p, a, b):
    L, R = mr_ref.read_wav(p)
    L, R = np.asarray(L)[int(a * FS):int(b * FS)], np.asarray(R)[int(a * FS):int(b * FS)]
    return 10 * np.log10((L ** 2).mean() + 1e-30), 10 * np.log10((R ** 2).mean() + 1e-30), L, R


def main():
    a = sys.argv[1:]
    d = a[0]
    os.makedirs(d, exist_ok=True)
    if "--analyze-only" not in a:
        record(d, a)
    print("リバーブの尾（離して 0.4〜1.5 秒）: 左・右（dB）、パン 64 との差")
    l0, r0, _, _ = lr(os.path.join(d, "rv_p064.wav"), 1.0, 2.1)
    for p in PANS:
        l, r, _, _ = lr(os.path.join(d, "rv_p%03d.wav" % p), 1.0, 2.1)
        print("  パン %3d: 左 %+.2f・右 %+.2f" % (p, l - l0, r - r0))
    print("コーラスの wet（鳴っている 0.5〜1.2 秒、Return 0 の組を引いた左右）、パン 64 との差")
    _, _, Ld, Rd = lr(os.path.join(d, "ch_dry.wav"), 0.5, 1.2)
    def wet(p):
        _, _, L, R = lr(p, 0.5, 1.2)
        n = min(len(L), len(Ld))
        return 10 * np.log10(((L[:n] ** 2).mean() - (Ld[:n] ** 2).mean()) + 1e-30), 10 * np.log10(((R[:n] ** 2).mean() - (Rd[:n] ** 2).mean()) + 1e-30)
    wl0, wr0 = wet(os.path.join(d, "ch_p064.wav"))
    for p in PANS:
        wl, wr = wet(os.path.join(d, "ch_p%03d.wav" % p))
        print("  パン %3d: 左 %+.2f・右 %+.2f" % (p, wl - wl0, wr - wr0))


if __name__ == "__main__":
    main()
