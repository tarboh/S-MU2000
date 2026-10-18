"""1 本の波形の中で、ベロシティによってサンプルが切り替わるかを見る録音。
  python mr_vel.py --dir 置き場 --waves 972,973,974 [--key 60] [--step 2]

雛形の音色で、決まった鍵をベロシティ 1〜127 を step 刻みで 0.4 秒ずつ鳴らして録る（mr_sweep.py と同じ並べ方。.txt の 1 列めはベロシティ）。
名前に「1-2」「1-4」などが付く波形（BdSnap1-2 など）は、ベロシティで 1 番・2 番…を切り替えていると見ている。
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
    waves = [int(v) for v in mr_ref.opt(a, "--waves", "972,973,974").split(",")]
    key = int(mr_ref.opt(a, "--key", "60"))
    step = int(mr_ref.opt(a, "--step", "2"))
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"),
                   mr_ref.opt(a, "--dev", "0"), d)
    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(waves[0])
        for wave in waves:
            base = os.path.join(d, "w%04d_k%03d" % (wave, key))
            if os.path.exists(base + ".wav"):
                continue
            r.el(0, 0x41, 0x03, wave >> 7, wave & 0x7F)
            time.sleep(0.15)
            ev, marks = [], []
            t = 0.4
            for v in range(1, 128, step):
                ev.append((t, lambda v=v: r.note_on(key, v)))
                ev.append((t + ON, lambda: r.note_off(key)))
                marks.append((v, t))
                t += ON + GAP
            L, R = r.record("_tmp", t + 0.4, ev)
            mr_sweep.write_wav24(base + ".wav", L, R)
            os.remove(os.path.join(d, "_tmp.wav"))
            with open(base + ".txt", "w") as f:
                f.write("\n".join("%d %.3f" % m for m in marks))
            print("wave %d key %d: peak %.3f" % (wave, key, mr_ref.peak(L)), flush=True)
        r.all_off()
        r.mode(3)
    finally:
        r.close()


if __name__ == "__main__":
    main()
