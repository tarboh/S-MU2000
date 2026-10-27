"""RPN 1（Fine Tune）・RPN 2（Coarse Tune）がマルチのパートで何をするかを実機で測る（2026-10-27）。
  python mr_rpn_tune.py 置き場 [--analyze-only]
マルチの編集バッファ（5F 00）を一括ダンプで控え、パート 1 の音色を Data List（37 00 00〜02）で替えて、チャンネル 1 に RPN を送ってから鳴らす。
ドラム（PRE キット 0）では「鍵をずらす（ノートシフト）」のか「音の高さを変える」のかを見分ける（鍵 36 の RPN 2 = 64 と、鍵 56 の 44・鍵 36 の 44 を比べる）。
ふつうの音色（PRE1:001）では、ずれの半音数・範囲の端（±24 で頭打ちか）・RPN 1 のセント・プログラムチェンジと CC121 のあとで残るかを見る。
送るのは Data List のパラメーターチェンジ・ダンプの要求と送り返し・CC・ノートだけ（保存しない）。結果は 置き場/rpn_tune.txt。録った音は配らないこと。"""
import os
import sys
import time

import numpy as np

import mr_midi
import mr_ref

FS = 44100
# 名前: (バンク LSB, 番号, 鍵, RPN 2 の値, RPN 1 の MSB, そのあとに送るもの)
TAKES = [
    ("dr_k36_c64", 32, 0, 36, 64, 64, None),
    ("dr_k56_c44", 32, 0, 56, 44, 64, None),
    ("dr_k36_c44", 32, 0, 36, 44, 64, None),
    ("dr_k38_c64", 32, 0, 38, 64, 64, None),
    ("dr_k58_c44", 32, 0, 58, 44, 64, None),
    ("dr_k38_c44", 32, 0, 38, 44, 64, None),
    ("dr_k60_c64", 32, 0, 60, 64, 64, None),
    ("dr_k60_c52", 32, 0, 60, 52, 64, None),
    ("dr_k48_c64", 32, 0, 48, 64, 64, None),
    ("pf_k60_c64", 0, 0, 60, 64, 64, None),
    ("pf_k60_c52", 0, 0, 60, 52, 64, None),
    ("pf_k60_c76", 0, 0, 60, 76, 64, None),
    ("pf_k60_c40", 0, 0, 60, 40, 64, None),
    ("pf_k60_c30", 0, 0, 60, 30, 64, None),
    ("pf_k60_c0", 0, 0, 60, 0, 64, None),
    ("pf_k60_c88", 0, 0, 60, 88, 64, None),
    ("pf_k60_c100", 0, 0, 60, 100, 64, None),
    ("pf_k60_c127", 0, 0, 60, 127, 64, None),
    ("pf_k69_c64", 0, 0, 69, 64, 64, None),
    ("pf_k69_f96", 0, 0, 69, 64, 96, None),
    ("pf_k69_f32", 0, 0, 69, 64, 32, None),
    ("pf_k69_f127", 0, 0, 69, 64, 127, None),
    ("pf_k48_c64", 0, 0, 48, 64, 64, None),
    ("pf_k72_c52", 0, 0, 72, 52, 64, None),
    ("pf_k60_c52_pc", 0, 0, 60, 52, 64, "pc"),
    ("pf_k60_c52_rst", 0, 0, 60, 52, 64, "rst"),
]


def rpn(r, num, msb, lsb=None):
    r.cc(101, 0)
    r.cc(100, num)
    r.cc(6, msb)
    if lsb is not None:
        r.cc(38, lsb)
    r.cc(101, 127)
    r.cc(100, 127)


def record(d, a):
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
    mi = mr_midi.MidiIn(mr_ref.opt(a, "--midi-in", "MOTIF-RACK ES-1"))
    saved = None
    cur = None
    try:
        r.all_off()
        r.param(0x00, 0x00, 0x34, 0)
        r.mode(3)
        saved = mr_midi.request_block(mi, r.mo, 0, 0x5F, 0x00)
        if not saved:
            raise SystemExit("マルチの編集バッファを控えられない（ダンプが返らない）ので止める")
        open(os.path.join(d, "saved_5F.syx"), "wb").write(b"".join(bytes(m) for m in saved))
        for name, lsb, prog, key, c, f, after in TAKES:
            if os.path.exists(os.path.join(d, name + ".wav")):
                continue
            r.all_off()
            if cur != (lsb, prog):
                r.param(0x37, 0x00, 0x00, 63)
                r.param(0x37, 0x00, 0x01, lsb)
                r.param(0x37, 0x00, 0x02, prog)
                time.sleep(1.5)
                cur = (lsb, prog)
            for cc, v in ((7, 100), (10, 64), (91, 0), (93, 0)):
                r.cc(cc, v)
            rpn(r, 2, c)
            rpn(r, 1, f, 0)
            time.sleep(0.1)
            if after == "pc":
                r.cc(0, 63)
                r.cc(32, lsb)
                r.mo.short(0xC0, prog)
                time.sleep(1.5)
            elif after == "rst":
                r.cc(121, 0)
                time.sleep(0.1)
            r.one_note(name, note=key, vel=100, hold=1.0, tail=0.5, pre=0.3)
            rpn(r, 2, 64)
            rpn(r, 1, 64, 0)
            print("録った:", name, flush=True)
    finally:
        r.all_off()
        rpn(r, 2, 64)
        rpn(r, 1, 64, 0)
        if saved:
            for m in saved:
                r.mo.sysex(bytes(m))
                time.sleep(0.03)
        r.mode(3)
        r.close()
        mi.close()


