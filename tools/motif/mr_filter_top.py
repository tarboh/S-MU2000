"""フィルターを上の端近くまで開いたときの形を測る（2026-10-24。Oxide の鍵 72 で、芯は FEG で開いた所が実機より暗い件）。
  python mr_filter_top.py --dir 置き場 [--types 1,0,4] [--fine]（--fine は目盛り 222〜240 を 2 つおきにレゾナンス 64 で）[--r0]（150〜250 を 10 おきにレゾナンス 0 で）[--res]（200〜250 を 10 おきにレゾナンス 8〜96 で）
雛形の音色（ノイズ 705、鍵 60）で、型ごとにカットオフ 180・200・220・240・255、レゾナンス 0・28・64 を 1.2 秒鳴らす。基準は THRU。
送るのはパラメーターチェンジとノートだけ（保存しない）。終わったらマルチへ戻す。"""
import os
import sys
import time

import mr_ref


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    types = [int(t) for t in mr_ref.opt(a, "--types", "1").split(",")]
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
        r.el(0, 0x43, 0x00, 21)
        time.sleep(0.05)
        if not os.path.exists(os.path.join(d, "thru.wav")):
            r.one_note("thru", note=60, vel=100, hold=1.2, tail=0.2, pre=0.2)
        for t in types:
            r.el(0, 0x43, 0x00, t)
            cs = (180, 200, 220, 240, 255) if "--fine" not in a else tuple(range(222, 242, 2))
            qs = (0, 28, 64) if "--fine" not in a else (64,)
            if "--r0" in a:                              # レゾナンス 0 の形を 150〜250 の 10 おきで
                cs, qs = tuple(range(150, 251, 10)), (0,)
            if "--res" in a:                             # 上の端（200〜250 の 10 おき）を、いくつかのレゾナンスで
                cs, qs = tuple(range(200, 251, 10)), (8, 16, 24, 32, 48, 64, 96)
            for c in cs:
                r.el(0, 0x43, 0x01, c >> 7, c & 0x7F)
                for q in qs:
                    r.el(0, 0x43, 0x05, q)
                    time.sleep(0.05)
                    name = "t%02d_c%03d_r%03d" % (t, c, q)
                    if not os.path.exists(os.path.join(d, name + ".wav")):
                        r.one_note(name, note=60, vel=100, hold=1.2, tail=0.2, pre=0.2)
                    print(name, "録った", flush=True)
    finally:
        r.all_off()
        r.mode(3)
        r.close()


if __name__ == "__main__":
    main()
