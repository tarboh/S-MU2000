"""mr_eq.py の続き。エレメントの音量を 64（約 -12dB）に下げて、EQ THRU と Boost6/12/18 を録り直す。
  python mr_eq_boostlow.py 置き場 [--rec ../../build/rec.exe]
音量 127 のままだと Boost12・18 がチップの中で頭打ちするので、その確かめ用。
送るのはパラメーターチェンジとノートだけ（保存しない）。最後に EQ と音量を戻し、マルチへ戻す。"""
import os
import sys
import time

import mr_cutoff
import mr_ref


def main():
    a = sys.argv[1:]
    rec = mr_ref.opt(a, "--rec", "../../build/rec.exe")
    d = a[0] if a else "."
    r = mr_ref.Ref("MOTIF-RACK ES-1", rec, "0", d)
    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(705)
        r.el(0, 0x43, 0x00, 21)
        r.el(0, 0x43, 0x01, 1, 127)
        r.el(0, 0x41, 0x20, 64)                     # エレメントの音量 64
        for t, nm in ((5, "thru"), (2, "6"), (3, "12"), (4, "18")):
            r.el(0, 0x44, 0x00, t)
            time.sleep(0.05)
            name = "low_" + ("thru" if nm == "thru" else "bo" + nm)
            if not os.path.exists(os.path.join(d, name + ".wav")):
                r.one_note(name, note=60, hold=mr_cutoff.HOLD, tail=0.3, pre=0.4)
        r.el(0, 0x44, 0x00, 5)
        r.el(0, 0x41, 0x20, 127)
        r.all_off()
        r.mode(3)
    finally:
        r.close()
    print("DONE")


if __name__ == "__main__":
    main()
