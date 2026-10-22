"""歪み系のインサーション（Distortion 09 00・Comp Distortion 09 01・V-Distortion 09 10・Overdrive 09 20・Amp Simulator 2 09 31）を、
プリセットが使うパラメーターの組のまま録り、組ごとに Wiener 型（線形の応答 → 頭打ち → LPF、mr_fx_dist.py）を当てはめる。
  python mr_fx_dist_sets.py list | record [--only 名前,…] | fit
録り方: 組の 16 個のパラメーターを入れ、Dry/Wet だけ 127 にして（混ぜ方は芯で掛ける）、
mr_fx.py --level 16（小さいノイズ、線形の応答）と mr_fx_comp.py --key 48（正弦波 8 段）。V-Distortion は Delay Mix（11）を 0 にして歪みの部分だけ録る。
LPF の欄: Distortion・Comp Distortion・Overdrive は 04、Amp Simulator 2 は 03（表 3）。V-Distortion は LPF の欄が無いので 20kHz とする。
結果は capture/fxds/<名前>_dist.npz と、C++ 用の <名前>.dsx（mr_fx_export.py と同じ形）。送るのはパラメーターチェンジ・ダンプの要求・ノートだけ（保存しない）。"""
import glob
import json
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CAP = os.path.join(HERE, "../../roms/motif-rack-es/capture")
OUT = os.path.join(CAP, "fxds")
TYPES = {(9, 0x00): 4, (9, 0x01): 4, (9, 0x20): 4, (9, 0x31): 3, (9, 0x10): None, (9, 0x30): 3}     # 型 → LPF の欄（Amp Simulator 09 30 も録るが、芯は式の方を使う）
T3 = [20, 22, 25, 28, 32, 36, 40, 45, 50, 56, 63, 70, 80, 90, 100, 110, 125, 140, 160, 180, 200, 225, 250, 280, 315, 355, 400, 450,
      500, 560, 630, 700, 800, 900, 1000, 1100, 1200, 1400, 1600, 1800, 2000, 2200, 2500, 2800, 3200, 3600, 4000, 4500, 5000, 5600,
      6300, 7000, 8000, 9000, 10000, 11000, 12000, 14000, 16000, 18000, 20000]


def key_of(ins):
    t = (ins["type"] >> 7, ins["type"] & 0x7F)
    ps = [ins["param%d" % i] for i in range(1, 17)]
    ps[9] = 127                                   # Dry/Wet は芯で掛ける
    if t == (9, 0x10):
        ps[10] = 0                                # V-Distortion の Delay Mix
    return "t%d_%d_%s" % (t[0], t[1], "_".join(str(v) for v in ps))


def needed():
    sets = {}
    for p in sorted(glob.glob(os.path.join(CAP, "voice_json", "*.json"))):
        v = os.path.basename(p)[:-5]
        js = json.load(open(p, encoding="utf-8"))
        for blk in ("insertion1", "insertion2"):
            ins = js[blk]
            t = (ins["type"] >> 7, ins["type"] & 0x7F)
            if t in TYPES and ins["param10"] > 1:
                sets.setdefault(key_of(ins), (ins, []))[1].append(v)
    return sets


def params_of(k):
    parts = k.split("_")
    t = (int(parts[0][1:]), int(parts[1]))
    return t, [int(x) for x in parts[2:18]]


def small_levels():
    """組ごとに小さいノイズを録り直す音量（fxds/_small_level.txt「名前 音量」）。音量 16 では、Drive の小さい組は出口が床（−87dB）に埋もれて
    h の利きが正しく読めず（Full/CVib の組で 130Hz が 7dB 小さい）、Drive の大きい組はもう歪んでいる。当てはめの「上げ幅の直し」が +3dB より大きい組は 64、
    −3dB より小さい組は 4 で録り直す"""
    p = os.path.join(OUT, "_small_level.txt")
    if not os.path.exists(p):
        return {}
    return dict((ln.split()[0], int(ln.split()[1])) for ln in open(p) if ln.strip())


