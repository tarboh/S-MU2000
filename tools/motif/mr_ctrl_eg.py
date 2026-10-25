"""コントロールセットの EG の行き先（50〜65: PEG・AEG・FEG の時間と深さ）を、ノートの前に CC1 を置いて測る（2026-10-25）。
  python mr_ctrl_eg.py --dir 置き場 [--dests 50,51,…,65] [--read]
鳴っている間に CC を動かしても EG の行き先は変わらなかったので、組 1 を「CC1・行き先・深さ」にしてから CC1 を 127（か 64）にし、そのあとでノートを鳴らす。
行き先ごとに、その欄の値そのものを振った基準（深さは REFS、時間は TREFS）と、欄を BASE に置いて深さを振ったもの（DEPTHS・TDEPTHS）を録り、
曲線（10ms ごと 3 秒）を基準の曲線と照らして実効の値に直す。（実効の値 − BASE）/ p が倍率 R（p =（深さ − 64）/ 64 × CC1 / 127）。
AEG は正弦波 704 の音量、PEG は正弦波の音程、FEG はノイズ 705（LPF24D・カットオフ 100）の明るさ（重心）で読む。鍵 60・ベロシティ 100、1.5 秒で離す。
送るのはパラメーターチェンジ・CC・ノートだけ（保存しない）。終わったら CC を戻し、マルチへ戻す。"""
import os
import sys
import time

import numpy as np

import mr_ctrl as M
import mr_ref

FS = 44100
BASE = 64
REFS = (32, 48, 64, 80, 96, 112)
DEPTHS = ((48, 127), (56, 127), (72, 127), (80, 127), (96, 127), (96, 64))   # （深さ, CC1）。深さの行き先（50・60）
# 時間の行き先は、ずらす量を小さくして、基準を 8 ずつ細かく（R が 256 前後なので、深さ ±2〜±8 で ±8〜±32）
TREFS = (40, 48, 56, 64, 72, 80, 88)
TDEPTHS = ((60, 127), (62, 127), (66, 127), (68, 127), (72, 64))
NOTE_ON, NOTE_OFF, LEN = 0.3, 1.8, 3.6

# 行き先 → （EG, 段, 読み方）。段: dpt 深さ・hld/atk/d1/d2/rel 時間
DESTS = {50: ("peg", "dpt"), 51: ("peg", "hld"), 52: ("peg", "atk"), 53: ("peg", "d1"), 54: ("peg", "d2"), 55: ("peg", "rel"),
         56: ("aeg", "atk"), 57: ("aeg", "d1"), 58: ("aeg", "d2"), 59: ("aeg", "rel"),
         60: ("feg", "dpt"), 61: ("feg", "hld"), 62: ("feg", "atk"), 63: ("feg", "d1"), 64: ("feg", "d2"), 65: ("feg", "rel")}
SEG = ("hld", "atk", "d1", "d2", "rel")


def setup(r, eg, seg, val):
    """雛形を作り、EG の段 seg の欄を val にする（dpt なら深さ）"""
    if eg == "feg":
        M.setup(r, "noi")
    else:
        M.setup(r, "sin")
    if eg == "aeg":
        # 時間: アタック・D1・D2・リリース。高さ: 初め・アタック・D1・D2
        # 測らない段は雛形のまま（D1・D2 は 127）。D1 と D2 の時間を両方 0 にすると実機は鳴らない（2026-10-25）
        t = {"atk": 0, "d1": 127, "d2": 127, "rel": 0}
        if seg == "d2":
            t["d1"] = 0
        lv = [127, 127, 127, 127]
        if seg == "atk":
            lv[0] = 0
        elif seg == "d1":
            lv[2] = lv[3] = 32
        elif seg == "d2":
            lv[3] = 32
        t[seg] = val
        for i, k in enumerate(("atk", "d1", "d2", "rel")):
            r.el(0, 0x41, 0x24 + i, t[k])
        for i in range(4):
            r.el(0, 0x41, 0x28 + i, lv[i])
        return
    # PEG・FEG: 高さは 中心からの目盛り（2 バイト、128 が 0）。AEG はリリースを長くして離したあとも鳴らす
    r.el(0, 0x41, 0x27, 90)
    blk, toff, loff, doff = (0x42, 0x05, 0x0A, 0x14) if eg == "peg" else (0x43, 0x0E, 0x13, 0x1D)
    times = [0] * 5
    levels = [0] * 5
    depth = 96 if eg == "peg" else 127
    amt = 32 if eg == "peg" else 64          # FEG は明るさの変わりが見えるように高さ ±64・深さ 127
    if seg == "dpt":
        levels = [32] * 5
        depth = val if eg == "peg" else val
    else:
        k = SEG.index(seg)
        times[k] = val
        if seg == "hld":
            levels = [amt, 0, 0, 0, 0]
        elif seg == "atk":
            levels = [amt, 0, 0, 0, 0]
        elif seg == "d1":
            levels = [0, 0, amt, amt, 0]
        elif seg == "d2":
            levels = [0, 0, 0, amt, 0]
        else:
            levels = [0, 0, 0, 0, amt]
    for i in range(5):
        r.el(0, blk, toff + i, times[i])
        v = 128 + levels[i]
        r.el(0, blk, loff + 2 * i, v >> 7, v & 0x7F)
    r.el(0, blk, doff, depth)


