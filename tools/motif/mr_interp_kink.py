"""ドラムの鍵を大きく下げて鳴らし、補間の出力そのものの形（折れ線か・曲線か）と読み出しの速さを実機から測る（2026-10-28）。
  python mr_interp_kink.py 置き場 [--analyze-only]
Hard Kit（PREDR:035）の鍵 26（SdGate1）・33（BdDistRM）を、鍵のインサーションを切り、フィルターを全開（カットオフ 255・レゾナンス 0）、
EQ を素通し（型 5）にして、RPN の Coarse 0・−12・−24 と鍵のコースチューン（48 kk 00）0・−24 の組で鳴らす。録音は光のデジタル出力なので、
直線補間なら波形は折れ線になり、2 階差分は折れ目だけに立つ。折れ目の間隔の逆数 × 44100 が読み出しの速さ fr。
ボイスモードで鳴らし、終わったら RPN を 0 に戻してマルチ（3）にし、控えたマルチの編集バッファ（5F 00）を送り返す（ボイスの編集は保存しない）。
送るのは Data List のパラメーターチェンジ・ダンプの要求と送り返し・CC（RPN）・ノートだけ。録った音は配らないこと。"""
import os
import sys
import time

import numpy as np

import mr_midi
import mr_ref

FS = 44100
KEYS = (26, 33)
CASES = [(0, 0), (-12, 0), (-24, 0), (-24, -24)]         # （RPN の Coarse、鍵のコースチューン）


def rpn(r, num, msb):
    for c, v in ((101, 0), (100, num), (6, msb), (38, 0), (101, 127), (100, 127)):
        r.cc(c, v)


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
        r.program(63, 32, 34)
        r.param(0x46, 0x70, 0x16, 0)
        for k in KEYS:
            kk = k - 24
            r.param(0x47, kk, 0x11, 0)            # 鍵のインサーションを切る
            r.param(0x49, kk, 0x01, 1, 127)       # カットオフ 255
            r.param(0x49, kk, 0x04, 0)            # レゾナンス 0
            r.param(0x4A, kk, 0x00, 5)            # EQ を素通し
        for c, v in ((7, 127), (11, 127), (10, 64), (91, 0), (93, 0), (74, 64), (71, 64)):
            r.cc(c, v)
        rpn(r, 1, 64)
        time.sleep(0.5)
        for ct, kc in CASES:
            rpn(r, 2, 64 + ct)
            for k in KEYS:
                r.param(0x48, k - 24, 0x00, 64 + kc)
            time.sleep(0.2)
            for k in KEYS:
                name = "k%03d_c%+03d_e%+03d" % (k, ct, kc)
                if os.path.exists(os.path.join(d, name + ".wav")):
                    continue
                r.all_off()
                time.sleep(0.15)
                r.one_note(name, note=k, vel=100, hold=1.2, tail=0.4, pre=0.3)
                print(name, flush=True)
    finally:
        r.all_off()
        rpn(r, 2, 64)
        rpn(r, 1, 64)
        r.mode(3)
        for m in saved or []:
            r.mo.sysex(bytes(m))
            time.sleep(0.03)
        r.close()
        mi.close()


def kinks(p, n=8000):
    """出だし 0.05 秒から n サンプルの 2 階差分を見る: 2 階差分の大きさの自己相関の山の間隔（= 折れ目の間隔）と、
    2 階差分の大きい 20% の点が全体の何割の大きさを持つか（折れ線なら 2 点ずつの組に集まる）"""
    L, R = mr_ref.read_wav(p)
    x = np.asarray(L, float)
    on = int(np.argmax(np.abs(x) > np.abs(x).max() * 0.02))
    y = x[on + 2205:on + 2205 + n]
    d2 = np.abs(np.diff(y, 2))
    e = d2 ** 2
    srt = np.sort(e)[::-1]
    conc = srt[:len(srt) // 5].sum() / e.sum()
    a = np.correlate(d2 - d2.mean(), d2 - d2.mean(), "full")[len(d2) - 1:]
    a /= a[0]
    lag = int(np.argmax(a[3:400])) + 3
    return conc, lag, a[lag]


def main():
    a = sys.argv[1:]
    d = a[0]
    os.makedirs(d, exist_ok=True)
    if "--analyze-only" not in a:
        record(d, a)
    for k in KEYS:
        for ct, kc in CASES:
            conc, lag, pk = kinks(os.path.join(d, "k%03d_c%+03d_e%+03d.wav" % (k, ct, kc)))
            print("鍵 %d Coarse %+d 鍵 %+d: 2 階差分の上位 20%% の点に %.0f%%、自己相関の山 %d サンプル（%.2f）" % (k, ct, kc, conc * 100, lag, pk))


if __name__ == "__main__":
    main()
