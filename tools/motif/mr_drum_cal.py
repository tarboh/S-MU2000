"""ドラムの鍵の欄の効き方を実機で測る（2026-10-25）。
  python mr_drum_cal.py 置き場 [--kit 0] [--key 36] [--vsweep]
ボイスモードで PRE のドラム（63/32）を選び、編集バッファの 1 鍵（Data List の DRUM VOICE KEY、47・49・4A ee）を変えながら鳴らして録る。
はじめに「素の鍵」（レベル 127・ベロシティ感度 0・パン中央・EQ thru・カットオフ 255・レゾナンス 0・送り 0・インサーション thru）にし、
1 つずつ変える: ベロシティ感度 × ベロシティ、レベル、パン、レゾナンスとカットオフ、AEG（波形を 704 Sine にして）とノートオフのリリース。
送るのは Data List のパラメーターチェンジ・バンクとプログラムの選び・ノートだけ（保存しない）。終わったらマルチへ戻す（編集バッファは捨てられる）。
録ったものは cal.csv（名前と変えた欄）と WAV。"""
import csv
import os
import sys
import time

import mr_ref


def main():
    a = sys.argv[1:]
    d = a[0]
    kit = int(mr_ref.opt(a, "--kit", "0"))
    key = int(mr_ref.opt(a, "--key", "36"))
    ee = key - 24
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
    rows = []

    def k(block, off, *data):
        r.param(block, ee, off, *data)

    def base(wave=None):
        k(0x47, 0x03, 0)                   # Receive Note Off off
        k(0x47, 0x05, 0)                   # Alternate Group off
        if wave is not None:
            k(0x47, 0x06, wave >> 7, wave & 0x7F)
        k(0x47, 0x0B, 64)                  # パン中央
        k(0x47, 0x0C, 0)                   # ランダムパン 0
        k(0x47, 0x0D, 64)                  # オルタネートパン 0
        k(0x47, 0x0E, 0)                   # リバーブ・コーラスの送り 0
        k(0x47, 0x0F, 0)
        k(0x47, 0x11, 0)                   # インサーション thru
        r.param(0x47, ee, 0x20, 127)       # レベル 127
        r.param(0x47, ee, 0x21, 64)        # ベロシティ感度 0
        k(0x49, 0x01, 1, 127)              # カットオフ 255
        k(0x49, 0x03, 64)
        k(0x49, 0x04, 0)                   # レゾナンス 0
        k(0x4A, 0x00, 5)                   # EQ thru
        time.sleep(0.2)

    def take(name, vel=100, hold=0.8, tail=0.6, **info):
        r.all_off()
        time.sleep(0.15)
        r.one_note(name, note=key, vel=vel, hold=hold, tail=tail, pre=0.3)
        rows.append(dict(name=name, vel=vel, hold=hold, **info))

    try:
        r.all_off()
        r.param(0x00, 0x00, 0x34, 0)
        r.mode(0)
        r.program(63, 32, kit)
        for c in (7, 11):
            r.cc(c, 127)
        r.cc(10, 64)
        r.cc(91, 0)
        r.cc(93, 0)
        base()
        take("base_v100")
        if "--vsweep" in a:
            # ベロシティ感度を 8 刻みで（--vsweep。ほかは録らない）
            for s in list(range(0, 128, 8)) + [4, 12, 124, 127]:
                r.param(0x47, ee, 0x21, s)
                time.sleep(0.1)
                for v in (1, 20, 50, 80, 100, 127):
                    take("vs%03d_v%03d" % (s, v), vel=v, vsens=s, hold=0.5, tail=0.3)
            return
        # ベロシティ感度 × ベロシティ
        for s in (0, 32, 96, 127):
            r.param(0x47, ee, 0x21, s)
            time.sleep(0.1)
            for v in (20, 50, 80, 100, 127):
                take("vs%03d_v%03d" % (s, v), vel=v, vsens=s)
        base()
        for v in (20, 50, 80, 127):
            take("vs064_v%03d" % v, vel=v, vsens=64)
        # レベル
        for lv in (100, 64, 32, 16):
            r.param(0x47, ee, 0x20, lv)
            time.sleep(0.1)
            take("lv%03d" % lv, level=lv)
        base()
        # パン
        for p in (1, 16, 32, 48, 80, 96, 112, 127):
            k(0x47, 0x0B, p)
            time.sleep(0.1)
            take("pan%03d" % p, pan=p)
        base()
        # フィルター（レゾナンスとカットオフ）
        for cut, res in ((255, 16), (255, 64), (200, 0), (160, 0), (120, 0), (160, 64), (120, 64)):
            k(0x49, 0x01, cut >> 7, cut & 0x7F)
            k(0x49, 0x04, res)
            time.sleep(0.1)
            take("flt_c%03d_r%03d" % (cut, res), cutoff=cut, reso=res)
        base()
        # AEG とリリース（波形を 704 Sine にして、鳴り続ける音で）
        base(704)
        for name, at, d1, d1l, d2, noff, hold in (("aeg_hold", 0, 0, 127, 127, 0, 1.5),
                                                   ("aeg_d1_64", 0, 64, 64, 127, 0, 2.0),
                                                   ("aeg_d2_64", 0, 0, 127, 64, 0, 3.0),
                                                   ("aeg_at_64", 64, 0, 127, 127, 0, 1.5),
                                                   ("aeg_noff_on", 0, 0, 127, 127, 1, 0.8),
                                                   ("aeg_noff_off", 0, 0, 127, 127, 0, 0.8)):
            r.param(0x47, ee, 0x23, at)
            r.param(0x47, ee, 0x24, d1)
            r.param(0x47, ee, 0x25, d2)
            r.param(0x47, ee, 0x29, d1l)
            k(0x47, 0x03, noff)
            time.sleep(0.1)
            take(name, hold=hold, tail=2.0, at=at, d1=d1, d1l=d1l, d2=d2, noff=noff)
    finally:
        r.all_off()
        r.mode(3)
        r.close()
        with open(os.path.join(d, "cal.csv"), "w", newline="") as f:
            keys = sorted({x for row in rows for x in row})
            w = csv.DictWriter(f, fieldnames=keys, lineterminator="\n")
            w.writeheader()
            for row in rows:
                w.writerow(row)


if __name__ == "__main__":
    main()
