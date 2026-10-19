"""MOTIF-RACK ES のエレメントの音量（AEG・ベロシティ・キーフォロー）を測る録音（正弦波の波形 704、フィルター THRU）。
  python mr_aeg.py --dir 置き場 [--part level,vel,kf,time,…]

  level : AEG の 4 つの高さ（init・attack・decay1・decay2）を全部 L、時間を全部 0 にして鍵 60 で鳴らす（al???.wav）。高さの目盛りを見る
  vel   : 音量のベロシティ感度（41 ee 21、64 が 0）× カーブ（41 ee 22）× ベロシティ 1, 8, …, 127（av???_c?_v???.wav）
  kf    : 音量のキーフォロー（41 ee 2F、64 が 0）× 鍵 36・48・60・72・84（ak???_k??.wav）
  time  : アタック 64・ディケイ 1 64（高さ 127 → 64）で、時間のベロシティ感度（41 ee 2E、対象 41 ee 2D = ALL）× ベロシティ、
          時間のキーフォロー（41 ee 3C、中心 41 ee 3D = 60）× 鍵（at???_v???.wav・ak2???_k??.wav）
  vel2  : 感度 +56・+60・+62（カーブ 0）と、感度 +32 でのカーブ 1〜4（av???_c?_v???.wav）
  time2 : ディケイ 1 を 80 にして、時間のベロシティ感度 × ベロシティと、時間のキーフォロー × 鍵（bt???_v???.wav・bk???_k??.wav）
  shape : 段の長さが高さの差によるか（sd???.wav）と、アタックの時間の目盛り（sa???.wav）
  vel3  : カーブ 1〜4 を感度 +8・+16・+24・+48 でも（av???_c?_v???.wav）
  scale : レベルスケーリング（全部に同じずらし ls???.wav、区切り 1 だけ −64 で鍵ごと lsk_k??.wav）
  seg   : 時間のベロシティ感度の効く段 0〜4（sg?.wav）
  kfslow: ディケイ 1 = 88・96・104（127 → 0）での時間のキーフォロー（kfs???_???_c??_k??.wav）
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
    parts = mr_ref.opt(a, "--part", "level,vel,kf,time,vel2,time2,shape,vel3,scale,seg,kfslow").split(",")
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
        if "vel2" in parts:
            # 感度 +56・+60・+62 の細かいところと、感度 +32 でのカーブ 1〜4
            for s, c in [(s, 0) for s in (120, 124, 126)] + [(96, c) for c in (1, 2, 3, 4)]:
                amp(0x01, s)
                amp(0x02, c)
                time.sleep(0.05)
                for v in VELS:
                    take("av%03d_c%d_v%03d" % (s, c, v), 0.8, 60, v)
            print("vel2 done", flush=True)
            base()
        if "time2" in parts:
            # ディケイ 1 を 80（3.2 秒）にして、速い側まで長さを読めるようにする
            amp(0x04, 0)
            amp(0x05, 80)
            amp(0x0A, 64)
            amp(0x0B, 64)
            for s in (72, 80, 88, 96, 112):
                amp(0x0E, s)
                time.sleep(0.05)
                for v in (16, 32, 64, 96, 127):
                    take("bt%03d_v%03d" % (s, v), 4.0, 60, v)
            amp(0x0E, 64)
            for s in (32, 80, 96, 127):
                amp(0x1C, s)
                time.sleep(0.05)
                for k in (36, 48, 72, 84):
                    take("bk%03d_k%02d" % (s, k), 4.0, k)
            print("time2 done", flush=True)
            base()
        if "vel3" in parts:
            # カーブ 1〜4 を感度 +8・+16・+24・+48 でも（カーブ 0 は済み）
            for s, c in [(s, c) for s in (72, 80, 88, 112) for c in (1, 2, 3, 4)]:
                amp(0x01, s)
                amp(0x02, c)
                time.sleep(0.05)
                for v in VELS:
                    take("av%03d_c%d_v%03d" % (s, c, v), 0.8, 60, v)
            print("vel3 done", flush=True)
            base()
        if "scale" in parts:
            # 音量のレベルスケーリング: 区切りの鍵 36・48・72・84、ずらし（2 バイト、128 が 0）
            for i, k in enumerate((36, 48, 72, 84)):
                amp(0x10 + i, k)
            # 全部同じずらしで、鍵 60 の音量
            for o in (0, 64, 96, 112, 144, 160, 192, 255):
                for i in range(4):
                    amp(0x14 + 2 * i, o >> 7, o & 0x7F)
                time.sleep(0.05)
                take("ls%03d" % o, 0.8)
            # 区切り 1 だけ 64（−64）で、鍵ごと
            for i in range(4):
                amp(0x14 + 2 * i, 1, 0)
            amp(0x14, 0, 64)
            time.sleep(0.05)
            for k in (24, 30, 36, 42, 48, 54, 60, 84, 96):
                take("lsk_k%02d" % k, 0.8, k)
            print("scale done", flush=True)
            base()
        if "seg" in parts:
            # 時間のベロシティ感度の効く段（41 ee 2D）: ディケイ 1 = 80（127 → 64）・リリース 64、感度 +32・ベロシティ 127
            amp(0x05, 80)
            amp(0x07, 64)
            amp(0x0A, 64)
            amp(0x0B, 64)
            amp(0x0E, 96)
            for sg in (0, 1, 2, 3, 4):
                amp(0x0D, sg)
                time.sleep(0.05)
                take("sg%d" % sg, 4.0, 60, 127)
            print("seg done", flush=True)
            base()
        if "kfslow" in parts:
            # 遅い段での時間のキーフォロー: ディケイ 1 を 88・96・104（127 → 0）にして、最初の 2 秒の傾き（dB/秒）を見る。
            # 感度 0・+32・+63、中心 60 と 24、鍵 36・60・84・96
            amp(0x0A, 0)
            amp(0x0B, 0)
            for t in (88, 96, 104):
                amp(0x05, t)
                for s, c in ((64, 60), (96, 60), (127, 60), (96, 24)):
                    amp(0x1C, s)
                    amp(0x1D, c)
                    time.sleep(0.05)
                    for k in ((60,) if s == 64 else (36, 60, 84, 96)):
                        take("kfs%03d_%03d_c%02d_k%02d" % (t, s, c, k), 2.5, k)
            print("kfslow done", flush=True)
            base()
        if "shape" in parts:
            # 段の長さが高さの差によるか（ディケイ 1 の時間 64 で 127 → 96・32・8・0）と、
            # アタック（初め 0 → 127）の長さが時間の値 32・48・64・80 でどうなるか
            amp(0x05, 64)
            amp(0x0B, 127)
            for L in (96, 32, 8, 0):
                amp(0x0A, L)
                amp(0x0B, L)
                time.sleep(0.05)
                take("sd%03d" % L, 3.0)
            base()
            amp(0x08, 0)
            for t in (32, 48, 64, 80):
                amp(0x04, t)
                time.sleep(0.05)
                take("sa%03d" % t, 4.0)
            print("shape done", flush=True)
            base()
        r.all_off()
        r.mode(3)
    finally:
        r.close()
    print("DONE", flush=True)


if __name__ == "__main__":
    main()
