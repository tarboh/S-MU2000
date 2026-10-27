"""ドラムの鍵で CC73・75・72（アタック・ディケイ・リリース）がどれだけ効くかを実機で測る（2026-10-27）。
  python mr_drum_cc.py 置き場 [--kit 0] [--key 36] [--tests attack,decay1,decay2,release] [--analyze-only]
ボイスモードで PRE のドラム（63/32）を選び、編集バッファの 1 鍵（Data List の DRUM VOICE KEY 47 ee）を「素の鍵」にして波形を 704 Sine にする
（mr_drum_cal.py と同じ）。mr_cc_sound.py と同じく、物差しとして「CC を 64 のまま、鍵の EG の欄を 8 刻みで動かした音」を録り、次に
「欄を真ん中（64）にして、CC を 0〜127 に動かした音」を録って、CC の音がどの値の音にいちばん近いかで 1 目盛りのずれを読む。
  attack: アタック（47 ee 23）。ディケイ 1 は 0・レベル 127・ディケイ 2 はホールド
  decay1: ディケイ 1（47 ee 24）。ディケイ 1 のレベル 0・ディケイ 2 はホールド
  decay2: ディケイ 2（47 ee 25。127 はホールドなので 0〜120）。ディケイ 1 は 0・レベル 127
  release: ドラムの鍵にはリリースの欄が無い（ノートオフを受ける鍵は離すとすぐ消える）。Receive Note Off を on にして CC72 だけ動かし、
    ノーマルボイスの雛形のリリース（mr_cc_sound.py の release_pNNN.wav。--ref で置き場）と比べる
始める前にマルチの編集バッファ（5F 00）を一括ダンプで控え、終わったら送り返す。送るのは Data List のパラメーターチェンジ・ダンプの要求と送り返し・
バンクとプログラムの選び・CC・ノートだけ（保存しない）。結果は 置き場/drum_cc.txt。録ったものはヤマハの音なので配らないこと。"""
import os
import sys
import time

import numpy as np

import mr_midi
import mr_ref
from mr_cc_sound import CCV, env

TESTS = {
    # 名前: (欄の値の範囲, 真ん中, CC, 欄（47 ee 2x の位置）)
    "attack": (range(0, 128, 8), 64, 73, 0x23),
    "decay1": (range(0, 128, 8), 64, 75, 0x24),
    "decay2": (range(0, 121, 8), 64, 75, 0x25),
    "release": (None, None, 72, None),
    "hold": (None, 127, 75, 0x25),
    "hold120": (None, 120, 75, 0x25),
}
HOLD = {"attack": 1.5, "decay1": 2.5, "decay2": 2.5, "release": 0.6, "hold": 2.5, "hold120": 2.5}
HOLD_CC = (0, 32, 48, 54, 60, 63, 64, 66, 72, 80, 127)


