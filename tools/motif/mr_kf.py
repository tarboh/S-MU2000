"""MOTIF-RACK ES のエレメントのフィルターの、キーフォローと HPF を測る録音（ノイズの波形 705）。
  python mr_kf.py --dir 置き場 [--part kf,hpf,hpfkf]

  kf    : 型 BEF12・カットオフ 128 で、カットオフのキーフォロー（0x22）× 鍵を録る（k??_f???.wav）。鍵ごとの素通しは thru_k??.wav
  hpf   : 型 LPF6・カットオフ 255 にして、HPF のカットオフ（0x07、0-255）を 2 刻みで鍵 60 で録る（h???.wav）
  hpfkf : 同じ設定・HPF のカットオフ 128 で、HPF のキーフォロー（0x2F）× 鍵を録る（hk??_f???.wav）
  基準は同じ設定で HPF を 0 にした音（lpref_k??.wav）。HPF が効くのは型が LPF12 と LPF6 のときだけ（2026-10-19 に確かめた）

条件は mr_cutoff.py と同じ（レゾナンス 0、スケーリング 0）。もうあるものは飛ばす。数を出すのは別。
送るのはパラメーターチェンジとノートだけ。ボイスモードの今の音色を作り変える（保存しない）。終わったらマルチへ戻す。"""
import os
import sys
import time

import mr_cutoff
import mr_ref

HOLD = mr_cutoff.HOLD
KEYS = (36, 48, 60, 72, 84)
KFS = (0, 16, 32, 48, 64, 80, 96, 112, 127)
HKFS = (0, 32, 64, 96, 127)


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    parts = mr_ref.opt(a, "--part", "kf,hpf,hpfkf").split(",")
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"),
                   mr_ref.opt(a, "--dev", "0"), d)

    def take(name, key):
        if not os.path.exists(os.path.join(d, name + ".wav")):
            r.one_note(name, note=key, hold=HOLD, tail=0.3, pre=0.4)

    def base():
        r.el(0, 0x43, 0x05, 0)                      # レゾナンス 0
        r.el(0, 0x43, 0x07, 0, 0)                   # HPF のカットオフ 0
        r.el(0, 0x43, 0x22, 64)                     # カットオフのキーフォロー 0
        r.el(0, 0x43, 0x2F, 64)                     # HPF のキーフォロー 0
        for off in (0x27, 0x29, 0x2B, 0x2D):
            r.el(0, 0x43, off, 1, 0)                # カットオフのスケーリングのずらし 0（128）
        r.el(0, 0x43, 0x00, 21)                     # THRU
        r.el(0, 0x43, 0x01, 1, 127)
        time.sleep(0.05)

    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(705)
        base()
        for k in KEYS:
            take("thru_k%02d" % k, k)
        if "kf" in parts:
            r.el(0, 0x43, 0x00, 12)
            r.el(0, 0x43, 0x01, 1, 0)               # カットオフ 128
            for kf in KFS:
                r.el(0, 0x43, 0x22, kf)
                time.sleep(0.05)
                for k in KEYS:
                    take("k%02d_f%03d" % (k, kf), k)
            print("kf done", flush=True)
            base()
        if "hpf" in parts or "hpfkf" in parts:
            r.el(0, 0x43, 0x00, 5)                  # LPF6（HPF が効くのは LPF12 と LPF6 だけ）
            r.el(0, 0x43, 0x01, 1, 127)             # カットオフ 255
            time.sleep(0.05)
            for k in KEYS:
                take("lpref_k%02d" % k, k)
        if "hpf" in parts:
            for c in range(0, 256, 2):
                r.el(0, 0x43, 0x07, c >> 7, c & 0x7F)
                time.sleep(0.05)
                take("h%03d" % c, 60)
            print("hpf done", flush=True)
            r.el(0, 0x43, 0x07, 0, 0)
        if "hpfkf" in parts:
            r.el(0, 0x43, 0x07, 1, 0)               # HPF のカットオフ 128
            for kf in HKFS:
                r.el(0, 0x43, 0x2F, kf)
                time.sleep(0.05)
                for k in KEYS:
                    take("hk%02d_f%03d" % (k, kf), k)
            print("hpfkf done", flush=True)
            base()
        r.all_off()
        r.mode(3)
    finally:
        r.close()
    print("DONE", flush=True)


if __name__ == "__main__":
    main()
