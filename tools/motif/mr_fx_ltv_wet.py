"""録った応答（.ltv）の揺れの線の直し（capture/fxltv/wetgain.txt）を作る。
  python mr_fx_ltv_wet.py [fxsim.exe の場所（既定 build/fxsim.exe）]
.ltv は枠ごとに均して作るので、LFO で動く遅れの成分（揺れの線）が実機より小さく出る（コーラス・セレステ・フランジャーで 1.3〜1.8 倍）。
組ごとに、.ltv を作ったときの実機の録音（fxltv/cs<名前>_long_dry / wet.wav）の dry を芯（src/motif/fxsim.cpp）に通し、wet と比べる:
  元の音の倍率 gd = 遅れ ±400 の中で dry と最も相関する所の倍率、揺れの線 gw = その残りの大きさ（dry との比）。
  g = 実機 gw / 芯 gw、c = 実機 gd − g × 芯 gd（左右の平均）。芯の出口 = g × H + c × 元の音（録った応答の中の遅れで）。
オフライン（mr_fx_ltv.apply、sampler_fx.h の ltvfx と同じ）は揺れの線の出方が違うので、同じやり方で別の g・c を測って並べる。
近似の組（入口の大きさで効き方が変わる型・歪みや雑音を含む組み合わせ、0D の LoRes など）と、揺れの線がほとんど無い組・芯の元の音が
ほとんど見えない組は外す。測るあいだは今の wetgain.txt をどけておく（芯が直したあとの音で測らないように）。"""
import glob
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mr_fx_ltv
import mr_ref

CAP = os.path.join(HERE, "../../roms/motif-rack-es/capture")
D = mr_fx_ltv.OUT
FS = 44100


def mix(dry, wet, same):
    out = []
    for ch in range(2):
        x0 = np.asarray(dry[ch], dtype=float)
        y0 = np.asarray(wet[ch], dtype=float)
        on = int(np.argmax(np.abs(x0) > 1e-5))
        onw = on if same else int(np.argmax(np.abs(y0) > 1e-5))
        a, aw = on + FS // 2, onw + FS // 2
        n = FS * 6
        x = x0[a:a + n]
        seg = y0[aw - 400:aw + n + 400]
        c = np.fft.irfft(np.fft.rfft(seg, 2 ** 20) * np.conj(np.fft.rfft(x, 2 ** 20)), 2 ** 20)[:801]
        k = int(np.argmax(np.abs(c)))
        g = c[k] / np.dot(x, x)
        r = seg[k:k + n] - g * x
        out.append((g, np.sqrt(np.mean(r ** 2) / np.mean(x ** 2))))
    return (out[0][0] + out[1][0]) / 2, (out[0][1] + out[1][1]) / 2


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "../../build/fxsim.exe")
    wg = os.path.join(D, "wetgain.txt")
    if os.path.exists(wg):
        os.replace(wg, wg + ".old")
    tmp = os.path.join(D, "_wet_tmp.wav")
    rows, skip = [], 0
    for wet in sorted(glob.glob(os.path.join(D, "cs*_long_wet.wav"))):
        k = os.path.basename(wet)[2:-len("_long_wet.wav")]
        if not os.path.exists(os.path.join(D, k + ".ltv")):
            continue
        v = [int(x) for x in k[1:].split("_")]
        dry = wet.replace("_wet.wav", "_dry.wav")
        d = mr_ref.read_wav(dry)
        hg, hw = mix(d, mr_ref.read_wav(wet), False)
        args = [exe, ",".join(str(x) for x in [v[0] * 128 + v[1]] + v[2:18]), os.path.join(CAP, "fxd"), dry, tmp]
        if v[18]:
            args.append("tempo=%d" % v[18])
        subprocess.run(args, check=True, stdout=subprocess.DEVNULL)
        cg, cw = mix(d, mr_ref.read_wav(tmp), True)
        if (v[0], v[1]) in mr_fx_ltv.APPROX_TYPES or v[0] == 13 or hw < 0.1 or cw < 0.05 or cg < 0.3 or abs(hg - cg) > 0.4:
            skip += 1
            continue
        # オフライン（sampler_fx.h の ltvfx と同じ STFT）は、その場で回す芯より揺れの線が 1.2〜1.3 倍大きく出るので、別に測る
        ins = {"type": v[0] * 128 + v[1]}
        for i in range(16):
            ins["param%d" % (i + 1)] = v[2 + i]
        og, ow = mix(d, mr_fx_ltv.apply(np.asarray(d[0], dtype=float), np.asarray(d[1], dtype=float), ins, v[18]), True)
        g = min(2.5, max(0.5, hw / cw))
        c = hg - g * cg
        g2 = min(2.5, max(0.5, hw / max(ow, 1e-6)))
        c2 = hg - g2 * og
        print("%-60s 芯 g %.3f c %+.3f  オフライン g %.3f c %+.3f" % (k[:60], g, c, g2, c2), flush=True)
        if abs(g - 1) >= 0.03 or abs(c) >= 0.02 or abs(g2 - 1) >= 0.03 or abs(c2) >= 0.02:
            rows.append("%s %.4f %.4f %.4f %.4f" % (k, g, c, g2, c2))
    os.remove(tmp)
    with open(wg, "w", encoding="utf-8", newline="") as f:
        f.write("# .ltv の組ごとの揺れの線の倍率 g と元の音の足し引き c（出口 = g × H + c × 元の音）。その場で回す芯の g c、オフラインの g c の順。mr_fx_ltv_wet.py\n")
        f.write("\n".join(rows) + "\n")
    print(len(rows), "組を書いた・", skip, "組は外した")


if __name__ == "__main__":
    main()
