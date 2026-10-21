"""サンプリング方式の芯で鳴らした WAV に、測ったインパルス応答でボイスのリバーブを掛ける（後処理。録音を読むだけで、実機には何も送らない）。
  python mr_fx_apply.py 入力.wav 音色.json 出力.wav [--ir 置き場（reverb_<型>_ir.npz）]

型ごとのインパルス応答（mr_fx.py → mr_fx_fit.py --save。型の既定のパラメーター・Return 96・送り 127 で測ったもの）を使い、
* Reverb Time（パラメーター 1）: Data List の表 4 の秒を 1kHz の残響時間とし、オクターブの帯ごとの減り方を、測ったときとの比で伸び縮みさせる
  （Hall 1 で値 10〜69 を測ると 1kHz の残響時間は表 4 と 5% 以内。帯ごとの比は「測ったときの比のまま」とした。長い所では高い帯が表より短い）。
  表 4 を使う型（Hall 1〜R3 Plate、White Room〜Canyon）だけ。Large Hall〜Rich Plate（表 25〜27）は伸び縮みさせない
* 音量: Return / 96 × 送り / 127（Return・CC91 とも、音量に比例すると測った。送りは音色の共通部分の Reverb Send）
* 入力は左右の平均（測ったときは真ん中の音）
コーラス（Chorus 1〜5・Celeste 1〜4 の型、05 00〜05 18）: 遅れの線 1 本を左右に置き、遅れ =
Delay Offset（表 2）+ 0.176ms × LFO Depth ×（三角波 0〜1）、速さは LFO Speed（表 1）、左右は逆の向き（mr_fx.py --train → mr_fx_chorus.py で測った）。
線の高さ 2.06（エネルギーの増え方から）× Return / 96 × 送り / 127。フィードバック・EQ・コーラスからリバーブへの送りは入れていない。
インサーションはまだ。"""
import json
import os
import sys

import numpy as np

import mr_fx
import mr_ref

FS = 44100
TABLE4 = [0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9, 2.0, 2.1, 2.2, 2.3, 2.4, 2.5, 2.6,
          2.7, 2.8, 2.9, 3.0, 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7, 3.8, 3.9, 4.0, 4.1, 4.2, 4.3, 4.4, 4.5, 4.6, 4.7, 4.8, 4.9, 5.0,
          5.5, 6.0, 6.5, 7.0, 7.5, 8.0, 8.5, 9.0, 9.5, 10.0, 11.0, 12.0, 13.0, 14.0, 15.0, 16.0, 17.0, 18.0, 19.0, 20.0, 25.0, 30.0]
TABLE4_TYPES = {(1, 0x00), (1, 0x01), (1, 0x10), (1, 0x11), (1, 0x12), (1, 0x20), (1, 0x21), (1, 0x30), (1, 0x02), (1, 0x13), (1, 0x31),
                (1, 0x40), (1, 0x41), (1, 0x42), (1, 0x43)}
