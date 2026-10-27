"""マルチのパートの音量（CC7 / 37 pp 0D）とドライの量（37 pp 15）の効きを実機で測り、芯（motif_play --multi --pc --seq）と比べる（2026-10-27）。
  python mr_multi_level.py 置き場 [--multi 41_000] [--voice 0:0] [--compare ../../build/motif_play.exe] [--analyze-only]
取ったマルチを編集バッファへ送り（mr_multi_hw.py と同じ）、パート 1 の音色をパラメーターチェンジ（37 00 00〜02）で替え、
リバーブ・コーラスの送りを CC91・93 で 0 にして、CC7 を 127・106・80・40、ドライの量を 127・120・64（CC7 127）にして鍵 60 を 1.5 秒鳴らす。
始める前に編集バッファ（5F 00）を控え、終わったら送り返す。送るのはダンプの要求・編集バッファへのバルク・パラメーターチェンジ・ノートと CC だけ（保存しない）。
芯は CC7 の組だけ同じ並びで鳴らす（ドライの量は MIDI で変えられないので、実機の比だけ見る）。録った音は配らないこと（ヤマハの音）。"""
import os
import subprocess
import sys
import time

import numpy as np

import mr_midi
import mr_multi_hw
import mr_multi_voice as MV
import mr_ref

FILL = "B0 03 00"
VOLS = (127, 106, 80, 40)
DRYS = (127, 120, 64)


def seq(vol):
    return ";".join(["B0 07 %02X" % vol, "B0 5B 00", "B0 5D 00", FILL, "90 3C 64"] + [FILL] * 15 + ["80 3C 00"])


def level(p):
    x = MV.load(p)
    o = MV.onset(x)
    m = x[:, o + 4410:o + 4410 + 44100]
    return 10 * np.log10(np.mean(m ** 2) + 1e-30)


def main():
    a = sys.argv[1:]
    d = a[0]
    os.makedirs(d, exist_ok=True)
    here = os.path.dirname(os.path.abspath(__file__))
    cap = os.path.join(here, "..", "..", "roms", "motif-rack-es", "capture")
    multi = mr_ref.opt(a, "--multi", "41_000")
    lsb, prog = (int(v) for v in mr_ref.opt(a, "--voice", "0:0").split(":"))
    exe = mr_ref.opt(a, "--compare")
    takes = [("v%d" % v, v, None) for v in VOLS] + [("d%d" % dr, 127, dr) for dr in DRYS]
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
            if "--voicemode" in a:
                # 比べ: 同じ音色をボイスモードで（CC7 127・送り 0）
                r.mode(0)
                r.program(63, lsb, prog)
                time.sleep(1.0)
                msgs = [bytes(int(x, 16) for x in s.split()) for s in seq(127).split(";")]
                ev = [(0.3 + 0.1 * i, (lambda m=m: r.mo.short(m[0], m[1], m[2]))) for i, m in enumerate(msgs)]
                r.record("voice_hw", 0.3 + 0.1 * len(msgs) + 1.5, ev)
                r.all_off()
                r.mode(3)
                print("録った: voice", flush=True)
                if "--voicemode-only" in a:
                    return
            for m in mr_multi_hw.to_edit_buffer(os.path.join(cap, "multi", multi + ".syx")):
                r.mo.sysex(m)
                time.sleep(0.03)
            time.sleep(2.0)
            r.param(0x37, 0x00, 0x00, 63)
            r.param(0x37, 0x00, 0x01, lsb)
            r.param(0x37, 0x00, 0x02, prog)
            time.sleep(1.5)
            if "--flat-only" in a:
                # パートの EQ を平らに（37 00 30・34・38 = 64）、ドライ 127 で 1 つだけ録る
                for off in (0x30, 0x34, 0x38):
                    r.param(0x37, 0x00, off, 64)
                r.param(0x37, 0x00, 0x15, 127)
                takes = [("flat", 127, None)]
            for name, vol, dry in takes:
                r.all_off()
                if dry is not None:
                    r.param(0x37, 0x00, 0x15, dry)
                time.sleep(0.3)
                msgs = [bytes(int(x, 16) for x in s.split()) for s in seq(vol).split(";")]
                ev = [(0.3 + 0.1 * i, (lambda m=m: r.mo.short(m[0], m[1], m[2]))) for i, m in enumerate(msgs)]
                r.record(name + "_hw", 0.3 + 0.1 * len(msgs) + 1.5, ev)
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
    if exe:
        names = sorted(os.listdir(os.path.join(cap, "multis")))
        for v in VOLS:
            subprocess.run([exe, "--nomidi", "--selftest", os.path.join(d, "v%d_sm.wav" % v), "--multi", str(names.index(multi + ".multi")),
                            "--pc", "1:63:%d:%d" % (lsb, prog), "--seq", seq(v)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ref = level(os.path.join(d, "v127_hw.wav"))
    sref = level(os.path.join(d, "v127_sm.wav")) if os.path.exists(os.path.join(d, "v127_sm.wav")) else None
    for name, vol, dry in takes:
        p = os.path.join(d, name + "_hw.wav")
        if not os.path.exists(p):
            continue
        hw = level(p) - ref
        line = "%-5s 実機 %+6.2fdB" % (name, hw)
        if dry is None:
            line += "  （値 / 127 の 2 乗なら %+6.2fdB）" % (40 * np.log10(vol / 127))
        else:
            line += "  （値 / 127 なら %+6.2fdB、2 乗なら %+6.2fdB）" % (20 * np.log10(dry / 127), 40 * np.log10(dry / 127))
        q = os.path.join(d, name + "_sm.wav")
        if sref is not None and os.path.exists(q):
            line += "  芯 %+6.2fdB  実機 − 芯（絶対） %+6.2fdB" % (level(q) - sref, level(p) - level(q))
        print(line)


if __name__ == "__main__":
    main()
