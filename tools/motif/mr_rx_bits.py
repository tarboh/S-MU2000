"""マルチのパートの受信スイッチ（37 pp 1F・20・21）のビットが、何の受信を止めるかを実機で測る（2026-10-27）。
  python mr_rx_bits.py 置き場 [--analyze-only]
マルチの編集バッファ（5F 00）を控え、パート 1 を PRE1:001 にして、1F・20・21 のビットを 1 つずつ切り、チャンネル 1 に
RPN 2（Coarse −12）・ピッチベンド（下いっぱい）・CC7（20）・CC74（0）・CC1（127）・アフタータッチ（127）を 1 つずつ送ってから鍵 60 を鳴らす。
何も切らない組と比べて、音の高さ・大きさ・明るさがその送ったもので変わったかを見る。
送るのは Data List のパラメーターチェンジ・ダンプの要求と送り返し・CC・ピッチベンド・アフタータッチ・ノートだけ（保存しない）。録った音は配らないこと。"""
import os
import sys
import time

import numpy as np

import mr_midi
import mr_ref
from mr_rpn_tune import logspec, mono, rpn, shift_cents

FS = 44100
# 送るもの: 名前 → 送る関数（r）と戻す関数
SENDS = {
    "rpn2": (lambda r: rpn(r, 2, 52), lambda r: rpn(r, 2, 64)),
    "pb": (lambda r: r.mo.short(0xE0, 0, 0), lambda r: r.mo.short(0xE0, 0, 64)),
    "cc7": (lambda r: r.cc(7, 20), lambda r: r.cc(7, 100)),
    "cc74": (lambda r: r.cc(74, 0), lambda r: r.cc(74, 64)),
    "cc11": (lambda r: r.cc(11, 20), lambda r: r.cc(11, 127)),
}
BITS = [(None, None)] + [(off, b) for off in (0x1F, 0x20, 0x21) for b in range(7)]


def tag(off, b):
    return "all" if off is None else "%02X_%d" % (off, b)


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
        r.param(0x37, 0x00, 0x00, 63)
        r.param(0x37, 0x00, 0x01, 0)
        r.param(0x37, 0x00, 0x02, 0)
        time.sleep(1.5)
        for off, b in BITS:
            for o in (0x1F, 0x20, 0x21):
                r.param(0x37, 0x00, o, 0x7F if o != off else 0x7F & ~(1 << b))
            time.sleep(0.2)
            for name, (send, undo) in [("none", (lambda r: None, lambda r: None))] + list(SENDS.items()):
                if off is not None and name == "none":
                    continue
                fn = "%s_%s" % (tag(off, b), name)
                if os.path.exists(os.path.join(d, fn + ".wav")):
                    continue
                r.all_off()
                for c, v in ((7, 100), (11, 127), (74, 64), (10, 64), (91, 0), (93, 0)):
                    r.cc(c, v)
                r.mo.short(0xE0, 0, 64)
                rpn(r, 2, 64)
                time.sleep(0.1)
                send(r)
                time.sleep(0.1)
                r.one_note(fn, note=60, vel=100, hold=0.8, tail=0.3, pre=0.3)
                undo(r)
            print("録った:", tag(off, b), flush=True)
    finally:
        r.all_off()
        if saved:
            for m in saved:
                r.mo.sysex(bytes(m))
                time.sleep(0.03)
        r.mode(3)
        r.close()
        mi.close()


def feats(p):
    m = mono(p)
    lv = 10 * np.log10((m ** 2).mean() + 1e-30)
    X = np.abs(np.fft.rfft(m * np.hanning(len(m)))) ** 2
    f = np.fft.rfftfreq(len(m), 1 / FS)
    cg = float((X * f).sum() / X.sum())
    return lv, cg, logspec(m)


def analyze(d):
    base = feats(os.path.join(d, "all_none.wav"))
    ref = {n: feats(os.path.join(d, "all_%s.wav" % n)) for n in SENDS}
    out = []
    for n in SENDS:
        sh, _ = shift_cents(ref[n][2], base[2], 2400)
        out.append("全部入り %-5s: 何も送らない音との差 高さ %+d セント・大きさ %+.1fdB・重心 %.0f → %.0fHz" % (n, sh, ref[n][0] - base[0], base[1], ref[n][1]))
    for off, b in BITS[1:]:
        row = []
        for n in SENDS:
            p = os.path.join(d, "%s_%s.wav" % (tag(off, b), n))
            if not os.path.exists(p):
                continue
            x = feats(p)
            sh, _ = shift_cents(x[2], base[2], 2400)
            got = abs(sh - shift_cents(ref[n][2], base[2], 2400)[0]) < 50 and abs(x[0] - ref[n][0]) < 1.5 and abs(x[1] - ref[n][1]) < 0.15 * ref[n][1]
            row.append("%s %s（%+d・%+.1f・%.0f）" % (n, "受けた" if got else "受けない", sh, x[0] - base[0], x[1]))
        out.append("%s を切る: %s" % (tag(off, b), " ".join(row)))
    for line in out:
        print(line)
    open(os.path.join(d, "rx_bits.txt"), "w", encoding="utf-8").write("\n".join(out) + "\n")


def main():
    a = sys.argv[1:]
    d = a[0]
    os.makedirs(d, exist_ok=True)
    if "--analyze-only" not in a:
        record(d, a)
    analyze(d)


if __name__ == "__main__":
    main()
