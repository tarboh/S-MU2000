"""強いレゾナンスで実機のフィルターが飽和する形を、フィルターに入る大きさを変えて測る（2026-10-24）。
  python mr_filter_sat.py --dir 置き場
雛形の音色（ノイズ 705、鍵 60）で、LPF24D・LPF12 をカットオフ 128・200、レゾナンス 0・32・64・96・127 にして、
フィルターゲイン（43 ee 0C、1 あたり 0.375dB）を 250・226・202・178・154（9dB おき）で鳴らす。基準は同じフィルターゲインの THRU。
送るのはパラメーターチェンジとノートだけ（保存しない）。終わったらマルチへ戻す。"""
import os
import sys
import time

import mr_ref

GAINS = (250, 226, 202, 178, 154)


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    os.makedirs(d, exist_ok=True)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(705)
        for off, v in ((0x03, 64), (0x06, 64), (0x22, 64), (0x2F, 64), (0x1D, 64)):
            r.el(0, 0x43, off, v)
        r.el(0, 0x43, 0x07, 0, 0)
        for i in range(4):
            r.el(0, 0x43, 0x27 + 2 * i, 1, 0)
        for fg in GAINS:
            r.el(0, 0x43, 0x0C, fg >> 7, fg & 0x7F)
            r.el(0, 0x43, 0x00, 21)
            time.sleep(0.05)
            name = "thru_g%03d" % fg
            if not os.path.exists(os.path.join(d, name + ".wav")):
                r.one_note(name, note=60, vel=100, hold=1.2, tail=0.2, pre=0.2)
            for t in (0, 4):
                r.el(0, 0x43, 0x00, t)
                for c in (128, 200):
                    r.el(0, 0x43, 0x01, c >> 7, c & 0x7F)
                    for q in (0, 32, 64, 96, 127):
                        name = "t%02d_c%03d_r%03d_g%03d" % (t, c, q, fg)
                        if os.path.exists(os.path.join(d, name + ".wav")):
                            continue
                        r.el(0, 0x43, 0x05, q)
                        time.sleep(0.05)
                        r.one_note(name, note=60, vel=100, hold=1.2, tail=0.2, pre=0.2)
            print("フィルターゲイン", fg, "録った", flush=True)
        # 2 回目: フィルターのあとで掛かるエレメントの音量を下げても頭打ちが残るか（中で飽和か、出口で頭打ちか）
        r.el(0, 0x43, 0x0C, 250 >> 7, 250 & 0x7F)
        r.el(0, 0x43, 0x00, 0)
        r.el(0, 0x43, 0x01, 128 >> 7, 128 & 0x7F)
        r.el(0, 0x43, 0x05, 96)
        for lv in (64, 32):
            r.el(0, 0x41, 0x20, lv)
            time.sleep(0.05)
            name = "t00_c128_r096_g250_l%03d" % lv
            if not os.path.exists(os.path.join(d, name + ".wav")):
                r.one_note(name, note=60, vel=100, hold=1.2, tail=0.2, pre=0.2)
        r.el(0, 0x41, 0x20, 127)
    finally:
        r.el(0, 0x43, 0x0C, 1, 122)
        r.all_off()
        r.mode(3)
        r.close()


if __name__ == "__main__":
    main()
