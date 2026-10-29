"""MOTIF-RACK ES のインサーションの Compressor（0A 00）を測る録音と読み取り。
  python mr_fx_comp.py --dir 置き場 [--set p3=103,p4=3,…] [--tag 名前] [--analyze] [--step-only] [--type 9:48] [--key 48]

正弦波の波形 704・鍵 60 をインサーション A に通す（つなぎ方は並列）。パラメーター: 01 Attack（表 8: 0〜19 → 1〜40ms）、02 Release（表 9: 0〜15 → 10〜680ms）、
03 Threshold（79〜121 → −48〜−6dB）、04 Ratio（表 10: 0〜7 → 1.0・1.5・2.0・3.0・5.0・7.0・10・20）、05 Output Level（0〜127）。
  static : エレメントの音量 127・112・96・80・64・48・32・16 で、スイッチを切った（dry）と入れた（wet）を 1 秒ずつ（cs<tag>_L???_dry/wet.wav）
  step   : AEG を「127 で 0.79 秒（アタックの値 64、高さ 127 → 127）、そのあと時間 0 で 64 へ」にして wet（cs<tag>_step.wav）。
           鳴り始めの効き方（アタック）と、下がったあとの戻り方（リリース）を見る
--type でほかのインサーションの型（MSB:LSB）、--key で鳴らす鍵を変えられる（歪み系の静的な曲線に使う）。
--wave 波形（既定 704 = 正弦波、705 = ノイズ）。--long 秒 は、音量 --level（既定 100）で dry と wet を 1 本ずつ長く録るだけ（cs<tag>_long_dry/wet.wav）。
--analyze で読むだけ（入力 = dry の音量、出力 = wet の音量、どちらも 0.5〜0.9 秒の RMS、dBFS）。
送るのはパラメーターチェンジ・ダンプの要求・ノートだけ（保存しない）。終わったらマルチへ戻す。"""
import json
import os
import sys
import time

import numpy as np

import mr_midi
import mr_params
import mr_ref

LEVELS = (127, 112, 96, 80, 64, 48, 32, 16)
FS = 44100


def rms_db(path, t0, t1):
    L, R = mr_ref.read_wav(path)
    x = np.asarray(L)
    on = int(np.argmax(np.abs(x) > 1e-5))
    s = x[on + int(t0 * FS):on + int(t1 * FS)]
    return 20 * np.log10(np.sqrt(np.mean(s ** 2)) + 1e-12)


def env(path, win=0.002):
    L, R = mr_ref.read_wav(path)
    x = np.asarray(L)
    on = int(np.argmax(np.abs(x) > 1e-5))
    x = x[on:]
    n = int(win * FS)
    m = len(x) // n
    return 20 * np.log10(np.sqrt(np.mean(x[:m * n].reshape(m, n) ** 2, axis=1)) + 1e-12)


