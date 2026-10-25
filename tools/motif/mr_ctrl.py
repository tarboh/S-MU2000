"""音色のコントロールセット（40 05 00 から 5 バイト × 6 組: ソース・行き先の上・行き先・深さ（64 が 0）・0）の、ソースと行き先の番号の意味を測る（2026-10-25）。
  python mr_ctrl.py --dir 置き場 --dests 0,3,41,45,66 [--kinds sin,noi,lfs,lfn] [--mod] [--read]
（--kinds の lfs は正弦波に LFO の音程・音量 32、lfn はノイズの LPF に LFO のフィルター 32 と FEG。--mod は行き先 0 からの揺れの差で読む）
  python mr_ctrl.py --dir 置き場 --sources 0,1,2,…,12 --dest 番号 [--read]
--dests: 組 1 をソース 1・行き先 d・深さ 127 にし、正弦波 704（THRU）とノイズ 705（LPF24D・カットオフ 100）を 2.4 秒鳴らす。1.0 秒で CC1 を 0 → 127。
--sources: 行き先を決めて、ソースの番号ごとに 1 本録る。1.3 秒から 0.3 秒おきに MSGS の CC・アフタータッチ・ピッチベンド（上）を 0 → 最大にして、
どの時刻で変わるかを見る（CC11 は音が消えるので送らない）。分かったこと: 0 PB・1 CC1・2/3 AT・6 CC22・7 CC2・8 CC16・9 CC17・10〜12 CC4（2026-10-25）。
--kinds の cls は正弦波にコモン LFO（箱 1 を音程・深さ 16）を掛けたもの。
エレメントのコントロールボックスのスイッチ（41 ee 18、2 バイトまとめて送る）は全部入れる。送るのはパラメーターチェンジ・CC・アフタータッチ・ピッチベンド・ノートだけ（保存しない）。
終わったら CC を戻し、マルチへ戻す。"""
import os
import sys
import time

import numpy as np

import mr_ref

FS = 44100
MSGS = [("CC1", 0xB0, 1), ("CC2", 0xB0, 2), ("CC4", 0xB0, 4), ("CC12", 0xB0, 12), ("CC13", 0xB0, 13), ("CC16", 0xB0, 16), ("CC17", 0xB0, 17),
        ("CC18", 0xB0, 18), ("CC19", 0xB0, 19), ("CC20", 0xB0, 20), ("CC21", 0xB0, 21), ("AT", 0xD0, None), ("PB", 0xE0, None)]


# CC18 は AEG のアタックをずらす（64 が中心、0 で速く 127 で遅く。パラメーターを変えても残り、プログラムチェンジと CC121 で中心に戻る。2026-10-25）。
# CC19〜21 はリリース・ディケイ・明るさに効かなかったが、同じく 64 に戻しておく
CENTER_CC = {18, 19, 20, 21}


def reset_ctrls(r):
    for _, st, n in MSGS:
        if st == 0xB0:
            r.mo.short(0xB0, n, 64 if n in CENTER_CC else 0)
        elif st == 0xD0:
            r.mo.short(0xD0, 0, 0)
        else:
            r.mo.short(0xE0, 0, 64)
    time.sleep(0.05)


def set_ctrl(r, src, dest, depth):
    # 欄ごとに送る: ソース（1 バイト）・行き先（2 バイトまとめて）・深さ・最後の 1 バイト（5 バイトまとめても、行き先を 1 バイトずつでも受け付けない）
    for i in range(6):
        s_, d_, dp = (src, dest, depth) if i == 0 else (0, 0, 64)
        r.param(0x40, 0x05, 5 * i, s_)
        r.param(0x40, 0x05, 5 * i + 1, d_ >> 7, d_ & 0x7F)
        r.param(0x40, 0x05, 5 * i + 3, dp)
        r.param(0x40, 0x05, 5 * i + 4, 0)


