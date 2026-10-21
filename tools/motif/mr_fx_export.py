"""測ったエフェクトの応答を、C++ の芯（src/motif/sampler_fx.h）が読む形に書き出す（録音から作ったものを読むだけで、実機には何も送らない）。
  python mr_fx_export.py [capture の置き場]

* リバーブ: fx/reverb_<型>_ir.npz → fx/reverb_<MSB>_<LSB>.irx
  1 行目「IRX frames=N rt=r125,r250,r500,r1k,r2k,r4k,r8k」（帯ごとの残響時間。読めない帯は 1kHz の値）、そのあと float32 の左右を交互に N 組
* 歪み系: fxd/<名前>_dist.npz → fxd/<名前>.dsx
  1 行目「DSX frames=N c=… p=… fc=… drive=…（測ったときの Drive）」、そのあと float32 の左右（頭打ちの前の応答 h、LPF は割り戻し済み）を交互に N 組
これらは録った音から作ったもので、配らないこと（mr_fx_apply.py と同じ内容）。"""
import glob
import json
import os
import sys

import numpy as np

import mr_fx
import mr_fx_fit as F

BANDS = (125, 250, 500, 1000, 2000, 4000, 8000)


def main():
    cap = sys.argv[1] if len(sys.argv) > 1 else "../../roms/motif-rack-es/capture"
    n = 0
    for msb, lsb, name in mr_fx.REVERBS:
        p = os.path.join(cap, "fx", "reverb_%s_ir.npz" % name)
        if not os.path.exists(p):
            continue
        d = np.load(p)
        L, R = d["L"].astype(np.float32), d["R"].astype(np.float32)
        rt = [F.rt60(F.octave(L.astype(np.float64) + R, fc)) for fc in BANDS]
        rt = [v if np.isfinite(v) and 0.05 < v < 60 else rt[3] for v in rt]
        head = "IRX frames=%d rt=%s\n" % (len(L), ",".join("%.4f" % v for v in rt))
        with open(os.path.join(cap, "fx", "reverb_%d_%d.irx" % (msb, lsb)), "wb") as f:
            f.write(head.encode("ascii"))
            f.write(np.stack([L, R], axis=1).astype("<f4").tobytes())
        n += 1
    m = 0
    for p in glob.glob(os.path.join(cap, "fxd", "*_dist.npz")):
        d = np.load(p)
        hL, hR = d["hL"].astype(np.float32), d["hR"].astype(np.float32)
        js = p.replace("_dist.npz", ".json")
        drive = json.load(open(js)).get("param1", 30) if os.path.exists(js) else 30
        head = "DSX frames=%d c=%.6f p=%.3f fc=%.1f drive=%d\n" % (len(hL), float(d["c"]), float(d["p"]), float(d["fc"]), drive)
        with open(p.replace("_dist.npz", ".dsx"), "wb") as f:
            f.write(head.encode("ascii"))
            f.write(np.stack([hL, hR], axis=1).astype("<f4").tobytes())
        m += 1
    print("リバーブ %d 型・歪み系 %d 組を書き出した" % (n, m))


if __name__ == "__main__":
    main()
