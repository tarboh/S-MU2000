"""ドラムの鍵が、ふつうの音色（雛形）のどの鍵と同じ音程で鳴るかを実機で測る（2026-10-25）。
  python mr_drum_root.py 置き場 [--kit 0] [--keys 36,38,42] [--range 48-72]
ボイスモードで PRE のドラム（バンク 63/32）の鍵を鳴らして録り、次に雛形（mr_ref.template）に同じ波形を入れて range の鍵を 1 つずつ録る。
はじめの 0.4 秒の対数スペクトル（1/3 オクターブより細かい）の相関がいちばん高い鍵を「同じ音程の鍵」とする。
送るのは Data List のパラメーターチェンジ・バンクとプログラムの選び・ノートだけ（保存しない）。終わったらマルチへ戻す。"""
import os
import sys
import time

import numpy as np

import mr_ref
import mr_voice


def logspec(x, n=8192):
    x = np.asarray(x[:int(0.4 * 44100)])
    if len(x) < n:
        x = np.pad(x, (0, n - len(x)))
    X = np.abs(np.fft.rfft(x[:n] * np.hanning(n)))
    f = np.fft.rfftfreq(n, 1 / 44100)
    # 40Hz〜16kHz を対数の目盛り 1/24 オクターブで
    edges = 40 * 2 ** (np.arange(0, 24 * np.log2(16000 / 40) + 1) / 24)
    out = []
    for lo, hi in zip(edges[:-1], edges[1:]):
        m = (f >= lo) & (f < hi)
        out.append(np.log(np.mean(X[m] ** 2) + 1e-20) if m.any() else -46)
    return np.array(out)


def onset(x):
    x = np.asarray(x)
    a = np.abs(x)
    th = max(a.max() * 0.01, 1e-5)
    i = int(np.argmax(a > th))
    return x[i:]


def main():
    a = sys.argv[1:]
    d = a[0]
    kit = int(mr_ref.opt(a, "--kit", "0"))
    keys = [int(k) for k in mr_ref.opt(a, "--keys", "36,38,42").split(",")]
    lo, hi = (int(x) for x in mr_ref.opt(a, "--range", "48-72").split("-"))
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
    here = os.path.dirname(os.path.abspath(__file__))
    b = mr_voice.blocks(os.path.join(here, "..", "..", "roms", "motif-rack-es", "capture", "drum", "20_%03d.syx" % kit))
    try:
        r.all_off()
        r.param(0x00, 0x00, 0x34, 0)            # アルペジオの MIDI 出力を切る（止めたアルペジオの録り込みが入れたまま）
        r.mode(0)
        r.program(63, 32, kit)
        for k in (7, 11):
            r.cc(k, 127)
        r.cc(91, 0)
        r.cc(93, 0)
        drum = {}
        for k in keys:
            o = b[(0x47, k - 24, 0x00)]
            w = o[6] << 7 | o[7]
            x, _ = r.one_note("drum_k%03d" % k, note=k, vel=100, hold=1.0, tail=0.5, pre=0.3)
            drum[k] = (w, x)
            r.all_off()
            time.sleep(0.2)
        r.program(63, 0, 0)
        r.template(0)
        res = []
        for k, (w, x) in drum.items():
            r.el(0, 0x41, 0x03, w >> 7, w & 0x7F)
            time.sleep(0.1)
            sd = logspec(onset(x))
            best = []
            for n in range(lo, hi + 1):
                y, _ = r.one_note("tmpl_w%04d_k%03d" % (w, n), note=n, vel=100, hold=0.8, tail=0.3, pre=0.3)
                r.all_off()
                time.sleep(0.1)
                st = logspec(onset(y))
                c = np.corrcoef(sd, st)[0, 1]
                best.append((c, n))
            best.sort(reverse=True)
            print("ドラムの鍵 %d（波形 %d）: いちばん近い雛形の鍵 %s" % (k, w, " ".join("%d:%.4f" % (n, c) for c, n in best[:4])), flush=True)
            res.append((k, w, best[0][1]))
    finally:
        r.all_off()
        r.mode(3)
        r.close()


if __name__ == "__main__":
    main()
