"""mr_multi.py で録った 1 音以外の鳴らし方を、芯（sampler_render --events）と比べる（2026-10-25）。
  python mr_multi_compare.py --dir mr_multi の置き場 --render sampler_render.exe [--capture capture の置き場]
雛形と同じ正弦波（波形 704）のパッチをその場で作り、録ったときと同じ並びの音を鳴らす。
  モノ・同じ鍵・レガート: 時刻ごとの音量と音程（実機 / 芯）
  ポルタメント（p2_*）: 2 音目から 10%・50%・90% を通る時刻
  奪い合い（st4w_*・st4d_*）: 離す前に消えている鍵
録音を読むだけで、実機には何も送らない。"""
import os
import re
import subprocess
import sys

import numpy as np

import mr_patch
import mr_ref

FS = 44100
T0 = 0.3      # 実機の録音で、最初の音を鳴らした時刻


def write_patch(path, smpx, ne=1, level=127, attack=0, mono=1, same=0, porta=0, pmode=0, ptime=0, ptmode=0):
    el = ("element wave=704 ins=0 note_lo=0 note_hi=127 vel_lo=1 vel_hi=127 pan=64 span=64 apan=64 level=%d fgain=250 "
          "aegt=%d,127,127,0 aegl=%d,127,127,127 aegtv=64 aegtvs=4 aegkf=64 lvel=64 lkf=64 lbp=0,0,0,0 loff=128,128,128,128 "
          "ftype=21 cutoff=255 fvel=64 pkf=96 pkc=60 pvel=64 pegd=64 fegd=64 eqtype=5\n"
          "zone key_lo=0 key_hi=127 vel_lo=1 vel_hi=127 file=%s\n") % (level, attack, 0 if attack else 127, os.path.basename(smpx))
    with open(path, "w", newline="\n") as f:
        f.write("voice volume=127 mono=%d same=%d porta=%d pmode=%d ptime=%d ptmode=%d\n" % (mono, same, porta, pmode, ptime, ptmode))
        f.write(el * ne)


def events(evs):
    return ",".join(("on@%.4f:%d:100" % (t - T0, k)) if on else ("off@%.4f:%d" % (t - T0, k)) for t, on, k in evs)


def render(exe, patch, evs, out):
    subprocess.run([exe, patch, "60", "100", "1", out, "--events", events(evs)], check=True, stdout=subprocess.DEVNULL)


def track(path, hop=441, win=2048, t_shift=0.0):
    L, R = mr_ref.read_wav(path)
    x = np.asarray(L, float) + np.asarray(R, float)
    if t_shift:
        x = np.concatenate([np.zeros(int(t_shift * FS)), x])
    fr = np.fft.rfftfreq(1 << 15, 1 / FS)
    out = []
    for i in range(0, len(x) - win, hop):
        s = x[i:i + win]
        sp = np.abs(np.fft.rfft(s * np.hanning(win), 1 << 15))
        k = int(np.argmax(sp[:int(3000 / fr[1])]))
        out.append((i / FS, 10 * np.log10(np.mean(s ** 2) + 1e-20), 1200 * np.log2(max(fr[k], 1) / 261.63)))
    return np.array(out)


def onset(path):
    L, R = mr_ref.read_wav(path)
    x = np.asarray(L, float) + np.asarray(R, float)
    return int(np.argmax(np.abs(x) > 1e-4)) / FS


def at(tr, t):
    j = int(np.argmin(np.abs(tr[:, 0] - t)))
    return tr[j, 1], (tr[j, 2] if tr[j, 1] > -70 else 0)


def glide(tr, iv, t1):
    m = (tr[:, 0] > t1 - 0.05) & (tr[:, 1] > -60)
    tt, c = tr[m, 0], tr[m, 2]
    res = []
    for q in (0.1, 0.5, 0.9):
        th = -4 + 100 * iv * q
        k = int(np.argmax(c > th))
        res.append(tt[k] - t1 if c[k] > th else np.nan)
    return res


