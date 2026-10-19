"""MOTIF-RACK ES のエレメントの音量（AEG・ベロシティ・キーフォロー）を測る録音（正弦波の波形 704、フィルター THRU）。
  python mr_aeg.py --dir 置き場 [--part level,vel,kf,time]

  level : AEG の 4 つの高さ（init・attack・decay1・decay2）を全部 L、時間を全部 0 にして鍵 60 で鳴らす（al???.wav）。高さの目盛りを見る
  vel   : 音量のベロシティ感度（41 ee 21、64 が 0）× カーブ（41 ee 22）× ベロシティ 1, 8, …, 127（av???_c?_v???.wav）
  kf    : 音量のキーフォロー（41 ee 2F、64 が 0）× 鍵 36・48・60・72・84（ak???_k??.wav）
  time  : アタック 64・ディケイ 1 64（高さ 127 → 64）で、時間のベロシティ感度（41 ee 2E、対象 41 ee 2D = ALL）× ベロシティ、
          時間のキーフォロー（41 ee 3C、中心 41 ee 3D = 60）× 鍵（at???_v???.wav・ak2???_k??.wav）
雛形の値（mr_ref.py）を基にし、測るもの以外は 0 にする。もうあるものは飛ばす。数を出すのは別。
送るのはパラメーターチェンジとノートだけ（保存しない）。終わったらマルチへ戻す。"""
import os
import sys
import time

import mr_ref

VELS = [1] + list(range(8, 128, 8)) + [127]


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    parts = mr_ref.opt(a, "--part", "level,vel,kf,time").split(",")
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"),
                   mr_ref.opt(a, "--dev", "0"), d)

    def amp(off, *v):
        r.el(0, 0x41, 0x20 + off, *v)

    def take(name, hold=1.2, key=60, vel=100):
        if not os.path.exists(os.path.join(d, name + ".wav")):
            r.record(name, 0.3 + hold + 0.5, [(0.3, lambda: r.note_on(key, vel)), (0.3 + hold, lambda: r.note_off(key))])

    def base():
        amp(0x00, 127)                              # エレメントの音量
        amp(0x01, 64)                               # ベロシティ感度 0
        amp(0x02, 0)                                # カーブ 0
        for i, v in enumerate((0, 0, 0, 0)):        # 時間 attack・decay1・decay2・release
            amp(0x04 + i, v)
        for i, v in enumerate((127, 127, 127, 127)):  # 高さ init・attack・decay1・decay2
            amp(0x08 + i, v)
        amp(0x0D, 4)                                # 時間のベロシティの対象 ALL
        amp(0x0E, 64)
        amp(0x0F, 64)                               # 音量のキーフォロー 0
        for i in range(4):
            amp(0x14 + 2 * i, 1, 0)                 # レベルスケーリングのずらし 0
        amp(0x1C, 64)
        amp(0x1D, 60)
        r.el(0, 0x43, 0x00, 21)
        r.el(0, 0x43, 0x01, 1, 127)
        r.el(0, 0x43, 0x22, 64)
        time.sleep(0.05)

    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(704)
        base()
        if "level" in parts:
            for L in (127, 120, 112, 104, 96, 88, 80, 64, 48, 32, 16, 8, 1, 0):
                for i in range(4):
                    amp(0x08 + i, L)
                time.sleep(0.05)
                take("al%03d" % L)
            print("level done", flush=True)
            base()
        if "vel" in parts:
            for s, c in [(s, 0) for s in (0, 16, 32, 48, 80, 96, 112, 127)] + [(127, c) for c in (1, 2, 3, 4)]:
                amp(0x01, s)
                amp(0x02, c)
                time.sleep(0.05)
                for v in VELS:
                    take("av%03d_c%d_v%03d" % (s, c, v), 0.8, 60, v)
            print("vel done", flush=True)
            base()
        if "kf" in parts:
            for s in (0, 32, 96, 127):
                amp(0x0F, s)
                time.sleep(0.05)
                for k in (36, 48, 60, 72, 84):
                    take("ak%03d_k%02d" % (s, k), 0.8, k)
            print("kf done", flush=True)
            base()
        if "time" in parts:
            amp(0x04, 64)
            amp(0x05, 64)
            amp(0x0A, 64)
            amp(0x0B, 64)
            for s in (64, 80, 96):
                amp(0x0E, s)
                time.sleep(0.05)
                for v in (1, 64, 127):
                    take("at%03d_v%03d" % (s, v), 3.0, 60, v)
            amp(0x0E, 64)
            for s in (96, 127):
                amp(0x1C, s)
                time.sleep(0.05)
                for k in (36, 60, 84):
                    take("ak2%03d_k%02d" % (s, k), 3.0, k)
            print("time done", flush=True)
            base()
        r.all_off()
        r.mode(3)
    finally:
        r.close()
    print("DONE", flush=True)


if __name__ == "__main__":
    main()
