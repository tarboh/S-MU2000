"""ユニバーサルのマスターボリューム（F0 7F 7F 04 01 ll mm F7）の効き方を実機で測る（2026-10-28）。
  python mr_master_vol.py 置き場 [--analyze-only]
初めにシステムのマスターボリューム（00 00 00）をパラメーター要求で読み、マルチの編集バッファ（5F 00）を控える。マルチのまま、ふつうに
鳴らしたときの大きさを物差しに、mm を 127・112・100・80・64・32・16 にして同じ音を鳴らす。終わったらマスターボリュームを読んだ値に書き戻し、
編集バッファも戻す。送るのはユニバーサルのマスターボリューム・パラメーター要求とパラメーターチェンジ・ダンプの要求と送り返し・ノートだけ（保存しない）。"""
import os
import sys
import time

import numpy as np

import mr_midi
import mr_ref

FS = 44100
MM = (127, 112, 100, 80, 64, 32, 16)


def read_param(mi, mo, ah, am, al, timeout=2.0):
    mi.poll()
    mo.sysex(bytes([0xF0, 0x43, 0x30, 0x7F, 0x00, ah, am, al, 0xF7]))
    t0 = time.time()
    while time.time() - t0 < timeout:
        for m in mi.poll():
            if len(m) >= 10 and m[1] == 0x43 and m[3:5] == b"\x7f\x00" and tuple(m[5:8]) == (ah, am, al):
                return m[8]
        time.sleep(0.005)
    return None


def record(d, a):
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
    mi = mr_midi.MidiIn(mr_ref.opt(a, "--midi-in", "MOTIF-RACK ES-1"))
    saved = None
    mv = None
    try:
        r.all_off()
        r.param(0x00, 0x00, 0x34, 0)
        r.mode(3)
        mv = read_param(mi, r.mo, 0x00, 0x00, 0x00)
        if mv is None:
            raise SystemExit("システムのマスターボリュームを読めないので止める")
        print("システムのマスターボリューム（00 00 00）:", mv, flush=True)
        open(os.path.join(d, "system_master_volume.txt"), "w").write("%d\n" % mv)
        saved = mr_midi.request_block(mi, r.mo, 0, 0x5F, 0x00)
        if not saved:
            raise SystemExit("マルチの編集バッファを控えられない（ダンプが返らない）ので止める")
        open(os.path.join(d, "saved_5F.syx"), "wb").write(b"".join(bytes(m) for m in saved))
        for mm in MM:
            r.mo.sysex(bytes([0xF0, 0x7F, 0x7F, 0x04, 0x01, 0x00, mm, 0xF7]))
            time.sleep(0.2)
            after = read_param(mi, r.mo, 0x00, 0x00, 0x00)
            r.all_off()
            time.sleep(0.1)
            r.one_note("mv%03d" % mm, note=60, vel=100, hold=0.8, tail=0.2, pre=0.2)
            print("mm %d を送ったあとの 00 00 00: %s" % (mm, after), flush=True)
    finally:
        r.all_off()
        if mv is not None:
            r.param(0x00, 0x00, 0x00, mv)
            time.sleep(0.1)
            print("書き戻したあと:", read_param(mi, r.mo, 0x00, 0x00, 0x00), flush=True)
        if saved:
            for m in saved:
                r.mo.sysex(bytes(m))
                time.sleep(0.03)
        r.mode(3)
        r.close()
        mi.close()


def level(p):
    L, R = mr_ref.read_wav(p)
    x = (np.asarray(L) ** 2 + np.asarray(R) ** 2) / 2
    a = np.abs(np.asarray(L))
    on = int(np.argmax(a > a.max() * 0.05))
    return 10 * np.log10(x[on + int(0.1 * FS):on + int(0.6 * FS)].mean() + 1e-30)


def main():
    a = sys.argv[1:]
    d = a[0]
    os.makedirs(d, exist_ok=True)
    if "--analyze-only" not in a:
        record(d, a)
    b = level(os.path.join(d, "mv127.wav"))
    for mm in MM:
        x = level(os.path.join(d, "mv%03d.wav" % mm)) - b
        print("mm %3d: %+6.2fdB（40·log10(mm/127) なら %+6.2f、20·log10 なら %+6.2f）" % (mm, x, 40 * np.log10(mm / 127), 20 * np.log10(mm / 127)))


if __name__ == "__main__":
    main()