BANDS = (125, 250, 500, 1000, 2000, 4000, 8000)
# Data List の Effect Data Assign Table の表 1（LFO の速さ、Hz）と表 2（遅れのずらし、ms）
LFO_HZ = [0.0, 0.04, 0.08, 0.13, 0.17, 0.21, 0.25, 0.29, 0.34, 0.38, 0.42, 0.46, 0.51, 0.55, 0.59, 0.63, 0.67, 0.72, 0.76, 0.8, 0.84, 0.88, 0.93, 0.97, 1.01, 1.05, 1.09, 1.14, 1.18, 1.22, 1.26, 1.3, 1.35, 1.39, 1.43, 1.47, 1.51, 1.56, 1.6, 1.64, 1.68, 1.72, 1.77, 1.81, 1.85, 1.89, 1.94, 1.98, 2.02, 2.06, 2.1, 2.15, 2.19, 2.23, 2.27, 2.31, 2.36, 2.4, 2.44, 2.48, 2.52, 2.57, 2.61, 2.65, 2.69, 2.78, 2.86, 2.94, 3.03, 3.11, 3.2, 3.28, 3.37, 3.45, 3.53, 3.62, 3.7, 3.87, 4.04, 4.21, 4.37, 4.54, 4.71, 4.88, 5.05, 5.22, 5.38, 5.55, 5.72, 6.06, 6.39, 6.73, 7.07, 7.4, 7.74, 8.08, 8.41, 8.75, 9.08, 9.42, 9.76, 10.1, 10.8, 11.4, 12.1, 12.8, 13.5, 14.1, 14.8, 15.5, 16.2, 16.8, 17.5, 18.2, 19.5, 20.9, 22.2, 23.6, 24.9, 26.2, 27.6, 28.9, 30.3, 31.6, 33.0, 34.3, 37.0, 39.7]
MOD_DELAY_MS = [0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9, 2.0, 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7, 2.8, 2.9, 3.0, 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7, 3.8, 3.9, 4.0, 4.1, 4.2, 4.3, 4.4, 4.5, 4.6, 4.7, 4.8, 4.9, 5.0, 5.1, 5.2, 5.3, 5.4, 5.5, 5.6, 5.7, 5.8, 5.9, 6.0, 6.1, 6.2, 6.3, 6.4, 6.5, 6.6, 6.7, 6.8, 6.9, 7.0, 7.1, 7.2, 7.3, 7.4, 7.5, 7.6, 7.7, 7.8, 7.9, 8.0, 8.1, 8.2, 8.3, 8.4, 8.5, 8.6, 8.7, 8.8, 8.9, 9.0, 9.1, 9.2, 9.3, 9.4, 9.5, 9.6, 9.7, 9.8, 9.9, 10.0, 11.1, 12.2, 13.3, 14.4, 15.5, 17.1, 18.6, 20.2, 21.8, 23.3, 24.9, 26.5, 28.0, 29.6, 31.2, 32.8, 34.3, 35.9, 37.5, 39.0, 40.6, 42.2, 43.7, 45.3, 46.9, 48.4, 50.0]
CHORUS_TYPES = {(5, 0x00), (5, 0x01), (5, 0x02), (5, 0x08), (5, 0x09), (5, 0x10), (5, 0x11), (5, 0x12), (5, 0x18)}
CHORUS_GAIN = 2.06    # コーラスを足したときのエネルギーの増え方から（線の山から読むと 1.4。ぼやけとフィードバックの分を取りこぼす）


def band_masks(N):
    f = np.fft.rfftfreq(N, 1 / FS)
    # 隣の帯と半分ずつ重なる三角の重み（オクターブの対数で）。合わせると 1
    lf = np.log2(np.maximum(f, 1e-3) / 1000.0)
    centers = np.log2(np.array(BANDS) / 1000.0)
    W = np.zeros((len(BANDS), len(f)))
    for i, c in enumerate(centers):
        W[i] = np.clip(1 - np.abs(lf - c), 0, 1)
    W[0][lf < centers[0]] = 1
    W[-1][lf > centers[-1]] = 1
    return W


def reshape(h, rt_meas, rt_target):
    """h（1 チャンネル）の帯ごとの減り方を、1kHz で rt_meas[3] → rt_target になるように伸び縮みさせる"""
    N = 1 << int(np.ceil(np.log2(len(h))))
    H = np.fft.rfft(h, N)
    W = band_masks(N)
    t = np.arange(len(h)) / FS
    k = rt_target / rt_meas[3]
    out = np.zeros(len(h))
    for i in range(len(BANDS)):
        hb = np.fft.irfft(H * W[i], N)[:len(h)]
        rm = rt_meas[i] if np.isfinite(rt_meas[i]) and 0.05 < rt_meas[i] < 60 else rt_meas[3]
        out += hb * np.exp(-6.9078 * t * (1.0 / (rm * k) - 1.0 / rm))
    return out