def env(path, eg):
    """10ms ごとの（時刻, 値）。時刻は音の頭から。値は aeg: 音量 dB、peg: 音程 セント（261.63Hz から）、feg: 重心の log2"""
    L, R = mr_ref.read_wav(path)
    x = np.asarray(L, float) + np.asarray(R, float)
    on = int(np.argmax(np.abs(x) > 1e-4))
    hop = 441
    n = 2048
    out = []
    win = np.hanning(n)
    fr = np.fft.rfftfreq(1 << 15, 1 / FS)
    for i in range(on, len(x) - n, hop):
        seg = x[i:i + n]
        if eg == "aeg":
            seg = x[i:i + 441]
            out.append(((i - on) / FS, 10 * np.log10(np.mean(seg ** 2) + 1e-20)))
            continue
        sp = np.abs(np.fft.rfft(seg * win, 1 << 15))
        if eg == "peg":
            k = int(np.argmax(sp[:int(4000 / fr[1])]))
            out.append(((i - on) / FS, 1200 * np.log2(max(fr[k], 1) / 261.63)))
        else:
            out.append(((i - on) / FS, np.log2(np.sum(fr * sp ** 2) / (np.sum(sp ** 2) + 1e-20) + 1)))
    return np.array(out)


def curve(path, eg):
    """曲線の読み（照らし合わせ用）: 音の頭から 3.0 秒まで 10ms ごと。aeg は音量 dB（−80 で止める）"""
    e = env(path, eg)
    t = np.arange(0, 3.0, 0.01)
    v = np.interp(t, e[:, 0], e[:, 1])
    return np.maximum(v, -80) if eg == "aeg" else v


def match(x, refs):
    """基準の曲線 refs（値 → 曲線）のうち x にいちばん近い所を、近い 2 本の間で比べて値にする"""
    vs = sorted(refs)
    err = np.array([np.sqrt(np.mean((x - refs[v]) ** 2)) for v in vs])
    i = int(np.argmin(err))
    j = i + 1 if i + 1 < len(vs) and (i == 0 or err[i + 1] < err[i - 1]) else i - 1
    # 2 本の間を 0〜1 で混ぜて、いちばん近い所
    best, bw = 1e9, 0.0
    for w in np.linspace(0, 1, 101):
        e = np.sqrt(np.mean((x - ((1 - w) * refs[vs[i]] + w * refs[vs[j]])) ** 2))
        if e < best:
            best, bw = e, w
    return vs[i] + bw * (vs[j] - vs[i]), best


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    dests = [int(x) for x in mr_ref.opt(a, "--dests", ",".join(str(k) for k in DESTS)).split(",")]
    os.makedirs(d, exist_ok=True)
    if "--read" not in a:
        r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
        try:
            r.all_off()
            r.mode(0)
            r.program(63, 0, 0)
            for dv in dests:
                eg, seg = DESTS[dv]
                refs, deps = (REFS, DEPTHS) if seg == "dpt" else (TREFS, TDEPTHS)
                takes = [("eg2_d%03d_ref%03d" % (dv, v), v, 64, 0) for v in refs] + [("eg2_d%03d_x%03d_v%03d" % (dv, dp, cv), BASE, dp, cv) for dp, cv in deps]
                for name, val, dp, cv in takes:
                    if os.path.exists(os.path.join(d, name + ".wav")):
                        continue
                    setup(r, eg, seg, val)
                    M.set_ctrl(r, 1, dv, dp)
                    M.reset_ctrls(r)
                    r.mo.short(0xB0, 1, cv)
                    time.sleep(0.05)
                    r.record(name, LEN, [(NOTE_ON, lambda: r.note_on(60, 100)), (NOTE_OFF, lambda: r.note_off(60))])
                    M.reset_ctrls(r)
                    print(name, "録った", flush=True)
        finally:
            try:
                M.reset_ctrls(r)
                M.set_ctrl(r, 0, 0, 64)
            finally:
                r.all_off()
                r.mode(3)
                r.close()
    print("行き先: 深さ × CC1 の曲線を、欄の値を振った基準の曲線と照らした実効の値 → R（ずれは照らしたときの残り）")
    for dv in dests:
        eg, seg = DESTS[dv]
        refs, deps = (REFS, DEPTHS) if seg == "dpt" else (TREFS, TDEPTHS)
        rc = {v: curve(os.path.join(d, "eg2_d%03d_ref%03d.wav" % (dv, v)), eg) for v in refs if os.path.exists(os.path.join(d, "eg2_d%03d_ref%03d.wav" % (dv, v)))}
        if len(rc) < 2:
            continue
        out = []
        for dp, cv in deps:
            p = os.path.join(d, "eg2_d%03d_x%03d_v%03d.wav" % (dv, dp, cv))
            if not os.path.exists(p):
                continue
            eff, err = match(curve(p, eg), rc)
            pp = (dp - 64) / 64.0 * cv / 127.0
            out.append("%+d×%d → %.1f（R %+.0f、ずれ %.2f）" % (dp - 64, cv, eff, (eff - BASE) / pp, err))
        print("%d %s %s: %s" % (dv, eg, seg, " | ".join(out)))


if __name__ == "__main__":
    main()