def main():
    a = sys.argv[1:]
    d = a[0]
    kit = int(mr_ref.opt(a, "--kit", "0"))
    key = int(mr_ref.opt(a, "--key", "36"))
    ref = mr_ref.opt(a, "--ref", os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../roms/motif-rack-es/capture/ccsound"))
    tests = mr_ref.opt(a, "--tests", "attack,decay1,decay2,release").split(",")
    ee = key - 24
    os.makedirs(d, exist_ok=True)
    if "--analyze-only" not in a:
        r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
        mi = mr_midi.MidiIn(mr_ref.opt(a, "--midi-in", "MOTIF-RACK ES-1"))
        saved = None

        def k(off, *data):
            r.param(0x47, ee, off, *data)

        def eg(at, d1, d1l, d2, noff):
            k(0x23, at)
            k(0x24, d1)
            k(0x25, d2)
            k(0x29, d1l)
            k(0x03, noff)
            time.sleep(0.1)

        def take(name, t):
            if os.path.exists(os.path.join(d, name + ".wav")):
                return
            r.all_off()
            time.sleep(0.15)
            r.one_note(name, note=key, vel=100, hold=HOLD[t], tail=2.5 if t == "release" else 0.3, pre=0.3)

        try:
            r.all_off()
            r.param(0x00, 0x00, 0x34, 0)
            r.mode(3)
            saved = mr_midi.request_block(mi, r.mo, 0, 0x5F, 0x00)
            if not saved:
                raise SystemExit("マルチの編集バッファを控えられない（ダンプが返らない）ので止める")
            open(os.path.join(d, "saved_5F.syx"), "wb").write(b"".join(bytes(m) for m in saved))
            r.mode(0)
            r.program(63, 32, kit)
            for c in (7, 11):
                r.cc(c, 127)
            r.cc(10, 64)
            r.cc(91, 0)
            r.cc(93, 0)
            for c in (71, 72, 73, 74, 75):
                r.cc(c, 64)
            # 素の鍵（mr_drum_cal.py の base と同じ）で、波形を 704 Sine に
            k(0x03, 0)
            k(0x05, 0)
            k(0x06, 704 >> 7, 704 & 0x7F)
            k(0x0B, 64)
            k(0x0C, 0)
            k(0x0D, 64)
            k(0x0E, 0)
            k(0x0F, 0)
            k(0x11, 0)
            k(0x20, 127)
            k(0x21, 64)
            r.param(0x49, ee, 0x01, 1, 127)
            r.param(0x49, ee, 0x03, 64)
            r.param(0x49, ee, 0x04, 0)
            r.param(0x4A, ee, 0x00, 5)
            time.sleep(0.3)
            for t in tests:
                rng, mid, cc, off = TESTS[t]
                if t == "attack":
                    base = dict(at=64, d1=0, d1l=127, d2=127, noff=0)
                elif t == "decay1":
                    base = dict(at=0, d1=64, d1l=0, d2=127, noff=0)
                elif t == "decay2":
                    base = dict(at=0, d1=0, d1l=127, d2=64, noff=0)
                elif t in ("hold", "hold120"):
                    base = dict(at=0, d1=0, d1l=127, d2=mid, noff=0)
                else:
                    base = dict(at=0, d1=0, d1l=127, d2=127, noff=1)
                if rng is not None:
                    for v in rng:
                        p = dict(base)
                        p[{0x23: "at", 0x24: "d1", 0x25: "d2"}[off]] = v
                        eg(**p)
                        take("%s_p%03d" % (t, v), t)
                eg(**base)
                for c in (HOLD_CC if t.startswith("hold") else CCV + (64,)):
                    r.cc(cc, c)
                    time.sleep(0.08)
                    take("%s_cc%03d" % (t, c), t)
                r.cc(cc, 64)
                print("%s を録った" % t, flush=True)
        finally:
            r.all_off()
            for c in (71, 72, 73, 74, 75):
                r.cc(c, 64)
            r.mode(3)
            if saved:
                for m in saved:
                    r.mo.sysex(bytes(m))
                    time.sleep(0.03)
            r.close()
            mi.close()
    out = []
    for t in tests:
        rng, mid, cc, off = TESTS[t]
        refs = []
        if t.startswith("hold"):
            # ホールド（ディケイ 2 が 127）・120 の鍵に CC75: decay2 の物差しと比べる
            for v in range(0, 121, 8):
                p = os.path.join(d, "decay2_p%03d.wav" % v)
                if os.path.exists(p):
                    refs.append((v, env(mr_ref.read_wav(p))))
        elif rng is None:
            # リリース: ノーマルボイスの雛形のリリース 0〜120 を物差しに
            for v in range(0, 128, 8):
                p = os.path.join(ref, "release_p%03d.wav" % v)
                if os.path.exists(p):
                    refs.append((v, env(mr_ref.read_wav(p))))
        else:
            for v in rng:
                p = os.path.join(d, "%s_p%03d.wav" % (t, v))
                if os.path.exists(p):
                    refs.append((v, env(mr_ref.read_wav(p))))
        line = ["%s（CC%d）:" % (t, cc)]
        for c in (HOLD_CC if t.startswith("hold") else CCV + (64,)):
            p = os.path.join(d, "%s_cc%03d.wav" % (t, c))
            if not os.path.exists(p) or not refs:
                continue
            f = env(mr_ref.read_wav(p))
            n = min(len(f), min(len(rf) for _, rf in refs))
            dist = sorted((float(np.mean(np.abs(np.maximum(f[:n], -60) - np.maximum(rf[:n], -60)))), v) for v, rf in refs)
            (d0, v0), (d1, v1) = dist[0], dist[1]
            est = v0 + (v1 - v0) * d0 / (d0 + d1 + 1e-9) if abs(v1 - v0) <= 8 else v0
            line.append("CC %d → %.1f（外れ %.1fdB）" % (c, est, d0))
        out.append(" ".join(line))
        print(out[-1], flush=True)
    open(os.path.join(d, "drum_cc.txt"), "w", encoding="utf-8").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
