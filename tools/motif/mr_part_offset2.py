"""マルチのパートのデチューン（37 pp 10、2 バイト）・AEG のサステイン（22）・FEG のずらし（27・28・2A・2B・2C）を実機で測る（2026-10-28）。
  python mr_part_offset2.py 置き場 [--analyze-only]
マルチの編集バッファ（5F 00）を控え、パート 1 だけがチャンネル 1 を受けるようにして、パートの欄を 1 つずつ変えて鳴らす。終わったら控えを送り返す。
  detune: Full Grand（PRE1:001）の鍵 36・84。値 0x80 が 0、−128〜+127（Data List では −12.8〜+12.7Hz）
  aeg_sus: BigStrings（PRE3:022）の鍵 60 を 3 秒押さえ、2〜2.8 秒の大きさ
  feg_*: FX-MWSweep（PRE5:060、ノイズを FEG で掃く。CC74 は 64）の鍵 60、20ms ごとの明るさ（重心）
送るのは Data List のパラメーターチェンジ・ダンプの要求と送り返し・CC・ノートだけ（保存しない）。録った音は配らないこと。"""
import os
import sys
import time

import numpy as np

import mr_midi
import mr_ref
from mr_rpn_tune import logspec, shift_cents

FS = 44100
VOICES = {"gp": (0, 0), "str": (2, 21), "sw": (4, 59), "dry": (4, 37)}


def takes():
    t = []
    for v in (0x80, 0x80 + 127, 0x80 - 128, 0x80 + 64, 0x80 + 32):
        for k in (36, 84):
            # 2 バイトに 4 ビットずつ（1 バイト目が上位、2 バイト目が下位。Data List の「1st bit3-0 → bit7-4、2nd bit3-0 → bit3-0」）
            t.append(("dt2_%03d_k%03d" % (v, k), "gp", [(0x10, v >> 4, v & 0x0F)], k, 0.8))
    for v in (64, 48, 32, 16, 0, 96, 127):
        t.append(("aegsus2_%03d" % v, "dry", [(0x22, v)], 60, 3.0))    # Dry Syn（エレメント 1 つ、ディケイ 2 が 54 でサステイン 104）
    for name, off in (("fegsus", 0x27), ("fegatk", 0x28), ("fegdec", 0x2A), ("fegrel", 0x2B), ("fegdep", 0x2C)):
        for v in (64, 32, 96):
            t.append(("%s_%03d" % (name, v), "sw", [(off, v)], 60, 2.0))
    return t


def record(d, a):
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
    mi = mr_midi.MidiIn(mr_ref.opt(a, "--midi-in", "MOTIF-RACK ES-1"))
    saved = None
    try:
        r.all_off()
        r.param(0x00, 0x00, 0x34, 0)
        r.mode(3)
        saved = mr_midi.request_block(mi, r.mo, 0, 0x5F, 0x00)
        if not saved:
            raise SystemExit("マルチの編集バッファを控えられない（ダンプが返らない）ので止める")
        open(os.path.join(d, "saved_5F.syx"), "wb").write(b"".join(bytes(m) for m in saved))
        for pp in range(1, 16):
            r.param(0x37, pp, 0x03, 0x7F)
        for off in (0x60, 0x61, 0x62, 0x63):
            r.param(0x36, 0x00, off, 0)
        base = [(0x03, 0), (0x04, 1), (0x0D, 100), (0x0E, 64), (0x12, 0), (0x13, 0), (0x15, 127), (0x16, 64), (0x17, 64), (0x18, 64),
                (0x22, 64), (0x23, 64), (0x25, 64), (0x26, 64), (0x27, 64), (0x28, 64), (0x2A, 64), (0x2B, 64), (0x2C, 64)]
        cur = None
        for name, voice, changes, key, hold in takes():
            if os.path.exists(os.path.join(d, name + ".wav")):
                continue
            if cur != voice:
                lsb, prog = VOICES[voice]
                r.param(0x37, 0x00, 0x00, 63)
                r.param(0x37, 0x00, 0x01, lsb)
                r.param(0x37, 0x00, 0x02, prog)
                time.sleep(1.5)
                cur = voice
            for off, v in base:
                r.param(0x37, 0x00, off, v)
            r.param(0x37, 0x00, 0x10, 8, 0)                 # デチューン 0（0x80 = 上位 8・下位 0）
            for c, v in ((7, 100), (11, 127), (74, 64), (71, 64), (1, 0), (91, 0), (93, 0)):
                r.cc(c, v)
            for ch in changes:
                r.param(0x37, 0x00, ch[0], *ch[1:])
            time.sleep(0.2)
            r.all_off()
            time.sleep(0.2)
            r.one_note(name, note=key, vel=100, hold=hold, tail=0.3, pre=0.2)
            print(name, flush=True)
    finally:
        r.all_off()
        if saved:
            for m in saved:
                r.mo.sysex(bytes(m))
                time.sleep(0.03)
        r.mode(3)
        r.close()
        mi.close()


