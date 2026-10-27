"""マルチのパートの AS1・AS2（CC16・17）の初めの値を実機で見る（2026-10-28）。
  python mr_as_init.py 置き場 [--analyze-only]
FX-MWSweep（PRE5:060。コントロールセットに AS1 → 66・AS2 → 32 深さ 127）をパート 1 にし、CC16・17 を (a) 送らない (b) 64 (c) 0 (d) 127 にして、
鍵 60 の明るさ（重心）の時間変化を比べる。(a) が (b)〜(d) のどれと同じかで、初めの値が分かる。マルチの編集バッファ（5F 00）を控え、終わったら戻す。
送るのは Data List のパラメーターチェンジ・ダンプの要求と送り返し・CC・ノートだけ（保存しない）。"""
import os
import sys
import time

import numpy as np

import mr_midi
import mr_ref

FS = 44100
CASES = [("none", None), ("v64", 64), ("v0", 0), ("v127", 127)]


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
        for pp in range(1, 16):
            r.param(0x37, pp, 0x03, 0x7F)
        for off in (0x60, 0x61, 0x62, 0x63):
            r.param(0x36, 0x00, off, 0)
        for name, v in CASES:
            # 毎回音色を読み直して、CC の値を初めに戻す（パートの音色を替えると、受けた CC は残るかもしれないので、組の順は none を先に）
            r.param(0x37, 0x00, 0x00, 63)
            r.param(0x37, 0x00, 0x01, 4)
            r.param(0x37, 0x00, 0x02, 59)
            r.param(0x37, 0x00, 0x03, 0)
            for off, x in ((0x12, 0), (0x13, 0), (0x15, 127), (0x0D, 100), (0x17, 64), (0x18, 64)):
                r.param(0x37, 0x00, off, x)
            time.sleep(1.5)
            if v is not None:
                r.cc(16, v)
                r.cc(17, v)
            time.sleep(0.2)
            r.all_off()
            time.sleep(0.2)
            r.one_note(name, note=60, vel=100, hold=2.0, tail=0.3, pre=0.2)
            print(name, flush=True)
    finally:
        r.all_off()
        r.cc(16, 64)
        r.cc(17, 64)
        if saved:
            for m in saved:
                r.mo.sysex(bytes(m))
                time.sleep(0.03)
        r.mode(3)
        r.close()
        mi.close()


def cg(p):
    L, R = mr_ref.read_wav(p)
    m = (np.asarray(L) + np.asarray(R)) / 2
    a = np.abs(m)
    on = int(np.argmax(a > a.max() * 0.02))
    m = m[on:]
    out = []
    for t in np.arange(0, 1.8, 0.2):
        y = m[int(t * FS):int(t * FS) + 882]
        Y = np.abs(np.fft.rfft(y * np.hanning(len(y)))) ** 2
        f = np.fft.rfftfreq(len(y), 1 / FS)
        out.append((Y * f).sum() / (Y.sum() + 1e-30))
    return out


def main():
    a = sys.argv[1:]
    d = a[0]
    os.makedirs(d, exist_ok=True)
    if "--analyze-only" not in a:
        record(d, a)
    for name, v in CASES:
        print("%-5s %s" % (name, " ".join("%5.0f" % c for c in cg(os.path.join(d, name + ".wav")))))


if __name__ == "__main__":
    main()
