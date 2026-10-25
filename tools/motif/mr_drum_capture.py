"""ドラムボイスが使う波形を、ドラムの鍵が鳴らす鍵（60 + コースチューン）で録る（2026-10-25）。
  python mr_drum_capture.py [--dir 置き場] [--capture 取ったものの置き場] [--hold 6]
ドラムの鍵は、雛形の音色の鍵 60 と同じ音で鳴る（mr_drum_root.py で 4 鍵とも相関 1.0000）。ふつうの下録り（mr_capture.py）は区間の真ん中の鍵で
録ってあり、ドラムの波形は鍵 0〜77 のような広い区間を鍵 38 で録ったものがあって、鍵 60 に上げると高い音が抜ける。なので、
ドラムで使う（波形, コースチューン）の組のうち、今あるサンプルを 2 半音より上げることになるものだけ、鍵 60 + コースチューン（0〜127 に収める）、ベロシティの層ごと（samples.csv の層）に、
雛形（mr_capture.py と同じ。フィルター THRU）を鳴らして録る。押さえる長さは、前に録ったサンプルの長さを鍵の差で伸び縮みさせて見積もる（6 秒まで）。
録ったものは D_w????_k???_v???.wav（24bit）と capture.csv。もうあるものは飛ばす。置き場に STOP を置くと止まる。
送るのはパラメーターチェンジとノートだけ（保存しない）。終わったらマルチへ戻す。録った音は配らないこと（ヤマハの波形）。"""
import collections
import csv
import glob
import os
import sys
import time

import mr_ref
import mr_sweep
import mr_voice


UP_OK = 2           # この半音までなら、今あるサンプルを上げて使ってよい


def pairs_used(cap):
    """ドラムの鍵が使う（波形, 鳴らす鍵）の組"""
    out = collections.Counter()
    for f in sorted(glob.glob(os.path.join(cap, "drum", "*.syx"))):
        if os.path.basename(f)[:2] not in ("20", "21", "28"):
            continue
        b = mr_voice.blocks(f)
        for kk in range(73):
            o, p = b.get((0x47, kk, 0x00)), b.get((0x48, kk, 0x00))
            if not o or o[0] == 0 or o[1] != 0:
                continue
            w = o[6] << 7 | o[7]
            out[(w, max(0, min(127, 60 + ((p[0] - 64) if p else 0))))] += 1
    return out


def main():
    a = sys.argv[1:]
    here = os.path.dirname(os.path.abspath(__file__))
    cap = mr_ref.opt(a, "--capture", os.path.join(here, "..", "..", "roms", "motif-rack-es", "capture"))
    d = mr_ref.opt(a, "--dir", os.path.join(cap, "drumwave"))
    hold_max = float(mr_ref.opt(a, "--hold", "6"))
    samples = list(csv.DictReader(open(os.path.join(cap, "samples", "samples.csv"))))
    alias = {int(r["wave"]): int(r["same_as"]) for r in csv.DictReader(open(os.path.join(cap, "wave_alias.csv")))}
    by_wave = collections.defaultdict(list)
    for s in samples:
        by_wave[int(s["wave"])].append(s)
    used = pairs_used(cap)
    # 今あるサンプル（区間の真ん中の鍵で録ったもの）を 2 半音より上げて使うことになる組だけ録る（下げるか、上げても 2 半音までなら高い音は欠けない）
    def need(wk):
        w, key = wk
        zs = by_wave.get(w) or by_wave.get(alias.get(w, -1)) or []
        z = [s for s in zs if int(s["key_lo"]) <= key <= int(s["key_hi"])]
        return not z or min(key - int(s["key"]) for s in z) > UP_OK
    pairs = sorted((wk for wk in used if need(wk)), key=lambda wk: (-used[wk], wk))    # よく使う組から（途中で止めても、多くのドラムが鳴るように）
    os.makedirs(d, exist_ok=True)
    log = os.path.join(d, "capture.csv")
    new_log = not os.path.exists(log)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
    done = 0
    try:
        with open(log, "a", newline="") as lf:
            wc = csv.writer(lf, lineterminator="\n")
            if new_log:
                wc.writerow(["wave", "key", "vel_lo", "vel_hi", "vel", "file", "hold"])
            r.all_off()
            r.param(0x00, 0x00, 0x34, 0)        # アルペジオの MIDI 出力は切っておく
            r.mode(0)
            r.program(63, 0, 0)
            r.template(pairs[0][0])
            r.el(0, 0x43, 0x00, 21)             # フィルター THRU（mr_capture.py と同じ雛形）
            r.el(0, 0x43, 0x01, 1, 127)
            r.el(0, 0x43, 0x22, 64)
            r.el(0, 0x43, 0x06, 64)
            cur = None
            for i, (w, key) in enumerate(pairs):
                zs = by_wave.get(w) or by_wave.get(alias.get(w, -1)) or []
                layers = sorted({(int(s["vel_lo"]), int(s["vel_hi"]), int(s["vel"])) for s in zs}) or [(1, 127, 100)]
                for vlo, vhi, vel in layers:
                    name = "D_w%04d_k%03d_v%03d" % (w, key, vel)
                    path = os.path.join(d, name + ".wav")
                    if os.path.exists(path):
                        continue
                    if os.path.exists(os.path.join(d, "STOP")):
                        raise StopIteration
                    # 長さの見積もり: その層の区間のうち鍵がいちばん近いものの長さを、鍵の差で伸び縮み
                    cand = [s for s in zs if int(s["vel_lo"]) == vlo] or zs
                    if cand:
                        s = min(cand, key=lambda s: abs(int(s["key"]) - key))
                        est = int(s["length"]) / 44100.0 * 2 ** ((int(s["key"]) - key) / 12.0)
                        h = max(1.0, min(hold_max, est * 1.3 + 0.5))     # 鳴り終わり（-70dB）まで入るように長めに
                    else:
                        h = hold_max
                    if cur != w:
                        r.el(0, 0x41, 0x03, w >> 7, w & 0x7F)
                        time.sleep(0.15)
                        cur = w
                    L, R = r.record("_tmp", 0.3 + h + 0.3, [(0.3, lambda k=key, v=vel: r.note_on(k, v)), (0.3 + h, lambda k=key: r.note_off(k))])
                    mr_sweep.write_wav24(path, L, R)
                    os.remove(os.path.join(d, "_tmp.wav"))
                    wc.writerow([w, key, vlo, vhi, vel, name + ".wav", "%.2f" % h])
                    lf.flush()
                    done += 1
                if i % 50 == 0:
                    print("%d / %d 組（%d 本）" % (i + 1, len(pairs), done), flush=True)
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
