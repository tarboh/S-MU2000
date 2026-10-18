"""全部の波形について、ベロシティでサンプルが切り替わるかをざっと調べる録音（mr_vel.py の広く浅い版）。
  python mr_velscan.py --dir 置き場 [--from 1] [--to 1859] [--key 60] [--hours 8]

波形ごとに、決まった鍵を VELS のベロシティで 0.4 秒ずつ鳴らして 1 つの WAV に録る（.txt の 1 列めはベロシティ）。
もうあるものは飛ばすので、途中で止めて続きから回せる。置き場に STOP という名前のファイルを置くと、今の波形を録り終えた所で止まる。
送るのはパラメーターチェンジとノートだけ。ボイスモードの今の音色を作り変える（保存しない）。終わったらマルチへ戻す。"""
import os
import sys
import time

import mr_ref
import mr_sweep

ON, GAP = mr_sweep.ON, mr_sweep.GAP
VELS = (1, 16, 32, 48, 64, 80, 96, 104, 112, 120, 127)


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    lo, hi = int(mr_ref.opt(a, "--from", "1")), int(mr_ref.opt(a, "--to", "1859"))
    key = int(mr_ref.opt(a, "--key", "60"))
    hours = float(mr_ref.opt(a, "--hours", "8"))
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"),
                   mr_ref.opt(a, "--dev", "0"), d)
    t_end = time.time() + hours * 3600
    done = 0
    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(lo)
        for wave in range(lo, hi + 1):
            base = os.path.join(d, "v%04d" % wave)
            if os.path.exists(base + ".wav"):
                continue
            if time.time() > t_end or os.path.exists(os.path.join(d, "STOP")):
                break
            r.el(0, 0x41, 0x03, wave >> 7, wave & 0x7F)
            time.sleep(0.15)
            ev, marks = [], []
            t = 0.4
            for v in VELS:
                ev.append((t, lambda v=v: r.note_on(key, v)))
                ev.append((t + ON, lambda: r.note_off(key)))
                marks.append((v, t))
                t += ON + GAP
            L, R = r.record("_tmp", t + 0.4, ev)
            mr_sweep.write_wav24(base + ".wav", L, R)
            os.remove(os.path.join(d, "_tmp.wav"))
            with open(base + ".txt", "w") as f:
                f.write("\n".join("%d %.3f" % m for m in marks))
            done += 1
            print("wave %d: peak %.3f" % (wave, mr_ref.peak(L)), flush=True)
        r.all_off()
        r.mode(3)
    finally:
        r.close()
    print("DONE %d waves" % done, flush=True)


if __name__ == "__main__":
    main()
