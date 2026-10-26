"""マルチのパートの EQ（37 pp 2E〜38）を実機で測る（2026-10-27）。
  python mr_part_eq.py 置き場 [--analyze-only]
マルチモードの編集バッファで、パート 1 をノイズだけの音色 Wind Blows（PRE5:053、04_052）にし、ほかのパートは受けないようにして、
パート 1 の EQ を平らにした音と、1 か所ずつ変えた音を録る。比（帯ごとの dB の差）が EQ の応答。
始める前に編集バッファ（5F 00）を一括ダンプで控え、終わったら送り返す。送るのは Data List のパラメーターチェンジ・ダンプの要求と送り返し・ノートだけ（保存しない）。
録ったものは 置き場/peq_<名前>.wav、読んだ結果は 置き場/part_eq.txt（帯 1/12 オクターブの差）。"""
import json
import os
import sys
import time

import numpy as np

import mr_midi
import mr_ref

FS = 44100
FLAT = {"lf": 0x40, "lg": 0x40, "mf": 0x80, "mg": 0x40, "mq": 0x10, "hf": 0xC0, "hg": 0x40}


def takes():
    t = [("flat", {})]
    for f in (46, 80, 120, 157, 182):
        t.append(("lo_g20_f%d" % f, {"lg": 0x20, "lf": f}))
    for g in (0x30, 0x50, 0x60):
        t.append(("lo_g%02X_f157" % g, {"lg": g, "lf": 157}))
    for f in (83, 120, 160, 200, 241):
        t.append(("mid_g60_f%d" % f, {"mg": 0x60, "mf": f, "mq": 0x10}))
    for q in (0, 8, 24, 31):
        t.append(("mid_g60_f160_q%d" % q, {"mg": 0x60, "mf": 160, "mq": q}))
    t.append(("mid_g20_f160", {"mg": 0x20, "mf": 160, "mq": 0x10}))
    for f in (131, 170, 207, 255):
        t.append(("hi_g20_f%d" % f, {"hg": 0x20, "hf": f}))
    t.append(("hi_g60_f207", {"hg": 0x60, "hf": 207}))
    return t


def send_eq(r, v):
    p = dict(FLAT, **v)
    r.param(0x37, 0x00, 0x2E, p["lf"] >> 7, p["lf"] & 0x7F)
    r.param(0x37, 0x00, 0x30, p["lg"])
    r.param(0x37, 0x00, 0x32, p["mf"] >> 7, p["mf"] & 0x7F)
    r.param(0x37, 0x00, 0x34, p["mg"])
    r.param(0x37, 0x00, 0x35, p["mq"])
    r.param(0x37, 0x00, 0x36, p["hf"] >> 7, p["hf"] & 0x7F)
    r.param(0x37, 0x00, 0x38, p["hg"])
    time.sleep(0.15)


def spec(path):
    L, R = mr_ref.read_wav(path)
    m = (np.asarray(L) + np.asarray(R)) / 2
    m = m[int(1.8 * FS):int(3.8 * FS)]
    n = 8192
    w = np.hanning(n)
    P = sum(np.abs(np.fft.rfft(m[i:i + n] * w)) ** 2 for i in range(0, len(m) - n, n // 4))
    f = np.fft.rfftfreq(n, 1 / FS)
    edges = 40 * 2 ** (np.arange(0, 12 * np.log2(16000 / 40) + 1) / 12)
    return [float((lo * hi) ** 0.5) for lo, hi in zip(edges[:-1], edges[1:])], np.array([10 * np.log10(P[(f >= lo) & (f < hi)].mean() + 1e-30) for lo, hi in zip(edges[:-1], edges[1:])])


def main():
    a = sys.argv[1:]
    d = a[0]
    os.makedirs(d, exist_ok=True)
    if "--analyze-only" not in a:
        midi_out = mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1")
        r = mr_ref.Ref(midi_out, mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
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
            # パート 1: Wind Blows・チャンネル 1・音量 100・パン中央・送り 0・ドライ 127・ずらし無し。ほかのパートは受けない
            for pp in range(1, 16):
                r.param(0x37, pp, 0x03, 0x7F)
            for off, v in ((0x00, 63), (0x01, 4), (0x02, 52), (0x03, 0), (0x04, 1), (0x0D, 100), (0x0E, 64), (0x12, 0), (0x13, 0), (0x15, 127),
                           (0x16, 64), (0x17, 64), (0x18, 64), (0x22, 64), (0x23, 64), (0x25, 64), (0x26, 64), (0x27, 64), (0x28, 64), (0x2A, 64),
                           (0x2B, 64), (0x2C, 64)):
                r.param(0x37, 0x00, off, v)
            for off in (0x60, 0x61, 0x62, 0x63):
                r.param(0x36, 0x00, off, 0)              # インサーションを使うパートは無し
            r.cc(91, 0)
            r.cc(93, 0)
            time.sleep(1.5)                              # 音色を読む間
            for name, v in takes():
                send_eq(r, v)
                r.all_off()
                time.sleep(0.2)
                r.record("peq_" + name, 4.3, [(0.3, lambda: [r.note_on(k, 100) for k in (48, 60, 72)]),
                                              (4.0, lambda: [r.note_off(k) for k in (48, 60, 72)])])
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
    f, base = spec(os.path.join(d, "peq_flat.wav"))
    out = {"freqs": f}
    for name, v in takes()[1:]:
        p = os.path.join(d, "peq_%s.wav" % name)
        if os.path.exists(p):
            out[name] = list(spec(p)[1] - base)
    json.dump(out, open(os.path.join(d, "part_eq.json"), "w"))
    for name in list(out)[1:]:
        v = np.array(out[name])
        idx = [i for i, x in enumerate(f) if x in f[::6]]
        print("%-18s %s" % (name, " ".join("%5.1f" % v[i] for i in range(0, len(f), 6))))
    print("帯（Hz）           " + " ".join("%5d" % f[i] for i in range(0, len(f), 6)))


if __name__ == "__main__":
    main()
