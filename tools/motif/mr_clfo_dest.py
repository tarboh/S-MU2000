"""音色のコモン LFO（40 06 00）の行き先 FMD・RESO・PAN・エレメント LFO の速さを測る。
  python mr_clfo_dest.py --dir 置き場 [--only 名前,…]
コモン LFO はユーザー波形 2 段（127・0。はじめの 2.4 秒が +1、次の 2.4 秒が −1）・速さ 16 にして、行き先が止まって +1・−1 にずれた所を、
LFO なしで値を変えた録音（st_…）と比べて、深さが何目盛りずらすかを読む（mr_clfo_dest_read.py）。
  fmd : ノイズ（705）・BEF12。カットオフ 128 で深さ 16・32・64・127、比べはカットオフ 32〜224
  reso: ノイズ・LPF24D・カットオフ 128。レゾナンス 64 で深さ 32・64・127、比べはレゾナンス 0〜127
  pan : 正弦波。パン中央で深さ 16・32・64・127
  lspd: 正弦波。エレメント LFO（三角・音程の深さ 64・速さ 32）の速さを深さ 16・32・64・127 で
送るのはパラメーターチェンジ・ダンプの要求・ノートだけ（保存しない）。終わったらマルチへ戻す。"""
import os
import sys

import mr_ref

BASE = {0x00: 0, 0x01: 12, 0x02: 16, 0x04: 0, 0x05: 0, 0x06: 0, 0x07: 127, 0x08: 0, 0x09: 1, 0x0B: 0,
        0x0D: 0, 0x0E: 0, 0x10: 0, 0x11: 0, 0x13: 0, 0x15: 127, 0x16: 0}
HOLD = 5.2

# 名前: (準備の種類, コモン LFO の欄, エレメントの欄 {(ブロック, 番地): 値})
TAKES = []
for c in (32, 64, 96, 128, 160, 192, 224):
    TAKES.append(("st_fmd_c%03d" % c, "noise_bef", {}, {(0x43, 0x01): (c >> 7, c & 0x7F)}))
for d in (16, 32, 64, 127):
    TAKES.append(("fmd%03d" % d, "noise_bef", {0x0A: 2, 0x0B: d}, {}))
for q in (0, 32, 64, 96, 127):
    TAKES.append(("st_reso_q%03d" % q, "noise_lpf", {}, {(0x43, 0x05): (q,)}))
for d in (32, 64, 127):
    TAKES.append(("reso%03d" % d, "noise_lpf", {0x0A: 3, 0x0B: d}, {}))
for d in (16, 32, 64, 127):
    TAKES.append(("pan%03d" % d, "sine", {0x0A: 4, 0x0B: d}, {}))
for d in (16, 32, 64, 127):
    TAKES.append(("lspd%03d" % d, "sine_lfo", {0x0A: 5, 0x0B: d}, {}))
TAKES.append(("st_lspd", "sine_lfo", {}, {}))
# 2 回目: 細かい深さ、±0.5（ユーザー波形 96・32）で深さ × 振れ だけで決まるか、レゾナンスのもとの値、遅い LFO で速さ
for d in (8, 24, 48, 96):
    TAKES.append(("pan%03d" % d, "sine", {0x0A: 4, 0x0B: d}, {}))
TAKES.append(("pan064h", "sine", {0x0A: 4, 0x0B: 64, 0x15: 96, 0x16: 32}, {}))
for d in (4, 8):
    TAKES.append(("fmd%03d" % d, "noise_bef", {0x0A: 2, 0x0B: d}, {}))
TAKES.append(("fmd032h", "noise_bef", {0x0A: 2, 0x0B: 32, 0x15: 96, 0x16: 32}, {}))
TAKES.append(("reso016", "noise_lpf", {0x0A: 3, 0x0B: 16}, {}))
for q in (16, 48, 80, 112):
    TAKES.append(("st_reso_q%03d" % q, "noise_lpf", {}, {(0x43, 0x05): (q,)}))
for q in (32, 96):
    TAKES.append(("reso032_b%03d" % q, "noise_lpf", {0x0A: 3, 0x0B: 32}, {(0x43, 0x05): (q,)}))
