"""1 音ごとに変わる要素（ランダムパン 41 ee 09・オルタネートパン 41 ee 0A・ランダムピッチ 42 ee 04）を、正弦波の雛形で 1 本に 16 音続けて鳴らして測る（2026-10-25）。
  python mr_pernote.py --dir 置き場 [--read]
音は 0.4 秒おきに 0.2 秒ずつ（鍵 60・ベロシティ 100）。1 音ごとに、鳴り始めから 0.04〜0.16 秒の左右の差（dB）と音程（261.63Hz からのセント）を読む。
送るのはパラメーターチェンジ・ノートだけ（保存しない）。終わったらマルチへ。"""
import os
import sys

import numpy as np

import mr_ctrl as M
import mr_ref

FS = 44100
N, T0, DT, HOLD = 16, 0.3, 0.4, 0.2
# 名前: （ブロック, 番地, 値）
TAKES = [("rp%03d" % v, 0x41, 0x09, v) for v in (0, 16, 32, 64, 96, 127)] + \
        [("ap%03d" % v, 0x41, 0x0A, v) for v in (64, 80, 96, 127, 48, 0)] + \
        [("rt%03d" % v, 0x42, 0x04, v) for v in (0, 8, 16, 32, 64, 96, 127)]


def notes(path):
    """1 音ごとの（左右 dB, セント）"""
    L, R = mr_ref.read_wav(path)
    L, R = np.asarray(L, float), np.asarray(R, float)
    x = L + R
    on = int(np.argmax(np.abs(x) > 1e-3))
    out = []
    fr = np.fft.rfftfreq(1 << 16, 1 / FS)
    for k in range(N):
        a = on + int((DT * k + 0.04) * FS)
        b = on + int((DT * k + 0.16) * FS)
        l, r = L[a:b], R[a:b]
        bal = 10 * np.log10(np.mean(l ** 2) + 1e-20) - 10 * np.log10(np.mean(r ** 2) + 1e-20)
        sp = np.abs(np.fft.rfft((l + r) * np.hanning(b - a), 1 << 16))
        i = int(np.argmax(sp[:int(2000 / fr[1])]))
        # 放物線で山を細かく
        y0, y1, y2 = np.log(sp[i - 1] + 1e-20), np.log(sp[i] + 1e-20), np.log(sp[i + 1] + 1e-20)
        f = fr[i] + 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2) * fr[1]
        out.append((bal, 1200 * np.log2(f / 261.63)))
    return np.array(out)


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    os.makedirs(d, exist_ok=True)
    if "--read" not in a:
        r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
        try:
            r.all_off()
            r.mode(0)
            r.program(63, 0, 0)
            for name, blk, off, v in TAKES:
                if os.path.exists(os.path.join(d, name + ".wav")):
                    continue
                M.setup(r, "sin")
                r.el(0, blk, off, v)
                M.reset_ctrls(r)
                ev = []
                for k in range(N):
                    ev.append((T0 + DT * k, lambda: r.note_on(60, 100)))
                    ev.append((T0 + DT * k + HOLD, lambda: r.note_off(60)))
                r.record(name, T0 + DT * N + 0.4, ev)
                print(name, "録った", flush=True)
        finally:
            r.all_off()
            r.mode(3)
            r.close()
    for name, _, _, _ in TAKES:
        p = os.path.join(d, name + ".wav")
        if not os.path.exists(p):
            continue
        x = notes(p)
        print("%s 左右 dB: %s" % (name, " ".join("%+.1f" % v for v in x[:, 0])))
        print("%s 音程 c : %s" % (" " * len(name), " ".join("%+.1f" % v for v in x[:, 1])))
        print("%s 左右の標準偏差 %.2f・音程の標準偏差 %.2f セント" % (" " * len(name), np.std(x[:, 0]), np.std(x[:, 1])))


if __name__ == "__main__":
    main()