def setup(r, kind):
    """雛形を 1 つ用意する: sin・lfs は正弦波 704、noi・lfn はノイズ 705。コントロールボックスは全部入れる"""
    wave = 704 if kind in ("sin", "lfs", "cls") else 705
    r.template(wave)
    r.el(0, 0x41, 0x18, 0, 0x3F)       # コントロールボックスのスイッチは 2 バイトの欄。1 バイトずつ送ると効かない
    if kind in ("noi", "lfn"):
        r.el(0, 0x43, 0x00, 0)
        r.el(0, 0x43, 0x01, 0, 100)
        r.el(0, 0x43, 0x05, 0)
    if kind in ("lfs", "lfn"):
        # エレメントの LFO（三角・速さ 32）を、正弦波は音程と音量に、ノイズはフィルターに 32 ずつ掛ける
        r.el(0, 0x44, 0x40, 1)
        r.el(0, 0x44, 0x41, 1)          # キーオンでそろえる（録るたびの位相のずれを消す）
        r.el(0, 0x44, 0x43, 32)
        r.el(0, 0x44, 0x44, 32 if kind == "lfs" else 0)
        r.el(0, 0x44, 0x45, 32 if kind == "lfs" else 0)
        r.el(0, 0x44, 0x46, 32 if kind == "lfn" else 0)
    if kind == "cls":
        # コモン LFO: 三角・速さ 40・キーオンでそろえる、箱 1 を音程（PMD）・深さ 16。エレメントは箱 1 に入れる（深さの割合 127）
        for off, v in {0x00: 0, 0x01: 0, 0x02: 40, 0x04: 0, 0x05: 0, 0x06: 0, 0x07: 127, 0x08: 0, 0x09: 1, 0x0A: 1, 0x0B: 16,
                       0x0D: 0, 0x0E: 0, 0x10: 0, 0x11: 0}.items():
            r.param(0x40, 0x06, off, v)
        r.el(0, 0x41, 0x1A, 0, 1)
        r.el(0, 0x44, 0x51, 127)
    if kind == "lfn":
        # FEG: 時間 0、高さ全部 +32、深さ 96（カットオフを上げたまま。FEG の深さの行き先が見える）
        for i in range(5):
            r.el(0, 0x43, 0x0E + i, 0)
            r.el(0, 0x43, 0x13 + 2 * i, 1, 32)
        r.el(0, 0x43, 0x1D, 96)


def frames(path):
    L, R = mr_ref.read_wav(path)
    L, R = np.asarray(L, float), np.asarray(R, float)
    x = L + R
    on = int(np.argmax(np.abs(x) > 1e-4))
    hop = 882
    out = []
    for i in range(on, len(x) - 4096, hop):
        seg = x[i:i + 4096]
        lv = 10 * np.log10(np.mean(seg ** 2) + 1e-20)
        sp = np.abs(np.fft.rfft(seg * np.hanning(4096), 1 << 16))
        fr = np.fft.rfftfreq(1 << 16, 1 / FS)
        k = int(np.argmax(sp[:int(4000 / fr[1])]))
        cen = float(np.sum(fr * sp ** 2) / (np.sum(sp ** 2) + 1e-20))
        bal = 10 * np.log10(np.mean(L[i:i + 4096] ** 2) + 1e-20) - 10 * np.log10(np.mean(R[i:i + 4096] ** 2) + 1e-20)
        out.append(((i - on) / FS, lv, fr[k], cen, bal))
    return np.array(out)