def mono(p):
    L, R = mr_ref.read_wav(p)
    m = (np.asarray(L, float) + np.asarray(R, float)) / 2
    on = int(np.argmax(np.abs(m) > np.abs(m).max() * 0.02))
    return m[on:on + int(0.8 * FS)]


def logspec(m):
    """20Hz〜16kHz を 1/100 半音（1 セント）刻みで並べた dB のスペクトル"""
    n = 1 << 16
    X = np.abs(np.fft.rfft(m * np.hanning(len(m)), n)) ** 2
    f = np.fft.rfftfreq(n, 1 / FS)
    grid = 20 * 2 ** (np.arange(0, int(1200 * np.log2(16000 / 20))) / 1200)
    return 10 * np.log10(np.interp(grid, f, X) + 1e-20)


def shift_cents(a, b, lim=6600):
    """b を何セントずらすと a にいちばん重なるか（a が高ければ正）"""
    best = (-1e9, 0)
    for s in range(-lim, lim + 1, 5):
        if s >= 0:
            x, y = a[s:], b[:len(b) - s]
        else:
            x, y = a[:len(a) + s], b[-s:]
        x = np.maximum(x, x.max() - 60)
        y = np.maximum(y, y.max() - 60)
        c = np.corrcoef(x, y)[0, 1]
        if c > best[0]:
            best = (c, s)
    return best[1], best[0]


def analyze(d):
    w = {name: mono(os.path.join(d, name + ".wav")) for name, *_ in TAKES if os.path.exists(os.path.join(d, name + ".wav"))}
    s = {k: logspec(v) for k, v in w.items()}
    out = []

    def same(a, b):
        n = min(len(w[a]), len(w[b]))
        x, y = w[a][:n], w[b][:n]
        return float(np.corrcoef(x, y)[0, 1])

    for a, b in (("dr_k36_c64", "dr_k56_c44"), ("dr_k36_c64", "dr_k36_c44"), ("dr_k38_c64", "dr_k58_c44"), ("dr_k38_c64", "dr_k38_c44"),
                 ("dr_k48_c64", "dr_k60_c52"), ("dr_k60_c64", "dr_k60_c52"), ("pf_k48_c64", "pf_k60_c52"), ("pf_k60_c64", "pf_k72_c52"),
                 ("pf_k60_c64", "pf_k60_c52_pc")):
        if a in w and b in w:
            sh, c = shift_cents(s[b], s[a])
            out.append("%s と %s: 波形の相関 %.3f、%s は %+d セント（スペクトルの重なり %.3f）" % (a, b, same(a, b), b, sh, c))
    for name in ("pf_k60_c52", "pf_k60_c76", "pf_k60_c40", "pf_k60_c30", "pf_k60_c0", "pf_k60_c88", "pf_k60_c100", "pf_k60_c127",
                 "pf_k60_c52_pc", "pf_k60_c52_rst"):
        if name in w and "pf_k60_c64" in w:
            sh, c = shift_cents(s[name], s["pf_k60_c64"])
            out.append("%s: %+d セント（%.2f 半音、重なり %.3f）" % (name, sh, sh / 100, c))
    for name in ("pf_k69_f96", "pf_k69_f32", "pf_k69_f127"):
        if name in w and "pf_k69_c64" in w:
            sh, c = shift_cents(s[name], s["pf_k69_c64"], 300)
            out.append("%s: %+d セント（重なり %.3f）" % (name, sh, c))
    for line in out:
        print(line)
    open(os.path.join(d, "rpn_tune.txt"), "w", encoding="utf-8").write("\n".join(out) + "\n")


def main():
    a = sys.argv[1:]
    d = a[0]
    os.makedirs(d, exist_ok=True)
    if "--analyze-only" not in a:
        record(d, a)
    analyze(d)


if __name__ == "__main__":
    main()
