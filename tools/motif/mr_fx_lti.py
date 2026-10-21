"""時間で変わらないインサーション（ディレイ類・Echo・ER・ゲート・Karaoke・インサーションのリバーブ・Ambience）を、
プリセットが使うパラメーターの組のまま実機で応答を録り、2×2（左右の入力 → 左右の出力）の畳み込みで再現する。
  python mr_fx_lti.py list                          使う組を出す
  python mr_fx_lti.py record [--only 名前,…]        録る（パラメーターチェンジ・ダンプの要求・ノートだけ。保存しない。終わったらマルチへ）
  python mr_fx_lti.py analyze                       応答を出して capture/fxlti/<名前>.lti に書く

録り方: mr_fx.py --block insa で、組の 16 個のパラメーター（と音色のテンポ）を入れ、エレメントのパンを 1（左だけ）・127（右だけ）にして
dry（スイッチを切る）と wet（入れる）を 1 本ずつ。Dry/Wet が 127 の組は 126 で録る（元の音の線が無いと時刻をそろえられない。
その線は読んだあと差し引く）。応答は wet ÷ dry（左だけの入力なら dry の左）で、元の音の線（遅れ 0 の付近の最大）を 0 にそろえる。
.lti の形: 1 行目「LTI frames=N」、そのあと float32 の LL・LR・RL・RR（入力 → 出力）を N 組。録った音とそこから作ったものは配らないこと。"""
import glob
import hashlib
import json
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CAP = os.path.join(HERE, "../../roms/motif-rack-es/capture")
OUT = os.path.join(CAP, "fxlti")
FS = 44100
LTI_TYPES = {(2, 0x00), (2, 0x10), (2, 0x18), (2, 0x20), (2, 0x40), (2, 0x48), (2, 0x50), (2, 0x58),
             (1, 0x00), (1, 0x01), (1, 0x10), (1, 0x11), (1, 0x12), (1, 0x20), (1, 0x21), (1, 0x30), (1, 0x40), (1, 0x41), (1, 0x42), (1, 0x43),
             (1, 0x02), (1, 0x13), (1, 0x31), (1, 0x08), (1, 0x09), (1, 0x18), (1, 0x19), (1, 0x38),
             (3, 0x00), (3, 0x01), (3, 0x10), (3, 0x20), (4, 0x00), (4, 0x01), (4, 0x02), (15, 0x30), (14, 0x40)}
NO_DW = {(14, 0x40)}        # Dry/Wet の欄が無い型（Isolator）
TEMPO_TYPES = {(2, 0x50), (2, 0x58)}


def key_of(ins, tempo):
    t = (ins["type"] >> 7, ins["type"] & 0x7F)
    ps = [ins["param%d" % i] for i in range(1, 17)]
    return "t%d_%d_%s_%d" % (t[0], t[1], "_".join(str(v) for v in ps), tempo if t in TEMPO_TYPES else 0)


def voice_tempo(v):
    sys.path.insert(0, HERE)
    import mr_patch
    return mr_patch.voice_tempo(os.path.join(CAP, "voice_json", v + ".json"))


def needed():
    """プリセットで、スイッチが通すブロックに入っている時間で変わらない型の組"""
    sets = {}
    for p in sorted(glob.glob(os.path.join(CAP, "voice_json", "*.json"))):
        v = os.path.basename(p)[:-5]
        js = json.load(open(p, encoding="utf-8"))
        sw = [e["oscillator"]["insertion_effect_switch"] for e in js["elements"] if e["oscillator"]["element_switch"]]
        con = js.get("insertion_connect", {}).get("insertion_connect_type", 0)
        used = set()
        for s in sw:
            if s == 1:
                used |= {"A"} | ({"B"} if con == 1 else set())
            if s == 2:
                used |= {"B"} | ({"A"} if con == 2 else set())
        for blk in used:
            ins = js["insertion1" if blk == "A" else "insertion2"]
            t = (ins["type"] >> 7, ins["type"] & 0x7F)
            if t in LTI_TYPES and (t in NO_DW or ins["param10"] > 1):      # Dry/Wet 1 は素通し
                tempo = voice_tempo(v) if t in TEMPO_TYPES else 0
                k = key_of(ins, tempo)
                sets.setdefault(k, (ins, tempo, []))[2].append(v)
    return sets


def tail_of(t):
    return 6.0 if t[0] in (1, 15) or t == (3, 0x00) or t == (3, 0x01) else 4.0


def record(only):
    os.makedirs(OUT, exist_ok=True)
    for k, (ins, tempo, voices) in needed().items():
        if only and k not in only:
            continue
        t = (ins["type"] >> 7, ins["type"] & 0x7F)
        ps = dict(("p%d" % i, ins["param%d" % i]) for i in range(1, 17))
        if ps["p10"] == 127 and t not in NO_DW:
            ps["p10"] = 126
        st = ",".join("%s=%d" % kv for kv in ps.items())
        for pan, side in ((1, "L"), (127, "R")):
            if os.path.exists(os.path.join(OUT, "insa_%s_%s_wet.wav" % (k, side))):
                continue
            cmd = [sys.executable, os.path.join(HERE, "mr_fx.py"), "--dir", OUT, "--block", "insa", "--types", "%d:%d" % t,
                   "--set", st, "--pan", str(pan), "--tail", str(tail_of(t)), "--tag", "x", "--flush"]
            if tempo:
                cmd += ["--tempo", str(tempo)]
            subprocess.run(cmd, check=True, cwd=HERE, stdout=subprocess.DEVNULL)
            for suf in ("_dry.wav", "_wet.wav", ".json"):
                src = os.path.join(OUT, "insa_t%d_%d_x%s" % (t[0], t[1], suf))
                os.replace(src, os.path.join(OUT, "insa_%s_%s%s" % (k, side, suf.replace(".json", ".json"))))
        print(k, "録った（%s）" % ",".join(voices), flush=True)