def analyze(d, tag):
    print("static（%s）: 入力 dBFS → 出力 dBFS（差）" % tag)
    pts = []
    for L in LEVELS:
        b = os.path.join(d, "cs%s_L%03d" % (tag, L))
        if not os.path.exists(b + "_wet.wav"):
            continue
        i, o = rms_db(b + "_dry.wav", 0.5, 0.9), rms_db(b + "_wet.wav", 0.5, 0.9)
        pts.append((i, o))
        print("  L %3d: %7.2f → %7.2f（%+.2f）" % (L, i, o, o - i))
    st = os.path.join(d, "cs%s_step.wav" % tag)
    if os.path.exists(st):
        e = env(st)
        print("step: 2ms ごとの dBFS（鳴り始め 0〜60ms）:", " ".join("%.1f" % v for v in e[:30]))
        k = int(0.79 / 0.002)
        print("step: 下げたあと（0.79 秒から 20ms ごと）:", " ".join("%.1f" % v for v in e[k - 5:k + 200:10]))
    return pts


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    tag = mr_ref.opt(a, "--tag", "")
    st = mr_ref.opt(a, "--set")
    if "--analyze" in a:
        analyze(d, tag)
        return
    sets = []
    for kv in (st.split(",") if st else []):
        k, v = kv.split("=")
        v = int(v)
        sets.append((0x02 + 2 * (int(k[1:]) - 1), (v >> 7, v & 0x7F)))
    itype = tuple(int(v) for v in mr_ref.opt(a, "--type", "10:0").split(":"))
    key = int(mr_ref.opt(a, "--key", "60"))
    os.makedirs(d, exist_ok=True)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
    mi = mr_midi.MidiIn(mr_ref.opt(a, "--midi-in", "MOTIF-RACK ES-1"))

    # --at 3:p16=1,8:p16=0 … --long の wet の途中で、鳴らし始めから t 秒にインサーションのパラメーターを替える（ロータリーの速さの切り替えなど）
    ats = []
    for s in (mr_ref.opt(a, "--at", "") or "").split(","):
        if s:
            t, kv = s.split(":")
            k, v = kv.split("=")
            ats.append((float(t), 0x02 + 2 * (int(k[1:]) - 1), int(v)))

    # --ccat 5:1=4,10:1=8 … --long の wet の途中で、鳴らし始めから t 秒に CC を送る（コントロールセットの目盛りを見る）
    ccs = []
    for s in (mr_ref.opt(a, "--ccat", "") or "").split(","):
        if s:
            t, kv = s.split(":")
            k, v = kv.split("=")
            ccs.append((float(t), int(k), int(v)))
    # --cs ソース:行き先:深さ … 雛形の音色のコントロールセット 1（40 05 00、5 バイト: ソース・行き先の上下・深さ・0）。行き先 4〜19 は INS1（A）の P1〜16
    cs = mr_ref.opt(a, "--cs")

    def take(name, hold=1.0, at=()):
        if not os.path.exists(os.path.join(d, name + ".wav")):
            ev = [(0.3, lambda: r.note_on(key, 100)), (0.3 + hold, lambda: r.note_off(key))]
            for t, off, v in at:
                ev.append((0.3 + t, lambda off=off, v=v: r.param(0x40, 0x03, off, v >> 7, v & 0x7F)))
            for t, c, v in ccs:
                ev.append((0.3 + t, lambda c=c, v=v: r.cc(c, v)))
            r.record(name, 0.3 + hold + 0.3, sorted(ev, key=lambda e: e[0]))

    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(int(mr_ref.opt(a, "--wave", "704")))     # 既定は正弦波。705 はノイズ（鳴らし始めから毎回同じ並び）
        r.el(0, 0x43, 0x00, 21)
        r.el(0, 0x43, 0x01, 1, 127)
        r.el(0, 0x43, 0x22, 64)
        r.param(0x40, 0x00, 0x0B, 0)
        r.param(0x40, 0x00, 0x0C, 0)
        r.param(0x40, 0x00, 0x40, 0)                # つなぎ方を並列に
        for e in range(4):
            r.el(e, 0x41, 0x16, 0)
        r.param(0x40, 0x03, 0x00, *itype)           # 型（既定は Compressor）
        if cs:
            src, dst, dep = (int(x) for x in cs.split(":"))
            r.param(0x40, 0x05, 0x00, src)
            r.param(0x40, 0x05, 0x01, dst >> 7, dst & 0x7F)
            r.param(0x40, 0x05, 0x03, dep)
        for c, _ in {(c, 0) for _, c, _ in ccs}:
            r.cc(c, 0)
        time.sleep(0.3)
        for off, v in sets:
            r.param(0x40, 0x03, off, *v)
        time.sleep(0.1)
        got = mr_midi.request_block(mi, r.mo, 0, 0x0F, 0x00)
        for m in got or []:
            if m[7:10] == bytes([0x40, 0x03, 0x00]):
                info = mr_params.decode_rows(mr_params.COMMON_BLOCKS[(0x40, 0x03, 0x00)], list(m[10:-2]))
                json.dump(info, open(os.path.join(d, "cs%s.json" % tag), "w"), indent=1)
        longs = mr_ref.opt(a, "--long")
        if longs:                                   # 決まった音量で長く 1 本ずつ（揺れる型の倍率と遅れを読む）
            r.el(0, 0x41, 0x20, int(mr_ref.opt(a, "--level", "100")))
            r.el(0, 0x41, 0x16, 0)
            time.sleep(0.05)
            take("cs%s_long_dry" % tag, float(longs))
            r.el(0, 0x41, 0x16, 1)
            time.sleep(0.05)
            take("cs%s_long_wet" % tag, float(longs), ats)
            return
        for L in (() if "--step-only" in a else LEVELS):
            r.el(0, 0x41, 0x20, L)
            r.el(0, 0x41, 0x16, 0)
            time.sleep(0.05)
            take("cs%s_L%03d_dry" % (tag, L))
            r.el(0, 0x41, 0x16, 1)
            time.sleep(0.05)
            take("cs%s_L%03d_wet" % (tag, L))
        r.el(0, 0x41, 0x20, 127)
        r.el(0, 0x41, 0x24, 64)                     # アタックの段 0.79 秒（127 → 127）
        r.el(0, 0x41, 0x25, 0)
        r.el(0, 0x41, 0x2A, 64)
        r.el(0, 0x41, 0x2B, 64)
        r.el(0, 0x41, 0x16, 1)
        time.sleep(0.05)
        take("cs%s_step" % tag, 2.0)
    finally:
        try:
            r.el(0, 0x41, 0x16, 0)
            for c, _ in {(c, 0) for _, c, _ in ccs}:
                r.cc(c, 0)
            r.all_off()
            r.mode(3)
        finally:
            mi.close()
            r.close()
    analyze(d, tag)


if __name__ == "__main__":
    main()
