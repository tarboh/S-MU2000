"""サンプルを読む補間の形を、正弦波の雛形（波形 704）の像の線で実機から測る（2026-10-28）。
  python mr_interp.py 置き場 [--analyze-only]
ボイスモードの雛形（mr_ref.template）で鍵 12〜84 を 1.5 秒鳴らす。読み出しの速さを fr とすると、正弦波 f0 の像は k·fr ± f0 に線として出る
（44.1kHz を超えた像は折り返す）。線の位置から fr を、基音との大きさの比から補間の周波数特性 H(f) を読む（直線補間なら sinc²(f/fr)）。
マルチの編集バッファ（5F 00）を控え、終わったらマルチ（3）に戻して送り返す。
送るのは Data List のパラメーターチェンジ・ダンプの要求と送り返し・CC・ノートだけ（保存しない）。"""
import os
import sys
import time

import numpy as np

import mr_midi
import mr_ref

FS = 44100
KEYS = (12, 24, 36, 48, 60, 72, 84)


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
        for c, v in ((7, 127), (11, 127), (10, 64), (91, 0), (93, 0)):
            r.cc(c, v)
        time.sleep(0.3)
        for k in KEYS:
            name = "sin_k%03d" % k
            if os.path.exists(os.path.join(d, name + ".wav")):
                continue
            r.all_off()
            time.sleep(0.15)
            r.one_note(name, note=k, vel=100, hold=1.5, tail=0.2, pre=0.2)
            print(name, flush=True)
    finally:
        r.all_off()
        r.mode(3)
        for m in saved or []:
            r.mo.sysex(bytes(m))
            time.sleep(0.03)
        r.close()
        mi.close()


def lines(p, top=12):
    L, R = mr_ref.read_wav(p)
    x = (np.asarray(L) + np.asarray(R)) / 2
    on = int(np.argmax(np.abs(x) > np.abs(x).max() * 0.1))
    y = x[on + 4410:on + 4410 + FS]
    X = np.abs(np.fft.rfft(y * np.blackman(len(y))))
    S = 20 * np.log10(X + 1e-30)
    pk = []
    for i in range(3, len(S) - 3):
        if S[i] == S[i - 3:i + 4].max() and S[i] > S.max() - 110:
            pk.append((S[i], i))
    pk.sort(reverse=True)
    return [(f, s - pk[0][0]) for s, f in sorted(pk[:top], key=lambda t: t[1])]


def main():
    a = sys.argv[1:]
    d = a[0]
    os.makedirs(d, exist_ok=True)
    if "--analyze-only" not in a:
        record(d, a)
    for k in KEYS:
        print("鍵 %d: %s" % (k, "  ".join("%dHz %.0fdB" % (f, s) for f, s in lines(os.path.join(d, "sin_k%03d.wav" % k)))))


if __name__ == "__main__":
    main()