for d in (16, 32, 64, 127):                                             # 三角（0 から上がる）・速さ 16 の 1 周で、振れごとの左右を読む
    TAKES.append(("panT%03d" % d, "sine", {0x01: 0, 0x0A: 4, 0x0B: d}, {}))
# 遅いエレメント LFO（速さ 1・8）で、速さの変わり目がいつ効くか: はじめ 0 のまま 2.4 秒、そのあと +1（lspdC）、はじめから +1（lspdP）
for sp in (1, 8):
    TAKES.append(("lspdC_s%02d" % sp, "sine_lfo%d" % sp, {0x0A: 5, 0x0B: 64, 0x15: 64, 0x16: 127}, {}, 8.0))
    TAKES.append(("lspdP_s%02d" % sp, "sine_lfo%d" % sp, {0x0A: 5, 0x0B: 64, 0x15: 127, 0x16: 127}, {}, 8.0))
    TAKES.append(("lspd0_s%02d" % sp, "sine_lfo%d" % sp, {}, {}, 8.0))
for w in (0, 2, 6):                                                     # RevReality（05_057）と同じ: 速い（47）コモン LFO で深さ 66、エレメント LFO は速さ 1
    TAKES.append(("lspdW%02d" % w, "sine_lfo1", {0x01: w, 0x02: 47, 0x0A: 5, 0x0B: 66}, {}, 6.0))
TAKES.append(("lspdW02a", "sine_lfo1", {0x01: 2, 0x02: 47, 0x0A: 5, 0x0B: 66}, {(0x44, 0x44): (127,), (0x44, 0x45): (0,)}, 6.0))   # 同じで音量を揺らす
TAKES.append(("lspd0a", "sine_lfo1", {}, {(0x44, 0x44): (127,), (0x44, 0x45): (0,)}, 6.0))
for d in (2, 8, 16, 32):
    TAKES.append(("lspdS%03d" % d, "sine_lfo", {0x0A: 5, 0x0B: d, 0x02: 8}, {}, 10.6))


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    only = mr_ref.opt(a, "--only")
    os.makedirs(d, exist_ok=True)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)

    def setup(kind):
        r.template(705 if kind.startswith("noise") else 704)
        r.el(0, 0x41, 0x1A, 1)                      # LFO の箱 1 に入れる
        r.el(0, 0x44, 0x51, 127)
        r.el(0, 0x43, 0x00, 21)                     # THRU（雛形は型を戻さない）
        if kind == "noise_bef":
            r.el(0, 0x43, 0x00, 12)                 # BEF12
            r.el(0, 0x43, 0x01, 1, 0)               # カットオフ 128
        elif kind == "noise_lpf":
            r.el(0, 0x43, 0x00, 0)                  # LPF24D
            r.el(0, 0x43, 0x01, 1, 0)
            r.el(0, 0x43, 0x05, 64)                 # レゾナンス 64
        elif kind.startswith("sine_lfo"):
            for off, v in ((0, 1), (1, 1), (2, 0), (3, int(kind[8:] or 32)), (4, 0), (5, 64), (6, 0), (7, 0)):
                r.el(0, 0x44, 0x40 + off, v)        # 三角・キーオンでそろえる・速さ 32・音程の深さ 64

    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        last = None
        for take in TAKES:
            name, kind, ch, els = take[:4]
            hold = take[4] if len(take) > 4 else HOLD
            if only and name not in only.split(","):
                continue
            if os.path.exists(os.path.join(d, name + ".wav")):
                continue
            if kind != last:
                setup(kind)
                last = kind
            prm = dict(BASE)
            prm.update(ch)
            for off, v in prm.items():
                r.param(0x40, 0x06, off, v)
            for (blk, off), v in els.items():
                r.el(0, blk, off, *v)
            r.record(name, 0.3 + hold + 0.3, [(0.3, lambda: r.note_on(69, 100)), (0.3 + hold, lambda: r.note_off(69))])
            if els:
                last = None                         # 変えた欄は次で雛形から作り直す
            print(name, "録った", flush=True)
    finally:
        r.all_off()
        r.param(0x40, 0x06, 0x0B, 0)
        r.mode(3)
        r.close()


if __name__ == "__main__":
    main()
