"""音の高さの下限を実機で測る（2026-10-27）。
  python mr_pitch_floor.py 置き場 [--waves 1088,1023] [--keys 60,52,48,44,40,36,32,28,24,16,8,0] [--coarse 0] [--analyze-only]
ボイスモードの雛形（mr_ref.template）に波形を入れ、鍵を下げながら鳴らす。下限に当たると、鍵を下げても音が長くならず暗くならない。
--coarse はエレメントのコースチューン（−48〜+48）。マルチの編集バッファ（5F 00）を控えて、終わったら戻す。
送るのは Data List のパラメーターチェンジ・ダンプの要求と送り返し・バンクとプログラムの選び・CC・ノートだけ（保存しない）。録った音は配らないこと。"""
import os
import sys
import time

import numpy as np

import mr_midi
import mr_ref

FS = 44100


def feats(p):
    L, R = mr_ref.read_wav(p)
    m = (np.asarray(L) + np.asarray(R)) / 2
    a = np.abs(m)
    on = int(np.argmax(a > a.max() * 0.02))
    n = 441
    e = np.array([np.sqrt(np.mean(m[i:i + n] ** 2)) for i in range(on, len(m) - n, n)])
    e = 20 * np.log10(e / e.max() + 1e-9)
    end = np.nonzero(e > -40)[0][-1] * 10
    y = m[on:on + int(0.3 * FS)]
    X = np.abs(np.fft.rfft(y)) ** 2
    f = np.fft.rfftfreq(len(y), 1 / FS)
    return end, (X * f).sum() / X.sum()


def main():
    a = sys.argv[1:]
    d = a[0]
    waves = [int(x) for x in mr_ref.opt(a, "--waves", "1088,1023").split(",")]
    keys = [int(x) for x in mr_ref.opt(a, "--keys", "60,52,48,44,40,36,32,28,24,16,8,0").split(",")]
    coarse = int(mr_ref.opt(a, "--coarse", "0"))
    os.makedirs(d, exist_ok=True)
    if "--analyze-only" not in a:
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
            for w in waves:
                r.template(w)
                r.el(0, 0x42, 0x00, 0x40 + coarse)
                time.sleep(0.2)
                for k in keys:
                    nm = "w%04d_c%+d_k%03d" % (w, coarse, k)
                    if os.path.exists(os.path.join(d, nm + ".wav")):
                        continue
                    r.all_off()
                    time.sleep(0.1)
                    r.one_note(nm, note=k, vel=100, hold=2.0, tail=0.3, pre=0.3)
                print("録った:", w, flush=True)
        finally:
            r.all_off()
            r.mode(3)
            if saved:
                for m in saved:
                    r.mo.sysex(bytes(m))
                    time.sleep(0.03)
            r.close()
            mi.close()
    for w in waves:
        row = []
        for k in keys:
            p = os.path.join(d, "w%04d_c%+d_k%03d.wav" % (w, coarse, k))
            if os.path.exists(p):
                end, cg = feats(p)
                row.append("鍵%d %dms %.0fHz" % (k, end, cg))
        print("波形 %d（コース %+d）: %s" % (w, coarse, "・".join(row)))


if __name__ == "__main__":
    main()
