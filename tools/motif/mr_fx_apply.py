"""サンプリング方式の芯で鳴らした WAV に、測ったインパルス応答でボイスのリバーブを掛ける（後処理。録音を読むだけで、実機には何も送らない）。
  python mr_fx_apply.py 入力.wav 音色.json 出力.wav [--ir 置き場（reverb_<型>_ir.npz）]

型ごとのインパルス応答（mr_fx.py → mr_fx_fit.py --save。型の既定のパラメーター・Return 96・送り 127 で測ったもの）を使い、
* Reverb Time（パラメーター 1）: Data List の表 4 の秒を 1kHz の残響時間とし、オクターブの帯ごとの減り方を、測ったときとの比で伸び縮みさせる
  （Hall 1 で値 10〜69 を測ると 1kHz の残響時間は表 4 と 5% 以内。帯ごとの比は「測ったときの比のまま」とした。長い所では高い帯が表より短い）。
  表 4 を使う型（Hall 1〜R3 Plate、White Room〜Canyon）だけ。Large Hall〜Rich Plate（表 25〜27）は伸び縮みさせない
* 音量: Return / 96 × 送り / 127（Return・CC91 とも、音量に比例すると測った。送りは音色の共通部分の Reverb Send）
* 入力は左右の平均（測ったときは真ん中の音）
コーラス・インサーションはまだ。"""
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
