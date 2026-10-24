"""MOTIF-RACK ES の波形を、区間ごと・ベロシティの層ごとに 1 音ずつ長く録る（サンプリング方式のソフトシンセの素材）。
  python mr_capture.py --dir 置き場 --capture 取ったものの置き場 [--waves 一覧.txt] [--hold 6] [--hours 8] [--min-free-gb 25] [--split-lowest]

鍵 48 より下の区間は、低いほどループが長くなるので、押さえる長さを 1 オクターブ下がるごとに 2 倍にする（24 秒まで）。
区間の境目は zones_all.csv の境目と「~」の所。12 鍵より広い区間は 12 鍵ずつに分けて録る（読む速さが 3 倍で音程が折り返すので、真ん中の 1 本から区間の全部は作れない。いちばん下の区間は除く）。
層は velzones.csv の相関が 0.98 を下回る所と、velkeys.csv（6 つの鍵）の境目を合わせたもの。
前の分け方で録ったものは残り、mr_patch.py は鍵とベロシティの幅が狭い区間を先に使う。

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


SPLIT_LOWEST = False   # --split-lowest
MAX_ZONE = 12          # これより広い区間は 12 鍵ずつに分けて録る（3 倍で折り返すので、1 本から広くは作れない）
LAYER_CORR = 0.98      # 隣のベロシティとの相関がこれを下回る所を層の境目とする（velzones.csv の 0.97 より厳しく）
VELSCAN = (1, 16, 32, 48, 64, 80, 96, 104, 112, 120, 127)   # mr_velscan.py のベロシティ


def zones_of(row):
    b = [int(v) for v in row["boundaries"].split()] if row["boundaries"] else []
    # 「~」（S と見たが相関 0.9〜0.95）も境目にする。波形 141 の鍵 48 / 49 は ~ で、実機の鍵 48 は鍵 49 で録ったものを半音下げても合わなかった
    first = int(row.get("first_sounding_key") or 0)
    b = sorted(set(b) | {first + i + 1 for i, c in enumerate(row.get("pattern") or "") if c == "~"})
    top = int(row["wrap_from"]) - 1 if row.get("wrap_from") else 128
    edges = [0] + [v for v in b if v < top] + [top]
    out = []
    for i in range(len(edges) - 1):
        lo, hi = edges[i], edges[i + 1]
        if hi <= lo:
            continue
        if hi - lo <= MAX_ZONE or (lo == 0 and not SPLIT_LOWEST):
            out.append((lo, hi))                  # いちばん下の区間は下へ読むだけなので分けない（--split-lowest なら分ける）
            continue
        if lo == 0:
            # いちばん下の区間も、上から 12 鍵ずつに分けて録り足す（真ん中の鍵の録音を 15 半音上げると、実機より暗かった。
            # Flangewall の鍵 48 = 波形 573 の鍵 36。2026-10-25）。前の真ん中の 1 本も残す
            out.append((lo, hi))
            for a in range(hi - MAX_ZONE, 0, -MAX_ZONE):
                if (a + min(hi, a + MAX_ZONE) - 1) // 2 > (lo + hi - 1) // 2:
                    out.append((a, min(hi, a + MAX_ZONE)))
            continue
        for a in range(lo, hi, MAX_ZONE):
            out.append((a, min(hi, a + MAX_ZONE)))
    return out


def layers_of(row, extra=()):
    b = set(int(v) for v in row["bounds"].split()) if row and row["bounds"] else set()
    if row and row.get("adjacent_corr"):
        cs = [float(v) for v in row["adjacent_corr"].split()]
        b |= {VELSCAN[i + 1] for i, c in enumerate(cs) if c < LAYER_CORR and i + 1 < len(VELSCAN)}
    b |= set(extra)
    edges = [1] + sorted(v for v in b if 1 < v < 128) + [128]
    return [(edges[i], edges[i + 1]) for i in range(len(edges) - 1) if edges[i + 1] > edges[i]]


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    cap = mr_ref.opt(a, "--capture", "../../roms/motif-rack-es/capture")
    wl = mr_ref.opt(a, "--waves", os.path.join(cap, "waves_used.txt"))
    hold = float(mr_ref.opt(a, "--hold", "6"))
    global SPLIT_LOWEST
    SPLIT_LOWEST = "--split-lowest" in a
    hours = float(mr_ref.opt(a, "--hours", "8"))
    min_free = float(mr_ref.opt(a, "--min-free-gb", "25"))
    zones = {int(r["wave"]): r for r in csv.DictReader(open(os.path.join(cap, "zones_all.csv")))}
    vz = {int(r["wave"]): r for r in csv.DictReader(open(os.path.join(cap, "velzones.csv")))}
    vk = {}                                         # 6 つの鍵で見た層の境目（mr_velscan.py --key、velkeys.csv）も合わせる
    if os.path.exists(os.path.join(cap, "velkeys.csv")):
        for r in csv.DictReader(open(os.path.join(cap, "velkeys.csv"))):
            vk[int(r["wave"])] = {int(v) for k, x in r.items() if k.endswith("_bounds") and x for v in x.split()}
    waves = [int(l.split(",")[0]) for l in open(wl) if l.strip() and l.split(",")[0].strip().isdigit()]
    ap = os.path.join(cap, "wave_alias.csv")        # 同じサンプルを共有する波形は録らない（mr_alias.py）。
    if os.path.exists(ap):                          # ただし層の分かれ方が元と違えば録る（下の層だけ同じで、上の層が別のものがある。276 と 270 など）
        same = {int(r["wave"]): int(r["same_as"]) for r in csv.DictReader(open(ap))}
        waves = [w for w in waves if w not in same
                 or layers_of(vz.get(w), vk.get(w, ())) != layers_of(vz.get(same[w]), vk.get(same[w], ()))]
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
                w_csv.writerow(["wave", "key_lo", "key_hi", "key", "vel_lo", "vel_hi", "vel", "file", "hold"])
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
                    for vlo, vhi in layers_of(vz.get(w), vk.get(w, ())):
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
                        h = hold if key >= 48 else min(24.0, hold * 2 ** ((48 - key) / 12.0))
                        L, R = r.record("_tmp", 0.3 + h + 0.3,
                                        [(0.3, lambda k=key, v=vel: r.note_on(k, v)), (0.3 + h, lambda k=key: r.note_off(k))])
                        mr_sweep.write_wav24(path, L, R)
                        os.remove(os.path.join(d, "_tmp.wav"))
                        w_csv.writerow([w, klo, khi - 1, key, vlo, vhi - 1, vel, name + ".wav", "%.2f" % h])
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
