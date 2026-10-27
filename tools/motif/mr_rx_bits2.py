"""マルチのパートの受信スイッチ（37 pp 1F・20・21）を、組ごとに状態を戻してから測る（2026-10-28。mr_rx_bits.py は、状態を戻す CC まで
切ったスイッチで止められていて、組どうしが混ざっていた）。
  python mr_rx_bits2.py 置き場 [--analyze-only]
組ごとに: スイッチを全部入れる → CC7 100・CC11 127・CC10 64・CC74 64・CC64 0・ベンド中央に戻す → 1 つのスイッチだけ切る → 試すメッセージを送る → 鳴らす。
試すもの（パート 1 = PRE1:001）: CC74 0（1F bit2 = CC）・ベンド下いっぱい（1F bit6 = PB）・CC7 20 と CC11 20（20 bit0 = 音量/エクスプレッション）・
CC10 1（20 bit1 = パン）・CC64 127 で離す（20 bit3 = サステイン）。スイッチを入れたままの組と切った組を比べる。
マルチの編集バッファ（5F 00）を控え、終わったら戻す。送るのは Data List のパラメーターチェンジ・ダンプの要求と送り返し・CC・ピッチベンド・ノートだけ（保存しない）。"""
import os
import sys
import time

import numpy as np

import mr_midi
import mr_ref
from mr_rpn_tune import logspec, shift_cents

FS = 44100
TESTS = [
    ("cc74", (0x1F, 2), [(0xB0, 74, 0)]),
    ("pb", (0x1F, 6), [(0xE0, 0, 0)]),
    ("cc7", (0x20, 0), [(0xB0, 7, 20)]),
    ("cc11", (0x20, 0), [(0xB0, 11, 20)]),
    ("cc10", (0x20, 1), [(0xB0, 10, 1)]),
    ("cc64", (0x20, 3), [(0xB0, 64, 127)]),
]


def reset(r):
    for o in (0x1F, 0x20, 0x21):
        r.param(0x37, 0x00, o, 0x7F)
    for c, v in ((7, 100), (11, 127), (10, 64), (74, 64), (64, 0), (1, 0), (91, 0), (93, 0)):
        r.cc(c, v)
    r.mo.short(0xE0, 0, 64)
    time.sleep(0.15)


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
        r.param(0x37, 0x00, 0x00, 63)
        r.param(0x37, 0x00, 0x01, 0)
        r.param(0x37, 0x00, 0x02, 0)
        r.param(0x37, 0x00, 0x03, 0)
        time.sleep(1.5)
        reset(r)
        r.all_off()
        time.sleep(0.2)
        r.record("base", 2.0, [(0.2, lambda: r.note_on(60, 100)), (0.9, lambda: r.note_off(60))])
        for name, (off, bit), msgs in TESTS:
            for state in ("on", "off"):
                reset(r)
                if state == "off":
                    r.param(0x37, 0x00, off, 0x7F & ~(1 << bit))
                    time.sleep(0.1)
                for m in msgs:
                    r.mo.short(*m)
                time.sleep(0.15)
                r.all_off() if name != "cc64" else None
                time.sleep(0.1)
                r.record("%s_%s" % (name, state), 2.0, [(0.2, lambda: r.note_on(60, 100)), (0.9, lambda: r.note_off(60))])
                print(name, state, flush=True)
    finally:
        r.all_off()
        reset(r)
        if saved:
            for m in saved:
                r.mo.sysex(bytes(m))
                time.sleep(0.03)
        r.mode(3)
        r.close()
        mi.close()


def feats(p):
    L, R = mr_ref.read_wav(p)
    L, R = np.asarray(L), np.asarray(R)
    m = (L + R) / 2
    a = np.abs(m)
    on = int(np.argmax(a > a.max() * 0.02))
    x = m[on + 2000:on + int(0.5 * FS)]
    lv = 10 * np.log10((x ** 2).mean() + 1e-30)
    X = np.abs(np.fft.rfft(x)) ** 2
    f = np.fft.rfftfreq(len(x), 1 / FS)
    cg = float((X * f).sum() / X.sum())
    lr = 10 * np.log10((L[on:on + 20000] ** 2).mean() / ((R[on:on + 20000] ** 2).mean() + 1e-30))
    tail = 10 * np.log10((m[on + int(1.0 * FS):on + int(1.4 * FS)] ** 2).mean() + 1e-30)
    return lv, cg, lr, tail, logspec(m[on:on + int(0.6 * FS)])


def main():
    a = sys.argv[1:]
    d = a[0]
    os.makedirs(d, exist_ok=True)
    if "--analyze-only" not in a:
        record(d, a)
    b = feats(os.path.join(d, "base.wav"))
    print("物差し: 大きさ %.1fdB・重心 %.0fHz・左右 %+.1fdB・離して 0.3 秒 %.1fdB" % (b[0], b[1], b[2], b[3]))
    for name, (off, bit), msgs in TESTS:
        row = []
        for state in ("on", "off"):
            x = feats(os.path.join(d, "%s_%s.wav" % (name, state)))
            sh, _ = shift_cents(x[4], b[4], 400)
            row.append("%s: 大きさ %+.1f・重心 %.0f・左右 %+.1f・離したあと %+.1f・高さ %+d" % (state, x[0] - b[0], x[1], x[2], x[3] - b[3], sh))
        print("%-5s（%02X bit%d）  %s" % (name, off, bit, "  |  ".join(row)))


if __name__ == "__main__":
    main()
