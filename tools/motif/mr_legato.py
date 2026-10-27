"""音色の Legato Slope（40 00 00 の 0x10）が何をするかを実機で測る（2026-10-28）。
  python mr_legato.py 置き場 [--analyze-only]
ボイスモードの雛形（mr_ref.template）をモノ（40 00 00 の 0x03 = 0）にして、鍵 60 を押したまま 0.5 秒で鍵 67 を押し（レガート）、1.0 秒で 60 を離す。
傾き 0・3・7 と、ポルタメントなし・あり（時間 40）。サイン（704）では高さと大きさ、ノイズ（705、AEG のアタック 64）ではアタックのやり直しを見る。
マルチの編集バッファ（5F 00）を控え、終わったら戻す。送るのは Data List のパラメーターチェンジ・ダンプの要求と送り返し・CC・ノートだけ（保存しない）。"""
import os
import sys
import time

import numpy as np

import mr_midi
import mr_ref

FS = 44100


def takes():
    t = []
    for wave in (704, 705):
        for porta in (0, 1):
            for slope in (0, 3, 7):
                t.append(("w%d_p%d_s%d%s" % (wave, porta, slope, "_i0" if wave == 705 else ""), wave, porta, slope))
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
        cur = None
        for name, wave, porta, slope in takes():
            if os.path.exists(os.path.join(d, name + ".wav")):
                continue
            if cur != wave:
                r.template(wave)
                r.el(0, 0x41, 0x24, 64 if wave == 705 else 0)     # ノイズはアタックを遅くして、やり直しを見えるように
                r.el(0, 0x41, 0x28, 0 if wave == 705 else 127)   # 初めの高さ 0（雛形は 127 で、やり直してもアタックが音に出ない）
                r.param(0x40, 0x00, 0x03, 0)                       # モノ
                cur = wave
            r.param(0x40, 0x00, 0x08, porta)                       # ポルタメントのスイッチ（0x08）
            r.param(0x40, 0x00, 0x09, 40)                          # ポルタメントの時間
            r.param(0x40, 0x00, 0x10, slope)
            time.sleep(0.2)
            r.all_off()
            time.sleep(0.1)
            r.record(name, 2.0, [(0.2, lambda: r.note_on(60, 100)), (0.7, lambda: r.note_on(67, 100)), (1.2, lambda: r.note_off(60)),
                                 (1.6, lambda: r.note_off(67))])
            print(name, flush=True)
    finally:
        r.all_off()
        r.mode(3)
        if saved:
            for m in saved:
                r.mo.sysex(bytes(m))
                time.sleep(0.03)
        r.close()
        mi.close()


def track(p):
    """10ms ごとの大きさ（dB）と、サインなら高さ（鍵の番号）"""
    L, R = mr_ref.read_wav(p)
    m = (np.asarray(L) + np.asarray(R)) / 2
    n = 441
    lv, ky = [], []
    for i in range(0, len(m) - 2048, n):
        y = m[i:i + 2048]
        lv.append(10 * np.log10((y[:n] ** 2).mean() + 1e-30))
        Y = np.abs(np.fft.rfft(y * np.hanning(2048), 1 << 15))
        f = np.fft.rfftfreq(1 << 15, 1 / FS)
        k = int(np.argmax(Y))
        ky.append(69 + 12 * np.log2(max(f[k], 1) / 440.0))
    return np.array(lv), np.array(ky)


def main():
    a = sys.argv[1:]
    d = a[0]
    os.makedirs(d, exist_ok=True)
    if "--analyze-only" not in a:
        record(d, a)
    for name, wave, porta, slope in takes():
        p = os.path.join(d, name + ".wav")
        if not os.path.exists(p):
            continue
        lv, ky = track(p)
        on = int(np.argmax(lv > lv.max() - 40))
        i0 = on + 50                                    # 2 つめの鍵（0.5 秒あと）
        seg = slice(i0 - 2, i0 + 14)
        print("%-12s 大きさ %s" % (name, " ".join("%4.0f" % v for v in lv[seg])))
        if wave == 704:
            print("%-12s 鍵     %s" % ("", " ".join("%4.1f" % v for v in ky[seg])))


if __name__ == "__main__":
    main()
