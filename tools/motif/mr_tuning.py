"""マイクロチューニング（40 00 00 の 0x04 Scale・0x0F Root）と、エレメントの Pitch Fine Scaling（42 ee 00 の 0x1D）を実機で測る（2026-10-28）。
  python mr_tuning.py 置き場 [--analyze-only]
ボイスモードの雛形（mr_ref.template、波形 704 のサイン）で鳴らし、0.15〜0.6 秒の高さ（FFT の山を放物線で補う）を、平均律の高さと比べる（セント）。
  scale<S>_root<R>_k<鍵>: 音階 S（0〜12）、主音 R の鍵 60〜71
  fs<V>_k<鍵>: Pitch Fine Scaling を V にした鍵 36〜84（音階は Equal）
マルチの編集バッファ（5F 00）を控え、終わったら戻す。送るのは Data List のパラメーターチェンジ・ダンプの要求と送り返し・CC・ノートだけ（保存しない）。"""
import os
import sys
import time

import numpy as np

import mr_midi
import mr_ref

FS = 44100
SCALES = ["Equal", "PureMajor", "PureMinor", "Werckmeist", "Kirnberger", "Vallot&Yng", "1/4 Shift", "1/4 Tone", "1/8 Tone", "Indian", "Arabic 1",
          "Arabic 2", "Arabic 3"]
FSV = (0, 32, 64, 96, 127)
FSK = (36, 48, 60, 72, 84)


def takes():
    t = []
    for s in range(13):
        for k in range(60, 72):
            t.append(("scale%02d_root00_k%03d" % (s, k), s, 0, 64, k))
    for k in range(60, 72):
        t.append(("scale04_root02_k%03d" % k, 4, 2, 64, k))
    for sc in (7, 8, 9):
        for k in list(range(48, 60)) + list(range(72, 84)):
            t.append(("scale%02d_root00_k%03d" % (sc, k), sc, 0, 64, k))
    for root in (2, 9):
        for k in range(60, 72):
            t.append(("r0F_scale01_root%02d_k%03d" % (root, k), 1, root, 64, k))
            t.append(("r0F_scale04_root%02d_k%03d" % (root, k), 4, root, 64, k))
    for sc in (7, 8):
        for k in list(range(24, 48)) + list(range(84, 108)):
            t.append(("scale%02d_root00_k%03d" % (sc, k), sc, 0, 64, k))
        for k in range(60, 72):
            t.append(("r0F_scale%02d_root02_k%03d" % (sc, k), sc, 2, 64, k))
    for k in list(range(24, 48)) + list(range(84, 108)):
        t.append(("scale00_root00_k%03d" % k, 0, 0, 64, k))
    for k in list(range(48, 60)) + list(range(72, 84)):
        t.append(("scale00_root00_k%03d" % k, 0, 0, 64, k))
    for v in FSV:
        for k in FSK:
            t.append(("fs%03d_k%03d" % (v, k), 0, 0, v, k))
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
        r.mode(0)
        r.program(63, 0, 0)
        r.template(704)
        for c in (1, 2):
            for x, y in ((101, 0), (100, c), (6, 64), (38, 0), (101, 127), (100, 127)):
                r.cc(x, y)
        r.mo.short(0xE0, 0, 64)
        cur = None
        for name, s, root, fsv, k in takes():
            if os.path.exists(os.path.join(d, name + ".wav")):
                continue
            if cur != (s, root, fsv):
                r.param(0x40, 0x00, 0x04, s)
                r.param(0x40, 0x00, 0x10, root)
                r.el(0, 0x42, 0x1D, fsv)
                time.sleep(0.2)
                cur = (s, root, fsv)
            r.all_off()
            time.sleep(0.05)
            r.one_note(name, note=k, vel=100, hold=0.6, tail=0.1, pre=0.1)
        print("録った", flush=True)
    finally:
        r.all_off()
        r.mode(3)
        if saved:
            for m in saved:
                r.mo.sysex(bytes(m))
                time.sleep(0.03)
        r.close()
        mi.close()


