"""MOTIF-RACK ES のエレメントの LFO を測る録音（正弦波の波形 704、鍵 60、フィルター THRU）。
  python mr_lfo.py --dir 置き場 [--more]

  speed : 形 tri・音程の深さ 64 で、速さ 0〜63 を 4 刻み（ls??.wav）
  wave  : 速さ 32・音程の深さ 64 で、形 saw・tri・squ（lw?.wav）
  pmod  : 速さ 32 で、音程の深さ 16・32・64・96・127（lp???.wav）
  amod  : 速さ 32 で、音量の深さ 32・64・96・127（la???.wav）
  fmod  : 型 BEF12・カットオフ 128・速さ 16 で、フィルターの深さ 32・64・127（lf???.wav、ノイズの波形 705。谷の動きで見る）
  delay : 音程の深さ 64・速さ 32 で、ディレイ 0・32・64・96（ld???.wav）
  fade  : 同じで、フェード 0・32・64・96（lfd???.wav）
  sync  : キーオンでそろえる off / on（lk?_?.wav を 2 回ずつ）
  --more: ディレイ 8・16・48、フェード 8・16・48・80、のこぎり・四角のそろえた始まり（lk1w?.wav）、音程と音量を同時に揺らす（lpa.wav）
もうあるものは飛ばす。数を出すのは別。送るのはパラメーターチェンジとノートだけ（保存しない）。終わったら LFO を切り、マルチへ戻す。"""
import os
import sys
import time

import mr_ref


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"),
                   mr_ref.opt(a, "--dev", "0"), d)

    def lfo(off, v):
        r.el(0, 0x44, 0x40 + off, v)

    def take(name, hold=3.0):
        if not os.path.exists(os.path.join(d, name + ".wav")):
            r.one_note(name, note=60, hold=hold, tail=0.3, pre=0.4)

    def off_all():
        for o, v in ((0, 1), (1, 1), (2, 0), (3, 32), (4, 0), (5, 0), (6, 0), (7, 0)):
            lfo(o, v)
        time.sleep(0.05)

    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(704)
        r.el(0, 0x43, 0x00, 21)
        r.el(0, 0x43, 0x01, 1, 127)
        r.el(0, 0x43, 0x22, 64)
        off_all()
        if "--more" in a:
            # 2 回目: ディレイとフェードのほかの値、のこぎり・四角のそろえた始まり、音量と音程の揺れの位相
            lfo(5, 64)
            for dl in (8, 16, 48):
                lfo(2, dl)
                time.sleep(0.05)
                take("ld%03d" % dl, 5.0)
            lfo(2, 0)
            for fd in (8, 16, 48, 80):
                lfo(7, fd)
                time.sleep(0.05)
                take("lfd%03d" % fd, 5.0 if fd < 64 else 9.0)
            lfo(7, 0)
            lfo(3, 16)
            for w in (0, 2):
                lfo(0, w)
                time.sleep(0.05)
                take("lk1w%d" % w, 1.5)
            lfo(0, 1)
            lfo(3, 32)
            lfo(4, 64)
            time.sleep(0.05)
            take("lpa", 3.0)
            off_all()
            r.all_off()
            r.mode(3)
            return
        lfo(5, 64)
        for sp in list(range(0, 64, 4)) + [63]:
            lfo(3, sp)
            time.sleep(0.05)
            take("ls%02d" % sp, 6.0 if sp < 12 else 3.0)
        lfo(3, 32)
        for w in (0, 1, 2):
            lfo(0, w)
            time.sleep(0.05)
            take("lw%d" % w)
        lfo(0, 1)
        for dp in (16, 32, 64, 96, 127):
            lfo(5, dp)
            time.sleep(0.05)
            take("lp%03d" % dp)
        lfo(5, 0)
        for dp in (32, 64, 96, 127):
            lfo(4, dp)
            time.sleep(0.05)
            take("la%03d" % dp)
        lfo(4, 0)
        lfo(5, 64)
        for dl in (0, 32, 64, 96):
            lfo(2, dl)
            time.sleep(0.05)
            take("ld%03d" % dl, 5.0)
        lfo(2, 0)
        for fd in (0, 32, 64, 96):
            lfo(7, fd)
            time.sleep(0.05)
            take("lfd%03d" % fd, 5.0)
        lfo(7, 0)
        for sync in (0, 1):
            lfo(1, sync)
            time.sleep(0.05)
            for i in (1, 2):
                take("lk%d_%d" % (sync, i), 1.5)
        lfo(1, 1)
        lfo(5, 0)
        # フィルターの深さ: ノイズで谷の動きを見る
        r.el(0, 0x41, 0x03, 705 >> 7, 705 & 0x7F)
        r.el(0, 0x43, 0x00, 21)
        time.sleep(0.15)
        take("lf_thru", 2.0)
        r.el(0, 0x43, 0x00, 12)
        r.el(0, 0x43, 0x01, 1, 0)
        lfo(3, 16)
        for dp in (32, 64, 127):
            lfo(6, dp)
            time.sleep(0.05)
            take("lf%03d" % dp, 4.0)
        off_all()
        r.el(0, 0x43, 0x00, 21)
        r.el(0, 0x43, 0x01, 1, 127)
        r.all_off()
        r.mode(3)
    finally:
        r.close()
    print("DONE", flush=True)


if __name__ == "__main__":
    main()