def gone(path, keys, t, rel=-15):
    L, R = mr_ref.read_wav(path)
    x = np.asarray(L, float) + np.asarray(R, float)
    win = 16384
    s = x[int(t * FS):int(t * FS) + win] * np.hanning(win)
    a = np.array([abs(np.sum(s * np.exp(-2j * np.pi * 440 * 2 ** ((k - 69) / 12) * np.arange(win) / FS))) for k in keys])
    lv = 20 * np.log10(a / np.median(a) + 1e-9)
    return [k for k, v in zip(keys, lv) if v < rel]


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    exe = mr_ref.opt(a, "--render", "sampler_render.exe")
    cap = mr_ref.opt(a, "--capture", "../../roms/motif-rack-es/capture")
    pdir = os.path.join(d, "patches")
    os.makedirs(pdir, exist_ok=True)
    smpx = os.path.join(pdir, "C_w0704_k063_v064.smpx")
    if not os.path.exists(smpx):
        mr_patch.pack(os.path.join(cap, "samples", "C_w0704_k063_v064.npz"), smpx, 63)
    patch = os.path.join(pdir, "multi.patch")
    sm = os.path.join(pdir, "sm.wav")

    def cmp_tracks(name, evs, ts, **kw):
        hw = os.path.join(d, name + ".wav")
        if not os.path.exists(hw):
            return
        write_patch(patch, smpx, **kw)
        render(exe, patch, evs, sm)
        # 芯は最初の音が 0 秒。実機は録音の始めから T0 のあとに頭が来る（遅れは onset で合わせる）
        th, ts_ = track(hw), track(sm, t_shift=onset(hw))
        row = []
        for t in ts:
            (lh, ch), (ls, cs) = at(th, t), at(ts_, t)
            row.append("%.2f:%.0f/%.0fdB %+.0f/%+.0fc" % (t, lh, ls, ch, cs))
        print("%-12s %s" % (name, " | ".join(row)))

    print("モノ・同じ鍵・レガート（時刻: 実機/芯の音量、実機/芯の音程）")
    on, off = (lambda t, k: (t, True, k)), (lambda t, k: (t, False, k))
    mono_ev = [on(0.3, 60), on(1.0, 67), off(1.7, 67), off(2.4, 60)]
    ts = (0.45, 0.9, 1.03, 1.1, 1.3, 1.73, 1.8, 2.0, 2.3, 2.5)
    for mono in (0, 1):
        cmp_tracks("mn_m%d" % mono, mono_ev, ts, attack=80, mono=mono)
    for same in (0, 1):
        cmp_tracks("sn_s%d" % same, [on(0.3, 60), on(1.0, 60), off(1.7, 60), off(1.9, 60)], (0.45, 0.9, 1.03, 1.1, 1.3, 1.75, 1.95), attack=80, same=same)
    for sw, mode in ((0, 0), (1, 0), (1, 1)):
        cmp_tracks("lg_sw%d_md%d" % (sw, mode), mono_ev, (0.9, 1.01, 1.03, 1.05, 1.2, 1.71, 1.73, 1.8, 2.0), attack=80, mono=0, porta=sw, pmode=mode, ptime=48)

    print("ポルタメント（時間のモード 値 幅: 実機 10%・50%・90% / 芯）")
    errs = []
    for f in sorted(os.listdir(d)):
        m = re.match(r"p2_tm(\d)_t(\d+)_i(\d+)\.wav", f)
        if not m:
            continue
        tm, pt, iv = map(int, m.groups())
        write_patch(patch, smpx, mono=0, porta=1, pmode=1, ptime=pt, ptmode=tm)
        render(exe, patch, [on(0.3, 60), off(0.9, 60), on(1.0, 60 + iv), off(3.8, 60 + iv)], sm)
        o = onset(os.path.join(d, f))
        gh = glide(track(os.path.join(d, f), hop=110, win=1024), iv, o + 0.7)
        gs = glide(track(sm, hop=110, win=1024), iv, 0.7)
        e = [abs(x - y) for x, y in zip(gh, gs) if not (np.isnan(x) or np.isnan(y))]
        errs += e
        print("%d %3d %2d: %s / %s" % (tm, pt, iv, " ".join("%.3f" % v for v in gh), " ".join("%.3f" % v for v in gs)))
    if errs:
        print("ポルタメントの時刻のずれの中央値 %.3f 秒（%d 点）" % (np.median(errs), len(errs)))

    print("奪い合い（離す前に消えている鍵: 実機 / 芯）")
    for f in sorted(os.listdir(d)):
        m = re.match(r"st4(w|d)_n(\d+)\.wav", f)
        if not m:
            continue
        kind, n = m.group(1), int(m.group(2))
        keys = [36 + 2 * i for i in range(n)] if kind == "w" else [114 - 2 * i for i in range(n)]
        evs = [on(0.3 + 0.05 * i, k) for i, k in enumerate(keys)]
        tend = 0.3 + 0.05 * n + 1.2
        evs += [off(tend, k) for k in keys]
        write_patch(patch, smpx, ne=4, level=20)
        render(exe, patch, evs, sm)
        o = onset(os.path.join(d, f))
        gh = gone(os.path.join(d, f), keys, o + 0.05 * n + 0.6)
        gs = gone(sm, keys, 0.05 * n + 0.6)
        print("%s %2d 鍵: %s / %s" % (kind, n, gh, gs))


if __name__ == "__main__":
    main()
