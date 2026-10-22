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
インサーション（apply_insertion）: 3 Band EQ（0F 00）と 2 Band EQ（0F 01、eq2_response）と Compressor（0A 00、compressor）と歪み系（Amp Simulator 09 30・31、Distortion 09 00、V-Distortion 09 10、distortion）と Cross Delay（02 30、cross_delay）と Tempo Cross Delay（02 60、テンポは音色の arp_tempo）とコーラス系（insertion_chorus）とフランジャー（06 00・01、flanger）と Harmonic Enhancer（0F 10、enhancer）。低・高は 1 次の棚（角は表 3 × √2・表 3 / √2）、中は「上げるときは分子だけ、
下げるときは分母だけに利きを入れる」山（Q = Mid Width / 10）、利きは 1 目盛り 1dB（mr_fx.py --block insa → mr_fx_eq.py で測った。
21 本の格子と 0.1〜0.4dB で合う）。周波数ごとに応答を掛ける（FFT）。全部のエレメントが同じインサーションを通る音色のときだけ正しい。"""
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


T3 = [20, 22, 25, 28, 32, 36, 40, 45, 50, 56, 63, 70, 80, 90, 100, 110, 125, 140, 160, 180, 200, 225, 250, 280, 315, 355, 400, 450,
      500, 560, 630, 700, 800, 900, 1000, 1100, 1200, 1400, 1600, 1800, 2000, 2200, 2500, 2800, 3200, 3600, 4000, 4500, 5000, 5600,
      6300, 7000, 8000, 9000, 10000, 11000, 12000, 14000, 16000, 18000, 20000]     # Data List の表 3（EQ の周波数）


def shelf1(kind, f0, g, f):
    """1 次の棚（双一次変換）。f0 は利きの半分（dB）になる所"""
    z = np.exp(-2j * np.pi * f / FS)
    A = 10 ** (g / 20)
    K = np.tan(np.pi * min(f0, FS * 0.49) / FS)
    r = np.sqrt(A)
    if kind == "low":
        b, a = (K * r + 1, K * r - 1), (K / r + 1, K / r - 1)
    else:
        b, a = (r + K, K - r), (1 / r + K, K - 1 / r)
    return (b[0] + b[1] * z) / (a[0] + a[1] * z)


def eq2_response(prm, f):
    """2 Band EQ（0F 01）: 01 低の周波数・02 低の利き・03 高の周波数・04 高の利き。1 次の棚で、利きの半分になる所が
    表 3 × √(利きの倍率)（低）・表 3 / √(利きの倍率)（高）。つまり棚が動き始める所（極）が表 3 の周波数。
    利き ±6・±12dB の 5 本と 0.03〜0.18dB（3 Band EQ は利きによらず √2 倍で、決め方が違う）"""
    H = np.ones(len(f), dtype=complex)
    for kind, fp, gp in (("low", "param1", "param2"), ("high", "param3", "param4")):
        g = prm[gp] - 64
        if g == 0:
            continue
        r = np.sqrt(10 ** (abs(g) / 20))
        f0 = T3[min(60, prm[fp])] * (r if kind == "low" else 1 / r)
        H *= shelf1(kind, f0, g, f)
    return H


def eq3_response(prm, f):
    """3 Band EQ の周波数応答（複素数）"""
    z = np.exp(-2j * np.pi * f / FS)
    H = np.ones(len(f), dtype=complex)
    for kind, gp, fp in (("low", "param1", "param6"), ("high", "param5", "param7")):
        g = prm[gp] - 64
        if g == 0:
            continue
        f0 = T3[min(60, prm[fp])] * (np.sqrt(2) if kind == "low" else 1 / np.sqrt(2))
        A = 10 ** (g / 20)
        K = np.tan(np.pi * min(f0, FS * 0.49) / FS)
        r = np.sqrt(A)
        if kind == "low":
            b, a = (K * r + 1, K * r - 1), (K / r + 1, K / r - 1)
        else:
            b, a = (r + K, K - r), (1 / r + K, K - 1 / r)
        H *= (b[0] + b[1] * z) / (a[0] + a[1] * z)
    g = prm["param3"] - 64
    if g != 0:
        A = 10 ** (g / 20)
        w = 2 * np.pi * T3[min(60, prm["param2"])] / FS
        al = np.sin(w) / (2 * max(0.1, prm["param4"] / 10))
        c = np.cos(w)
        num, den = (1 + al, -2 * c, 1 - al), (1 + al, -2 * c, 1 - al)
        if g > 0:
            num = (1 + al * A, -2 * c, 1 - al * A)
        else:
            den = (1 + al / A, -2 * c, 1 - al / A)
        H *= (num[0] + num[1] * z + num[2] * z * z) / (den[0] + den[1] * z + den[2] * z * z)
    return H


COMP_ATTACK_MS = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 16, 18, 20, 23, 26, 30, 35, 40]           # 表 8
COMP_RELEASE_MS = [10, 15, 25, 35, 45, 55, 65, 75, 85, 100, 115, 140, 170, 230, 340, 680]          # 表 9
COMP_RATIO = [1.0, 1.5, 2.0, 3.0, 5.0, 7.0, 10.0, 20.0]                                             # 表 10


def compressor(L, R, prm):
    """Compressor（0A 00）。mr_fx_comp.py で測った形:
    しきい値 = 値 − 127 dB を、左右の平均の RMS（5ms）の dBFS と比べる（正弦波で −24・−36 が −23.9・−35.9 に当てはまった）。
    比率は表 10 で折れ線（膝はほぼ無い）。出力は 40·log10(Output Level / 64) dB 上げる（96 で +7.0dB）。
    下げる量（dB）は、増えるときは時定数 1.6 × 表 8 の指数で（1・10・40ms で約 2・16・63ms）、
    減るときは dB でまっすぐ 7.5dB / 表 9 の速さで戻る（45・100・680ms で 160・80・7.8 dB/秒）"""
    x = (np.asarray(L) + np.asarray(R)) / 2
    n = len(x)
    win = int(0.005 * FS)
    p = np.convolve(x * x, np.ones(win) / win, mode="full")[:n]
    lev = 10 * np.log10(p + 1e-20)
    T = prm["param3"] - 127
    Rr = COMP_RATIO[min(7, prm["param4"])]
    target = np.where(lev > T, (lev - T) * (1 - 1 / Rr), 0.0)
    ka = np.exp(-1.0 / (1.6 * COMP_ATTACK_MS[min(19, prm["param1"])] / 1000 * FS))
    rel = 7.5 / (COMP_RELEASE_MS[min(15, prm["param2"])] / 1000) / FS
    gr = np.empty(n)
    g = 0.0
    for i in range(n):
        t = target[i]
        if t > g:
            g = t + (g - t) * ka
        else:
            g = max(t, g - rel)
        gr[i] = g
    makeup = 40 * np.log10(max(1, prm["param5"]) / 64)
    k = 10 ** ((makeup - gr) / 20)
    return np.asarray(L) * k, np.asarray(R) * k


# 歪み系（mr_fx_dist.py で当てはめた「線形の応答 h → 頭打ち → LPF」）。型ごとに測った組: (MSB, LSB) → [(アンプの型 or None, Drive, ファイル名)]
DIST_SETS = {
    (9, 48): [(1, 30, "insa_t9_48_lev16"), (0, 30, "insa_t9_48_amptype0_lev16"), (2, 30, "insa_t9_48_amptype2_lev16"),
              (3, 30, "insa_t9_48_amptype3_lev16"), (1, 0, "insa_t9_48_drive0_lev16"), (1, 64, "insa_t9_48_drive64_lev16"),
              (1, 5, "insa_t9_48_drive5_lev16"), (1, 10, "insa_t9_48_drive10_lev16"), (1, 15, "insa_t9_48_drive15_lev16"),
              (1, 20, "insa_t9_48_drive20_lev16")],
    (9, 49): [(None, None, "insa_t9_49_amp2def_lev16")],
    (9, 0): [(None, None, "insa_t9_0_distdef_lev16")],
    (9, 16): [(None, None, "insa_t9_16_vdistdef_lev16")],
}
DIST_DEFAULTS = {(9, 48): dict(drive=30, out=80), (9, 49): None, (9, 0): None, (9, 16): None}
# Amp Simulator（Stack）の 130Hz の小さい音の上げ幅（Drive 0〜10 は変わらず、15 から上がる。127 は 64 からの延ばし）
DRIVE_GAIN_DB = [(0, 0.9), (10, 0.9), (15, 6.7), (20, 11.0), (30, 17.3), (64, 28.7), (127, 28.7 + 0.34 * 63)]
# 頭打ちの大きさ c（Drive 0〜10 で下がり、15 から上はほぼ一定）と、Edge による硬さ p（Drive 15 から上で読んだ）
DRIVE_CLIP = [(0, 0.625), (5, 0.429), (10, 0.168), (15, 0.153), (127, 0.153)]
EDGE_P = [(0, 2.0), (64, 4.0), (112, 32.0), (127, 32.0)]


def drive_gain_db(drive):
    xs = [d for d, _ in DRIVE_GAIN_DB]
    return float(np.interp(drive, xs, [g for _, g in DRIVE_GAIN_DB]))


def distortion(L, R, prm, t, fxdir, default_prm):
    """歪み系の後処理。Amp Simulator は アンプの型・Drive の近い組を選び、Drive の差は頭打ちの前の上げ幅（DRIVE_GAIN_DB を dB で補間）で直す。
    ほかの型は既定のパラメーターで測った 1 組だけ（Drive の差は Amp Simulator と同じ割合で直す）。出力は既定からの比で 40·log10、
    Dry/Wet（パラメーター 10）は元の音 cos・wet sin（確かめた）"""
    if t != (9, 48):
        import mr_fx_dist_sets
        z = os.path.join(os.path.dirname(fxdir), "fxds", "insa_%s_dist.npz" % mr_fx_dist_sets.key_of(prm))
        if os.path.exists(z):
            return per_set_distortion(L, R, prm, t, z)
    sets = DIST_SETS.get(t, DIST_SETS[(9, 0)])     # Comp Distortion・Overdrive は組の録音が無いとき Distortion の既定値の組で代える
    if t == (9, 48):
        amp, drive = prm["param2"], prm["param1"]
        cand = sorted(sets, key=lambda e: (e[0] != amp, abs(e[1] - drive)))
        amp_s, drive_s, name = cand[0]
    else:
        amp_s, drive_s, name = sets[0]
        drive, drive_s = prm["param1"], default_prm.get("param1", prm["param1"])
    z = np.load(os.path.join(fxdir, name + "_dist.npz"))
    hL, hR, c, p, fc = z["hL"].astype(np.float64), z["hR"].astype(np.float64), float(z["c"]), float(z["p"]), float(z["fc"])
    lpf_param = {(9, 48): "param3", (9, 49): "param3"}.get(t)
    if lpf_param and prm.get(lpf_param):
        fc = T3[min(60, prm[lpf_param])]                # パラメーターの LPF Cutoff（表 3）。測ったときの 5kHz は既定の値 48
    k = 10 ** ((drive_gain_db(drive) - drive_gain_db(drive_s)) / 20)
    if t == (9, 48):
        # 頭打ちの大きさは Drive から、硬さは Edge から（Drive 10 以下は組の当てはめの硬さのまま）
        c = float(np.interp(drive, [d for d, _ in DRIVE_CLIP], [v for _, v in DRIVE_CLIP]))
        if drive >= 15:
            p = float(np.exp(np.interp(prm.get("param11", 112), [e for e, _ in EDGE_P], [np.log(v) for _, v in EDGE_P])))
    stereo = False      # 歪み系は左右を足す（別々に通しても 49 音色の比べは変わらなかった）
    xs = [np.asarray(L, dtype=np.float64), np.asarray(R, dtype=np.float64)] if stereo else [(np.asarray(L) + np.asarray(R)) / 2] * 2
    outs = []
    for x, h in zip(xs, (hL, hR)):
        n = len(x) + len(h)
        N = 1 << int(np.ceil(np.log2(n)))
        u = np.fft.irfft(np.fft.rfft(x, N) * np.fft.rfft(h, N), N)[:len(x)] * k
        u = u / (1 + np.abs(u / c) ** p) ** (1 / p)
        f = np.fft.rfftfreq(N, 1 / FS)
        Hl = 1 / (1 + 1j * (f / fc) / 1.6 - (f / fc) ** 2)          # 山のある 2 次（Q 1.6）。h からは測ったときの LPF を割り戻してある
        outs.append(np.fft.irfft(np.fft.rfft(u, N) * Hl, N)[:len(x)])
    og = 1.0
    if default_prm and "param4" in prm and t == (9, 48):
        og = 10 ** (40 * np.log10(max(1, prm["param4"]) / max(1, default_prm.get("out", 80))) / 20)
    # Dry/Wet: 元の音 cos・wet sin（角 π/2 (v − 1) / 126）。Phaser Vin の Distortion の組で Dry/Wet 20・64 が +0.65・+0.25dB（式 +0.67・+0.28dB）
    a = np.pi / 2 * (max(1, prm.get("param10", 127)) - 1) / 126.0
    sg = 1.0 if hL[int(np.argmax(np.abs(hL)))] >= 0 else -1.0     # ブロック全体の向き（per_set_distortion を見よ）
    return [sg * np.cos(a) * np.asarray(src) + np.sin(a) * o * og for src, o in zip((L, R), outs)], name


def vdist_delay(y, prm, ch):
    """V-Distortion の中のディレイ（確かめていない）: 帰りの線（遅れ 08 × 0.1ms、帰り (09 − 64) / 64、戻りに 1 次の LPF (1 − 12 / 10)²）から、
    左は 06・右は 07（× 0.1ms）で取り出し、Delay Mix（11 / 95）で足す。遅れと帰りは実機で確かめた（01_049・01_068）"""
    n = len(y)
    dt = max(1, prm["param6" if ch == 0 else "param7"]) * FS // 10000
    df = max(1, prm["param8"]) * FS // 10000
    fb = (prm["param9"] - 64) / 64.0
    a = (1 - max(1, prm["param12"]) / 10.0) ** 2
    line = np.zeros(n)
    lp = 0.0
    for t in range(n):
        back = line[t - df] if t >= df else 0.0
        lp = (1 - a) * back + a * lp
        line[t] = y[t] + fb * lp
    out = y.copy()
    out[dt:] += prm["param11"] / 95.0 * line[:n - dt]      # Delay Mix 30 の 2 組で、こだまが元の音より −10dB（/127 では −13dB）
    return out


def per_set_distortion(L, R, prm, t, z):
    """プリセットの組のまま録って当てはめた Wiener 型（mr_fx_dist_sets.py）。Dry/Wet は元の音 cos・wet sin"""
    d = np.load(z)
    hL, hR, c, p, fc = d["hL"].astype(np.float64), d["hR"].astype(np.float64), float(d["c"]), float(d["p"]), float(d["fc"])
    stereo = False      # 歪み系は左右を足す（別々に通しても 49 音色の比べは変わらなかった）
    xs = [np.asarray(L, dtype=np.float64), np.asarray(R, dtype=np.float64)] if stereo else [(np.asarray(L) + np.asarray(R)) / 2] * 2
    outs = []
    for ch, h in enumerate((hL, hR)):
        x = xs[ch]
        n = len(x) + len(h)
        N = 1 << int(np.ceil(np.log2(n)))
        u = np.fft.irfft(np.fft.rfft(x, N) * np.fft.rfft(h, N), N)[:len(x)]
        u = u / (1 + np.abs(u / c) ** p) ** (1 / p)
        f = np.fft.rfftfreq(N, 1 / FS)
        y = np.fft.irfft(np.fft.rfft(u, N) / (1 + 1j * (f / fc) / 1.6 - (f / fc) ** 2), N)[:len(x)]
        if t == (9, 16) and prm["param11"] > 0:
            y = vdist_delay(y, prm, ch)
        outs.append(y)
    a = np.pi / 2 * (max(1, prm.get("param10", 127)) - 1) / 126.0      # 元の音 cos・wet sin（distortion と同じ）
    # インサーションのブロック全体（元の音の側も）が負の向きで出る組がある（Phaser Vin の Distortion、Drive 0 で h が −0.55。
    # Dry/Wet 64 の正弦波は元の音と同じ向きに足されて +0.25dB）。元の音の向きを h の山の向きにそろえる
    sg = 1.0 if hL[int(np.argmax(np.abs(hL)))] >= 0 else -1.0
    return [sg * np.cos(a) * np.asarray(src) + np.sin(a) * o for src, o in zip((L, R), outs)], "組の録音"


TABLE14_Q = [1 / 12, 3 / 32, 1 / 8, 1 / 6, 3 / 16, 1 / 4, 1 / 3, 3 / 8, 1 / 2, 2 / 3, 3 / 4, 1, 4 / 3, 3 / 2, 2, 8 / 3, 3, 4, 5, 6]   # 表 14（4 分音符 = 1）


def cross_delay(L, R, prm, tempo=None):
    """Cross Delay（02 30）。mr_fx.py --block insa で測った形: 左の入力 →（左 → 右、パラメーター 1 × 0.1ms）→ 右の出力、
    右の入力 →（右 → 左、パラメーター 2）→ 左の出力。出たものは反対側の線へ、フィードバック (値 − 64) / 64 を掛け、1 次の LPF
    （係数 (1 − High Damp / 10)²）を通して戻る（最初のこだまは LPF を通らない）。入力の選び方（4）: 0 = L だけ左 → 右へ、1 = R だけ右 → 左へ、
    2 = 両方。Dry/Wet（10）: 元の音 min(1, (127 − 値) / 63)²、こだま min(1, (値 − 1) / 63)²。EQ（13〜16）は入れていない"""
    L, R = np.asarray(L, dtype=np.float64), np.asarray(R, dtype=np.float64)
    n = len(L)
    if tempo is None:
        d1 = max(1, int(round(prm["param1"] * 0.1e-3 * FS)))
        d2 = max(1, int(round(prm["param2"] * 0.1e-3 * FS)))
    else:
        # Tempo Cross Delay（02 60）: 遅れは表 14 の音符 × 60 / テンポ + Lag（パラメーター 6、(値 − 64) ms を両方に）。
        # テンポ 120 で 8 分・4 分が 250・500ms、Lag +20 で 270・520ms
        lag = (prm.get("param6", 64) - 64) * 1e-3
        q = 60.0 / max(1, tempo)
        d1 = max(1, int(round((TABLE14_Q[min(19, prm["param1"])] * q + lag) * FS)))
        d2 = max(1, int(round((TABLE14_Q[min(19, prm["param2"])] * q + lag) * FS)))
    fb = (prm["param3"] - 64) / 64.0
    sel = prm["param4"]
    a = (1 - max(1, prm["param5"]) / 10.0) ** 2
    v = max(1, prm.get("param10", 127))
    gd = min(1.0, (127 - v) / 63.0) ** 2
    gw = min(1.0, (v - 1) / 63.0) ** 2
    if tempo is not None:
        # Tempo Cross Delay はエネルギーを保つ混ぜ方（32・64・96・127 でこだま 0.378・0.710・0.928・1）
        gd = np.cos(np.pi / 2 * (v - 1) / 126.0)
        gw = np.sin(np.pi / 2 * (v - 1) / 126.0)
    inL = L if sel in (0, 2) else np.zeros(n)
    inR = R if sel in (1, 2) else np.zeros(n)
    lr = np.zeros(n)     # 左 → 右の線の出力（右へ）
    rl = np.zeros(n)     # 右 → 左の線の出力（左へ）
    lr_in = np.zeros(n)
    rl_in = np.zeros(n)
    plr = prl = 0.0      # 戻りの LPF の状態
    for t in range(n):
        if t >= d1:
            lr[t] = lr_in[t - d1]
        if t >= d2:
            rl[t] = rl_in[t - d2]
        plr = (1 - a) * lr[t] + a * plr
        prl = (1 - a) * rl[t] + a * prl
        lr_in[t] = inL[t] + fb * prl
        rl_in[t] = inR[t] + fb * plr
    return gd * L + gw * rl, gd * R + gw * lr


def insertion_chorus(L, R, prm):
    """インサーションのコーラス（Chorus 1〜5・Celeste 1〜4）。遅れの動きはシステムのコーラスと同じ（mr_fx.py --block insa --train で確かめた）。
    Dry/Wet（10）: 元の音 cos(π/2 (v − 1) / 126)、コーラスの線のエネルギー（元の音を 1 として）は 32・64・127 で 0.364・0.838・1.5 を補間。
    EQ（6〜9）はフランジャーと同じ欄なので fx_eq を掛ける（コーラスでは確かめていない）。Input Mode（15）は input_mode。Feedback（3）は入れていない"""
    v = max(1, prm.get("param10", 64))
    gd = np.cos(np.pi / 2 * (v - 1) / 126.0)
    gw = np.sqrt(np.interp(v, [1, 32, 64, 127], [0.0, 0.364, 0.838, 1.5]))
    hz = LFO_HZ[min(127, prm["param1"])]
    depth = 0.176 * prm["param2"] / 1000 * FS
    off = MOD_DELAY_MS[min(127, prm["param4"])] / 1000 * FS
    xs = input_mode(L, R, prm)
    n = len(xs[0])
    t = np.arange(n)
    ph = (t * hz / FS) % 1.0
    tri = np.where(ph < 0.5, 2 * ph, 2 - 2 * ph)
    out = [np.interp(t - (off + depth * tr), t, x, left=0.0) for x, tr in zip(xs, (tri, 1 - tri))]
    return fx_eq(gd * np.asarray(L) + gw * out[0], gd * np.asarray(R) + gw * out[1], prm)


FLANGER_TYPES = {(6, 0x00), (6, 0x01)}       # Flanger 1・2（測ったのは Flanger 1。2 は遅れと揺れが同じで EQ の既定だけ違う）
FLANGER_MS_PER_DEPTH = 0.0914                 # 深さ 1 あたりの遅れの幅（14・64・127 で 1.28・5.83・11.65ms）
FLANGER_RATE = 1.0                            # 表 1 に掛ける揺れの速さの直し


def flanger(L, R, prm):
    """インサーションのフランジャー（mr_fx.py --block insa --types 6:0 --train 6 で測った形）。左右それぞれ
    y = x + 帰り × 遅れた y、遅れ = 表 2[Delay Offset（4）] + 深さ（2）× 0.0914ms × 三角波（速さは表 1[1]）。
    右の三角波は左から LFO Phase Difference（14、(値 − 64) × 3°）ずらす（4 で逆向き、64 で同じ）。帰りは (値 − 64) / 64。
    Dry/Wet（10）: 元の音 cos、揺れの線 sin（角 π/2 (v − 1) / 126）"""
    v = max(1, prm.get("param10", 64))
    gd = np.cos(np.pi / 2 * (v - 1) / 126.0)
    gw = np.sin(np.pi / 2 * (v - 1) / 126.0)
    hz = LFO_HZ[min(127, prm["param1"])] * FLANGER_RATE
    depth = FLANGER_MS_PER_DEPTH * prm["param2"] / 1000 * FS
    off = MOD_DELAY_MS[min(127, prm["param4"])] / 1000 * FS
    fb = (prm["param3"] - 64) / 64.0
    dph = (prm.get("param14", 64) - 64) * 3.0 / 360.0
    x = (np.asarray(L, dtype=np.float64) + np.asarray(R, dtype=np.float64)) / 2
    n = len(x)
    t = np.arange(n)
    outs = []
    for ph0 in (0.0, dph):
        ph = (t * hz / FS + ph0) % 1.0
        d = off + depth * np.where(ph < 0.5, 2 * ph, 2 - 2 * ph)
        if fb == 0:
            outs.append(np.interp(t - d, t, x, left=0.0))
            continue
        y = np.zeros(n)
        w = np.zeros(n)      # 遅れの線の出力
        for i in range(n):
            p = i - d[i]
            k = int(np.floor(p))
            f = p - k
            if k >= 0:
                w[i] = (1 - f) * y[k] + f * (y[k + 1] if k + 1 < i else 0.0)
            y[i] = x[i] + fb * w[i]
        outs.append(w)
    oL, oR = gd * np.asarray(L) + gw * outs[0], gd * np.asarray(R) + gw * outs[1]
    return fx_eq(oL, oR, prm)


def enhancer_response(prm, f):
    """Harmonic Enhancer（0F 10）の応答: 1 + g × HPF。HPF は 3 次のバターワース（双一次変換、角は表 3[01 HPF Cutoff]）、
    g = 8.13 × Drive（02）/ 64 × Mix Level（03）/ 64（小さいノイズで、角 4 通り・Drive と Mix 5 通りが 0.1〜0.9dB）"""
    fc = min(T3[min(60, prm["param1"])], FS * 0.49)
    s = 1j * np.tan(np.pi * np.minimum(f, FS * 0.4999) / FS) / np.tan(np.pi * fc / FS)
    g = 8.13 * prm["param2"] / 64.0 * prm["param3"] / 64.0
    return 1 + g * (s / (s + 1)) * (s * s / (s * s + s * np.sqrt(2) + 1))


def enhancer(L, R, prm):
    """倍音は足さない（正弦波 8 段で、出口が ±1.0 に届くまでは倍音が無い）。出口は ±1.0 で頭打ち（届いたときは方形波の倍音）"""
    N = 1 << int(np.ceil(np.log2(len(L) + 1)))
    H = enhancer_response(prm, np.fft.rfftfreq(N, 1 / FS))
    return tuple(np.clip(np.fft.irfft(np.fft.rfft(np.asarray(x), N) * H, N)[:len(x)], -1.0, 1.0) for x in (L, R))


FX_EQ_FORM = "eq2"


def fx_eq(L, R, prm, base=6):
    """揺れる系のエフェクトの EQ（6 低の周波数・7 低の利き・8 高の周波数・9 高の利き。Ensemble Detune は 11〜14）を、出口（元の音も含む）に掛ける"""
    k = ["param%d" % (base + i) for i in range(4)]
    if prm.get(k[1], 64) == 64 and prm.get(k[3], 64) == 64:
        return L, R
    q = dict(param1=prm[k[0]], param2=prm[k[1]], param3=prm[k[2]], param4=prm[k[3]])
    N = 1 << int(np.ceil(np.log2(len(L) + 1)))
    f = np.fft.rfftfreq(N, 1 / FS)
    if FX_EQ_FORM == "eq2":
        H = eq2_response(q, f)
    else:
        H = eq3_response(dict(param1=q["param2"], param2=34, param3=64, param4=10, param5=q["param4"], param6=q["param1"], param7=q["param3"]), f)
    return tuple(np.fft.irfft(np.fft.rfft(np.asarray(x), N) * H, N)[:len(x)] for x in (L, R))


ENSEMBLE_HZ = 1 / 1.485      # のこぎりの回る速さ（Detune −10・+50 セントで同じ）


def ensemble_detune(L, R, prm):
    """Ensemble Detune（05 30）。左は Detune（01、(値 − 64) セント）だけ下げ、右は上げる、遅れ線の音程変え。
    遅れはのこぎりで 0.25〜1.25 × 幅を回り（幅 = 速さの差 × 1.485 秒）、回りの最後の 1/4 で、近い側から入る 2 本目へまっすぐ（振幅の和が 1）移る。
    左右はのこぎりが半周ずれる。02・03 Initial Delay（表 2）は遅れに足す（確かめていない）。Dry/Wet（10）は元の音 cos・wet sin（確かめた）"""
    v = max(1, prm.get("param10", 64))
    gd = np.cos(np.pi / 2 * (v - 1) / 126.0)
    gw = np.sin(np.pi / 2 * (v - 1) / 126.0)
    cents = prm["param1"] - 64
    xs = input_mode(L, R, prm)
    n = len(xs[0])
    t = np.arange(n)
    outs = []
    for x, c, ph0, dp in ((xs[0], -cents, 0.0, "param2"), (xs[1], cents, 0.5, "param3")):
        init = MOD_DELAY_MS[min(127, prm.get(dp, 0))] / 1000 * FS
        rate = 1 - 2 ** (c / 1200.0)          # 遅れの伸びる速さ（下げるときに正）
        span = abs(rate) / ENSEMBLE_HZ * FS
        if span < 1e-9:
            outs.append(np.interp(t - init, t, x, left=0.0))
            continue
        sw = (t * ENSEMBLE_HZ / FS + ph0) % 1.0
        s1 = sw if rate > 0 else 1 - sw         # 主の線: 0.25 + s1（音程を上げるときは遅れが縮む向き）
        d1 = init + span * (0.25 + s1)
        d2 = init + span * (s1 - 0.75) if rate > 0 else init + span * (s1 + 1.25)
        u = np.clip((sw - 0.75) / 0.25, 0, 1)
        y1 = np.interp(t - d1, t, x, left=0.0)
        y2 = np.interp(t - d2, t, x, left=0.0)
        outs.append((1 - u) * y1 + u * y2)
    oL, oR = gd * np.asarray(L) + gw * outs[0], gd * np.asarray(R) + gw * outs[1]
    return fx_eq(oL, oR, prm, 11)


def input_mode(L, R, prm):
    """コーラス系と Ensemble Detune の Input Mode（15）: 0 = mono（左右を足して半分）、1 = stereo（左右それぞれ）。測った音は mono で、確かめていない"""
    L, R = np.asarray(L, dtype=np.float64), np.asarray(R, dtype=np.float64)
    if prm.get("param15", 0) == 1:
        return L, R
    x = (L + R) / 2
    return x, x


# Pitch Change 1（0B 00）の Fine（値 − 64）→ セント（正弦波 鍵 69 で測った。まっすぐでなく、+ と − でも違う）
PC_FINE = [(-50, -26.2), (-10, -10.79), (-3, -5.51), (0, 0.0), (3, 2.48), (5, 5.13), (6, 5.47), (7, 7.88), (8, 7.58), (10, 10.59),
           (15, 10.22), (20, 10.06), (30, 18.26), (40, 23.77), (50, 26.15)]
# のこぎりの遅れの下限と幅（ms）を |Fine| で（ノイズの列で読んだ ±3・±5・±10・±50）
PC_GEOM = [(3, 2.1, 11.4), (5, 3.2, 12.7), (10, 4.6, 16.7), (50, 13.0, 29.8)]


def pitch_change(L, R, prm):
    """Pitch Change 1（0B 00）: 2 本の音程変え（Pitch（01、半音）+ Fine 1・2（03・04））。1 本目は Pan 1（11）・Output Level 1（12）、
    2 本目は Pan 2（13）・Level 2（14）。遅れ線ののこぎりと最後の 1/4 の受け渡しは Ensemble Detune と同じ形にした（受け渡しの長さは確かめていない）。
    下限と幅は |Fine| の表から（音程を半音で変えるときは幅 1280 サンプル・下限は幅の 1/4 として近似。確かめていない）。
    Feedback（05）と Initial Delay（02）は入れていない。Dry/Wet（10）は元の音 cos・wet sin"""
    v = max(1, prm.get("param10", 64))
    gd = np.cos(np.pi / 2 * (v - 1) / 126.0)
    gw = np.sin(np.pi / 2 * (v - 1) / 126.0)
    x = (np.asarray(L, dtype=np.float64) + np.asarray(R, dtype=np.float64)) / 2
    n = len(x)
    t = np.arange(n)
    oL, oR = np.zeros(n), np.zeros(n)
    semi = prm["param1"] - 64
    for fk, pk, lk, ph0 in (("param3", "param11", "param12", 0.0), ("param4", "param13", "param14", 0.5)):
        fine = prm[fk] - 64
        c = 100 * semi + float(np.interp(fine, [a for a, _ in PC_FINE], [b for _, b in PC_FINE]))
        rate = 1 - 2 ** (c / 1200.0)
        if semi == 0:
            af = min(50, max(3, abs(fine)))
            lo = float(np.interp(af, [g[0] for g in PC_GEOM], [g[1] for g in PC_GEOM])) / 1000 * FS
            span = float(np.interp(af, [g[0] for g in PC_GEOM], [g[2] for g in PC_GEOM])) / 1000 * FS
        else:
            span = 1280.0
            lo = span / 4
        if abs(rate) < 1e-9:
            y = np.interp(t - lo, t, x, left=0.0)
        else:
            hz = abs(rate) * FS / span
            sw = (t * hz / FS + ph0) % 1.0
            s1 = sw if rate > 0 else 1 - sw
            # 主の線は lo + span·s1。最後の 1/4 で、次の周の始まりへ続く線へまっすぐ渡す
            d1 = lo + span * s1
            d2 = lo + span * (s1 - 1) if rate > 0 else lo + span * (s1 + 1)
            u = np.clip((sw - 0.75) / 0.25, 0, 1)
            d2 = np.maximum(d2, 0)
            y = (1 - u) * np.interp(t - d1, t, x, left=0.0) + u * np.interp(t - d2, t, x, left=0.0)
        a = np.pi / 2 * (max(1, prm[pk]) - 1) / 126.0
        g = prm[lk] / 127.0
        oL += y * g * np.cos(a)
        oR += y * g * np.sin(a)
    return gd * np.asarray(L) + gw * oL, gd * np.asarray(R) + gw * oR


# Lo-Fi の Word Length（02）→ 1 サンプルで動ける量の上限（正弦波 440Hz・振幅 0.42 で、出口の傾きの 99.5% 点）。127 は制限なし
LOFI_SLEW = [(1, 0.00209), (2, 0.00222), (4, 0.00248), (8, 0.00300), (16, 0.00405), (32, 0.00614), (64, 0.01279), (96, 0.02606)]
LOFI_EMPH = 0.88     # Emphasis（08）: 前で (x − a·x[−1]) / (1 − a)、後で 1 次の LPF（係数 a、直流で 1）。正弦波の三角波の倍音（−22・−34dB）から


def lofi_quant(x, prm):
    """Lo-Fi の語長: 1 サンプルあたりの動きを ±s に切る（スルーレートの制限。語長を下げると正弦波が三角波になり、三角波の振幅は
    入口の大きさによらない）。s は LOFI_SLEW を対数で補間し、Bit Assign（07）が 1 増えるごとに 2 倍（4 以上では制限が効かない）"""
    wl = prm["param2"]
    if wl >= 127:
        return x
    s = float(np.exp(np.interp(wl, [a for a, _ in LOFI_SLEW], [np.log(b) for _, b in LOFI_SLEW]))) * 2 ** prm.get("param7", 0)
    a = LOFI_EMPH if prm.get("param8", 0) else 0.0
    if a:
        x = np.append(x[0], x[1:] - a * x[:-1]) / (1 - a)
    y = np.empty(len(x))
    v = 0.0
    for i, xi in enumerate(x.tolist()):
        d = xi - v
        v += s if d > s else (-s if d < -s else d)
        y[i] = v
    if a:
        out = np.empty(len(y))
        acc = 0.0
        for i, yi in enumerate(y.tolist()):
            acc = (1 - a) * yi + a * acc
            out[i] = acc
        y = out
    return y


def lofi(L, R, prm, fxdir=None):
    """Lo-Fi（0D 00）。入口の 1 次の HPF（20.5Hz）→ Pre-LPF（双一次変換の 2 次、角は表 3[04]、Q = 06 / 10）→ 標本化の間引き
    （表 13 = 44100 / (01 + 1)、01 + 1 サンプルごとに値を持ち替える）→ 語長（lofi_quant）→ Filter Type（05、録った応答 fxlf/lofi_type<n>.ir）→ Output Gain（(03 − 6) dB）。
    Dry/Wet（10）は元の音 cos・wet sin とした（確かめていない）"""
    fxdir = fxdir or os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../roms/motif-rack-es/capture/fxd")
    xs = input_mode(L, R, prm)
    outs = []
    for x in xs:
        n = len(x)
        N = 1 << int(np.ceil(np.log2(n + 1)))
        f = np.fft.rfftfreq(N, 1 / FS)
        sh = 1j * np.tan(np.pi * np.minimum(f, FS * 0.4999) / FS) / np.tan(np.pi * 20.5 / FS)
        fc = T3[min(60, max(0, prm["param4"]))]
        sl = 1j * np.tan(np.pi * np.minimum(f, FS * 0.4999) / FS) / np.tan(np.pi * min(fc, FS * 0.49) / FS)
        q = max(1, prm["param6"]) / 10.0
        H = (sh / (sh + 1)) / (sl * sl + sl / q + 1)
        y = np.fft.irfft(np.fft.rfft(x, N) * H, N)[:n]
        k = prm["param1"] + 1
        if k > 1:
            y = np.repeat(y[::k], k)[:n]
        y = lofi_quant(y, prm)
        ft = prm["param5"]
        if 1 <= ft <= 5:
            p = os.path.join(os.path.dirname(fxdir), "fxlf", "lofi_type%d.ir" % ft)
            if os.path.exists(p):
                h = np.fromfile(p, dtype="<f4").astype(np.float64)
                # 山を 64 点目に置いて切ってある。山より前にもエネルギーがあるので（電話の型で 2dB）、頭は落とさない（1.4ms 遅れる）
                M = 1 << int(np.ceil(np.log2(n + len(h))))
                y = np.fft.irfft(np.fft.rfft(y, M) * np.fft.rfft(h, M), M)[:n]
        outs.append(y * 10 ** ((prm["param3"] - 6) / 20.0))
    v = max(1, prm.get("param10", 127))
    gd, gw = np.cos(np.pi / 2 * (v - 1) / 126.0), np.sin(np.pi / 2 * (v - 1) / 126.0)
    return gd * np.asarray(L) + gw * outs[0], gd * np.asarray(R) + gw * outs[1]


def pitch_change2(L, R, prm):
    """Pitch Change 2（0B 01）: Pitch Change 1 と同じ形で、Fine 1・2（03・04）がセントそのもの（Data List の注）。確かめていない"""
    q = dict(prm)
    for k in ("param3", "param4"):
        c = prm[k] - 64
        # Pitch Change 1 の表を逆に引いて、そのセントになる Fine の値にする
        fx, fy = [a for a, _ in PC_FINE], [b for _, b in PC_FINE]
        order = np.argsort(fy)
        q[k] = 64 + float(np.interp(c, np.array(fy)[order], np.array(fx)[order]))
    return pitch_change(L, R, q)


# Data List の表 19（Ring Mod の搬送波、Hz）
TABLE19 = [0.5, 1.5, 2.0, 2.5, 3.5, 4.0, 4.5, 5.5, 6.0, 6.5, 7.5, 8.0, 8.5, 8.9, 9.4, 10.0, 10.6, 11.2, 11.8, 12.5, 13.2, 13.9, 14.7, 15.6,
           16.5, 17.4, 18.4, 19.4, 20.6, 21.7, 23.0, 24.3, 25.7, 27.1, 28.7, 30.3, 32.0, 33.9, 35.8, 37.8, 40.0, 42.3, 44.7, 47.3, 49.9,
           52.8, 55.8, 59.0, 62.4, 65.9, 69.7, 73.7, 77.9, 82.3, 87.0, 92.0, 97.2, 103, 109, 115, 121, 128, 136, 143, 152, 160, 169, 179,
           189, 200, 211, 224, 236, 250, 264, 279, 295, 312, 330, 348, 368, 389, 412, 435, 460, 486, 514, 543, 574, 607, 642, 678, 717,
           758, 801, 847, 895, 946, 1000, 1050, 1110, 1180, 1240, 1320, 1390, 1470, 1550, 1640, 1740, 1840, 1940, 2050, 2170, 2290,
           2430, 2560, 2710, 2870, 3030, 3200, 3390, 3580, 3790, 4000, 4230, 4470, 4730, 5000]


def ring_mod(L, R, prm):
    """Ring Modulator（0F 70）。入口に 1 次の HPF（表 3[06]）と LPF（表 3[07]）、搬送波（正弦、振幅 1.07）との掛け算、EQ（13〜16）、
    元の音 cos・wet sin。搬送波 = 表 19[01] + 1.70Hz × Fine（02）（Coarse 64 で 32・64・127 を確かめた）。
    LFO（03 形 0 = 2 つの値を行き来・1 = 正弦、04 深さ、05 速さ 表 1）は搬送波を 2^(±0.35 × 深さ / 64) 倍に動かす（深さ 64 で 685 / 1105Hz を確かめた。近似）"""
    v = max(1, prm.get("param10", 64))
    gd, gw = np.cos(np.pi / 2 * (v - 1) / 126.0), np.sin(np.pi / 2 * (v - 1) / 126.0)
    if v <= 1:
        return np.asarray(L), np.asarray(R)
    f0 = TABLE19[min(127, prm["param1"])] + 1.70 * prm["param2"]
    n = len(L)
    t = np.arange(n) / FS
    hz = LFO_HZ[min(127, prm["param5"])]
    ph = (t * hz) % 1.0
    m = np.sin(2 * np.pi * ph) if prm["param3"] == 1 else np.where(ph < 0.5, 1.0, -1.0)
    fi = f0 * 2 ** (0.35 * prm["param4"] / 64.0 * m)
    car = 1.07 * np.cos(2 * np.pi * np.cumsum(fi) / FS)
    outs = []
    for x in (np.asarray(L, dtype=np.float64), np.asarray(R, dtype=np.float64)):
        N = 1 << int(np.ceil(np.log2(n + 1)))
        f = np.fft.rfftfreq(N, 1 / FS)
        z = 1j * np.tan(np.pi * np.minimum(f, FS * 0.4999) / FS)
        hp = z / np.tan(np.pi * T3[min(60, prm["param6"])] / FS)
        lp = z / np.tan(np.pi * min(T3[min(60, prm["param7"])], FS * 0.49) / FS)
        y = np.fft.irfft(np.fft.rfft(x, N) * (hp / (hp + 1)) / (lp + 1), N)[:n]
        outs.append(y * car)
    outs = fx_eq(outs[0], outs[1], prm, 13)
    return gd * np.asarray(L) + gw * outs[0], gd * np.asarray(R) + gw * outs[1]


def apply_block(L, R, ins, fxdir=None):
    """インサーション 1 つ（A か B）を掛ける。まだの型はそのまま返す"""
    t = (ins["type"] >> 7, ins["type"] & 0x7F)
    if t == (0, 0):
        return L, R, "なし"
    if t in DIST_SETS or t in ((9, 1), (9, 32)):
        fxdir = fxdir or os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../roms/motif-rack-es/capture/fxd")
        dflt = dict(param1=30, out=80) if t == (9, 48) else {}
        if t != (9, 48):
            js = os.path.join(fxdir, DIST_SETS.get(t, DIST_SETS[(9, 0)])[0][2] + ".json")
            if os.path.exists(js):
                dflt = json.load(open(js))
        (oL, oR), name = distortion(L, R, ins, t, fxdir, dflt)
        return oL, oR, "歪み系 %d:%d（%s）" % (t[0], t[1], name)
    if t == (15, 0x71) and ins.get("param10", 64) <= 1:
        return L, R, "Dynamic Ring Mod（Dry/Wet 1 で素通し）"
    if t in ((10, 0), (10, 8)):                     # 0A 08 は表に無いが、値の並び（Attack・Release・Threshold・Ratio・Output）がコンプなので同じ模型
        oL, oR = compressor(L, R, ins)
        return oL, oR, "Compressor（%ddB・%.1f:1）" % (ins["param3"] - 127, COMP_RATIO[min(7, ins["param4"])])
    if t in CHORUS_TYPES:
        oL, oR = insertion_chorus(L, R, ins)
        return oL, oR, "コーラス %d:%d（Dry/Wet %d）" % (t[0], t[1], ins.get("param10", 64))
    if t == (5, 0x30):
        oL, oR = ensemble_detune(L, R, ins)
        return oL, oR, "Ensemble Detune（%+d セント・Dry/Wet %d）" % (ins["param1"] - 64, ins.get("param10", 64))
    if t == (15, 0x70):
        oL, oR = ring_mod(L, R, ins)
        return oL, oR, "Ring Mod（%.0fHz・Dry/Wet %d）" % (TABLE19[min(127, ins["param1"])] + 1.70 * ins["param2"], ins.get("param10", 64))
    if t == (13, 0):
        oL, oR = lofi(L, R, ins, fxdir)
        return oL, oR, "Lo-Fi（%.0fHz・語長 %d）" % (FS / (ins["param1"] + 1), ins["param2"])
    if t == (11, 1):
        oL, oR = pitch_change2(L, R, ins)
        return oL, oR, "Pitch Change 2（%+d 半音・Fine %+d / %+d セント）" % (ins["param1"] - 64, ins["param3"] - 64, ins["param4"] - 64)
    if t == (11, 0):
        oL, oR = pitch_change(L, R, ins)
        return oL, oR, "Pitch Change 1（%+d 半音・Fine %+d / %+d・Dry/Wet %d）" % (ins["param1"] - 64, ins["param3"] - 64, ins["param4"] - 64, ins.get("param10", 64))
    if t == (15, 16):
        oL, oR = enhancer(L, R, ins)
        return oL, oR, "Enhancer（%.0fHz・Drive %d・Mix %d）" % (T3[min(60, ins["param1"])], ins["param2"], ins["param3"])
    if t in FLANGER_TYPES:
        oL, oR = flanger(L, R, ins)
        return oL, oR, "フランジャー %d:%d（Dry/Wet %d）" % (t[0], t[1], ins.get("param10", 64))
    if t == (2, 96):
        tempo = ins.get("_tempo", 120)
        oL, oR = cross_delay(L, R, ins, tempo)
        return oL, oR, "Tempo Cross Delay（テンポ %d）" % tempo
    if t == (2, 48):
        oL, oR = cross_delay(L, R, ins)
        return oL, oR, "Cross Delay（%.1f / %.1f ms）" % (ins["param1"] / 10, ins["param2"] / 10)
    if t in ((15, 0), (15, 1)):
        n = len(L) + FS
        N = 1 << int(np.ceil(np.log2(n)))
        f = np.fft.rfftfreq(N, 1 / FS)
        H = eq3_response(ins, f) if t == (15, 0) else eq2_response(ins, f)
        out = [np.fft.irfft(np.fft.rfft(x, N) * H, N)[:len(x)] for x in (np.asarray(L), np.asarray(R))]
        if t == (15, 1):
            return out[0], out[1], "2 Band EQ（%+d / %+d dB）" % (ins["param2"] - 64, ins["param4"] - 64)
        return out[0], out[1], "3 Band EQ（%+d / %+d / %+d dB）" % (ins["param1"] - 64, ins["param3"] - 64, ins["param5"] - 64)
    import mr_fx_dyn
    if t in mr_fx_dyn.TYPES:
        r = mr_fx_dyn.apply(L, R, ins)
        if r is not None:
            return r[0], r[1], "音量ごとに録った応答 %d:%d" % t
    import mr_fx_ltv
    if t in mr_fx_ltv.TYPES:
        r = mr_fx_ltv.apply(L, R, ins, ins.get("_tempo", 120))
        if r is not None:
            return r[0], r[1], "録った揺れる応答 %d:%d" % t
    import mr_fx_mod
    if t in mr_fx_mod.TYPES:
        r = mr_fx_mod.apply(L, R, ins, ins.get("_tempo", 120))
        if r is not None:
            return r[0], r[1], "録った揺れ %d:%d" % t
    import mr_fx_lti
    if t in mr_fx_lti.LTI_TYPES:
        r = mr_fx_lti.apply(L, R, ins, ins.get("_tempo", 120))
        if r is not None:
            return r[0], r[1], "録った応答 %d:%d" % t
    return L, R, "型 %d:%d はまだ（通さない）" % t


def apply_insertion(L, R, voice, fxdir=None):
    """エレメントのインサーションのスイッチ（全部のエレメントで同じとする）とつなぎ方（40 00 40）に合わせて A・B を掛ける。
    つなぎ方 0 = 並列（スイッチ 1 は A、2 は B）、1 = A → B（スイッチ 1 は A → B、2 は B）、2 = B → A（スイッチ 2 は B → A、1 は A）"""
    sw = [e["oscillator"]["insertion_effect_switch"] for e in voice["elements"] if e["oscillator"]["element_switch"]]
    sw = max(set(sw), key=sw.count) if sw else 0
    con = voice.get("insertion_connect", {}).get("insertion_connect_type", 0)
    if sw == 0:
        return L, R, "インサーションを通さない"
    chain = {(0, 1): "A", (0, 2): "B", (1, 1): "AB", (1, 2): "B", (2, 1): "A", (2, 2): "BA"}[(con, sw)]
    # 芯の出力には共通の音量（40·log10(値 / 127)）が入っているが、実機ではインサーションの後で掛かるので、前で戻して後で掛け直す
    vg = (voice.get("common", {}).get("volume", 127) / 127.0) ** 2
    if vg > 0:
        L, R = np.asarray(L) / vg, np.asarray(R) / vg
    msgs = []
    for blk in chain:
        ins = dict(voice["insertion1" if blk == "A" else "insertion2"])
        ins["_tempo"] = voice.get("arp_tempo", 120)
        L, R, m = apply_block(L, R, ins, fxdir)
        msgs.append("%s: %s" % (blk, m))
    if vg > 0:
        L, R = np.asarray(L) * vg, np.asarray(R) * vg
    return L, R, " → ".join(msgs)


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
