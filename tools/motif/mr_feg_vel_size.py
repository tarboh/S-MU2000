"""FEG の高さのベロシティ感度（43 ee 20）の倍率が、FEG の大きさ（高さ × 深さ）でどう変わるかを測る（2026-10-24）。
  python mr_feg_vel_size.py --dir 置き場
雛形の音色（ノイズ 705、鍵 60）で、型 BEF12、FEG の時間は全部 0（すぐ着く）、高さは 5 つとも同じ h、深さ d。
正の h（8・16・32・64・96）はカットオフ 48、負の h（−16・−32・−64・−96）はカットオフ 200 から。深さ 80・96・127。
それぞれ感度 64（倍率 1）のベロシティ 100 と、感度 127 のカーブ 0・2 × ベロシティ 20・40・70・100・127 を 1 秒押さえる。
FEG なし（深さ 64）と素通しはカットオフ・ベロシティごと。谷を読むのは別。送るのはパラメーターチェンジとノートだけ（保存しない）。終わったらマルチへ戻す。
  --curves: 高さ +64・−64 × 深さ 127 だけで、感度 96・108・127 × カーブ 0〜4 × ベロシティ 20・40・70・100 を録る（cv_ で始まる名前）"""
import os
import sys
import time

import mr_ref

POS = (8, 16, 32, 64, 96)
NEG = (-16, -32, -64, -96)
DEPTHS = (80, 96, 127)
VELS = (20, 40, 70, 100, 127)
HOLD = 1.0


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    os.makedirs(d, exist_ok=True)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)

    def take(name, vel):
        if not os.path.exists(os.path.join(d, name + ".wav")):
            r.record(name, 0.4 + HOLD + 0.6, [(0.4, lambda: r.note_on(60, vel)), (0.4 + HOLD, lambda: r.note_off(60))])

    def lv(off, v):
        x = 128 + v
        r.el(0, 0x43, off, x >> 7, x & 0x7F)

    def cutoff(c):
        r.el(0, 0x43, 0x01, c >> 7, c & 0x7F)

    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(705)
        for off, v in ((0x05, 0), (0x06, 64), (0x22, 64), (0x2F, 64), (0x03, 64), (0x1F, 64), (0x30, 64), (0x20, 64)):
            r.el(0, 0x43, off, v)
        r.el(0, 0x43, 0x07, 0, 0)
        r.el(0, 0x43, 0x0A, 0, 0)
        r.el(0, 0x43, 0x0C, 1, 122)
        r.el(0, 0x43, 0x1E, 4)
        r.el(0, 0x43, 0x31, 60)
        for i in range(4):
            r.el(0, 0x43, 0x27 + 2 * i, 1, 0)
        for i in range(5):
            r.el(0, 0x43, 0x0E + i, 0)
        # 素通し
        r.el(0, 0x43, 0x1D, 64)
        r.el(0, 0x43, 0x00, 21)
        cutoff(255)
        time.sleep(0.05)
        for v in VELS:
            take("thru_v%03d" % v, v)
        r.el(0, 0x43, 0x00, 12)
        if "--curves" in a:
            for c, h in ((48, 64), (200, -64)):
                cutoff(c)
                r.el(0, 0x43, 0x1D, 64)
                r.el(0, 0x43, 0x20, 64)
                time.sleep(0.05)
                for v in VELS:
                    take("nofeg_c%03d_v%03d" % (c, v), v)
                for i in range(5):
                    lv(0x13 + 2 * i, h)
                r.el(0, 0x43, 0x1D, 127)
                tag = "cv_h%s%03d" % ("p" if h > 0 else "m", abs(h))
                time.sleep(0.05)
                take(tag + "_ref", 100)
                for sv in (96, 108, 127):
                    r.el(0, 0x43, 0x20, sv)
                    for cv in range(5):
                        r.el(0, 0x43, 0x21, cv)
                        time.sleep(0.05)
                        for v in VELS[:4]:
                            take("%s_s%03d_c%d_v%03d" % (tag, sv, cv, v), v)
                print("高さ", h, "録った", flush=True)
            return
        for c, hs in ((48, POS), (200, NEG)):
            cutoff(c)
            r.el(0, 0x43, 0x1D, 64)
            r.el(0, 0x43, 0x20, 64)
            time.sleep(0.05)
            for v in VELS:
                take("nofeg_c%03d_v%03d" % (c, v), v)
            for h in hs:
                for i in range(5):
                    lv(0x13 + 2 * i, h)
                for dp in DEPTHS:
                    r.el(0, 0x43, 0x1D, dp)
                    r.el(0, 0x43, 0x20, 64)
                    time.sleep(0.05)
                    tag = "h%s%03d_d%03d" % ("p" if h > 0 else "m", abs(h), dp)
                    take(tag + "_ref", 100)
                    r.el(0, 0x43, 0x20, 127)
                    for cv in (0, 2):
                        r.el(0, 0x43, 0x21, cv)
                        time.sleep(0.05)
                        for v in VELS:
                            take("%s_c%d_v%03d" % (tag, cv, v), v)
                print("高さ", h, "録った", flush=True)
    finally:
        r.all_off()
        r.mode(3)
        r.close()


if __name__ == "__main__":
    main()
