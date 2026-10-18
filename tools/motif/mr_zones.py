"""mr_sweep.py で録った下録り（w0001.wav …）から、鍵ごとのサンプルの割り当てを見立てる。
  python mr_zones.py 置き場 [波形番号 …] [-o zones2.csv]

隣り合う鍵 n と n+1 を比べて、次のどれかに分ける。
  S  同じサンプルを半音ぶん速く読んでいる（片方を 2^(85/1024) か 2^(86/1024) 倍に縮めると重なる）
  ~  S と見るが、音程のある音で相関が 0.9〜0.95（よく似た隣のサンプルとの境目かもしれない）
  =  同じサンプルを同じ速さで読んでいる（そのまま重なる。音程が頭打ちになった所や、音程を追わない波形）
  s  S と同じだが、出だしの位置が 5 サンプル以上ずれていた（雑音の波形や、前の音の余韻が残っていた鍵。出だしの見つけ方がぶれる）
  l  S と同じだが、低い帯域だけで重なった（元の鍵より上の雑音の波形。高域は折り返しで鍵ごとに変わる）
  ?  見分けられない（雑音の波形で、波形は重ならないが音量の形は重なる。高い鍵の折り返しで波形が崩れる所）
  |  別のサンプル（どちらでも重ならない）= 境目
  .  どちらかが無音
  w  音程の天井より上（下の「折り返し」）。境目に数えない
区間の数は | の数 + 1（? は境目に数えない）。
鍵 n と n+12 がそのまま重なる所が多い波形は「1 オクターブごとにくり返す」（音程を追わない効果音の並び）として octave 列に印を付ける。

折り返し: 元の音程より上へ行きすぎると、音程が 1 オクターブ下へ戻り、そこからまた上がる（何オクターブでもくり返す）。
コースチューンを ±12 すると折り返しの鍵もちょうど 12 動くので、鍵の区間ではなく音程の天井（mr_wrap.py の録音、2026-10-18）。
ある鍵 K から上で「鍵 n と n+12 がそのまま重なる」なら、K+12 から上を折り返しとして wrap_from 列に K+12 を書く。
mr_sweep.py が録りながら出す見立て（S か、それ以外か）より細かい。高い鍵で音程が頭打ちになる所を、境目と取り違えない。"""
import csv
import os
import sys

import numpy as np

ON = 0.40
N = 6000


def read24(path):
    d = open(path, "rb").read()
    p = d.find(b"data")
    raw = np.frombuffer(d, dtype=np.uint8, offset=p + 8)
    n = len(raw) // 6
    a = raw[:n * 6].reshape(n, 2, 3).astype(np.int32)
    v = a[:, :, 0] | (a[:, :, 1] << 8) | (a[:, :, 2] << 16)
    v = np.where(v & 0x800000, v - 0x1000000, v)
    return v[:, 0] / 8388608.0, v[:, 1] / 8388608.0


def corr(a2, b, n):
    best = 0.0
    for sh in range(-4, 5):
        u = a2[8 + sh:n - 8 + sh]
        v = b[8:n - 8]
        den = float(np.sqrt(np.dot(u, u) * np.dot(v, v)))
        if den > 0:
            best = max(best, float(np.dot(u, v)) / den)
    return best


W = 300
RATIOS = (2 ** (85 / 1024), 2 ** (86 / 1024))


def xc(u, v, maxsh):
    """v を u の上で 0〜2×maxsh ずらしたときの、いちばん大きい正規化した相関。"""
    n = len(v)
    vv = float(np.dot(v, v))
    L = 1
    while L < len(u) + n:
        L *= 2
    c = np.fft.irfft(np.fft.rfft(u, L) * np.conj(np.fft.rfft(v, L)), L)[:2 * maxsh + 1]
    cs = np.cumsum(np.concatenate([[0.0], u * u]))
    e = cs[n:n + 2 * maxsh + 1] - cs[:2 * maxsh + 1]
    return float(np.max(c / np.sqrt(np.maximum(e * vv, 1e-30))))


