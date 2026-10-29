"""オルガンの音色（Dual Rotary Speaker の Speed Control を MW で動かすもの）で、MW の値ごとにホーンの回る速さを測る（つまみの目盛りを見る）。
  python mr_fx_rot_mw.py [--voices 00_114,00_122,…] [--analyze]
音色を選び（バンク MSB 63・LSB mm、プログラム nn）、鍵 60 を押さえたまま MW（CC1）を 0・2・4・8・16・32・64・127 と替え、替えてから 5 秒待って 2 秒録る。
速さは、2〜8kHz の帯の大きさ（10ms ごと）の揺れの周波数（0.4〜12Hz で山）。録音は capture/rotmw/<音色>_mw<値>.wav。
送るのはプログラムチェンジ・ノート・CC だけ（保存しない）。録った音は配らないこと。"""
import os
import sys
import time

import numpy as np

import mr_ref

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "../../roms/motif-rack-es/capture/rotmw")
MWS = (0, 2, 4, 8, 16, 32, 64, 127)
FS = 44100


def rate_of(path):
    L, R = mr_ref.read_wav(path)
    x = (np.asarray(L, dtype=np.float64) + np.asarray(R, dtype=np.float64)) / 2
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / FS)
    X[(f < 2000) | (f > 8000)] = 0
    y = np.fft.irfft(X, len(x))
    k = 441
    env = np.sqrt((y[:len(y) // k * k].reshape(-1, k) ** 2).mean(1))
    e = np.log(env + 1e-9)
    e -= e.mean()
    E = np.abs(np.fft.rfft(e * np.hanning(len(e)), 8192))
    fe = np.fft.rfftfreq(8192, k / FS)
    m = (fe > 0.4) & (fe < 12)
    return float(fe[m][np.argmax(E[m])]), float(np.std(e))


def main():
    a = sys.argv[1:]
    voices = mr_ref.opt(a, "--voices", "00_114,00_122,00_121,00_117").split(",")
    os.makedirs(OUT, exist_ok=True)
    if "--analyze" not in a:
        r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), OUT)
        try:
            r.all_off()
            r.mode(0)
            for v in voices:
                mm, nn = int(v[:2]), int(v[3:])
                r.program(63, mm, nn)
                time.sleep(1.0)
                r.cc(1, 0)
                r.note_on(60, 100)
                time.sleep(4.0)
                for mw in MWS:
                    r.cc(1, mw)
                    time.sleep(5.0)
                    r.record("%s_mw%03d" % (v, mw), 2.0, [])
                r.note_off(60)
                r.cc(1, 0)
                time.sleep(0.5)
        finally:
            r.cc(1, 0)
            r.all_off()
            r.mode(3)
            r.close()
    for v in voices:
        out = []
        for mw in MWS:
            p = os.path.join(OUT, "%s_mw%03d.wav" % (v, mw))
            if os.path.exists(p):
                hz, d = rate_of(p)
                out.append("MW %d: %.2fHz（揺れ %.2f）" % (mw, hz, d))
        print(v, "・".join(out), flush=True)


if __name__ == "__main__":
    main()
