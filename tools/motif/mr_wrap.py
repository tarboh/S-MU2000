"""高い鍵で「鍵 n と n+12 が同じ音になる」所（1 オクターブごとのくり返し）が、音程で決まるのか、鍵で決まるのかを見分ける録音。
  python mr_wrap.py --dir 置き場 [--waves 125,69,1589,972] [--coarse -12,0,12] [--lo 40]

雛形の音色で、エレメントのコースチューンだけを変えて、鍵 lo〜127 を 0.4 秒ずつ鳴らして録る（mr_sweep.py と同じ並べ方）。
音程の天井なら、コースチューンを +12 にするとくり返しの始まりが 12 鍵下がる。区間の割り当て（わざとの折り返し）なら動かない。
送るのはパラメーターチェンジとノートだけ。ボイスモードの今の音色を作り変える（保存しない）。終わったらマルチへ戻す。"""
import os
import sys
import time

import mr_ref
import mr_sweep

ON, GAP = mr_sweep.ON, mr_sweep.GAP


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    waves = [int(v) for v in mr_ref.opt(a, "--waves", "125,69,1589,972").split(",")]
    coarse = [int(v) for v in mr_ref.opt(a, "--coarse", "-12,0,12").split(",")]
    lo = int(mr_ref.opt(a, "--lo", "40"))
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"),
                   mr_ref.opt(a, "--dev", "0"), d)
    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(waves[0])
        for wave in waves:
            r.el(0, 0x41, 0x03, wave >> 7, wave & 0x7F)
            for c in coarse:
                base = os.path.join(d, "w%04d_c%+03d" % (wave, c))
                if os.path.exists(base + ".wav"):
                    continue
                r.el(0, 0x42, 0x00, 0x40 + c)
                time.sleep(0.15)
                ev, marks = [], []
                t = 0.4
                for n in range(lo, 128):
                    ev.append((t, lambda n=n: r.note_on(n, 100)))
                    ev.append((t + ON, lambda n=n: r.note_off(n)))
                    marks.append((n, t))
                    t += ON + GAP
                L, R = r.record("_tmp", t + 0.4, ev)
                mr_sweep.write_wav24(base + ".wav", L, R)
                os.remove(os.path.join(d, "_tmp.wav"))
                with open(base + ".txt", "w") as f:
                    f.write("\n".join("%d %.3f" % m for m in marks))
                print("wave %d coarse %+d: peak %.3f" % (wave, c, mr_ref.peak(L)), flush=True)
        r.el(0, 0x42, 0x00, 0x40)
        r.all_off()
        r.mode(3)
    finally:
        r.close()


if __name__ == "__main__":
    main()
