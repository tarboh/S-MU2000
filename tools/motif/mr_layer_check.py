"""1 本の波形のベロシティの層の境目を、雛形（ふつうの音色）とドラムの鍵で比べる（2026-10-25）。
  python mr_layer_check.py 置き場 --wave 1317 [--drum-key 88] [--kit 0] [--vels 90-106]
雛形（mr_ref.template、フィルター THRU）の鍵 60 と、PRE のドラムの鍵（波形が同じもの）を、ベロシティを 1 ずつ変えて鳴らし、
いちばん上とベロシティ 60 の音のどちらにスペクトルが近いか（上の層か下の層か）を出す。
送るのは Data List のパラメーターチェンジ・バンクとプログラムの選び・ノートだけ（保存しない）。終わったらマルチへ戻す。"""
import sys
import time

import numpy as np

import mr_drum_compare as C
import mr_ref


def main():
    a = sys.argv[1:]
    d = a[0]
    wave = int(mr_ref.opt(a, "--wave", "1317"))
    dkey = int(mr_ref.opt(a, "--drum-key", "88"))
    kit = int(mr_ref.opt(a, "--kit", "0"))
    lo, hi = (int(x) for x in mr_ref.opt(a, "--vels", "90-106").split("-"))
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
    import os
    try:
        r.all_off()
        r.param(0x00, 0x00, 0x34, 0)
        r.mode(0)
        for side in ("tmpl", "drum"):
            if side == "tmpl":
                r.program(63, 0, 0)
                r.template(wave)
                r.el(0, 0x43, 0x00, 21)
                key = 60
            else:
                r.program(63, 32, kit)
                r.cc(91, 0)
                r.cc(93, 0)
                key = dkey
            time.sleep(0.2)
            names = {}
            for v in [60, 127] + list(range(lo, hi + 1)):
                nm = "%s_w%04d_v%03d" % (side, wave, v)
                r.all_off()
                time.sleep(0.1)
                r.one_note(nm, note=key, vel=v, hold=0.4, tail=0.2, pre=0.3)
                names[v] = os.path.join(d, nm + ".wav")
            shape = lambda p: (lambda f: f[2] - f[2].max())(C.feats(p, True))
            L, H = shape(names[60]), shape(names[127])
            print(side, " ".join("%d:%s" % (v, "H" if np.mean(np.abs(shape(names[v]) - H)) < np.mean(np.abs(shape(names[v]) - L)) else "L")
                                 for v in range(lo, hi + 1)), flush=True)
    finally:
        r.all_off()
        r.mode(3)
        r.close()


if __name__ == "__main__":
    main()
