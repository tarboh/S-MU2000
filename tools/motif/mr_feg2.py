"""MOTIF-RACK ES の FEG の残り（ホールド、ベロシティ感度、時間のキーフォロー）を測る録音（ノイズの波形 705、型 BEF12・カットオフ 128）。
  python mr_feg2.py --dir 置き場 [--part hold,lvel,tvel,tkf]

  hold : 高さ hold −64 → attack +64（アタックの時間 0）、ホールドの時間 T（hd???.wav）。谷が跳ぶまでの時間を見る
  lvel : 高さを全部 +32・深さ +32 にして、高さのベロシティ感度（0x20）× ベロシティ（lv???_c?_v???.wav、c はカーブ 0x21）
  tvel : アタック（−64 → +64、時間 64）で、時間のベロシティ感度（0x1F、対象 0x1E = ALL）× ベロシティ（tv???_v???.wav）
  tkf  : 同じアタックで、時間のキーフォロー（0x30、中心の鍵 0x31 = 60）× 鍵（tk???_k??.wav）。鍵ごとの素通しは thru_k??.wav
  lvel2: lvel をベロシティ 8 刻みで（カーブ 0-4 × 感度 127、カーブ 0 × 感度 96・80。lw???_c?_v???.wav）
  tvel2: tvel を小さい感度 72・80・96 で（tw???_v???.wav）
  tvseg: 時間のベロシティ感度が効く段（0x1E = 0〜4）を、ディケイ 1 とアタックで（tvs_seg?.wav・tvsa_seg?.wav）
  tkf2 : 時間のキーフォローを中心 0・60、感度 84・96、鍵 24〜96 で（tk2_s???_c???_k??.wav）
  lvel3: 深さ 127 で高さ +16・+32・−32・−64、感度 127・カーブ 2、ベロシティ 1・40・64・100・127（l3[pm]???_v???.wav）
基準は thru.wav・thru_v???.wav。mr_feg.py と同じく、ほかの感度とキーフォローは 0 にする。
もうあるものは飛ばす。数を出すのは別。送るのはパラメーターチェンジとノートだけ（保存しない）。終わったらマルチへ戻す。"""
import os
import sys
import time

import mr_ref

