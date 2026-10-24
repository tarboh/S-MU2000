"""FEG の時間のキーフォロー（43 ee 30、中心の鍵 43 ee 31）を、谷が測れる範囲の深さで測り直す（2026-10-25。Crazy Mod・Talker は中心 24・鍵 60 で
FEG の動きが芯より遅い件。mr_feg2.py の tkf2 は深さ 127 で谷が上の端に近づき、読みが粗かった）。
  python mr_feg_tkf.py --dir 置き場
雛形の音色（ノイズ 705）で、型 BEF12・カットオフ 128、FEG はアタック（−48 → +48、時間 64）、深さ 96（±1.4 オクターブほど）。
中心 24・60 × 感度 64・76・84・96 × 鍵 36・48・60・72・84。素通しは鍵ごと。
--short は短い段（ディケイ 1 の値 16〜60、キーフォローなし、ds_ で始まる名前）。--decay は下がる段（ディケイ 1 の値 56・64・72・80、+48 → −48）を中心 24 × 感度 64・84・96 × 鍵 36・60・84 で（dk_ で始まる名前）。
送るのはパラメーターチェンジとノートだけ（保存しない）。終わったらマルチへ戻す。"""
import os
import sys
import time

import mr_ref

KEYS = (36, 48, 60, 72, 84)


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    os.makedirs(d, exist_ok=True)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)

    def take(name, key):
        if not os.path.exists(os.path.join(d, name + ".wav")):
            r.record(name, 0.4 + 3.0 + 0.6, [(0.4, lambda: r.note_on(key, 100)), (3.4, lambda: r.note_off(key))])

    def lv(off, v):
        x = 128 + v
        r.el(0, 0x43, off, x >> 7, x & 0x7F)

    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(705)
        for off, v in ((0x05, 0), (0x06, 64), (0x22, 64), (0x2F, 64), (0x03, 64), (0x1F, 64), (0x20, 64)):
            r.el(0, 0x43, off, v)
        r.el(0, 0x43, 0x07, 0, 0)
        r.el(0, 0x43, 0x0A, 0, 0)
        r.el(0, 0x43, 0x0C, 1, 122)
        r.el(0, 0x43, 0x1E, 4)
        for i in range(4):
            r.el(0, 0x43, 0x27 + 2 * i, 1, 0)
        r.el(0, 0x43, 0x1D, 64)
        r.el(0, 0x43, 0x00, 21)
        r.el(0, 0x43, 0x01, 1, 127)
        time.sleep(0.05)
        for k in KEYS:
            take("thru_k%02d" % k, k)
        r.el(0, 0x43, 0x00, 12)
        r.el(0, 0x43, 0x01, 1, 0)
        for i, t in enumerate((0, 64, 0, 0, 0)):
            r.el(0, 0x43, 0x0E + i, t)
        for i, v in enumerate((-48, 48, 48, 48, 48)):
            lv(0x13 + 2 * i, v)
        r.el(0, 0x43, 0x1D, 96)
        if "--short" in a:
            # 短い段の長さ（キーフォローなし）: ディケイ 1 の値 16〜60、+48 → −48、鍵 60
            for i, v in enumerate((48, 48, -48, -48, -48)):
                lv(0x13 + 2 * i, v)
            r.el(0, 0x43, 0x30, 64)
            for T in (16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60):
                for i, t in enumerate((0, 0, T, 0, 0)):
                    r.el(0, 0x43, 0x0E + i, t)
                time.sleep(0.05)
                take("ds_t%03d_k60" % T, 60)
            print("短い段 録った", flush=True)
            return
        if "--decay" in a:
            # 下がる段: 初め +48・アタック 0 で +48、ディケイ 1（値 T）で −48。中心 24 × 感度 64・84・96 × 鍵 36・60・84
            for i, v in enumerate((48, 48, -48, -48, -48)):
                lv(0x13 + 2 * i, v)
            r.el(0, 0x43, 0x31, 24)
            for T in (56, 64, 72, 80):
                for i, t in enumerate((0, 0, T, 0, 0)):
                    r.el(0, 0x43, 0x0E + i, t)
                for s in (64, 84, 96):
                    r.el(0, 0x43, 0x30, s)
                    time.sleep(0.05)
                    for k in (36, 60, 84):
                        take("dk_t%03d_s%03d_k%02d" % (T, s, k), k)
                print("ディケイ", T, "録った", flush=True)
            return
        for c in (24, 60):
            r.el(0, 0x43, 0x31, c)
            for s in (64, 76, 84, 96):
                r.el(0, 0x43, 0x30, s)
                time.sleep(0.05)
                for k in KEYS:
                    take("tk_c%03d_s%03d_k%02d" % (c, s, k), k)
            print("中心", c, "録った", flush=True)
    finally:
        r.el(0, 0x43, 0x30, 64)
        r.el(0, 0x43, 0x31, 60)
        r.all_off()
        r.mode(3)
        r.close()


if __name__ == "__main__":
    main()
