"""マルチのパート 1 の音色をいろいろ替えて、実機と芯（motif_play --multi --pc --seq）で同じ並びを鳴らして比べる（2026-10-27）。
  python mr_multi_voice.py 置き場 [--multi 41_000] [--voices 1:20-81,0:0,...] [--compare ../../build/motif_play.exe] [--analyze-only]
実機: 始める前に編集バッファ（5F 00）を一括ダンプで控え、取ったマルチを編集バッファへ送り（mr_multi_hw.py と同じ）、
パート 1 の音色を Data List のパラメーターチェンジ（37 00 00〜02 = バンクの MSB・LSB・番号）で替えて、並び SEQ を流して録る。
終わったら控えを送り返す。送るのはダンプの要求・編集バッファへのバルク・パラメーターチェンジ・ノートと CC だけ（保存しない）。
芯: motif_play --selftest 置き場/<名前>_sm.wav --multi <番号> --pc 1:63:<LSB>:<番号> --seq SEQ。
録ったものは 置き場/<LSB>_<番号>_hw.wav・_sm.wav。録った音は配らないこと（ヤマハの音）。"""
import os
import subprocess
import sys
import time

import numpy as np

import mr_midi
import mr_multi_hw
import mr_ref

FS = 44100
FILL = "B0 03 00"
# 和音（48・55・60・64）を 1.5 秒、離して 0.6 秒、低い 40 を 1 秒、高い 76 を 1 秒（ベロシティ 100。ch1）
SEQ = ";".join(["90 30 64", "90 37 64", "90 3C 64", "90 40 64"] + [FILL] * 15 + ["80 30 00", "80 37 00", "80 3C 00", "80 40 00"]
               + [FILL] * 5 + ["90 28 64"] + [FILL] * 9 + ["80 28 00"] + [FILL] * 5 + ["90 4C 64"] + [FILL] * 9 + ["80 4C 00"])
# 既定の音色: PRE2 のギター 021〜082 と、ほかの楽器をいくつか（LSB:番号、番号は 0 から）
DEFAULT = [(1, p) for p in range(20, 82)] + [(0, 0), (0, 46), (1, 6), (1, 86), (1, 99), (1, 117), (2, 0), (2, 40), (2, 80),
                                             (3, 0), (3, 40), (3, 80), (4, 0), (4, 60), (5, 0), (5, 60)]


def parse_voices(s):
    out = []
    for item in s.split(","):
        lsb, r = item.split(":")
        if "-" in r:
            a, b = r.split("-")
            out += [(int(lsb), p) for p in range(int(a), int(b) + 1)]
        else:
            out.append((int(lsb), int(r)))
    return out


def seq_bytes():
    return [bytes(int(x, 16) for x in s.split()) for s in SEQ.split(";")]


def record_hw(d, a, voices, multi, cap):
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
    mi = mr_midi.MidiIn(mr_ref.opt(a, "--midi-in", "MOTIF-RACK ES-1"))
    saved = None
    seq = seq_bytes()
    try:
        r.all_off()
        r.param(0x00, 0x00, 0x34, 0)
        r.mode(3)
        saved = mr_midi.request_block(mi, r.mo, 0, 0x5F, 0x00)
        if not saved:
            raise SystemExit("マルチの編集バッファを控えられない（ダンプが返らない）ので止める")
        open(os.path.join(d, "saved_5F.syx"), "wb").write(b"".join(bytes(m) for m in saved))
        for m in mr_multi_hw.to_edit_buffer(os.path.join(cap, "multi", multi + ".syx")):
            r.mo.sysex(m)
            time.sleep(0.03)
        time.sleep(2.0)
        for lsb, prog in voices:
            name = "%d_%03d" % (lsb, prog)
            if os.path.exists(os.path.join(d, name + "_hw.wav")):
                continue
            r.all_off()
            r.param(0x37, 0x00, 0x00, 63)
            r.param(0x37, 0x00, 0x01, lsb)
            r.param(0x37, 0x00, 0x02, prog)
            time.sleep(1.5)                            # 音色を読む間
            ev = [(0.3 + 0.1 * i, (lambda m=m: r.mo.short(m[0], m[1] if len(m) > 1 else 0, m[2] if len(m) > 2 else 0))) for i, m in enumerate(seq)]
            r.record(name + "_hw", 0.3 + 0.1 * len(seq) + 3.0, ev)
            print("録った:", name, flush=True)
    finally:
        r.all_off()
        if saved:
            for m in saved:
                r.mo.sysex(bytes(m))
                time.sleep(0.03)
        r.mode(3)
        r.close()
        mi.close()


