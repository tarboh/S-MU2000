"""プリセットの音色を、エレメントを 1 つずつだけ鳴らして録る（ほかのエレメントのスイッチ 41 ee 00 を切る。エディットバッファだけで保存しない）。
  python mr_elem_solo.py 音色（00_011 など） [--key 60] [--vel 100] [--hold 2] [--dir 置き場] [--only 1,2] [--set 40:06:0E=0,…] [--tag 名前]
<置き場>/<音色>_e<n>_k<鍵>_hw<タグ>.wav。--set はコモンのパラメーター（16 進の番地=値）をエディットバッファで変える。インサーションのスイッチは切り、CC91・93 は 0。終わったらマルチへ戻す。"""
import json
import os
import sys
import time

import mr_midi
import mr_ref

HERE = os.path.dirname(os.path.abspath(__file__))
CAP = os.path.join(HERE, "../../roms/motif-rack-es/capture")


def main():
    a = sys.argv[1:]
    v = a[0]
    key = int(mr_ref.opt(a, "--key", "60"))
    vel = int(mr_ref.opt(a, "--vel", "100"))
    hold = float(mr_ref.opt(a, "--hold", "2"))
    d = mr_ref.opt(a, "--dir", os.path.join(CAP, "solo"))
    os.makedirs(d, exist_ok=True)
    only = mr_ref.opt(a, "--only")
    tag = mr_ref.opt(a, "--tag", "")
    sets = []
    for it in (mr_ref.opt(a, "--set") or "").split(","):
        if it:
            ad, val = it.split("=")
            sets.append([int(x, 16) for x in ad.split(":")] + [int(val)])
    js = json.load(open(os.path.join(CAP, "voice_json", v + ".json"), encoding="utf-8"))
    on = [i for i, e in enumerate(js["elements"]) if e["oscillator"]["element_switch"]]
    mm, nn = int(v[:2], 16), int(v[3:6])
    import mr_preset_compare as P
    msb, lsb = P.bank_select(mm)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
    try:
        r.all_off()
        r.mode(0)
        for solo in on:
            if only and str(solo + 1) not in only.split(","):
                continue
            r.program(msb, lsb, nn)
            time.sleep(0.3)
            r.param(0x40, 0x70, 0x16, 0)
            r.cc(91, 0)
            r.cc(93, 0)
            for hi, mid, lo, val in sets:
                r.param(hi, mid, lo, val)
            for e in on:
                r.el(e, 0x41, 0x16, 0)
                r.el(e, 0x41, 0x00, 1 if e == solo else 0)
            time.sleep(0.1)
            name = "%s_e%d_k%03d_hw%s" % (v, solo + 1, key, tag)
            r.record(name, 0.3 + hold + 2.0, [(0.3, lambda: r.note_on(key, vel)), (0.3 + hold, lambda: r.note_off(key))])
            print(name, "録った", flush=True)
    finally:
        r.all_off()
        r.mode(3)
        r.close()


if __name__ == "__main__":
    main()
