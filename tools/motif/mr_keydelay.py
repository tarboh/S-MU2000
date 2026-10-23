"""エレメントの Key On Delay（41 ee 11、0〜127）の遅れを測る。
  python mr_keydelay.py --dir 置き場 [--values 0,1,2,5,10,20,40,80,127] [--reps 3] [--hold 2.5]
雛形の音色（波形 704 = 正弦波、エレメント 1 だけ）に Key On Delay を入れ、鍵 60 を鳴らして録る。録音の頭は 1 本ごとに ±4ms ほど揺れるので、
値ごとに reps 回録り、音の頭（|x| > 1e-4）の位置の中央値を、値 0 の中央値との差で出す。
送るのはパラメーターチェンジ・ダンプの要求・ノートだけ（保存しない）。終わったらマルチへ戻す。"""
import os
import sys

import numpy as np

import mr_ref

FS = 44100


def onset(path):
    L, R = mr_ref.read_wav(path)
    x = np.abs(np.asarray(L)) + np.abs(np.asarray(R))
    return int(np.argmax(x > 1e-4))


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    vals = [int(v) for v in mr_ref.opt(a, "--values", "0,1,2,5,10,20,40,80,127").split(",")]
    reps = int(mr_ref.opt(a, "--reps", "3"))
    hold = float(mr_ref.opt(a, "--hold", "2.5"))
    os.makedirs(d, exist_ok=True)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(704)
        for v in vals:
            r.el(0, 0x41, 0x11, v)
            for i in range(reps):
                name = "kd%03d_%d" % (v, i)
                if not os.path.exists(os.path.join(d, name + ".wav")):
                    r.record(name, 0.3 + hold + 0.5, [(0.3, lambda: r.note_on(60, 100)), (0.3 + hold, lambda: r.note_off(60))])
    finally:
        r.el(0, 0x41, 0x11, 0)
        r.all_off()
        r.mode(3)
        r.close()
    base = np.median([onset(os.path.join(d, "kd000_%d.wav" % i)) for i in range(reps)])
    for v in vals:
        o = [onset(os.path.join(d, "kd%03d_%d.wav" % (v, i))) for i in range(reps)]
        print("Key On Delay %3d: 遅れ %.1f ms（%s）" % (v, (np.median(o) - base) / FS * 1000, " ".join("%.1f" % ((x - base) / FS * 1000) for x in o)))


if __name__ == "__main__":
    main()