def record(only):
    os.makedirs(OUT, exist_ok=True)
    for k, (ins, voices) in needed().items():
        if only and k not in only:
            continue
        t, ps = params_of(k)
        st = ",".join("p%d=%d" % (i + 1, v) for i, v in enumerate(ps))
        small = os.path.join(OUT, "insa_%s_wet.wav" % k)
        if not os.path.exists(small):
            subprocess.run([sys.executable, os.path.join(HERE, "mr_fx.py"), "--dir", OUT, "--block", "insa", "--types", "%d:%d" % t,
                            "--set", st, "--level", "16", "--tail", "1", "--tag", "x"], check=True, cwd=HERE, stdout=subprocess.DEVNULL)
            for suf in ("_dry.wav", "_wet.wav", ".json"):
                os.replace(os.path.join(OUT, "insa_t%d_%d_x%s" % (t[0], t[1], suf)), os.path.join(OUT, "insa_%s%s" % (k, suf)))
        lv = small_levels().get(k)
        if lv and not os.path.exists(os.path.join(OUT, "insa_%s_s%d_wet.wav" % (k, lv))):
            subprocess.run([sys.executable, os.path.join(HERE, "mr_fx.py"), "--dir", OUT, "--block", "insa", "--types", "%d:%d" % t,
                            "--set", st, "--level", str(lv), "--tail", "1", "--tag", "x"], check=True, cwd=HERE, stdout=subprocess.DEVNULL)
            for suf in ("_dry.wav", "_wet.wav", ".json"):
                os.replace(os.path.join(OUT, "insa_t%d_%d_x%s" % (t[0], t[1], suf)), os.path.join(OUT, "insa_%s_s%d%s" % (k, lv, suf)))
        if not os.path.exists(os.path.join(OUT, "cs%s_L016_wet.wav" % k)):
            subprocess.run([sys.executable, os.path.join(HERE, "mr_fx_comp.py"), "--dir", OUT, "--type", "%d:%d" % t, "--key", "48",
                            "--set", st, "--tag", k], check=True, cwd=HERE, stdout=subprocess.DEVNULL)
        print(k, "録った（%s）" % ",".join(voices), flush=True)


def fit():
    for k, (ins, voices) in needed().items():
        lv = small_levels().get(k)
        if lv and not os.path.exists(os.path.join(OUT, "insa_%s_s%d_wet.wav" % (k, lv))):
            subprocess.run([sys.executable, os.path.join(HERE, "mr_fx.py"), "--dir", OUT, "--block", "insa", "--types", "%d:%d" % t,
                            "--set", st, "--level", str(lv), "--tail", "1", "--tag", "x"], check=True, cwd=HERE, stdout=subprocess.DEVNULL)
            for suf in ("_dry.wav", "_wet.wav", ".json"):
                os.replace(os.path.join(OUT, "insa_t%d_%d_x%s" % (t[0], t[1], suf)), os.path.join(OUT, "insa_%s_s%d%s" % (k, lv, suf)))
        if not os.path.exists(os.path.join(OUT, "cs%s_L016_wet.wav" % k)):
            continue
        t, ps = params_of(k)
        col = TYPES[t]
        fc = T3[min(60, ps[col - 1])] if col else 20000
        lv = small_levels().get(k)
        small = os.path.join(OUT, "insa_%s_s%d" % (k, lv)) if lv and os.path.exists(os.path.join(OUT, "insa_%s_s%d_wet.wav" % (k, lv))) else os.path.join(OUT, "insa_" + k)
        ref = 64 if lv == 64 and small.endswith("_s64") else 16
        r = subprocess.run([sys.executable, os.path.join(HERE, "mr_fx_dist.py"), small, OUT, k, str(fc), str(ref)],
                           cwd=HERE, capture_output=True, text=True, encoding="utf-8", env=dict(os.environ, PYTHONIOENCODING="utf-8"))
        line = (r.stdout.strip().split("\n") or [""])[0]
        print("%s（%s）: %s" % (k, ",".join(voices), line), flush=True)
        if small != os.path.join(OUT, "insa_" + k) and os.path.exists(small + "_dist.npz"):
            os.replace(small + "_dist.npz", os.path.join(OUT, "insa_%s_dist.npz" % k))
        z = os.path.join(OUT, "insa_%s_dist.npz" % k)
        if os.path.exists(z):
            d = np.load(z)
            hL, hR = d["hL"].astype(np.float32), d["hR"].astype(np.float32)
            head = "DSX frames=%d c=%.6f p=%.3f fc=%.1f drive=0\n" % (len(hL), float(d["c"]), float(d["p"]), float(d["fc"]))
            with open(os.path.join(OUT, k + ".dsx"), "wb") as f:
                f.write(head.encode("ascii"))
                f.write(np.stack([hL, hR], axis=1).astype("<f4").tobytes())


def main():
    a = sys.argv[1:]
    cmd = a[0] if a else "list"
    if cmd == "list":
        s = needed()
        for k, (ins, voices) in s.items():
            print(k, ",".join(voices))
        print("%d 組" % len(s))
    elif cmd == "record":
        record(set(a[a.index("--only") + 1].split(",")) if "--only" in a else None)
    elif cmd == "fit":
        fit()


if __name__ == "__main__":
    main()
