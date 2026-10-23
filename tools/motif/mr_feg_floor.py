"""FEG でカットオフの目盛りが 0 より下に出たとき、折り返すか 0 で止まるかを型ごとに見る（2026-10-24）。
  python mr_feg_floor.py --dir 置き場
雛形の音色（ノイズ 705、鍵 60）で、FEG の高さを全部 L・時間を全部 0 にして、カットオフ c・深さ 0（高さ −127 なら +8 オクターブ下）。
型 LPF24D・LPF12・BEF12 で、FEG なし（深さ 64）と、c − 202 目盛り相当を録る。送るのはパラメーターチェンジとノートだけ（保存しない）。
終わったらマルチへ戻す。"""
import os
import sys
import time

import mr_ref

HOLD = 1.5
# 名前: (型, カットオフ, FEG の高さ, 深さ)
TAKES = [("lpf24_c064_off", 0, 64, 0, 64), ("lpf24_c064_m202", 0, 64, 127, 0), ("lpf24_c000_off", 0, 0, 0, 64),
         ("lpf12_c064_off", 4, 64, 0, 64), ("lpf12_c064_m202", 4, 64, 127, 0),
         ("bef12_c128_off", 12, 128, 0, 64), ("bef12_c128_m202", 12, 128, 127, 0),
         ("lpf24_c064_m100", 0, 64, 64, 0), ("lpf24_c000_m80", 0, 0, 50, 0)]


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    os.makedirs(d, exist_ok=True)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"),
                   mr_ref.opt(a, "--dev", "0"), d)

    def lv(off, v):
        x = 128 + v
        r.el(0, 0x43, off, x >> 7, x & 0x7F)

    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(705)
        for off, v in ((0x05, 0), (0x06, 64), (0x22, 64), (0x2F, 64), (0x03, 64), (0x1F, 64), (0x20, 64), (0x30, 64)):
            r.el(0, 0x43, off, v)
        r.el(0, 0x43, 0x07, 0, 0)
        r.el(0, 0x43, 0x0A, 0, 0)
        for i in range(4):
            r.el(0, 0x43, 0x27 + 2 * i, 1, 0)
        for i in range(5):
            r.el(0, 0x43, 0x0E + i, 0)
        for name, typ, c, level, depth in TAKES:
            if os.path.exists(os.path.join(d, name + ".wav")):
                continue
            r.el(0, 0x43, 0x00, typ)
            r.el(0, 0x43, 0x01, c >> 7, c & 0x7F)
            for i in range(5):
                lv(0x13 + 2 * i, level)
            r.el(0, 0x43, 0x1D, depth)
            time.sleep(0.05)
            r.record(name, 0.3 + HOLD + 0.5, [(0.3, lambda: r.note_on(60, 100)), (0.3 + HOLD, lambda: r.note_off(60))])
            print(name, "録った", flush=True)
    finally:
        r.all_off()
        r.mode(3)
        r.close()


if __name__ == "__main__":
    main()