def mono(p):
    L, R = mr_ref.read_wav(p)
    m = (np.asarray(L) + np.asarray(R)) / 2
    a = np.abs(m)
    on = int(np.argmax(a > a.max() * 0.02))
    return m[on:]


def main():
    a = sys.argv[1:]
    d = a[0]
    os.makedirs(d, exist_ok=True)
    if "--analyze-only" not in a:
        record(d, a)
    print("デチューン（0x80 との差。鍵 36 は 4 倍音、鍵 84 は基音の山で読む）")
    def f0(p, fexp):
        m = mono(p)
        y = m[4410:30000]
        n = 1 << 19
        Y = np.abs(np.fft.rfft(y * np.hanning(len(y)), n))
        f = np.fft.rfftfreq(n, 1 / FS)
        sel = (f > fexp * 0.97) & (f < fexp * 1.03)
        i = int(np.argmax(np.where(sel, Y, 0)))
        al, be, ga = np.log(Y[i - 1]), np.log(Y[i]), np.log(Y[i + 1])
        return (i + 0.5 * (al - ga) / (al - 2 * be + ga)) * FS / n
    for k, fe, h in ((36, 65.406 * 4, 4), (84, 1046.502, 1)):
        r = f0(os.path.join(d, "dt2_%03d_k%03d.wav" % (0x80, k)), fe)
        for v in (0x80 + 127, 0x80 - 128, 0x80 + 64, 0x80 + 32):
            x = f0(os.path.join(d, "dt2_%03d_k%03d.wav" % (v, k)), fe)
            print("  鍵 %d 値 %+4d: %+.2f セント・%+.2fHz（Data List の %+.1fHz）" % (k, v - 0x80, 1200 * np.log2(x / r), (x - r) / h, (v - 0x80) / 10.0))
    print("AEG のサステイン（Dry Syn、64 との差、1.5〜2.5 秒の大きさ）")
    lv = lambda p: 10 * np.log10((mono(p)[int(1.5 * FS):int(2.5 * FS)] ** 2).mean() + 1e-30)
    b = lv(os.path.join(d, "aegsus2_064.wav"))
    for v in (48, 32, 16, 0, 96, 127):
        print("  %3d: %+.2fdB" % (v, lv(os.path.join(d, "aegsus2_%03d.wav" % v)) - b))
    print("FEG（20ms ごとの重心 Hz、0〜1.6 秒を 0.2 秒おき）")
    def cg(p):
        m = mono(p)
        out = []
        for t in np.arange(0, 1.8, 0.2):
            y = m[int(t * FS):int(t * FS) + 882]
            Y = np.abs(np.fft.rfft(y * np.hanning(len(y)))) ** 2
            f = np.fft.rfftfreq(len(y), 1 / FS)
            out.append((Y * f).sum() / (Y.sum() + 1e-30))
        return out
    for name in ("fegsus", "fegatk", "fegdec", "fegrel", "fegdep"):
        for v in (64, 32, 96):
            print("  %s %3d: %s" % (name, v, " ".join("%5.0f" % c for c in cg(os.path.join(d, "%s_%03d.wav" % (name, v))))))


if __name__ == "__main__":
    main()
