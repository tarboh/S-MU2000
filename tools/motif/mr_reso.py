"""MOTIF-RACK ES のエレメントのフィルターのレゾナンスを測る録音（ノイズの波形 705、鍵 60）。
  python mr_reso.py --dir 置き場 [--types 0,1,4] [--cutoffs 96,128,160,192] [--step 8]

mr_cutoff.py と同じ条件（キーフォロー 0、スケーリング 0、HPF 0、基準は THRU）で、型 × カットオフ × レゾナンス（0〜127）を録る。
ファイルは t??_c???_r???.wav と thru.wav。もうあるものは飛ばす。数を出すのは別（録音を読むだけの道具）。
送るのはパラメーターチェンジとノートだけ。ボイスモードの今の音色を作り変える（保存しない）。終わったらマルチへ戻す。"""
import os
import sys
import time

import mr_cutoff
import mr_ref

NOTE = mr_cutoff.NOTE
HOLD = mr_cutoff.HOLD


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    types = [int(t) for t in mr_ref.opt(a, "--types", "0,1,2,3,4,5,6,7,8,10,11,12,13,14,15,16,17,19").split(",")]
    cutoffs = [int(c) for c in mr_ref.opt(a, "--cutoffs", "96,128,160,192").split(",")]
    step = int(mr_ref.opt(a, "--step", "8"))
    resos = sorted(set(list(range(0, 128, step)) + [127]))
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"),
                   mr_ref.opt(a, "--dev", "0"), d)

    def take(name):
        if not os.path.exists(os.path.join(d, name + ".wav")):
            r.one_note(name, note=NOTE, hold=HOLD, tail=0.3, pre=0.4)

    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(705)
        r.el(0, 0x43, 0x05, 0)                      # レゾナンス 0
        r.el(0, 0x43, 0x07, 0, 0)                   # HPF のカットオフ 0
        r.el(0, 0x43, 0x22, 64)                     # カットオフのキーフォロー 0
        r.el(0, 0x43, 0x2F, 64)                     # HPF のキーフォロー 0
        for off in (0x27, 0x29, 0x2B, 0x2D):
            r.el(0, 0x43, off, 1, 0)                # カットオフのスケーリングのずらし 0（128）
        r.el(0, 0x43, 0x00, 21)
        r.el(0, 0x43, 0x01, 1, 127)
        time.sleep(0.05)
        take("thru")
        for t in types:
            r.el(0, 0x43, 0x00, t)
            for co in cutoffs:
                r.el(0, 0x43, 0x01, co >> 7, co & 0x7F)
                for q in resos:
                    r.el(0, 0x43, 0x05, q)
                    time.sleep(0.05)
                    take("t%02d_c%03d_r%03d" % (t, co, q))
            print("type %d done" % t, flush=True)
            r.el(0, 0x43, 0x05, 0)
        r.all_off()
        r.mode(3)
    finally:
        r.close()
    print("DONE", flush=True)


if __name__ == "__main__":
    main()