def periodicity(v):
    """自己相関のいちばん高い山（16 サンプルより先）。音程のある音は 0.6 を超え、雑音は低い。"""
    v = v - v.mean()
    L = 1
    while L < 2 * len(v):
        L *= 2
    F = np.fft.rfft(v, L)
    ac = np.fft.irfft(F * np.conj(F), L)[:len(v) // 2]
    return float(ac[16:].max() / ac[0]) if ac[0] > 0 else 0.0


def lowpass(x, fc):
    X = np.fft.rfft(x)
    X[np.fft.rfftfreq(len(x), 1 / 44100) > fc] = 0
    return np.fft.irfft(X, len(x))


def env(x, h=32):
    n = len(x) // h
    return np.log(np.sqrt((x[:n * h].reshape(n, h) ** 2).mean(1)) + 1e-5)


def classify(a, b, n, tail=False):
    # 高い鍵ほどサンプルを速く読み進むので、比べる長さを短くする（長いままだと、ループの回り方の違いで重ならなくなる）
    m = int(max(800, min(N, N * 2.0 ** (-(n - 72) / 12.0))))
    # 半音の比は 2^(1/12) ちょうどではない。チップの音程は 1 オクターブ = 1024 目盛りで、鍵ごとに丸めるので、
    # 隣の鍵との差は 85 か 86 目盛りになる。雑音の波形は、この 0.02〜0.05% の違いで重ならなくなる
    cs, ratio = max((corr(np.interp(np.arange(m) * r, np.arange(len(a)), a), b, m), r) for r in RATIOS)
    ce = corr(a[:m], b, m)
    # 音程のある音は、同じサンプルならふつう 0.99 を超える。ただ、よく似た隣のサンプル（ピアノ 9 の鍵 77/78）が 0.925、
    # 同じサンプルのはずの所（ピアノ 7 の鍵 29-31）が 0.91 と、0.9〜0.95 では見分けられない。境目には数えず、~ の印を付ける
    if cs >= 0.9 and cs >= ce:
        return "S" if cs >= 0.95 or periodicity(b[:m]) < 0.6 else "~"
    if ce >= 0.9:
        return "="
    # 音程のある音は、ここまでで重ならなければ境目。ずれを広く探すと、隣のサンプルの似た周期に乗ってしまう。
    # ただし前の音の余韻が残っていた鍵は、出だしの位置が当てにならないので、広く探す
    if periodicity(b[:m]) >= 0.6 and not tail:
        return "|"
    ar = np.concatenate([np.zeros(W), np.interp(np.arange(m + 2 * W) * ratio, np.arange(len(a)), a)])
    if xc(ar, b[:m], W) >= 0.9:
        return "s"
    # 元の鍵より上（読む速さ r > 1）では、高域が折り返されて鍵ごとに違う音になる。折り返しは 22050 × (2 − r) Hz より下へは落ちないので、
    # 低いほうの帯域だけで比べる。r は分からないので切る所を 4 つ試す。音程のある音には使わない（似た周期の隣のサンプルに乗る）
    if periodicity(b[:m]) < 0.6:
        for fc in (8000, 4000, 2000, 1000):
            al, bl = lowpass(a, fc), lowpass(b[:m + 2 * W], fc)
            for r in RATIOS:
                ar = np.concatenate([np.zeros(W), np.interp(np.arange(m + 2 * W) * r, np.arange(len(al)), al)])
                if xc(ar, bl[:m], W) >= 0.9:
                    return "l"
    ea = env(np.interp(np.arange(len(b)) * ratio, np.arange(len(a)), a))
    eb = env(b)
    k = min(len(ea), len(eb))
    ea, eb = ea[:k] - ea[:k].mean(), eb[:k] - eb[:k].mean()
    den = float(np.sqrt(np.dot(ea, ea) * np.dot(eb, eb)))
    if den > 0 and float(np.dot(ea, eb)) / den >= 0.97:
        return "?"
    return "|"


def analyse(base):
    L, R = read24(base + ".wav")
    x = L + R
    marks = [(int(a), float(b)) for a, b in (l.split() for l in open(base + ".txt"))]
    segs = {}
    tails = set()
    for n, t in marks:
        s = int((t - 0.03) * 44100)
        seg = x[s:s + int((ON + 0.05) * 44100)]
        hot = np.abs(seg) > 2e-4
        if hot.any():
            on = int(np.argmax(hot))
            segs[n] = seg[on:on + 12000]
            if on == 0:
                tails.add(n)
    out = []
    for n in range(0, 127):
        a, b = segs.get(n), segs.get(n + 1)
        if a is None or b is None or len(a) < 7000 or len(b) < N + 16:
            out.append(".")
            continue
        out.append(classify(a, b, n, n in tails or n + 1 in tails))
    # 音程の天井: 上から見て、鍵 n と n+12 が重なり続ける所（雑音の波形は出だしがぶれるので、ずれを広く探す）
    wrap = None
    for n in range(115, -1, -1):
        a, b = segs.get(n), segs.get(n + 12)
        if a is None or b is None or len(a) < 3600 or len(b) < 3000:
            continue
        if xc(np.concatenate([np.zeros(W), a[:3000 + 2 * W]]), b[:3000], W) < 0.97:
            break
        wrap = n + 12
    # 鍵 24 より下から重なるのは天井ではなく、音程を追わない並びのくり返し（下の octave）
    if wrap is not None and wrap <= 24:
        wrap = None
    top = 127 if wrap is None else wrap - 1
    # 1 オクターブごとのくり返し: 境目の所で、鍵 n と n+12 がそのまま重なるか
    hit = tot = 0
    for n in range(0, 127):
        if out[n] != "|" or n + 12 > top:
            continue
        a, b = segs.get(n), segs.get(n + 12)
        if a is None or b is None or len(a) < 3000 or len(b) < 3000:
            continue
        tot += 1
        hit += corr(a[:3000], b, 3000) >= 0.9
    octave = tot >= 6 and hit >= 0.8 * tot
    if octave:
        wrap = None
    if wrap is not None:
        out = out[:wrap - 1] + ["w"] * (127 - (wrap - 1))
    return "".join(out), sorted(segs), octave, wrap


def main():
    a = sys.argv[1:]
    out = None
    if "-o" in a:
        i = a.index("-o")
        out = a[i + 1]
        del a[i:i + 2]
    d = a[0]
    waves = [int(v) for v in a[1:]]
    if not waves:
        waves = sorted(int(f[1:5]) for f in os.listdir(d) if f.startswith("w") and f.endswith(".wav") and f[1:5].isdigit())
    rows = []
    for w in waves:
        s, keys, octave, wrap = analyse(os.path.join(d, "w%04d" % w))
        bounds = [i + 1 for i, c in enumerate(s) if c == "|"]
        first = keys[0] if keys else -1
        clamp = s.find("=")
        rows.append([w, first, len(bounds) + 1, clamp + 1 if clamp >= 0 else "", "o" if octave else "", wrap if wrap is not None else "", " ".join(map(str, bounds)), s])
        if not out:
            print("%4d first key %3d zones %3d %s %s" % (w, first, len(bounds) + 1, "o" if octave else " ", s))
    if out:
        with open(out, "w", newline="\n") as f:
            wr = csv.writer(f, lineterminator="\n")
            wr.writerow(["wave", "first_sounding_key", "zones", "first_equal_key", "octave", "wrap_from", "boundaries", "pattern"])
            wr.writerows(rows)
        print("%d 波形" % len(rows))


if __name__ == "__main__":
    main()
