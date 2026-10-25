"""コントロールセットの深さとソースの値で、行き先がどれだけ動くかを測る（2026-10-25）。
  python mr_ctrl_depth.py --dir 置き場 [--dests 41:sin,42:sin,…] [--depths 0,32,48,80,96,127] [--src 1] [--read]
mr_ctrl.py と同じ雛形（sin・noi・lfs・lfn）で、組 1 をソース・行き先・深さにして鍵 60 を鳴らし、0.5 秒から 0.6 秒おきに
ソースの値を 0・32・64・96・127 と上げる（ソース 1 は CC1）。段ごとに、音量・音程・左右・明るさ（重心）・音量と音程の揺れを読む。
送るのはパラメーターチェンジ・CC・ノートだけ（保存しない）。終わったら CC を戻し、マルチへ戻す。"""
import os
import sys

import numpy as np

import mr_ctrl as M
import mr_ref

DESTS = "1:sin,41:sin,42:sin,48:sin,49:sin,45:lfs,46:lfs,47:lfn,60:lfn,66:noi,67:noi,68:noi,56:sin,59:sin"
STEPS = (0, 32, 64, 96, 127)
T0, DT = 0.5, 0.6


def steps(path):
    """段ごとの（音量 dB・音程 Hz・左右 dB・重心 Hz・音量の揺れ dB・音程の揺れ セント）"""
    f = M.frames(path)
    out = []
    for j in range(len(STEPS)):
        t = T0 + DT * j
        s = f[(f[:, 0] >= t + 0.15) & (f[:, 0] < t + DT - 0.02)]
        p = np.maximum(s[:, 2], 1)
        out.append((np.median(s[:, 1]), np.median(p), np.median(s[:, 4]), np.median(s[:, 3]), np.std(s[:, 1]), 1200 * np.std(np.log2(p))))
    return np.array(out)


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    dests = [(int(x.split(":")[0]), x.split(":")[1]) for x in mr_ref.opt(a, "--dests", DESTS).split(",")]
    depths = [int(x) for x in mr_ref.opt(a, "--depths", "0,32,48,80,96,127").split(",")]
    src = int(mr_ref.opt(a, "--src", "1"))
    cc = {1: 1, 8: 16, 9: 17}.get(src, 1)
    os.makedirs(d, exist_ok=True)
    if "--read" not in a:
        r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
        try:
            r.all_off()
            r.mode(0)
            r.program(63, 0, 0)
            for dv, kind in dests:
                for dp in depths:
                    name = "dp_s%d_d%03d_%s_x%03d" % (src, dv, kind, dp)
                    if os.path.exists(os.path.join(d, name + ".wav")):
                        continue
                    M.setup(r, kind)
                    M.set_ctrl(r, src, dv, dp)
                    M.reset_ctrls(r)
                    ev = [(0.3, lambda: r.note_on(60, 100))]
                    for j, v in enumerate(STEPS[1:]):
                        ev.append((0.3 + T0 + DT * (j + 1), lambda v=v: r.mo.short(0xB0, cc, v)))
                    end = 0.3 + T0 + DT * len(STEPS)
                    ev.append((end, lambda: r.note_off(60)))
                    r.record(name, end + 0.4, ev)
                    M.reset_ctrls(r)
                    print(name, "録った", flush=True)
        finally:
            try:
                M.reset_ctrls(r)
                M.set_ctrl(r, 0, 0, 64)
            finally:
                r.param(0x40, 0x06, 0x0B, 0)
                r.all_off()
                r.mode(3)
                r.close()
    print("段（ソースの値 %s）ごとの、値 0 の段からの差" % ",".join(map(str, STEPS)))
    for dv, kind in dests:
        for dp in depths:
            p = os.path.join(d, "dp_s%d_d%03d_%s_x%03d.wav" % (src, dv, kind, dp))
            if not os.path.exists(p):
                continue
            s = steps(p)
            b = s[0]
            row = ["%+.1fdB %+.0fc %+.1fLR x%.2f t%+.1f v%+.0f" % (x[0] - b[0], 1200 * np.log2(x[1] / b[1]), x[2] - b[2], x[3] / b[3], x[4] - b[4], x[5] - b[5])
                   for x in s[1:]]
            print("%3d %s %3d: %s" % (dv, kind, dp, " | ".join(row)))


if __name__ == "__main__":
    main()
