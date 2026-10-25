"""1 音以外の鳴らし方を正弦波の雛形で測る録音（2026-10-25）。
  python mr_multi.py --dir 置き場 [--part chord,timing,mono,porta,steal]
  chord : 鍵 60・64・67 を 1 つずつ（ch_k??）と、3 つ同時（ch_chord）。8 音の固まり（ch_cluster、鍵 60〜74 の白鍵）とその 1 つずつも
  timing: 0.4 秒おきに、押さえる長さ 2・5・10・20・30・50・80・120ms の音（tm_hold）と、2ms 刻みでずらした頭（tm_onset、0.3 秒 + k × 2.0ms）
  mono  : モノ（40 00 03 = 0）とポリで、鍵 60 を押さえたまま 67 を重ね、67 を離し、60 を離す（mn_*）。アタック 80 で EG のやり直しが見える。
          同じ鍵の扱い（40 00 02）0・1 で、鍵 60 を重ねて 2 回（sn_*）
  porta : ポルタメント（40 00 08 = 1）。時間 32・64・96、モード（07）0・1、時間のモード（0A）0〜3 で、60 → 72 を重ねる（レガート）と離してから（pt_*）
  porta2: フルタイム・モノで、60 を離してから 4・12・24 半音上へ。時間の値 0〜127（時間のモード 0）と、32・64・96 × 時間のモード 1〜3（p2_*）
  legato: アタック 80・モノで重ねて弾き、ポルタメントなし・フィンガード・フルタイムで EG がやり直すか（lg_*）
  steal : エレメント 2 も同じ正弦波で入れ、鍵 36 から 65 鍵を 30ms おきに押さえたまま（2 エレメント × 65 = 130 音）。その後 1 秒で全部離す（st_*）
送るのはパラメーターチェンジ・ノートだけ（保存しない）。各本の前に All Sound Off。終わったらマルチへ。"""
import os
import sys
import time

