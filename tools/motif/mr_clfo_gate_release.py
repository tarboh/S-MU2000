"""コモン LFO の AMD で音を下げている間に鍵を離すと、リリースがどうなるかを測る（2026-10-24。Eclipse 03_087 で実機だけ小さくなった件）。
  python mr_clfo_gate_release.py --dir 置き場
雛形の音色（正弦波・エレメントの音量 100）。コモン LFO はユーザー波形 2 段（64・0）・速さ 16 で、2.43〜4.85 秒に AMD が効く。
3.0 秒で離し、AMD の深さ 0・32・64・80・96・104・127 で、開いたあと（4.85 秒〜）の音量を比べる。リリース 100 と 90。
送るのはパラメーターチェンジとノートだけ（保存しない）。終わったらマルチへ戻す。"""
import os
import sys
import time

import mr_ref

BASE = {0x00: 0, 0x01: 12, 0x02: 16, 0x04: 0, 0x05: 0, 0x06: 0, 0x07: 127, 0x08: 0, 0x09: 1, 0x0A: 0,
        0x0D: 0, 0x0E: 0, 0x10: 0, 0x11: 0, 0x13: 0, 0x15: 64, 0x16: 0}


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    os.makedirs(d, exist_ok=True)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"),
                   mr_ref.opt(a, "--dev", "0"), d)
    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(704)
        r.el(0, 0x43, 0x00, 21)
        r.el(0, 0x41, 0x1A, 1)
        r.el(0, 0x44, 0x51, 127)
        r.el(0, 0x41, 0x20, 100)
        for off, v in BASE.items():
            r.param(0x40, 0x06, off, v)
        for rel in (100, 90):
            r.el(0, 0x41, 0x27, rel)
            for dp in (0, 32, 64, 80, 96, 104, 127):
                name = "gr_r%03d_d%03d" % (rel, dp)
                if os.path.exists(os.path.join(d, name + ".wav")):
                    continue
                r.param(0x40, 0x06, 0x0B, dp)
                time.sleep(0.05)
                r.record(name, 0.3 + 7.5, [(0.3, lambda: r.note_on(69, 100)), (3.3, lambda: r.note_off(69))])
                print(name, "録った", flush=True)
    finally:
        r.all_off()
        r.param(0x40, 0x06, 0x0B, 0)
        r.mode(3)
        r.close()


if __name__ == "__main__":
    main()
