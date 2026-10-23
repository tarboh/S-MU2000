"""AEG の時間のキーフォロー（41 ee 3C、中心 3D）がリリースにも効くかを見る（2026-10-24）。
  python mr_aeg_kf_release.py --dir 置き場
雛形の音色（正弦波）で、リリースの時間 80・キーフォロー 64（0）と 96（中心 48）で、鍵 60 と 84 を 1 秒鳴らして離す。
同じ組でディケイ 1（高さ 64 へ、時間 80）も録り、ディケイには効くことを確かめる。送るのはパラメーターチェンジとノートだけ（保存しない）。"""
import os
import sys
import time

import mr_ref


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
        r.el(0, 0x41, 0x3D, 48)                          # 時間のキーフォローの中心
        for seg in ("rel", "dcy"):
            if seg == "rel":
                r.el(0, 0x41, 0x27, 80)                  # リリース 80
                r.el(0, 0x41, 0x25, 127)
                r.el(0, 0x41, 0x2A, 127)
            else:
                r.el(0, 0x41, 0x27, 0)
                r.el(0, 0x41, 0x25, 80)                  # ディケイ 1: 80 で高さ 64 へ
                r.el(0, 0x41, 0x2A, 64)
                r.el(0, 0x41, 0x2B, 64)
            for kf in (64, 96):
                r.el(0, 0x41, 0x3C, kf)
                time.sleep(0.05)
                for k in (60, 84):
                    name = "%s_kf%03d_k%03d" % (seg, kf, k)
                    if not os.path.exists(os.path.join(d, name + ".wav")):
                        r.one_note(name, note=k, vel=100, hold=1.0 if seg == "rel" else 3.0, tail=3.0 if seg == "rel" else 0.5, pre=0.3)
                    print(name, "録った", flush=True)
        # 2 回目: リリースだけ、鍵・感度・中心を広げて式を決める
        r.el(0, 0x41, 0x27, 80)
        r.el(0, 0x41, 0x25, 127)
        r.el(0, 0x41, 0x2A, 127)
        r.el(0, 0x41, 0x2B, 127)
        for center, kf, keys in ((48, 96, (36, 48, 72, 96)), (48, 127, (36, 60, 84)), (72, 96, (60, 84)), (48, 32, (36, 84))):
            r.el(0, 0x41, 0x3D, center)
            r.el(0, 0x41, 0x3C, kf)
            time.sleep(0.05)
            for k in keys:
                name = "rel_c%03d_kf%03d_k%03d" % (center, kf, k)
                if not os.path.exists(os.path.join(d, name + ".wav")):
                    r.one_note(name, note=k, vel=100, hold=1.0, tail=3.0, pre=0.3)
                print(name, "録った", flush=True)
    finally:
        r.all_off()
        r.mode(3)
        r.close()


if __name__ == "__main__":
    main()