def pitch_cents(p, key):
    L, R = mr_ref.read_wav(p)
    m = (np.asarray(L) + np.asarray(R)) / 2
    a = np.abs(m)
    on = int(np.argmax(a > a.max() * 0.1))
    y = m[on + int(0.1 * FS):on + int(0.5 * FS)]
    n = 1 << 18
    Y = np.abs(np.fft.rfft(y * np.hanning(len(y)), n))
    f0 = 440.0 * 2 ** ((key - 69) / 12)
    f = np.fft.rfftfreq(n, 1 / FS)
    sel = (f > f0 / 1.3) & (f < f0 * 1.3)
    i = int(np.argmax(np.where(sel, Y, 0)))
    al, be, ga = np.log(Y[i - 1] + 1e-30), np.log(Y[i] + 1e-30), np.log(Y[i + 1] + 1e-30)
    pk = 0.5 * (al - ga) / (al - 2 * be + ga)
    fr = (i + pk) * FS / n
    return 1200 * np.log2(fr / f0)


def main():
    a = sys.argv[1:]
    d = a[0]
    os.makedirs(d, exist_ok=True)
    if "--analyze-only" not in a:
        record(d, a)
    c = {n: pitch_cents(os.path.join(d, n + ".wav"), k) for n, s, root, fsv, k in takes() if os.path.exists(os.path.join(d, n + ".wav"))}
    # 雛形そのもののずれ（Equal の各鍵）を引く
    eq = {k: c["scale00_root00_k%03d" % k] for k in range(60, 72)}
    out = ["音階（主音 C、鍵 60〜71。Equal との差、セント）"]
    for s in range(13):
        out.append("  %2d %-10s " % (s, SCALES[s]) + " ".join("%+6.1f" % (c["scale%02d_root00_k%03d" % (s, k)] - eq[k]) for k in range(60, 72)))
    out.append("  Kirnberger の主音 D: " + " ".join("%+6.1f" % (c["scale04_root02_k%03d" % k] - eq[k]) for k in range(60, 72)))
    eqa = {k: c.get("scale00_root00_k%03d" % k, 0.0) for k in range(48, 84)}
    for sc in (7, 8, 9):
        for o in (48, 72):
            if "scale%02d_root00_k%03d" % (sc, o) in c:
                out.append("  %-10s 鍵 %d〜: " % (SCALES[sc], o) + " ".join("%+6.1f" % (c["scale%02d_root00_k%03d" % (sc, k)] - eqa[k]) for k in range(o, o + 12)))
    for sc, nm in ((1, "PureMajor"), (4, "Kirnberger")):
        for root in (2, 9):
            if "r0F_scale%02d_root%02d_k060" % (sc, root) in c:
                out.append("  %s 主音 %d: " % (nm, root) + " ".join("%+6.1f" % (c["r0F_scale%02d_root%02d_k%03d" % (sc, root, k)] - eq[k]) for k in range(60, 72)))
    eqa = {k: c.get("scale00_root00_k%03d" % k, 0.0) for k in range(24, 108)}
    for sc in (7, 8):
        out.append("  %s 鍵 24〜107（Equal との差）:" % SCALES[sc])
        for o in range(24, 108, 12):
            out.append("    %3d: " % o + " ".join("%+7.1f" % (c["scale%02d_root00_k%03d" % (sc, k)] - eqa[k]) if "scale%02d_root00_k%03d" % (sc, k) in c else "    ・" for k in range(o, o + 12)))
        if "r0F_scale%02d_root02_k060" % sc in c:
            out.append("    主音 2 の鍵 60〜: " + " ".join("%+7.1f" % (c["r0F_scale%02d_root02_k%03d" % (sc, k)] - eq[k]) for k in range(60, 72)))
    out.append("Pitch Fine Scaling（鍵 %s の Equal・値 64 との差、セント）" % ",".join(str(k) for k in FSK))
    for v in FSV:
        out.append("  %3d: " % v + " ".join("%+7.1f" % (c["fs%03d_k%03d" % (v, k)] - c["fs064_k%03d" % k]) for k in FSK))
    for line in out:
        print(line)
    open(os.path.join(d, "tuning.txt"), "w", encoding="utf-8").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
