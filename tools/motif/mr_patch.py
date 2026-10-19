"""ダンプした音色（mr_voice.py --json の JSON）と、切り出したサンプル（mr_extract_all.py の samples.csv と .npz）から、
サンプリング方式の芯（src/motif/sampler_render）で鳴らす「パッチ」の文字ファイルを作る。使うサンプルは .smpx に直して置く。
  python mr_patch.py 音色.json サンプルの置き場 出力の置き場

パッチ: エレメントごとに 1 行（element 欄=値 …）、その下に区間ごとの行（zone key_lo=… key_hi=… vel_lo=… vel_hi=… file=…）。
.smpx: 1 行目が「SMPX key=… loop_start=… loop_length=… stretch=… frames=…」、そのあと float32 の左右を交互に frames 組。
欄の値は Data List の値のまま（コースチューン・ファインチューンだけ 64 を引く）。録音を読むだけで、実機には何も送らない。"""
import csv
import json
import os
import sys

import numpy as np


def pack(npz, out, key):
    d = np.load(npz)
    L, R = d["L"].astype(np.float32), d["R"].astype(np.float32)
    head = "SMPX key=%d loop_start=%d loop_length=%d stretch=%.9f frames=%d\n" % (
        key, int(d["loop_start"]), int(d["loop_length"]), float(d["stretch"]), len(L))
    with open(out, "wb") as f:
        f.write(head.encode("ascii"))
        f.write(np.stack([L, R], axis=1).astype("<f4").tobytes())


def main():
    vpath, sdir, odir = sys.argv[1:4]
    os.makedirs(odir, exist_ok=True)
    v = json.load(open(vpath, encoding="utf-8"))
    samples = list(csv.DictReader(open(os.path.join(sdir, "samples.csv"))))
    alias = {}
    ap = os.path.join(sdir, "..", "wave_alias.csv")     # 同じサンプルを共有する波形は、もとの波形のサンプルを使う（mr_alias.py）
    if os.path.exists(ap):
        alias = {int(r["wave"]): int(r["same_as"]) for r in csv.DictReader(open(ap))}
    by_wave = {}
    for s in samples:
        by_wave.setdefault(int(s["wave"]), []).append(s)
    lines = ["# %s" % v.get("name", "")]
    missing = []
    for e in v["elements"]:
        osc, amp, pit, flt, eq = e["oscillator"], e["amplitude"], e["pitch"], e["filter"], e["eq"]
        if not osc["element_switch"]:
            continue
        w = osc["wave_number"]
        f = {
            "wave": w, "note_lo": osc["note_limit_low"], "note_hi": osc["note_limit_high"],
            "vel_lo": osc["velocity_limit_low"], "vel_hi": osc["velocity_limit_high"], "pan": osc["pan"],
            "level": amp["element_level"], "release": amp["aeg_release_time"],
            "coarse": pit["coarse_tune"] - 64, "fine": pit["fine_tune"] - 64,
            "ftype": flt["filter_type"], "cutoff": flt["filter_cutoff"], "fvel": flt["filter_cutoff_velocity_sensitivity"],
            "reso": flt["filter_resonance"], "rvel": flt["filter_resonance_velocity_sensitivity"], "hpf": flt["hpf_cutoff"],
            "distance": flt["distance"], "fkf": flt["filter_cutoff_key_follow_sensitivity"],
            "hkf": flt["hpf_cutoff_key_follow_sensitivity"],
            "bp": "%d,%d,%d,%d" % tuple(flt["filter_cutoff_scaling_break_point%d" % i] for i in range(1, 5)),
            "off": "%d,%d,%d,%d" % tuple(flt["filter_cutoff_scaling_offset%d" % i] for i in range(1, 5)),
            "fegt": "%d,%d,%d,%d,%d" % tuple(flt["feg_%s_time" % n] for n in ("hold", "attack", "decay1", "decay2", "release")),
            "fegl": "%d,%d,%d,%d,%d" % tuple(flt["feg_%s_level" % n] - 128 for n in ("hold", "attack", "decay1", "decay2", "release")),
            "fegd": flt["feg_depth"], "fegtv": flt["feg_time_velocity_sensitivity"], "feglv": flt["feg_level_velocity_sensitivity"],
            "feglc": flt["feg_level_velocity_curve"], "fegkf": flt["feg_time_key_follow_sensitivity"],
            "fegkc": flt["feg_time_key_follow_center_note"],
            "eqtype": eq["eq_type"], "eqres": eq["eq_resonance"], "eqf1": eq["eq1_frequency"], "eqg1": eq["eq1_gain"],
            "eqf2": eq["eq2_frequency"], "eqg2": eq["eq2_gain"],
        }
        lines.append("element " + " ".join("%s=%s" % kv for kv in f.items()))
        zs = by_wave.get(w, []) or by_wave.get(alias.get(w, -1), [])
        if not zs:
            missing.append(w)
        for s in zs:
            name = s["file"].replace(".npz", ".smpx")
            if not os.path.exists(os.path.join(odir, name)):
                pack(os.path.join(sdir, s["file"]), os.path.join(odir, name), int(s["key"]))
            lines.append("zone key_lo=%s key_hi=%s vel_lo=%s vel_hi=%s file=%s" % (s["key_lo"], s["key_hi"], s["vel_lo"], s["vel_hi"], name))
    out = os.path.join(odir, os.path.splitext(os.path.basename(vpath))[0] + ".patch")
    open(out, "w", newline="\n").write("\n".join(lines) + "\n")
    print("書いた: %s（エレメント %d、サンプルの無い波形 %s）" % (out, sum(1 for l in lines if l.startswith("element")), missing or "なし"))


if __name__ == "__main__":
    main()
