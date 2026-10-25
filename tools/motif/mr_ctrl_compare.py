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


ET = (0.02, 0.05, 0.1, 0.2, 0.4, 0.8, 1.6, 2.3)


def env_feats(path):
    """ノートの前に動かした本（…pre）の読み: 音の頭からの時刻 ET ごとの音量 dB と明るさ（重心の log2）、音程（セント）。2.3 秒は離したあと"""
    L, R = mr_ref.read_wav(path)
    x = np.asarray(L, float) + np.asarray(R, float)
    on = int(np.argmax(np.abs(x) > 1e-4))
    out = []
    fr = np.fft.rfftfreq(1 << 14, 1 / 44100)
    for t in ET:
        i = on + int(t * 44100)
        seg = x[i:i + 2048]
        sp = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), 1 << 14))
        out.append((10 * np.log10(np.mean(seg ** 2) + 1e-20), np.log2(np.sum(fr * sp ** 2) / (np.sum(sp ** 2) + 1e-20) + 1),
                    1200 * np.log2(max(fr[int(np.argmax(sp[:int(4000 / fr[1])]))], 1))))
    return np.array(out)


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
    pre_errs = []
    for v in voices:
        jp = os.path.join(cap, "voice_json", v + ".json")
        sys.argv = ["mr_patch.py", jp, os.path.join(cap, "samples"), pdir]
        mr_patch.main()
        patch = os.path.join(pdir, v + ".patch")
        rows = []
        for m in ["none"] + [os.path.basename(p).split("_k060_")[1][:-4] for p in sorted(glob.glob(os.path.join(d, "cp_%s_k060_*.wav" % v))) if not p.endswith("_sm.wav")]:
            hw = os.path.join(d, "cp_%s_k060_%s.wav" % (v, m))
            sm = os.path.join(d, "cp_%s_k060_%s_sm.wav" % (v, m))
            pre = m.endswith("pre")
            mm = m[:-3] if pre else m
            if not os.path.exists(hw) or (m not in ("none", "nonez") and mm not in CTL) or any(r[0] == m for r in rows):
                continue
            cmd = [render, patch, "60", "100", "2.4", sm]
            if m not in ("none", "nonez"):
                cmd += ["--ctl", ",".join("%d=%d@%s" % (sv[0], sv[1], "0" if pre else "1.0") for sv in CTL[mm])]
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
            rows.append((m, feats(hw), feats(sm), env_feats(hw), env_feats(sm)))
        # 動かさないときの前後の変化は、音色ごとの全部の本の中央値とする（none の 1 本目は、ほかの本とずれていることがある。
        # 05_017・02_035 で全部の行に同じずれ。ほとんどの本はその音色に効かないメッセージなので、中央値は効かないときの変化になる）
        post = [r for r in rows if not r[0].endswith("pre") and r[0] != "nonez"]
        bh = np.median([r[1] for r in post], axis=0)
        bs = np.median([r[2] for r in post], axis=0)
        # ノートの前に動かした本は、何も動かさない 2 本（none・nonez）の平均からの、時刻ごとの音量・明るさ・音程の差で比べる
        nz = [r for r in rows if r[0] in ("none", "nonez")]
        if any(r[0].endswith("pre") for r in rows) and nz:
            eh = np.mean([r[3] for r in nz], axis=0)
            es = np.mean([r[4] for r in nz], axis=0)
            if len(nz) == 2:
                print("%s none と nonez の差（実機）: 音量 %s" % (v, " ".join("%+.1f" % x for x in nz[1][3][:, 0] - nz[0][3][:, 0])))
            for m, _, _, fh, fs in rows:
                if not m.endswith("pre"):
                    continue
                dh, ds = fh - eh, fs - es
                pre_errs.append(np.median(np.abs(dh - ds), axis=0))
                print("%s %-9s 音量 dB（%s 秒）: %s" % (v, m, ",".join("%g" % t for t in ET), " ".join("%+.1f/%+.1f" % (x, y) for x, y in zip(dh[:, 0], ds[:, 0]))))
                print("%s %-9s 明るさ oct: %s | 音程 c: %s" % (v, "", " ".join("%+.2f/%+.2f" % (x, y) for x, y in zip(dh[:, 1], ds[:, 1])),
                                                         " ".join("%+.0f/%+.0f" % (x, y) for x, y in zip(dh[:, 2], ds[:, 2]))))
        for m, fh, fs, _, _ in post:
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
    if pre_errs:
        e = np.array(pre_errs)
        print("ノートの前に動かした本のずれ（時刻ごとの中央値の中央値）: 音量 %.2fdB・明るさ %.2f oct・音程 %.0f セント（%d 本）" % (*np.median(e, axis=0), len(e)))


if __name__ == "__main__":
    main()
