"""MOTIF-RACK ES の波形を、区間ごと・ベロシティの層ごとに 1 音ずつ長く録る（サンプリング方式のソフトシンセの素材）。
  python mr_capture.py --dir 置き場 --capture 取ったものの置き場 [--waves 一覧.txt] [--hold 6] [--hours 8] [--min-free-gb 25]

区間は zones_all.csv の境目（折り返しより上は除く）、層は velzones.csv の境目（鍵 60 で見たもの）。
区間の真ん中の鍵、層の真ん中のベロシティで、雛形の音色（フィルター THRU・キーフォロー 0）を hold 秒鳴らして録る（C_w????_k???_v???.wav）。
録ったものの一覧は capture.csv に足していく。もうあるものは飛ばすので、途中で止めて続きから回せる。
置き場に STOP を置くと今の 1 音を録り終えた所で止まる。ディスクの空きが min-free-gb を切っても止まる。
送るのはパラメーターチェンジとノートだけ（保存しない）。終わったらマルチへ戻す。録った音は配らないこと（ヤマハの波形）。"""
import csv
import os
import shutil
import sys
import time

import mr_ref
import mr_sweep


def zones_of(row):
    b = [int(v) for v in row["boundaries"].split()] if row["boundaries"] else []
    top = int(row["wrap_from"]) - 1 if row.get("wrap_from") else 128
    edges = [0] + [v for v in b if v < top] + [top]
    return [(edges[i], edges[i + 1]) for i in range(len(edges) - 1) if edges[i + 1] > edges[i]]


def layers_of(row):
    b = [int(v) for v in row["bounds"].split()] if row and row["bounds"] else []
    edges = [1] + b + [128]
    return [(edges[i], edges[i + 1]) for i in range(len(edges) - 1) if edges[i + 1] > edges[i]]


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    cap = mr_ref.opt(a, "--capture", "../../roms/motif-rack-es/capture")
    wl = mr_ref.opt(a, "--waves", os.path.join(cap, "waves_used.txt"))
    hold = float(mr_ref.opt(a, "--hold", "6"))
    hours = float(mr_ref.opt(a, "--hours", "8"))
    min_free = float(mr_ref.opt(a, "--min-free-gb", "25"))
    zones = {int(r["wave"]): r for r in csv.DictReader(open(os.path.join(cap, "zones_all.csv")))}
    vz = {int(r["wave"]): r for r in csv.DictReader(open(os.path.join(cap, "velzones.csv")))}
    waves = [int(l.split(",")[0]) for l in open(wl) if l.strip() and l.split(",")[0].strip().isdigit()]
    os.makedirs(d, exist_ok=True)
    log = os.path.join(d, "capture.csv")
    new_log = not os.path.exists(log)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"),
                   mr_ref.opt(a, "--dev", "0"), d)
    t_end = time.time() + hours * 3600
    done = 0
    try:
        with open(log, "a", newline="") as lf:
            w_csv = csv.writer(lf, lineterminator="\n")
            if new_log:
                w_csv.writerow(["wave", "key_lo", "key_hi", "key", "vel_lo", "vel_hi", "vel", "file"])
            r.all_off()
            r.mode(0)
            r.program(63, 0, 0)
            r.template(waves[0])
            r.el(0, 0x43, 0x00, 21)
            r.el(0, 0x43, 0x01, 1, 127)
            r.el(0, 0x43, 0x22, 64)
            r.el(0, 0x43, 0x06, 64)
            cur = None
            for w in waves:
                if w not in zones:
                    continue
                for klo, khi in zones_of(zones[w]):
                    key = (klo + khi - 1) // 2
                    for vlo, vhi in layers_of(vz.get(w)):
                        vel = max(1, min(127, (vlo + vhi - 1) // 2))
                        name = "C_w%04d_k%03d_v%03d" % (w, key, vel)
                        path = os.path.join(d, name + ".wav")
                        if os.path.exists(path):
                            continue
                        if time.time() > t_end or os.path.exists(os.path.join(d, "STOP")):
                            raise StopIteration
                        if shutil.disk_usage(d).free / 1e9 < min_free:
                            print("ディスクの空きが %.0fGB を切ったので止める" % min_free, flush=True)
                            raise StopIteration
                        if cur != w:
                            r.el(0, 0x41, 0x03, w >> 7, w & 0x7F)
                            time.sleep(0.15)
                            cur = w
                        L, R = r.record("_tmp", 0.3 + hold + 0.3,
                                        [(0.3, lambda k=key, v=vel: r.note_on(k, v)), (0.3 + hold, lambda k=key: r.note_off(k))])
                        mr_sweep.write_wav24(path, L, R)
                        os.remove(os.path.join(d, "_tmp.wav"))
                        w_csv.writerow([w, klo, khi - 1, key, vlo, vhi - 1, vel, name + ".wav"])
                        lf.flush()
                        done += 1
                print("wave %d done (%d takes so far)" % (w, done), flush=True)
    except StopIteration:
        pass
    finally:
        try:
            r.all_off()
            r.mode(3)
        finally:
            r.close()
    print("DONE %d takes" % done, flush=True)


if __name__ == "__main__":
    main()
