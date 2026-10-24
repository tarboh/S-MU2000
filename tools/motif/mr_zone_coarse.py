"""区間（サンプル）を選ぶ鍵が、コースチューンを足した鍵か、押した鍵かを、波形ごとに直に確かめる（2026-10-25。Flangewall の鍵 48・コース −12 は、
芯の「鍵 + コース = 36 の区間」より、鍵 48 の区間を 12 半音下げた音に近かった件）。
  python mr_zone_coarse.py --dir 置き場 --waves 573,301 [--pairs 36:0,48:-12,60:-24]
雛形の音色（フィルター THRU）で、鍵とコースの組（同じ音程になる）を 1.5 秒ずつ鳴らす。比べるのは別（スペクトルの形）。
送るのはパラメーターチェンジとノートだけ（保存しない）。終わったらマルチへ戻す。録った音は配らないこと（ヤマハの波形）。"""
import os
import sys
import time

import mr_ref


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    waves = [int(x) for x in mr_ref.opt(a, "--waves", "573").split(",")]
    pairs = [tuple(int(y) for y in x.split(":")) for x in mr_ref.opt(a, "--pairs", "36:0,48:-12,60:-24").split(",")]
    os.makedirs(d, exist_ok=True)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        for w in waves:
            r.template(w)
            r.el(0, 0x43, 0x00, 21)
            r.el(0, 0x43, 0x01, 1, 127)
            for k, c in pairs:
                r.el(0, 0x42, 0x00, 64 + c)
                time.sleep(0.05)
                name = "zc_w%04d_k%03d_c%+03d" % (w, k, c)
                if not os.path.exists(os.path.join(d, name + ".wav")):
                    r.record(name, 0.3 + 1.5 + 0.4, [(0.3, lambda k=k: r.note_on(k, 100)), (1.8, lambda k=k: r.note_off(k))])
            r.el(0, 0x42, 0x00, 64)
            print("波形", w, "録った", flush=True)
    finally:
        r.el(0, 0x42, 0x00, 64)
        r.all_off()
        r.mode(3)
        r.close()


if __name__ == "__main__":
    main()