def summary(path, t0=(0.3, 0.75), t1=(1.3, 1.9)):
    f = frames(path)
    a = f[(f[:, 0] >= t0[0]) & (f[:, 0] < t0[1])]
    b = f[(f[:, 0] >= t1[0]) & (f[:, 0] < t1[1])]
    cents = 1200 * np.log2(np.median(b[:, 2]) / np.median(a[:, 2]))
    vib = 1200 * np.std(np.log2(b[:, 2])) - 1200 * np.std(np.log2(a[:, 2]))
    trem = np.std(b[:, 1]) - np.std(a[:, 1])
    return np.median(b[:, 1]) - np.median(a[:, 1]), cents, vib, trem, np.median(b[:, 3]) / np.median(a[:, 3]), np.median(b[:, 4]) - np.median(a[:, 4])


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    dests = mr_ref.opt(a, "--dests")
    sources = mr_ref.opt(a, "--sources")
    sdest = int(mr_ref.opt(a, "--dest", "0"))
    kinds = mr_ref.opt(a, "--kinds", "sin,noi").split(",")
    os.makedirs(d, exist_ok=True)
    if "--read" not in a:
        r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
        try:
            r.all_off()
            r.mode(0)
            r.program(63, 0, 0)
            if dests:
                for dv in [int(x) for x in dests.split(",")]:
                    for kind, wave in [(k, 704 if k in ("sin", "lfs") else 705) for k in kinds]:
                        name = "cd%03d_%s" % (dv, kind)
                        if os.path.exists(os.path.join(d, name + ".wav")):
                            continue
                        setup(r, kind)
                        set_ctrl(r, 1, dv, 127)
                        reset_ctrls(r)
                        r.record(name, 0.3 + 2.4 + 0.4, [(0.3, lambda: r.note_on(60, 100)), (1.3, lambda: r.mo.short(0xB0, 1, 127)),
                                                        (2.7, lambda: r.note_off(60))])
                        reset_ctrls(r)
                    print("行き先", dv, "録った", flush=True)
            if sources:
                r.template(704)
                r.el(0, 0x41, 0x18, 0, 0x3F)
                for sv in [int(x) for x in sources.split(",")]:
                    name = "cs%03d_d%03d" % (sv, sdest)
                    if os.path.exists(os.path.join(d, name + ".wav")):
                        continue
                    set_ctrl(r, sv, sdest, 127)
                    reset_ctrls(r)
                    ev = [(0.3, lambda: r.note_on(60, 100))]
                    for j, (_, st, n) in enumerate(MSGS):
                        t = 1.3 + 0.3 * j
                        if st == 0xB0:
                            ev.append((t, lambda n=n: r.mo.short(0xB0, n, 127 if n != 11 else 0)))
                        elif st == 0xD0:
                            ev.append((t, lambda: r.mo.short(0xD0, 127, 0)))
                        else:
                            ev.append((t, lambda: r.mo.short(0xE0, 127, 127)))
                    ev.append((1.3 + 0.3 * len(MSGS) + 0.3, lambda: r.note_off(60)))
                    r.record(name, 1.3 + 0.3 * len(MSGS) + 0.8, ev)
                    reset_ctrls(r)
                    print("ソース", sv, "録った", flush=True)
        finally:
            try:
                reset_ctrls(r)
                set_ctrl(r, 0, 0, 64)
            finally:
                r.all_off()
                r.mode(3)
                r.close()
    if dests and "--mod" in a:
        # 基準（行き先 0）からの差: 揺れの大きさ（音程のセント・音量の dB・明るさの比の標準偏差）と中央値
        def mf(p):
            f = frames(p)
            b = f[(f[:, 0] >= 1.4) & (f[:, 0] < 2.6)]
            return (np.median(b[:, 1]), 1200 * np.std(np.log2(np.maximum(b[:, 2], 1))), np.std(b[:, 1]), np.median(b[:, 3]), np.std(np.log2(b[:, 3])),
                    np.median(b[:, 4]), 1200 * np.log2(np.median(np.maximum(b[:, 2], 1))))
        bs = {k: mf(os.path.join(d, "cd000_%s.wav" % k)) for k in kinds}
        print("行き先: 種類ごとに（音量の差 dB・音程の揺れの差 セント・音量の揺れの差 dB・明るさの比・明るさの揺れの差 オクターブ・左右の差 dB・音程の差 セント）")
        for dv in [int(x) for x in dests.split(",")]:
            row = []
            for k in kinds:
                x, b0 = mf(os.path.join(d, "cd%03d_%s.wav" % (dv, k))), bs[k]
                row.append("%s %+5.1f %+6.1f %+4.1f %.2f %+5.2f %+5.1f %+6.1f" % (k, x[0] - b0[0], x[1] - b0[1], x[2] - b0[2], x[3] / b0[3], x[4] - b0[4], x[5] - b0[5], x[6] - b0[6]))
            print("行き先 %3d: %s" % (dv, " | ".join(row)))
        return
    if dests:
        print("行き先: 音量 dB・音程 セント・ビブラートの増え セント・音量の揺れの増え dB・明るさ（ノイズの重心の比）・左右 dB")
        for dv in [int(x) for x in dests.split(",")]:
            s = summary(os.path.join(d, "cd%03d_sin.wav" % dv))
            n = summary(os.path.join(d, "cd%03d_noi.wav" % dv))
            print("行き先 %3d: 音量 %+5.1f 音程 %+6.1f ビブラート %+5.1f 揺れ %+4.1f | ノイズ: 音量 %+5.1f 明るさ %.2f 左右 %+4.1f" % (dv, s[0], s[1], s[2], s[3], n[0], n[4], s[5]))
    if sources:
        print("ソースごとに、変わった時刻（1.3 秒から 0.3 秒おきに " + "・".join(m[0] for m in MSGS) + "）")
        for sv in [int(x) for x in sources.split(",")]:
            f = frames(os.path.join(d, "cs%03d_d%03d.wav" % (sv, sdest)))
            base = f[(f[:, 0] > 0.6) & (f[:, 0] < 1.25)]
            l0, p0 = np.median(base[:, 1]), np.median(base[:, 2])
            hit = []
            for j, (nm, _, _) in enumerate(MSGS):
                t = 1.0 + 0.3 * j      # frames の時刻は音の頭（録音の 0.3 秒）から
                seg = f[(f[:, 0] > t + 0.12) & (f[:, 0] < t + 0.28)]
                if len(seg):
                    hit.append("%s %+.1fdB %+.0fc" % (nm, np.median(seg[:, 1]) - l0, 1200 * np.log2(np.median(seg[:, 2]) / p0)))
            print("ソース %2d: %s" % (sv, " | ".join(hit)))


if __name__ == "__main__":
    main()