def load_ir(irdir, msb, lsb):
    for m, l, name in mr_fx.REVERBS:
        if (m, l) == (msb, lsb):
            p = os.path.join(irdir, "reverb_%s_ir.npz" % name)
            if os.path.exists(p):
                d = np.load(p)
                return name, d["L"].astype(np.float64), d["R"].astype(np.float64)
    return None, None, None


def apply(L, R, voice, irdir, rt_cache={}):
    rv = voice["reverb"]
    msb, lsb = rv["type"] >> 7, rv["type"] & 0x7F
    name, hL, hR = load_ir(irdir, msb, lsb)
    if name is None:
        return L, R, "リバーブの型 %d:%d の応答が無い" % (msb, lsb)
    if (msb, lsb) in TABLE4_TYPES:
        import mr_fx_fit as F
        key = name
        if key not in rt_cache:
            rt_cache[key] = [F.rt60(F.octave(hL + hR, fc)) for fc in BANDS]
        target = TABLE4[min(69, max(0, rv["param1"]))]
        hL = reshape(hL, rt_cache[key], target)
        hR = reshape(hR, rt_cache[key], target)
    g = rv.get("return", 96) / 96.0 * voice["common"].get("reverb_send", 127) / 127.0
    x = (np.asarray(L) + np.asarray(R)) / 2
    n = len(x) + len(hL)
    N = 1 << int(np.ceil(np.log2(n)))
    X = np.fft.rfft(x, N)
    wL = np.fft.irfft(X * np.fft.rfft(hL, N), N)[:n] * g
    wR = np.fft.irfft(X * np.fft.rfft(hR, N), N)[:n] * g
    oL = np.zeros(n)
    oR = np.zeros(n)
    oL[:len(L)] += L
    oR[:len(R)] += R
    return oL + wL, oR + wR, "%s（Reverb Time %d、Return %d、送り %d）" % (name, rv["param1"], rv.get("return", 96), voice["common"].get("reverb_send", 127))


def apply_chorus(L, R, voice):
    ch = voice["chorus"]
    msb, lsb = ch["type"] >> 7, ch["type"] & 0x7F
    send = voice["common"].get("chorus_send", 0)
    if (msb, lsb) not in CHORUS_TYPES or send == 0:
        return L, R, "コーラスなし（型 %d:%d・送り %d）" % (msb, lsb, send)
    hz = LFO_HZ[min(127, ch["param1"])]
    depth = 0.176 * ch["param2"] / 1000 * FS
    off = MOD_DELAY_MS[min(127, ch["param4"])] / 1000 * FS
    g = CHORUS_GAIN * ch.get("return", 96) / 96.0 * send / 127.0
    x = (np.asarray(L) + np.asarray(R)) / 2
    n = len(x)
    t = np.arange(n)
    ph = (t * hz / FS) % 1.0
    tri = np.where(ph < 0.5, 2 * ph, 2 - 2 * ph)        # 0..1..0
    out = []
    for tr in (tri, 1 - tri):                           # 左右は逆の向き
        d = off + depth * tr
        out.append(np.interp(t - d, t, x, left=0.0) * g)
    return np.asarray(L) + out[0], np.asarray(R) + out[1], "コーラス 型 %d:%d（%.2fHz・%.1f〜%.1fms・高さ %.2f）" % (
        msb, lsb, hz, off / FS * 1000, (off + depth) / FS * 1000, g)


def write_wav(path, L, R):
    import struct
    x = np.stack([L, R], axis=1).astype("<f4")
    with open(path, "wb") as f:
        data = x.tobytes()
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 3, 2, FS, FS * 8, 8, 32))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def main():
    a = sys.argv[1:]
    src, vj, dst = a[0], a[1], a[2]
    irdir = mr_ref.opt(a, "--ir", "../../roms/motif-rack-es/capture/fx")
    L, R = mr_ref.read_wav(src)
    v = json.load(open(vj, encoding="utf-8"))
    oL, oR, msg = apply(np.asarray(L), np.asarray(R), v, irdir)
    write_wav(dst, oL, oR)
    print(msg)


if __name__ == "__main__":
    main()
