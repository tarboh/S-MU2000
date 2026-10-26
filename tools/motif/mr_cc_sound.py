"""CC71〜75（レゾナンス・リリース・アタック・明るさ・ディケイ）の効きの大きさを実機で測る（2026-10-26）。
  python mr_cc_sound.py 置き場 [--tests cutoff,reso,attack,release,decay1,decay2] [--analyze-only]
ボイスモードの雛形（mr_ref.template）で、物差しとして「CC を 64 のまま、元のパラメーターを 8 刻みで動かした音」を録り、
次に「パラメーターを真ん中にして、CC を 0〜127 に動かした音」を録る。CC の音がどのパラメーターの値の音にいちばん近いかで、CC の 1 目盛りが
パラメーターの何目盛りにあたるかを読む（近い 2 点の間は距離で比例配分）。
  cutoff: ノイズ（705）・LPF24D・レゾナンス 0、カットオフ 64〜255 / CC74（カットオフ 160）。比べるのは 0.3〜0.9 秒の 1/3 オクターブのスペクトル
  reso: ノイズ・カットオフ 160、レゾナンス 0〜127 / CC71（レゾナンス 64）
  attack・release・decay1・decay2: サイン波（704）の 10ms ごとの大きさ（dB）の形。attack はアタックの時間、release はリリースの時間（離してから）、
  decay1 はディケイ 1 の時間（行き先 0）、decay2 はディケイ 1 を 0 にしてディケイ 2 の時間（行き先 0）を動かす。CC は 73・72・75・75
送るのは Data List のパラメーターチェンジ・CC・ノートだけ（保存しない）。終わったらマルチへ戻す。結果は 置き場/cc_sound.txt。"""
import os
import sys
import time

import numpy as np

import mr_ref

FS = 44100
CCV = (0, 16, 32, 48, 80, 96, 112, 127)


def spec(x):
    m = (np.asarray(x[0]) + np.asarray(x[1])) / 2
    m = m[int(0.6 * FS):int(1.2 * FS)]
    X = np.abs(np.fft.rfft(m * np.hanning(len(m)))) ** 2
    f = np.fft.rfftfreq(len(m), 1 / FS)
    edges = 31.5 * 2 ** (np.arange(0, 30) / 3)
    return np.array([10 * np.log10(X[(f >= lo) & (f < hi)].mean() + 1e-30) for lo, hi in zip(edges[:-1], edges[1:]) if hi < 18000])


def env(x):
    m = np.sqrt(((np.asarray(x[0]) ** 2 + np.asarray(x[1]) ** 2) / 2))
    n = FS // 100
    e = np.array([np.sqrt(np.mean(m[i:i + n] ** 2)) for i in range(0, len(m) - n, n)])
    return 20 * np.log10(e / (e.max() + 1e-30) + 1e-6)


TESTS = {
    # 名前: (パラメーターの範囲, 真ん中の値, CC, 何を比べるか)
    "cutoff": (range(64, 256, 8), 160, 74, "spec"),
    "reso": (range(0, 128, 8), 64, 71, "spec"),
    "attack": (range(0, 128, 8), 64, 73, "env"),
    "release": (range(0, 128, 8), 64, 72, "env"),
    "decay1": (range(0, 128, 8), 64, 75, "env"),
    "decay2": (range(0, 128, 8), 64, 75, "env"),
}


def setup(r, test):
    r.template(705 if test in ("cutoff", "reso") else 704)
    r.el(0, 0x43, 0x00, 0)                     # LPF24D
    r.el(0, 0x43, 0x01, 1, 127)
    r.el(0, 0x43, 0x05, 0)
    if test == "cutoff":
        pass
    elif test == "reso":
        r.el(0, 0x43, 0x01, 1, 160 - 128)      # カットオフ 160
    elif test == "attack":
        r.el(0, 0x41, 0x28, 0)                 # 初めの高さ 0（雛形は 127 で、アタックの時間が音に出ない）
    elif test == "decay1":
        r.el(0, 0x41, 0x2A, 0)                 # ディケイ 1 の行き先 0
    elif test == "decay2":
        r.el(0, 0x41, 0x25, 0)                 # ディケイ 1 は 0（すぐ）、行き先 127
        r.el(0, 0x41, 0x2B, 0)                 # ディケイ 2 の行き先 0
    for c in (71, 72, 73, 74, 75):
        r.cc(c, 64)
    time.sleep(0.2)


