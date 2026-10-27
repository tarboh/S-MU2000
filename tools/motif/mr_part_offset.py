"""マルチのパートのフィルター・EG のずらし（37 pp 17・18・22〜2C）が、CC71〜75 と同じ利きかを実機で比べる（2026-10-27）。
  python mr_part_offset.py 置き場 [--analyze-only]
マルチモードの編集バッファで、パート 1 をノイズの音色 Wind Blows（PRE5:053）にし、ほかのパートは受けないようにして、
「パートの値を v にした音」と「パートは 64 のまま CC を v にした音」を組にして録る（カットオフ ↔ CC74、レゾナンス ↔ CC71、アタック ↔ CC73、
ディケイ ↔ CC75、リリース ↔ CC72）。同じ利きなら 2 つは同じ音になる。CC の無いもの（サステイン・FEG）は、値を変えた音だけ録る。
始める前に編集バッファ（5F 00）を一括ダンプで控え、終わったら送り返す。送るのは Data List のパラメーターチェンジ・ダンプの要求と送り返し・CC・ノートだけ
（保存しない）。録ったものは 置き場/po_<名前>.wav。録った音は配らないこと（ヤマハの音）。"""
import os
import sys
import time

import numpy as np

import mr_midi
import mr_ref

FS = 44100
# 名前: (パートの番地, CC の番号（無ければ None）)
PARAMS = {"cutoff": (0x17, 74), "reso": (0x18, 71), "atk": (0x23, 73), "dec": (0x25, 75), "rel": (0x26, 72),
          "sus": (0x22, None), "fatk": (0x28, None), "fdec": (0x2A, None), "frel": (0x2B, None), "fsus": (0x27, None), "fdepth": (0x2C, None)}
VALUES = {"cutoff": (32, 96), "reso": (112,), "atk": (96,), "dec": (32,), "rel": (96,), "sus": (32,), "fatk": (96,), "fdec": (32,),
          "frel": (96,), "fsus": (32,), "fdepth": (32, 96)}


def takes():
    t = [("base", None, None, None)]
    for name, vals in VALUES.items():
        off, cc = PARAMS[name]
        for v in vals:
            t.append(("%s_p%d" % (name, v), off, v, None))
            if cc:
                t.append(("%s_c%d" % (name, v), None, v, cc))
    # 物差し: CC74・CC71 を細かく動かした音と、パートの値をもっと動かした音（パートの 1 目盛りが CC の何目盛りかを読む）
    for v in (0, 8, 16, 24, 40, 48, 56, 72, 80, 88, 104, 112, 120, 127):
        t.append(("cutoff_c%d" % v, None, v, 74))
    for v in (0, 16, 48, 80, 112, 127):
        t.append(("cutoff_p%d" % v, 0x17, v, None))
    for v in (72, 80, 88, 96, 104, 120, 127):
        t.append(("reso_c%d" % v, None, v, 71))
    for v in (80, 96, 127):
        t.append(("reso_p%d" % v, 0x18, v, None))
    return t


def ladder(d, name):
    """パートの値ごとに、いちばん近い CC の値（近い 2 点の間は距離で比例配分）"""
    import glob
    import re
    cs = sorted((int(re.search(r"_c(\d+)\.wav", p).group(1)), spec(p)) for p in glob.glob(os.path.join(d, "po_%s_c*.wav" % name)))
    cs = [(64, spec(os.path.join(d, "po_base.wav")))] + cs
    cs.sort(key=lambda x: x[0])
    for p in sorted(glob.glob(os.path.join(d, "po_%s_p*.wav" % name)), key=lambda p: int(re.search(r"_p(\d+)", p).group(1))):
        v = int(re.search(r"_p(\d+)", p).group(1))
        s = spec(p)
        dist = [np.mean(np.abs(s - x)) for _, x in cs]
        i = int(np.argmin(dist))
        j = i + 1 if i + 1 < len(cs) and (i == 0 or dist[i + 1] < dist[i - 1]) else i - 1
        w = dist[i] / (dist[i] + dist[j] + 1e-9)
        eq = cs[i][0] + (cs[j][0] - cs[i][0]) * w
        print("%s パート %3d ≒ CC %5.1f（パートの −64 あたり %.2f 倍、距離 %.1fdB）" % (name, v, eq, (eq - 64) / (v - 64) if v != 64 else 0, dist[i]))


