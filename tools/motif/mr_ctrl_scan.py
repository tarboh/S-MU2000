"""コントロールセットの行き先の番号の意味を、実際の音色（既定は Crazy Mod）を土台に測る（2026-10-25。雛形の音色ではコントロールセットが効かなかった）。
  python mr_ctrl_scan.py --dir 置き場 [--voice 04_063] [--dests 0,1,…,68] [--depth 127] [--read]
音色を選び直してから、組 1〜6 を切り、組 1 だけ「ソース 1（CC1）・行き先 d・深さ」にする（欄ごとに送る。行き先は 2 バイトまとめて）。
鍵 60 を 2.4 秒押さえ、1.0 秒で CC1 を 0 → 127。読むのは CC1 の前と後の、音量・明るさ（スペクトルの重心）・左右・音程・揺れ（10ms ごとの標準偏差）。
リバーブ・コーラスの送りは 0、インサーションのスイッチは切る。送るのはプログラムチェンジ・パラメーターチェンジ・CC・ノートだけ（保存しない）。終わったらマルチへ。"""
import os
import sys
import time

import numpy as np

import mr_ctrl as M
import mr_ctrl_preset as C
import mr_preset_compare as P
import mr_ref


def feats(path):
    f = M.frames(path)
    a = f[(f[:, 0] >= 0.5) & (f[:, 0] < 1.25)]
    b = f[(f[:, 0] >= 1.55) & (f[:, 0] < 2.35)]
    pa, pb = np.maximum(a[:, 2], 1), np.maximum(b[:, 2], 1)
    return np.array([np.median(b[:, 1]) - np.median(a[:, 1]), np.log2(np.median(b[:, 3]) / np.median(a[:, 3])),
                     np.median(b[:, 4]) - np.median(a[:, 4]), 1200 * np.log2(np.median(pb) / np.median(pa)),
                     np.std(b[:, 1]) - np.std(a[:, 1]), np.std(np.log2(b[:, 3])) - np.std(np.log2(a[:, 3])),
                     1200 * (np.std(np.log2(pb)) - np.std(np.log2(pa)))])


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    v = mr_ref.opt(a, "--voice", "04_063")
    dests = [int(x) for x in mr_ref.opt(a, "--dests", ",".join(str(i) for i in range(69))).split(",")]
    depth = int(mr_ref.opt(a, "--depth", "127"))
    os.makedirs(d, exist_ok=True)
    if "--read" not in a:
        r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
        try:
            r.all_off()
            r.mode(0)
            mm, nn = int(v[:2], 16), int(v[3:6])
            msb, lsb = P.bank_select(mm)
            for dv in dests:
                name = "sc_%s_d%03d_x%03d" % (v, dv, depth)
                if os.path.exists(os.path.join(d, name + ".wav")):
                    continue
                r.program(msb, lsb, nn)
                time.sleep(0.3)
                r.param(0x40, 0x70, 0x16, 0)
                r.cc(91, 0)
                r.cc(93, 0)
                for e in range(4):
                    r.el(e, 0x41, 0x16, 0)
                for i in range(6):
                    # 欄ごとに送る: ソース（1 バイト）・行き先（2 バイトまとめて）・深さ・最後の 1 バイト。5 バイトまとめても、行き先を 1 バイトずつでも受け付けない
                    src, dst, dep = (1, dv, depth) if i == 0 else (0, 0, 64)
                    r.param(0x40, 0x05, 5 * i, src)
                    r.param(0x40, 0x05, 5 * i + 1, dst >> 7, dst & 0x7F)
                    r.param(0x40, 0x05, 5 * i + 3, dep)
                    r.param(0x40, 0x05, 5 * i + 4, 0)
                time.sleep(0.1)
                C.reset(r)
                r.record(name, 3.4, [(0.3, lambda: r.note_on(60, 100)), (1.3, lambda: r.mo.short(0xB0, 1, 127)), (2.7, lambda: r.note_off(60))])
                C.reset(r)
                print("行き先", dv, "録った", flush=True)
        finally:
            C.reset(r)
            r.all_off()
            r.mode(3)
            r.close()
    base = feats(os.path.join(d, "sc_%s_d000_x%03d.wav" % (v, depth)))
    print("行き先（CC1 の前後の差 − 行き先 0 の差）: 音量 dB・明るさ オクターブ・左右 dB・音程 セント・音量の揺れ dB・明るさの揺れ・音程の揺れ セント")
    for dv in dests:
        x = feats(os.path.join(d, "sc_%s_d%03d_x%03d.wav" % (v, dv, depth))) - base
        tags = []
        if abs(x[0]) > 1.5: tags.append("音量")
        if abs(x[1]) > 0.3: tags.append("明るさ")
        if abs(x[2]) > 2: tags.append("左右")
        if abs(x[3]) > 20: tags.append("音程")
        if x[4] > 1: tags.append("音量の揺れ")
        if x[5] > 0.1: tags.append("明るさの揺れ")
        if x[6] > 8: tags.append("音程の揺れ")
        print("%3d: %+5.1f %+5.2f %+5.1f %+7.1f %+5.1f %+5.2f %+6.1f  %s" % (dv, *x, " ".join(tags)))


if __name__ == "__main__":
    main()