VELS = (1, 32, 64, 100, 127)


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    parts = mr_ref.opt(a, "--part", "hold,lvel,tvel,tkf").split(",")
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"),
                   mr_ref.opt(a, "--dev", "0"), d)

    def take(name, hold, key=60, vel=100):
        if not os.path.exists(os.path.join(d, name + ".wav")):
            r.record(name, 0.4 + hold + 0.8, [(0.4, lambda: r.note_on(key, vel)), (0.4 + hold, lambda: r.note_off(key))])

    def lv(off, v):
        x = 128 + v
        r.el(0, 0x43, off, x >> 7, x & 0x7F)

    def feg(times, levels, depth):
        for i, t in enumerate(times):
            r.el(0, 0x43, 0x0E + i, t)
        for i, v in enumerate(levels):
            lv(0x13 + 2 * i, v)
        r.el(0, 0x43, 0x1D, depth)

    def base():
        r.el(0, 0x43, 0x05, 0)
        r.el(0, 0x43, 0x06, 64)
        r.el(0, 0x43, 0x07, 0, 0)
        r.el(0, 0x43, 0x22, 64)
        r.el(0, 0x43, 0x2F, 64)
        r.el(0, 0x43, 0x03, 64)
        r.el(0, 0x43, 0x0A, 0, 0)
        r.el(0, 0x43, 0x0C, 1, 122)
        r.el(0, 0x43, 0x1E, 4)                      # 時間のベロシティの対象 ALL
        r.el(0, 0x43, 0x1F, 64)
        r.el(0, 0x43, 0x20, 64)
        r.el(0, 0x43, 0x21, 0)                      # カーブ 0
        r.el(0, 0x43, 0x30, 64)
        r.el(0, 0x43, 0x31, 60)                     # 時間のキーフォローの中心 C3
        for i in range(4):
            r.el(0, 0x43, 0x27 + 2 * i, 1, 0)
        feg((0, 0, 0, 0, 0), (0, 0, 0, 0, 0), 64)
        r.el(0, 0x43, 0x00, 12)
        r.el(0, 0x43, 0x01, 1, 0)
        time.sleep(0.05)

    def thru(name, key=60, vel=100):
        r.el(0, 0x43, 0x00, 21)
        r.el(0, 0x43, 0x01, 1, 127)
        time.sleep(0.05)
        take(name, 1.2, key, vel)
        r.el(0, 0x43, 0x00, 12)
        r.el(0, 0x43, 0x01, 1, 0)
        time.sleep(0.05)

    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(705)
        base()
        thru("thru")
        if "hold" in parts:
            for T in (32, 48, 64, 72, 80):
                feg((T, 0, 0, 0, 0), (-64, 64, 64, 64, 64), 127)
                time.sleep(0.05)
                take("hd%03d" % T, 4.0)
            print("hold done", flush=True)
            base()
        if "lvel" in parts:
            for v in VELS:
                thru("thru_v%03d" % v, 60, v)
            jobs = [(s, 0) for s in (0, 32, 96, 127)] + [(127, c) for c in (1, 2, 3, 4)]
            for s, c in jobs:
                feg((0, 0, 0, 0, 0), (32,) * 5, 96)
                r.el(0, 0x43, 0x20, s)
                r.el(0, 0x43, 0x21, c)
                time.sleep(0.05)
                for v in VELS:
                    take("lv%03d_c%d_v%03d" % (s, c, v), 1.2, 60, v)
            print("lvel done", flush=True)
            base()
        if "tvel" in parts:
            for s in (0, 127):
                feg((0, 64, 0, 0, 0), (-64, 64, 64, 64, 64), 127)
                r.el(0, 0x43, 0x1F, s)
                time.sleep(0.05)
                for v in (1, 64, 127):
                    take("tv%03d_v%03d" % (s, v), 4.0, 60, v)
            print("tvel done", flush=True)
            base()
        if "lvel2" in parts:
            vs = [1] + list(range(8, 128, 8)) + [127]
            for v in vs:
                thru("thru_v%03d" % v, 60, v)
            for s, c in [(127, c) for c in range(5)] + [(96, 0), (80, 0)]:
                feg((0, 0, 0, 0, 0), (32,) * 5, 96)
                r.el(0, 0x43, 0x20, s)
                r.el(0, 0x43, 0x21, c)
                time.sleep(0.05)
                for v in vs:
                    take("lw%03d_c%d_v%03d" % (s, c, v), 1.0, 60, v)
            print("lvel2 done", flush=True)
            base()
        if "lvel3" in parts:
            # 2026-10-24: 大きい FEG でも倍率が同じか（BalladEP の e3 は高さ 97・深さ 127・感度 127・カーブ 2）。深さ 127 で高さ +16・+32・−32・−64
            vs = (1, 40, 64, 100, 127)
            for v in vs:
                thru("thru_v%03d" % v, 60, v)
            for h in (16, 32, -32, -64):
                feg((0, 0, 0, 0, 0), (h,) * 5, 127)
                r.el(0, 0x43, 0x20, 127)
                r.el(0, 0x43, 0x21, 2)
                time.sleep(0.05)
                for v in vs:
                    take("l3%s%03d_v%03d" % ("p" if h > 0 else "m", abs(h), v), 1.0, 60, v)
            print("lvel3 done", flush=True)
            base()
        if "tvel2" in parts:
            for s in (72, 80, 96):
                feg((0, 64, 0, 0, 0), (-64, 64, 64, 64, 64), 127)
                r.el(0, 0x43, 0x1F, s)
                time.sleep(0.05)
                for v in VELS:
                    take("tw%03d_v%03d" % (s, v), 4.0, 60, v)
            print("tvel2 done", flush=True)
            base()
        if "tkf" in parts:
            for k in (36, 84):
                thru("thru_k%02d" % k, k)
            for s in (96, 127):
                feg((0, 64, 0, 0, 0), (-64, 64, 64, 64, 64), 127)
                r.el(0, 0x43, 0x30, s)
                time.sleep(0.05)
                for k in (36, 60, 84):
                    take("tk%03d_k%02d" % (s, k), 4.0, k)
            print("tkf done", flush=True)
            base()
        if "tvseg" in parts:
            # 2026-10-24: 時間のベロシティ感度が効く段（0x1E）。ディケイ 1（+64 → −64、時間 64）を、感度 103 で段の指定 0〜4、ベロシティ 100
            feg((0, 0, 64, 0, 0), (64, 64, -64, -64, -64), 127)
            r.el(0, 0x43, 0x1F, 64)
            time.sleep(0.05)
            take("tvs_ref", 3.0, 60, 100)
            for seg in range(5):
                r.el(0, 0x43, 0x1E, seg)
                r.el(0, 0x43, 0x1F, 103)
                time.sleep(0.05)
                take("tvs_seg%d" % seg, 3.0, 60, 100)
            # アタック（−64 → +64、時間 64）でも同じ
            feg((0, 64, 0, 0, 0), (-64, 64, 64, 64, 64), 127)
            r.el(0, 0x43, 0x1F, 64)
            time.sleep(0.05)
            take("tvsa_ref", 3.0, 60, 100)
            for seg in range(5):
                r.el(0, 0x43, 0x1E, seg)
                r.el(0, 0x43, 0x1F, 103)
                time.sleep(0.05)
                take("tvsa_seg%d" % seg, 3.0, 60, 100)
            print("tvseg done", flush=True)
            base()
        if "tkf2" in parts:
            # 2026-10-24: 中心の鍵から遠いとき（Oxide は感度 84・中心 0 で鍵 72）。中心 0・60、感度 84・96、鍵 24〜96
            for k in (24, 48, 72, 96):
                thru("thru_k%02d" % k, k)
            for s_, c in ((84, 0), (84, 60), (96, 0)):
                feg((0, 64, 0, 0, 0), (-64, 64, 64, 64, 64), 127)
                r.el(0, 0x43, 0x30, s_)
                r.el(0, 0x43, 0x31, c)
                time.sleep(0.05)
                for k in (24, 48, 72, 96):
                    take("tk2_s%03d_c%03d_k%02d" % (s_, c, k), 4.0, k)
            r.el(0, 0x43, 0x31, 60)
            print("tkf2 done", flush=True)
            base()
        r.all_off()
        r.mode(3)
    finally:
        r.close()
    print("DONE", flush=True)


if __name__ == "__main__":
    main()