def set_param(r, test, v):
    if test == "cutoff":
        r.el(0, 0x43, 0x01, v >> 7, v & 0x7F)
    elif test == "reso":
        r.el(0, 0x43, 0x05, v)
    elif test == "attack":
        r.el(0, 0x41, 0x24, v)
    elif test == "release":
        r.el(0, 0x41, 0x27, v)
    elif test == "decay1":
        r.el(0, 0x41, 0x25, v)
    elif test == "decay2":
        r.el(0, 0x41, 0x26, v)
    time.sleep(0.08)


def take(r, name, test):
    r.all_off()
    time.sleep(0.1)
    hold = 1.5 if test != "release" else 0.6
    return r.one_note(name, note=60, vel=100, hold=hold, tail=2.5 if test == "release" else 0.3, pre=0.3)


def main():
    a = sys.argv[1:]
    d = a[0]
    tests = mr_ref.opt(a, "--tests", "cutoff,reso,attack,release,decay1,decay2").split(",")
    os.makedirs(d, exist_ok=True)
    if "--analyze-only" not in a:
        r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
        try:
            r.all_off()
            r.param(0x00, 0x00, 0x34, 0)
            r.mode(0)
            r.program(63, 0, 0)
            for t in tests:
                rng, mid, cc, kind = TESTS[t]
                setup(r, t)
                for v in rng:
                    set_param(r, t, v)
                    if not os.path.exists(os.path.join(d, "%s_p%03d.wav" % (t, v))):
                        take(r, "%s_p%03d" % (t, v), t)
                set_param(r, t, mid)
                for c in CCV:
                    r.cc(cc, c)
                    time.sleep(0.08)
                    if not os.path.exists(os.path.join(d, "%s_cc%03d.wav" % (t, c))):
                        take(r, "%s_cc%03d" % (t, c), t)
                r.cc(cc, 64)
                print("%s を録った" % t, flush=True)
        finally:
            r.all_off()
            for c in (71, 72, 73, 74, 75):
                r.cc(c, 64)
            r.mode(3)
            r.close()
    out = []
    for t in tests:
        rng, mid, cc, kind = TESTS[t]
        feat = spec if kind == "spec" else env
        refs = []
        for v in rng:
            p = os.path.join(d, "%s_p%03d.wav" % (t, v))
            if os.path.exists(p):
                refs.append((v, feat(mr_ref.read_wav(p))))
        line = ["%s（CC%d、真ん中 %d）:" % (t, cc, mid)]
        for c in CCV:
            p = os.path.join(d, "%s_cc%03d.wav" % (t, c))
            if not os.path.exists(p):
                continue
            f = feat(mr_ref.read_wav(p))
            n = min(len(f), min(len(rf) for _, rf in refs))
            dist = [(float(np.mean(np.abs(f[:n] - rf[:n]))), v) for v, rf in refs]
            dist.sort()
            (d0, v0), (d1, v1) = dist[0], dist[1]
            est = v0 + (v1 - v0) * d0 / (d0 + d1 + 1e-9) if abs(v1 - v0) <= 8 else v0
            line.append("CC %d → %.1f（ずれ %+.1f、1 目盛りあたり %s）" % (c, est, est - mid, "%.2f" % ((est - mid) / (c - 64)) if c != 64 else "-"))
        out.append(" ".join(line))
        print(out[-1], flush=True)
    open(os.path.join(d, "cc_sound.txt"), "w", encoding="utf-8").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
