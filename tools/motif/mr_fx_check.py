"""インサーションの模型（mr_fx_apply.apply_block）を、実機の dry の録音に掛けて、実機の wet と 50ms ごとの音量で比べる（録音を読むだけ）。
  python mr_fx_check.py 名前…（mr_fx.py --block insa の _dry.wav・_wet.wav・.json の前まで）

逆畳み込みの線の高さは、0.1 秒おきの音を 1 発目の型で割るので、ノイズの音が回ごとに違うぶん 0.9 倍ほどに読める（dry をそのまま wet にしても 0.90）。
こちらは割らずに音量を比べるので、その偏りがない。出すのは「模型 − 実機」の dB の中央値と、10・90% の点（左右とも、鳴っている窓だけ）。"""
import json
import sys

import numpy as np

import mr_fx_apply as A
import mr_fx_fit as F

FS = 44100


def env(x, n):
    m = len(x) // n
    return 10 * np.log10(np.mean(x[:m * n].reshape(m, n) ** 2, axis=1) + 1e-20)


def main():
    for base in sys.argv[1:]:
        prm = json.load(open(base + ".json"))
        dL, dR = F.load(base + "_dry.wav")
        wL, wR = F.load(base + "_wet.wav")
        od = int(np.argmax(np.abs(dL) > 1e-4))
        ow = int(np.argmax(np.abs(wL) > 1e-4))
        if abs(ow - od) > int(0.05 * FS):   # Dry/Wet 127 では wet の頭が遅れた線になるので、録音の頭どうしでそろえる
            od = ow = 0
        sh = min(od, ow)
        dL, dR, wL, wR = dL[od - sh:], dR[od - sh:], wL[ow - sh:], wR[ow - sh:]
        n = min(len(dL), len(wL))
        mL, mR, name = A.apply_block(dL[:n], dR[:n], prm)
        w = int(0.05 * FS)
        out = []
        for m, h in ((mL, wL[:n]), (mR, wR[:n])):
            em, eh = env(m, w), env(h, w)
            ok = eh > eh.max() - 40
            out.append(em[ok] - eh[ok])
        dd = np.concatenate(out)
        print("%-24s %s: 模型 − 実機 中央値 %+.2f dB（10%% %+.2f・90%% %+.2f）" % (
            base.replace("\\", "/").split("/")[-1], name, np.median(dd), np.percentile(dd, 10), np.percentile(dd, 90)))


if __name__ == "__main__":
    main()