def spec(path, t0=0.8, t1=1.8):
    L, R = mr_ref.read_wav(path)
    m = (np.asarray(L) + np.asarray(R)) / 2
    m = m[int(t0 * FS):int(t1 * FS)]
    X = np.abs(np.fft.rfft(m * np.hanning(len(m)))) ** 2
    f = np.fft.rfftfreq(len(m), 1 / FS)
    edges = 50 * 2 ** (np.arange(0, 25) / 3)
    return np.array([10 * np.log10(X[(f >= lo) & (f < hi)].mean() + 1e-30) for lo, hi in zip(edges[:-1], edges[1:])])


def env(path):
    L, R = mr_ref.read_wav(path)
    m = (np.asarray(L) ** 2 + np.asarray(R) ** 2) / 2
    n = FS // 50
    return np.array([10 * np.log10(np.mean(m[i:i + n]) + 1e-20) for i in range(0, len(m) - n, n)])


def main():
    a = sys.argv[1:]
    d = a[0]
    os.makedirs(d, exist_ok=True)
    if "--analyze-only" not in a:
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
            base = [(0x00, 63), (0x01, 4), (0x02, 52), (0x03, 0), (0x04, 1), (0x0D, 100), (0x0E, 64), (0x12, 0), (0x13, 0), (0x15, 127), (0x16, 64)]
            base += [(off, 64) for off, _ in PARAMS.values()]
            for off, v in base:
                r.param(0x37, 0x00, off, v)
            for off in (0x60, 0x61, 0x62, 0x63):
                r.param(0x36, 0x00, off, 0)
            r.cc(91, 0)
            r.cc(93, 0)
            time.sleep(1.5)
            for name, off, v, cc in takes():
                if os.path.exists(os.path.join(d, "po_%s.wav" % name)):
                    continue
                if off is not None:
                    r.param(0x37, 0x00, off, v)
                if cc:
                    r.cc(cc, v)
                r.all_off()
                time.sleep(0.3)
                r.record("po_" + name, 4.3, [(0.3, lambda: r.note_on(60, 100)), (2.3, lambda: r.note_off(60))])
                if off is not None:
                    r.param(0x37, 0x00, off, 64)
                if cc:
                    r.cc(cc, 64)
                print(name, flush=True)
        finally:
            r.all_off()
            for cc in (71, 72, 73, 74, 75):
                r.cc(cc, 64)
            if saved:
                for m in saved:
                    r.mo.sysex(bytes(m))
                    time.sleep(0.03)
            r.mode(3)
            r.close()
            mi.close()
    b = os.path.join(d, "po_base.wav")
    sb, eb = spec(b), env(b)
    for name, off, v, cc in takes()[1:]:
        p = os.path.join(d, "po_%s.wav" % name)
        if not os.path.exists(p) or cc:
            continue
        s, e = spec(p), env(p)
        line = "%-10s 元の音とのスペクトルの差 %.1fdB、包絡の差 %.1fdB" % (name, np.mean(np.abs(s - sb)), np.mean(np.abs(np.maximum(e, -90) - np.maximum(eb, -90))))
        q = p.replace("_p%d" % v, "_c%d" % v)
        if os.path.exists(q):
            s2, e2 = spec(q), env(q)
            line += " | CC との差: スペクトル %.2fdB、包絡 %.2fdB" % (np.mean(np.abs(s - s2)), np.mean(np.abs(np.maximum(e, -90) - np.maximum(e2, -90))))
        print(line)
    ladder(d, "cutoff")
    ladder(d, "reso")


if __name__ == "__main__":
    main()