import mr_ctrl as M
import mr_ref


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    parts = mr_ref.opt(a, "--part", "chord,timing,mono,porta,steal").split(",")
    os.makedirs(d, exist_ok=True)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)

    def common(off, v):
        r.param(0x40, 0x00, off, v)

    def take(name, sec, ev):
        if os.path.exists(os.path.join(d, name + ".wav")):
            return
        r.all_off()
        time.sleep(0.2)
        r.record(name, sec, ev)
        print(name, "録った", flush=True)

    def on(k, v=100):
        return lambda: r.note_on(k, v)

    def off(k):
        return lambda: r.note_off(k)

    def base(attack=0):
        M.setup(r, "sin")
        r.el(0, 0x41, 0x28, 0 if attack else 127)
        r.el(0, 0x41, 0x24, attack)
        common(0x03, 1)          # ポリ
        common(0x02, 0)
        common(0x08, 0)          # ポルタメント切
        M.reset_ctrls(r)
        time.sleep(0.05)

    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        if "chord" in parts:
            base()
            for k in (60, 64, 67):
                take("ch_k%02d" % k, 1.5, [(0.3, on(k)), (1.1, off(k))])
            take("ch_chord", 1.5, [(0.3, on(60)), (0.3, on(64)), (0.3, on(67)), (1.1, off(60)), (1.1, off(64)), (1.1, off(67))])
            ks = (60, 62, 64, 65, 67, 69, 71, 72)
            take("ch_cluster", 1.5, [(0.3, on(k)) for k in ks] + [(1.1, off(k)) for k in ks])
            for k in ks:
                take("ch1_k%02d" % k, 1.5, [(0.3, on(k)), (1.1, off(k))])
        if "timing" in parts:
            base()
            hs = (0.002, 0.005, 0.010, 0.020, 0.030, 0.050, 0.080, 0.120)
            ev = []
            for i, h in enumerate(hs):
                t = 0.3 + 0.4 * i
                ev += [(t, on(60)), (t + h, off(60))]
            take("tm_hold", 0.3 + 0.4 * len(hs) + 0.3, ev)
            ev = []
            for i in range(10):
                t = 0.3 + 0.3 * i + 0.002 * i
                ev += [(t, on(60)), (t + 0.1, off(60))]
            take("tm_onset", 3.6, ev)
        if "mono" in parts:
            for mono in (0, 1):
                base(attack=80)
                common(0x03, mono)
                take("mn_m%d" % mono, 3.2, [(0.3, on(60)), (1.0, on(67)), (1.7, off(67)), (2.4, off(60))])
                # 重ねずに離してから（モノでもやり直すか）
                take("mn_m%d_sep" % mono, 2.4, [(0.3, on(60)), (0.9, off(60)), (1.0, on(67)), (1.8, off(67))])
            for same in (0, 1):
                base(attack=80)
                common(0x02, same)
                take("sn_s%d" % same, 2.6, [(0.3, on(60)), (1.0, on(60)), (1.7, off(60)), (1.9, off(60))])
        if "porta" in parts:
            for mono in (0, 1):
                for mode in (0, 1):
                    for tm in (0, 1, 2, 3):
                        for pt in (32, 64, 96):
                            if tm and pt != 64:
                                continue
                            base()
                            common(0x03, mono)
                            common(0x08, 1)
                            common(0x07, mode)
                            common(0x09, pt)
                            common(0x0A, tm)
                            time.sleep(0.05)
                            nm = "pt_m%d_md%d_tm%d_t%03d" % (mono, mode, tm, pt)
                            take(nm + "_leg", 3.2, [(0.3, on(60)), (1.0, on(72)), (1.05, off(60)), (2.6, off(72))])
                            take(nm + "_sep", 3.2, [(0.3, on(60)), (0.9, off(60)), (1.0, on(72)), (2.6, off(72))])
            base()
        if "porta2" in parts:
            # フルタイム（07 = 1）・モノで、鍵 60 を離してから幅 4・12・24 半音上を弾く。時間の値を 0〜127、時間のモード 0〜3
            for tm in (0, 1, 2, 3):
                for pt in ((0, 16, 32, 48, 64, 80, 96, 112, 127) if tm == 0 else (32, 64, 96)):
                    for iv in (4, 12, 24):
                        base()
                        common(0x03, 0)
                        common(0x08, 1)
                        common(0x07, 1)
                        common(0x09, pt)
                        common(0x0A, tm)
                        time.sleep(0.05)
                        take("p2_tm%d_t%03d_i%02d" % (tm, pt, iv), 4.4, [(0.3, on(60)), (0.9, off(60)), (1.0, on(60 + iv)), (3.8, off(60 + iv))])
            base()
        if "legato" in parts:
            # アタック 80 で、モノ・ポルタメントあり（フィンガード・フルタイム）・なしで重ねて弾き、EG がやり直すか（lg_*）
            for sw, mode in ((0, 0), (1, 0), (1, 1)):
                base(attack=80)
                common(0x03, 0)
                common(0x08, sw)
                common(0x07, mode)
                common(0x09, 48)
                time.sleep(0.05)
                take("lg_sw%d_md%d" % (sw, mode), 3.2, [(0.3, on(60)), (1.0, on(67)), (1.7, off(67)), (2.4, off(60))])
            base()
        if "steal" in parts:
            # エレメント 2〜ne も同じ正弦波で入れ（雛形はエレメント 2〜4 を切っている）、鍵 36 から n 鍵を 30ms おきに押さえたまま、1 秒で全部離す
            # 重ねても出口で頭打ちにならないように、エレメントの音量 20・CC7 64（1 音 −49dB 前後。前は 2 鍵で 1.0 に届いていた）
            for ne, ns in ((2, (60, 64, 65)), (4, (32, 33, 40, 48, 65))):
                base()
                r.el(0, 0x41, 0x20, 20)
                r.cc(7, 64)
                for e in range(1, ne):
                    r.template(704, e, others_off=False)
                    r.el(e, 0x43, 0x00, 21)                  # THRU
                for e in range(ne):
                    r.el(e, 0x41, 0x20, 20)
                    r.el(e, 0x43, 0x00, 21)
                r.cc(7, 64)
                for n in ns:
                    ev = [(0.3 + 0.03 * i, on(36 + i)) for i in range(n)]
                    tend = 0.3 + 0.03 * n + 1.0
                    ev += [(tend, off(36 + i)) for i in range(n)]
                    take("st_n%03d" % n if ne == 2 else "st4_n%03d" % n, tend + 0.6, ev)
            # 4 エレメントのまま、鍵を全音おき（36・38・…）にして読み分けやすくし、鍵の数を 1 つずつ増やす（st4w_n??）
            for n in (31, 32, 33, 34, 35, 36, 38, 40, 46):
                ev = [(0.3 + 0.05 * i, on(36 + 2 * i)) for i in range(n)]
                tend = 0.3 + 0.05 * n + 1.2
                ev += [(tend, off(36 + 2 * i)) for i in range(n)]
                take("st4w_n%03d" % n, tend + 0.6, ev)
            # 高い鍵から下へ（鍵 114・112・…・36 の 40 鍵）。最初の音を残すのか、いちばん低い音を残すのか（st4d_n040）
            ev = [(0.3 + 0.05 * i, on(114 - 2 * i)) for i in range(40)]
            tend = 0.3 + 0.05 * 40 + 1.2
            ev += [(tend, off(114 - 2 * i)) for i in range(40)]
            take("st4d_n040", tend + 0.6, ev)
            # 同じ鍵を重ねられるようにして（40 00 02 = 1）、鍵 36 を 1 回押さえたあと、鍵 37〜99 を 3 回ずつ 10ms おきに重ねる（4 エレメント × 190 音）
            common(0x02, 1)
            ev = [(0.3, on(36))]
            t = 0.5
            for rep in range(3):
                for k in range(37, 100):
                    ev.append((t, on(k)))
                    t += 0.01
            ev += [(t + 0.8, off(k)) for k in range(36, 100)] * 3
            take("st4_multi", t + 1.6, ev)
            common(0x02, 0)
            r.cc(7, 127)
    finally:
        r.all_off()
        r.cc(7, 127)
        for e in (1, 2, 3):
            r.el(e, 0x41, 0x00, 0)
        common(0x03, 1)
        common(0x08, 0)
        r.mode(3)
        r.close()


if __name__ == "__main__":
    main()
