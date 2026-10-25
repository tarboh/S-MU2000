"""ダンプした音色（mr_voice.py --json の JSON）と、切り出したサンプル（mr_extract_all.py の samples.csv と .npz）から、
サンプリング方式の芯（src/motif/sampler_render）で鳴らす「パッチ」の文字ファイルを作る。使うサンプルは .smpx に直して置く。
  python mr_patch.py 音色.json サンプルの置き場 出力の置き場

パッチ: 頭にエフェクトの行（fx reverb=… chorus=… insa=… insb=… connect=… tempo=…）、エレメントごとに 1 行（element 欄=値 …）、
その下に区間ごとの行（zone key_lo=… key_hi=… vel_lo=… vel_hi=… file=…）。
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


def voice_tempo(json_path):
    """音色のアルペジオのテンポ（ダンプの 40 70 00 の 0x28 から 2 バイト）。voice_json/<名前>.json の隣の voice/<名前>.syx から読む。無ければ 120"""
    import mr_voice
    syx = os.path.join(os.path.dirname(json_path), "..", "voice", os.path.basename(json_path).replace(".json", ".syx"))
    if not os.path.exists(syx):
        return 120
    x = mr_voice.blocks(syx).get((0x40, 0x70, 0x00))
    return (x[0x28] << 7 | x[0x29]) if x and len(x) > 0x29 else 120


def voice_common_lfo(json_path):
    """音色のコモン LFO（ダンプの 40 06 00）。欄: 0 位相・1 波形・2 速さ・3 テンポの速さ・4 テンポ同期・5 遅れ・6 フェードイン・7 ホールド・8 フェードアウト・
    9 キーオンリセット・10/13/16 箱 1〜3 の行き先・11/14/17 深さ・19 ユーザー波形の段の数・20 傾き・21〜 段の値"""
    import mr_voice
    syx = os.path.join(os.path.dirname(json_path), "..", "voice", os.path.basename(json_path).replace(".json", ".syx"))
    if not os.path.exists(syx):
        return None
    x = mr_voice.blocks(syx).get((0x40, 0x06, 0x00))
    if not x or len(x) < 29:
        return None
    return {"phase": x[0], "wave": x[1], "speed": x[2], "tspeed": x[3], "sync": x[4], "delay": x[5], "fadein": x[6], "hold": x[7],
            "fadeout": x[8], "reset": x[9], "dest": "%d,%d,%d" % (x[10], x[13], x[16]), "depth": "%d,%d,%d" % (x[11], x[14], x[17]),
            "usteps": x[19], "uvals": ",".join(str(v) for v in x[21:21 + 16])}


def voice_ctrl(json_path):
    """音色のコントロールセット（ダンプの 40 05 00、5 バイト × 6 組: ソース・行き先の上下・深さ（64 が 0）・0）を「ソース:行き先:深さ」の並びにする"""
    import mr_voice
    syx = os.path.join(os.path.dirname(json_path), "..", "voice", os.path.basename(json_path).replace(".json", ".syx"))
    if not os.path.exists(syx):
        return None
    x = mr_voice.blocks(syx).get((0x40, 0x05, 0x00))
    if not x or len(x) < 30:
        return None
    return ",".join("%d:%d:%d" % (x[5 * i], x[5 * i + 1] << 7 | x[5 * i + 2], x[5 * i + 3]) for i in range(6))


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
    # エフェクト（芯の sampler_fx.h が読む）。各ブロックは「型,パラメーター 1〜16」、リバーブ・コーラスは後ろに Return と送り
    fx = {}
    for key, blk in (("reverb", "reverb"), ("chorus", "chorus"), ("insa", "insertion1"), ("insb", "insertion2")):
        b = v.get(blk)
        if not b:
            continue
        vals = [b["type"]] + [b.get("param%d" % i, 0) for i in range(1, 17)]
        if key in ("reverb", "chorus"):
            vals += [b.get("return", 96), v["common"].get("%s_send" % key, 0)]
        fx[key] = ",".join(str(x) for x in vals)
    fx["connect"] = v.get("insertion_connect", {}).get("insertion_connect_type", 0)
    fx["tempo"] = voice_tempo(vpath)
    lines.append("fx " + " ".join("%s=%s" % kv for kv in fx.items()))
    lines.append("voice volume=%d" % v.get("common", {}).get("volume", 127))
    cl = voice_common_lfo(vpath)
    if cl:
        lines.append("clfo " + " ".join("%s=%s" % kv for kv in cl.items()))
    cs = voice_ctrl(vpath)
    if cs:
        c = v.get("common", {})
        lines.append("ctrl sets=%s pbup=%d pbdown=%d" % (cs, c.get("pitch_bend_range_upper", 64), c.get("pitch_bend_range_lower", 64)))
    missing = []
    for e in v["elements"]:
        osc, amp, pit, flt, eq, lfo = e["oscillator"], e["amplitude"], e["pitch"], e["filter"], e["eq"], e["lfo"]
        if not osc["element_switch"]:
            continue
        w = osc["wave_number"]
        f = {
            "wave": w, "ins": osc.get("insertion_effect_switch", 0), "lbox": osc.get("lfo_box_sw", 0), "cbox": osc.get("control_box_sw", 0),
            "lratio": "%d,%d,%d,%d" % tuple(e.get("common_lfo_ratio", {}).get(k, d) for k, d in (("common_lfo_phase_offset", 0),
                ("common_lfo_box1_depth_ratio", 127), ("common_lfo_box2_depth_ratio", 127), ("common_lfo_box3_depth_ratio", 127))), "kdelay": osc.get("key_on_delay", 0), "ksync": osc.get("key_on_delay_tempo_sync", 0), "klen": osc.get("key_on_delay_tempo", 0), "note_lo": osc["note_limit_low"], "note_hi": osc["note_limit_high"],
            "vel_lo": osc["velocity_limit_low"], "vel_hi": osc["velocity_limit_high"], "pan": osc["pan"], "span": osc.get("scaling_pan_depth", 64),
            "level": amp["element_level"], "fgain": flt["filter_gain"], "release": amp["aeg_release_time"],
            "aegt": "%d,%d,%d,%d" % tuple(amp["aeg_%s_time" % n] for n in ("attack", "decay1", "decay2", "release")),
            "aegl": "%d,%d,%d,%d" % tuple(amp["aeg_%s_level" % n] for n in ("init", "attack", "decay1", "decay2")),
            "aegtv": amp["aeg_time_velocity_sensitivity"], "aegtvs": amp["aeg_time_velocity_segment"], "aegkf": amp["aeg_time_key_follow_sensitivity"],
            "aegkc": amp["aeg_time_key_follow_center_note"],
            "lvel": amp["level_velocity_sensitivity"], "lcurve": amp["level_velocity_curve"],
            "lkf": amp["level_key_follow_sensitivity"],
            "lbp": "%d,%d,%d,%d" % tuple(amp["level_scaling_break_point%d" % i] for i in range(1, 5)),
            "loff": "%d,%d,%d,%d" % tuple(amp["level_scaling_offset%d" % i] for i in range(1, 5)),
            "lfow": lfo["lfo_wave"], "lfosync": lfo["lfo_key_on_sync"], "lfodelay": lfo["lfo_key_on_delay_time"],
            "lfospeed": lfo["lfo_speed"], "lfoa": lfo["lfo_amod_depth"], "lfop": lfo["lfo_pmod_depth"],
            "lfof": lfo["lfo_fmod_depth"], "lfofade": lfo["lfo_fade_in_time"],
            "coarse": pit["coarse_tune"] - 64, "fine": pit["fine_tune"] - 64,
            "ftype": flt["filter_type"], "cutoff": flt["filter_cutoff"], "fvel": flt["filter_cutoff_velocity_sensitivity"],
            "reso": flt["filter_resonance"], "rvel": flt["filter_resonance_velocity_sensitivity"], "hpf": flt["hpf_cutoff"],
            "distance": flt["distance"], "fkf": flt["filter_cutoff_key_follow_sensitivity"],
            "hkf": flt["hpf_cutoff_key_follow_sensitivity"],
            "bp": "%d,%d,%d,%d" % tuple(flt["filter_cutoff_scaling_break_point%d" % i] for i in range(1, 5)),
            "off": "%d,%d,%d,%d" % tuple(flt["filter_cutoff_scaling_offset%d" % i] for i in range(1, 5)),
            "fegt": "%d,%d,%d,%d,%d" % tuple(flt["feg_%s_time" % n] for n in ("hold", "attack", "decay1", "decay2", "release")),
            "fegl": "%d,%d,%d,%d,%d" % tuple(flt["feg_%s_level" % n] - 128 for n in ("hold", "attack", "decay1", "decay2", "release")),
            "fegd": flt["feg_depth"], "fegtv": flt["feg_time_velocity_sensitivity"], "fegtvs": flt["feg_time_velocity_segment"], "feglv": flt["feg_level_velocity_sensitivity"],
            "feglc": flt["feg_level_velocity_curve"], "fegkf": flt["feg_time_key_follow_sensitivity"],
            "fegkc": flt["feg_time_key_follow_center_note"],
            "pegt": "%d,%d,%d,%d,%d" % tuple(pit["peg_%s_time" % n] for n in ("hold", "attack", "decay1", "decay2", "release")),
            "pegl": "%d,%d,%d,%d,%d" % tuple(pit["peg_%s_level" % n] - 128 for n in ("hold", "attack", "decay1", "decay2", "release")),
            "pegd": pit["peg_depth"], "pegtv": pit["peg_time_velocity_sensitivity"], "pegtvs": pit["peg_time_velocity_segment"],
            "peglv": pit["peg_level_velocity_sensitivity"], "peglc": pit["peg_level_velocity_curve"],
            "pegkf": pit["peg_time_key_follow_sensitivity"], "pegkc": pit["peg_time_key_follow_center_note"],
            "pkf": pit["pitch_key_follow_sensitivity"], "pkc": pit["pitch_key_follow_center_note"], "pvel": pit["pitch_velocity_sensitivity"],
            "eqtype": eq["eq_type"], "eqres": eq["eq_resonance"], "eqf1": eq["eq1_frequency"], "eqg1": eq["eq1_gain"],
            "eqf2": eq["eq2_frequency"], "eqg2": eq["eq2_gain"],
        }
        lines.append("element " + " ".join("%s=%s" % kv for kv in f.items()))
        zs = by_wave.get(w, []) or by_wave.get(alias.get(w, -1), [])   # 自分のサンプルが無いときだけ、同じ音の波形のものを使う（層の数が違うことがある）
        if not zs:
            missing.append(w)
        # 鍵とベロシティの幅が狭い区間を先に（後から細かく録り足したものを、前の広い区間より先に使う。芯は最初に合った区間を使う）
        zs = sorted(zs, key=lambda s: (int(s["key_hi"]) - int(s["key_lo"]), int(s["vel_hi"]) - int(s["vel_lo"])))
        for s in zs:
            name = s["file"].replace(".npz", ".smpx")
            sp, np_ = os.path.join(odir, name), os.path.join(sdir, s["file"])
            if not os.path.exists(sp) or os.path.getmtime(sp) < os.path.getmtime(np_):    # 切り出し直したサンプルは作り直す
                pack(os.path.join(sdir, s["file"]), os.path.join(odir, name), int(s["key"]))
            lines.append("zone key_lo=%s key_hi=%s vel_lo=%s vel_hi=%s file=%s" % (s["key_lo"], s["key_hi"], s["vel_lo"], s["vel_hi"], name))
    out = os.path.join(odir, os.path.splitext(os.path.basename(vpath))[0] + ".patch")
    open(out, "w", newline="\n").write("\n".join(lines) + "\n")
    print("書いた: %s（エレメント %d、サンプルの無い波形 %s）" % (out, sum(1 for l in lines if l.startswith("element")), missing or "なし"))


if __name__ == "__main__":
    main()