def load(path):
    sys.path.insert(0, HERE)
    import mr_ref
    L, R = mr_ref.read_wav(path)
    return np.asarray(L, dtype=np.float64), np.asarray(R, dtype=np.float64)


def ir_pair(base, side, n):
    """左だけ（side L）か右だけ（R）の入力から、左右の出力への応答 2 本"""
    dL, dR = load(base + "_%s_dry.wav" % side)
    wL, wR = load(base + "_%s_wet.wav" % side)
    d = dL if side == "L" else dR
    od = int(np.argmax(np.abs(d) > 1e-4))
    x = d[max(0, od - 200):]
    N = 1 << int(np.ceil(np.log2(len(x) + n + 4096)))
    X = np.fft.rfft(x, N)
    P = np.abs(X) ** 2
    hs = []
    for w in (wL, wR):
        y = w[max(0, od - 200):]
        H = np.fft.rfft(y, N) * np.conj(X) / (P + 1e-6 * P.max())
        hs.append(np.fft.irfft(H, N))
    # 元の音の線: 入力と同じ側の出力の、遅れ ±8ms の中の最大（録音の頭の揺れ。Dry/Wet 126 でも線は残る）
    h0 = hs[0] if side == "L" else hs[1]
    win = int(0.008 * FS)
    cand = np.concatenate([h0[-win:], h0[:win]])
    lag = int(np.argmax(np.abs(cand))) - win
    hs = [np.roll(h, -lag)[:n] for h in hs]
    return hs, lag


def analyze():
    os.makedirs(OUT, exist_ok=True)
    for k, (ins, tempo, voices) in needed().items():
        base = os.path.join(OUT, "insa_" + k)
        if not os.path.exists(base + "_R_wet.wav"):
            continue
        t = (ins["type"] >> 7, ins["type"] & 0x7F)
        n = int(tail_of(t) * FS)
        (LL, LR), lagL = ir_pair(base, "L", n)
        (RL, RR), lagR = ir_pair(base, "R", n)
        if ins["param10"] == 127 and t not in NO_DW:             # 126 で録った元の音の線を差し引く
            for h in (LL, RR):
                h[0] = 0.0
        with open(os.path.join(OUT, k + ".lti"), "wb") as f:
            f.write(("LTI frames=%d\n" % n).encode("ascii"))
            f.write(np.stack([LL, LR, RL, RR], axis=1).astype("<f4").tobytes())
        e = lambda h: 10 * np.log10(np.sum(h ** 2) + 1e-20)
        print("%s（%s）: 頭のずれ %d / %d・元の音 %.3f / %.3f・エネルギー LL %.1f LR %.1f RL %.1f RR %.1f dB" % (
            k, ",".join(voices), lagL, lagR, LL[0], RR[0], e(LL), e(LR), e(RL), e(RR)))


def apply(L, R, ins, tempo, cache={}):
    """組の .lti があれば 2×2 で畳み込む。Dry/Wet 1 は素通し。無ければ None"""
    t = (ins["type"] >> 7, ins["type"] & 0x7F)
    if t not in NO_DW and ins.get("param10", 64) <= 1:
        return np.asarray(L, dtype=np.float64), np.asarray(R, dtype=np.float64)
    k = key_of(ins, tempo)
    p = os.path.join(OUT, k + ".lti")
    if not os.path.exists(p):
        return None
    if k not in cache:
        with open(p, "rb") as f:
            f.readline()
            cache[k] = np.frombuffer(f.read(), dtype="<f4").reshape(-1, 4).astype(np.float64)
    h = cache[k]
    L, R = np.asarray(L, dtype=np.float64), np.asarray(R, dtype=np.float64)
    n = len(L)
    N = 1 << int(np.ceil(np.log2(n + len(h))))
    FL, FR = np.fft.rfft(L, N), np.fft.rfft(R, N)
    Hs = [np.fft.rfft(h[:, i], N) for i in range(4)]
    oL = np.fft.irfft(FL * Hs[0] + FR * Hs[2], N)[:n]
    oR = np.fft.irfft(FL * Hs[1] + FR * Hs[3], N)[:n]
    return oL, oR


def main():
    a = sys.argv[1:]
    cmd = a[0] if a else "list"
    if cmd == "list":
        s = needed()
        for k, (ins, tempo, voices) in s.items():
            print(k, ",".join(voices))
        print("%d 組" % len(s))
    elif cmd == "record":
        only = None
        if "--only" in a:
            only = set(a[a.index("--only") + 1].split(","))
        record(only)
    elif cmd == "analyze":
        analyze()


if __name__ == "__main__":
    main()
