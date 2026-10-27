"""マルチのマスター EQ（36 10 00、5 バンド × ゲイン・周波数・Q・形）を実機で測る（2026-10-27）。
  python mr_master_eq.py 置き場 [--analyze-only]
マルチの編集バッファ（5F 00）を控え、パート 1 をノイズの音色 FX-MWSweep（PRE5:060。LFO も乱数も無く、録るたびに同じ音）にしてフィルターを開き切り（パートのカットオフ 127・CC74 127・
パートのレゾナンス 0・CC71 0・CC1 0）、ほかのパートはチャンネル 1 を受けないようにして、
マスターエフェクト（36 11 00 の型）を thru にする。5 バンドを全部平ら（ゲイン 64）にした音を物差しに、1 バンドずつゲイン・周波数・Q・形を変えた音を録る。
解析は、物差しとの 1/12 オクターブのスペクトルの比を、エフェクトの EQ と同じ形（1 次の棚・RBJ の山）などと比べる（--analyze-only）。
送るのは Data List のパラメーターチェンジ・ダンプの要求と送り返し・CC・ノートだけ（保存しない）。録った音は配らないこと。"""
import os
import sys
import time

import numpy as np

import mr_midi
import mr_ref

FS = 44100
# 平らなときの各バンドの値（ゲイン, 周波数, Q, 形）。周波数は表 3 の目盛り（12 = 80Hz、22 = 225Hz、34 = 1kHz、44 = 3.2kHz、54 = 11kHz）
FLAT = [(64, 12, 7, 0), (64, 22, 7, 0), (64, 34, 7, 0), (64, 44, 7, 0), (64, 54, 7, 0)]


def takes():
    t = [("base", None), ("base2", None)]
    for sh in (0, 1):
        for g in (76, 52):
            t.append(("b0_g%d_f12_q7_s%d" % (g, sh), (0, g, 12, 7, sh)))
            t.append(("b4_g%d_f54_q7_s%d" % (g, sh), (4, g, 54, 7, sh)))
        t.append(("b0_g76_f24_q7_s%d" % sh, (0, 76, 24, 7, sh)))
        t.append(("b4_g76_f44_q7_s%d" % sh, (4, 76, 44, 7, sh)))
        t.append(("b0_g76_f24_q30_s%d" % sh, (0, 76, 24, 30, sh)))
        t.append(("b4_g76_f44_q30_s%d" % sh, (4, 76, 44, 30, sh)))
    for b, f in ((1, 22), (2, 34), (3, 44)):
        for g in (76, 52, 70):
            t.append(("b%d_g%d_f%d_q7_s0" % (b, g, f), (b, g, f, 7, 0)))
        for q in (2, 20, 60):
            t.append(("b%d_g76_f%d_q%d_s0" % (b, f, q), (b, 76, f, q, 0)))
    t.append(("b1_g76_f30_q7_s0", (1, 76, 30, 7, 0)))
    t.append(("b3_g76_f50_q7_s0", (3, 76, 50, 7, 0)))
    t.append(("b2_g76_f34_q7_s1", (2, 76, 34, 7, 1)))      # 中のバンドに形の欄が効くか
    return t


def set_band(r, b, g, f, q, sh):
    for k, v in enumerate((g, f, q, sh)):
        r.param(0x36, 0x10, 4 * b + k, v)


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
        for off, v in ((0x00, 63), (0x01, 4), (0x02, 59), (0x03, 0), (0x04, 1), (0x0D, 100), (0x0E, 64), (0x12, 0), (0x13, 0), (0x15, 127), (0x16, 64),
                       (0x17, 127), (0x18, 0)):
            r.param(0x37, 0x00, off, v)
        for off in (0x60, 0x61, 0x62, 0x63):
            r.param(0x36, 0x00, off, 0)                       # インサーションを使うパートなし
        r.param(0x36, 0x11, 0x00, 0, 0)                       # マスターエフェクトを thru
        for b, v in enumerate(FLAT):
            set_band(r, b, *v)
        for c, v in ((91, 0), (93, 0), (74, 127), (71, 0), (1, 0), (7, 100), (11, 127)):
            r.cc(c, v)
        time.sleep(1.5)
        for name, ch in takes():
            if os.path.exists(os.path.join(d, name + ".wav")):
                continue
            r.all_off()
            time.sleep(0.2)
            # 1 回の打鍵の中で比べる: 前半は平ら、2.0 秒で 1 バンドを変える（同じノイズを 2 つ重ねた音色なので、打鍵ごとに全体の大きさが 2〜5dB 変わる）
            ev = [(0.3, lambda: r.note_on(60, 100)), (4.6, lambda: r.note_off(60))]
            if ch:
                ev.append((2.0, lambda ch=ch: set_band(r, *ch)))
            r.record(name, 5.0, ev)
            if ch:
                set_band(r, ch[0], *FLAT[ch[0]])
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


GRID = 20 * 2 ** (np.arange(0, int(12 * np.log2(18000 / 20)) + 1) / 12)       # 1/12 オクターブ


def spec(p, seg):
    L, R = mr_ref.read_wav(p)
    m = (np.asarray(L) + np.asarray(R)) / 2
    m = m[int(seg[0] * FS):int(seg[1] * FS)]
    n = 8192
    acc = 0
    for i in range(0, len(m) - n, n // 2):
        acc = acc + np.abs(np.fft.rfft(m[i:i + n] * np.hanning(n))) ** 2
    f = np.fft.rfftfreq(n, 1 / FS)
    out = []
    for g in GRID:
        sel = (f >= g * 2 ** (-1 / 24)) & (f < g * 2 ** (1 / 24))
        out.append(10 * np.log10(acc[sel].mean() + 1e-30) if sel.any() else np.nan)
    return np.array(out)


def main():
    a = sys.argv[1:]
    d = a[0]
    os.makedirs(d, exist_ok=True)
    if "--analyze-only" not in a:
        record(d, a)
    A, B = (0.6, 1.9), (2.3, 4.5)                  # 前半（平ら）と後半（変えたあと）
    drift = np.mean([spec(os.path.join(d, n + ".wav"), B) - spec(os.path.join(d, n + ".wav"), A) for n in ("base", "base2")], axis=0)
    np.save(os.path.join(d, "grid.npy"), GRID)
    resp = {"drift(base2)": spec(os.path.join(d, "base2.wav"), B) - spec(os.path.join(d, "base2.wav"), A) - drift}
    for name, ch in takes()[2:]:
        p = os.path.join(d, name + ".wav")
        if os.path.exists(p):
            resp[name] = spec(p, B) - spec(p, A) - drift
    np.save(os.path.join(d, "resp.npy"), resp, allow_pickle=True)
    show = [31.5, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000]
    idx = [int(np.argmin(np.abs(GRID - s))) for s in show]
    print("%-20s" % "" + " ".join("%6.0f" % s for s in show))
    for name, v in resp.items():
        print("%-20s" % name + " ".join("%6.1f" % v[i] for i in idx))


if __name__ == "__main__":
    main()
