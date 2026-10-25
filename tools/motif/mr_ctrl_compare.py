"""プリセットでコントローラーを動かした実機の音（mr_ctrl_preset.py）と、芯（sampler_render --ctl）を比べる（2026-10-25）。
  python mr_ctrl_compare.py --dir mr_ctrl_preset の置き場 --render sampler_render.exe [--voices 04_063,…]
動かす前（0.4〜0.95 秒）と後（1.25〜2.3 秒、音の頭から）の差（音量・明るさ・音程・左右・音量の揺れ・音程の揺れ）を、実機と芯で並べる。
音そのものの時間の変化は、実機と芯の両方に入る。録音を読むだけで、実機には何も送らない。"""
import glob
import json
import os
import subprocess
import sys

import numpy as np

import mr_ctrl as M
import mr_ctrl_preset as C
import mr_patch
import mr_ref

# メッセージ → （ソースの番号, 値）。--ctl に渡す（ソースが同じ番号を共有するもの: AT は 2 と 3、FC は 10〜12）
CTL = {"CC1": [(1, 127)], "CC2": [(7, 127)], "CC4": [(10, 127), (11, 127), (12, 127)], "CC16": [(8, 127)], "CC16lo": [(8, 0)],
       "CC17": [(9, 127)], "CC17lo": [(9, 0)], "CC22": [(6, 127)], "CC22lo": [(6, 0)], "AT": [(2, 127), (3, 127)], "PB": [(0, 127)]}


def feats(path):
    f = M.frames(path)
    a = f[(f[:, 0] >= 0.4) & (f[:, 0] < 0.95)]
    b = f[(f[:, 0] >= 1.25) & (f[:, 0] < 2.3)]
    pa, pb = np.maximum(a[:, 2], 1), np.maximum(b[:, 2], 1)
    return np.array([np.median(b[:, 1]) - np.median(a[:, 1]), np.log2(np.median(b[:, 3]) / np.median(a[:, 3])),
                     1200 * np.log2(np.median(pb) / np.median(pa)), np.median(b[:, 4]) - np.median(a[:, 4]),
                     np.std(b[:, 1]) - np.std(a[:, 1]), 1200 * (np.std(np.log2(pb)) - np.std(np.log2(pa)))])


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    render = mr_ref.opt(a, "--render", "sampler_render.exe")
    cap = mr_ref.opt(a, "--capture", "../../roms/motif-rack-es/capture")
    voices = mr_ref.opt(a, "--voices")
    voices = voices.split(",") if voices else sorted({os.path.basename(p)[3:9] for p in glob.glob(os.path.join(d, "cp_*_none.wav"))})
    pdir = mr_ref.opt(a, "--pdir", os.path.join(cap, "presets_r2", "patches"))     # サンプルの .smpx は 100 音色の比べと共有する
    os.makedirs(pdir, exist_ok=True)
    print("差（後 − 前）: 音量 dB・明るさ oct・音程 セント・左右 dB・音量の揺れ dB・音程の揺れ セント。実機 / 芯")
    errs = []
    for v in voices:
        jp = os.path.join(cap, "voice_json", v + ".json")
        sys.argv = ["mr_patch.py", jp, os.path.join(cap, "samples"), pdir]
        mr_patch.main()
        patch = os.path.join(pdir, v + ".patch")
        rows = []
        for m in ["none"] + [os.path.basename(p).split("_k060_")[1][:-4] for p in sorted(glob.glob(os.path.join(d, "cp_%s_k060_*.wav" % v))) if not p.endswith("_sm.wav")]:
            hw = os.path.join(d, "cp_%s_k060_%s.wav" % (v, m))
            sm = os.path.join(d, "cp_%s_k060_%s_sm.wav" % (v, m))
            if not os.path.exists(hw) or (m != "none" and m not in CTL) or any(r[0] == m for r in rows):
                continue
            cmd = [render, patch, "60", "100", "2.4", sm]
            if m != "none":
                cmd += ["--ctl", ",".join("%d=%d@1.0" % sv for sv in CTL[m])]
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
            rows.append((m, feats(hw), feats(sm)))
        # 動かさないときの前後の変化は、音色ごとの全部の本の中央値とする（none の 1 本目は、ほかの本とずれていることがある。
        # 05_017・02_035 で全部の行に同じずれ。ほとんどの本はその音色に効かないメッセージなので、中央値は効かないときの変化になる）
        bh = np.median([r[1] for r in rows], axis=0)
        bs = np.median([r[2] for r in rows], axis=0)
        for m, fh, fs in rows:
            if m == "none":
                continue
            dh, ds = fh - bh, fs - bs
            e = np.abs(dh - ds)
            errs.append(e)
            big = np.any(np.abs(dh) > (1.5, 0.3, 20, 2, 1.5, 30)) or np.any(np.abs(ds) > (1.5, 0.3, 20, 2, 1.5, 30))
            print("%s %-6s %s%s" % (v, m, "  ".join("%+6.1f/%+6.1f" % (x, y) for x, y in zip(dh, ds)), "  *" if big else ""))
    if errs:
        e = np.array(errs)
        print("差のずれの中央値: 音量 %.2fdB・明るさ %.2f oct・音程 %.0f セント・左右 %.2fdB・音量の揺れ %.2fdB・音程の揺れ %.0f セント（%d 本）" % (
            *np.median(e, axis=0), len(e)))


if __name__ == "__main__":
    main()