def render_sm(d, exe, voices, mnum):
    for lsb, prog in voices:
        out = os.path.join(d, "%d_%03d_sm.wav" % (lsb, prog))
        subprocess.run([exe, "--nomidi", "--selftest", out, "--multi", str(mnum), "--pc", "1:63:%d:%d" % (lsb, prog), "--seq", SEQ],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def load(p):
    """実機の録音は float、芯（motif_play --selftest）は 16bit"""
    if open(p, "rb").read(22)[20] == 3:
        L, R = mr_ref.read_wav(p)
        return np.array([np.asarray(L, float), np.asarray(R, float)])
    import wave
    w = wave.open(p)
    return np.frombuffer(w.readframes(w.getnframes()), "<i2").reshape(-1, 2).T / 32768.0


def onset(x):
    m = np.abs(x[0]) + np.abs(x[1])
    return int(np.argmax(m > m.max() * 0.01))


EDGES = 31.5 * 2 ** (np.arange(0, 30) / 3)


def bands(m):
    X = np.abs(np.fft.rfft(m * np.hanning(len(m)))) ** 2
    f = np.fft.rfftfreq(len(m), 1 / FS)
    return np.array([10 * np.log10(X[(f >= lo) & (f < hi)].sum() + 1e-30) for lo, hi in zip(EDGES[:-1], EDGES[1:]) if hi < 18000])


# 比べる区間（出だしからの秒）: 和音・低い音・高い音
SEGS = [("和音", 0.1, 1.5), ("低", 2.1, 3.0), ("高", 3.6, 4.5)]


def compare(hwp, smp):
    """区間ごとに（音量の差、スペクトルの差）。スペクトルは左右それぞれで比べて平均する（左右を足すと、Pitch Change やコーラスでずらした 2 本が
    打ち消し合う帯が録るたびに変わり、実機と芯が同じ作りでも離れて見える）"""
    hw, sm = load(hwp), load(smp)
    a0, b0 = onset(hw), onset(sm)
    res = []
    for _, t0, t1 in SEGS:
        h = hw[:, a0 + int(t0 * FS):a0 + int(t1 * FS)]
        c = sm[:, b0 + int(t0 * FS):b0 + int(t1 * FS)]
        lv = 10 * np.log10(np.mean(h ** 2) / (np.mean(c ** 2) + 1e-30) + 1e-30)
        sd = []
        for ch in range(2):
            bh, bc = bands(h[ch]), bands(c[ch])
            ok = bh > bh.max() - 40
            sd.append(np.mean(np.abs((bh - bh.max()) - (bc - bc.max()))[ok]))
        res.append((lv, float(np.mean(sd))))
    return res


def main():
    a = sys.argv[1:]
    d = a[0]
    os.makedirs(d, exist_ok=True)
    here = os.path.dirname(os.path.abspath(__file__))
    cap = os.path.join(here, "..", "..", "roms", "motif-rack-es", "capture")
    multi = mr_ref.opt(a, "--multi", "41_000")
    vs = mr_ref.opt(a, "--voices")
    voices = parse_voices(vs) if vs else DEFAULT
    if "--analyze-only" not in a and "--sm-only" not in a:
        record_hw(d, a, voices, multi, cap)
    exe = mr_ref.opt(a, "--compare")
    if exe:
        names = sorted(os.listdir(os.path.join(cap, "multis")))
        render_sm(d, exe, voices, names.index(multi + ".multi"))
    import csv
    vn = {}
    for x in csv.DictReader(open(os.path.join(cap, "voices.csv"), encoding="utf-8")):
        vn.setdefault((x["bank"], int(x["number"])), x["name"])
    rows = []
    for lsb, prog in voices:
        hwp = os.path.join(d, "%d_%03d_hw.wav" % (lsb, prog))
        smp = os.path.join(d, "%d_%03d_sm.wav" % (lsb, prog))
        if not (os.path.exists(hwp) and os.path.exists(smp)):
            continue
        res = compare(hwp, smp)
        name = vn.get(("PRE%d" % (lsb + 1), prog + 1), "?")
        rows.append((np.mean([s for _, s in res]), lsb, prog, name, res))
    for sd, lsb, prog, name, res in sorted(rows, key=lambda r: -r[0]):
        print("PRE%d:%03d %-11s スペクトルの差 %.2fdB | " % (lsb + 1, prog + 1, name, sd)
              + " ".join("%s 音量 %+5.1f 形 %4.1f" % (n, lv, s) for (n, _, _), (lv, s) in zip(SEGS, res)))
    if rows:
        print("平均 %.2fdB（%d 音色）、音量の差の平均 %+.2fdB" % (np.mean([r[0] for r in rows]), len(rows),
                                                      np.mean([np.mean([lv for lv, _ in r[4]]) for r in rows])))


if __name__ == "__main__":
    main()
