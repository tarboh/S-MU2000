"""取ったドラムボイスのダンプ（capture/drum/20_nnn.syx など）から、芯（sampler_engine.h）で鳴らすパッチを書く（2026-10-25）。
  python mr_drum_patch.py [capture の置き場] [出力の置き場（既定は capture/patches_all）]
ドラムの鍵 1 つ（C0〜C6 の 73 鍵、Data List の DRUM VOICE KEY 47・48・49・4A ee）を、ふつうの音色のエレメント 1 つとして書く:
  鍵の範囲はその鍵だけ、音程と区間は押した鍵に依らず決まった鍵（dnote）で、EG はアタック → ディケイ 1（レベルまで）→ ディケイ 2（0 へ。127 はホールド）。
  ドラムだけの欄: noff（Receive Note Off）・alt（オルタネートグループ）・assign（0 single・1 multi）・rsend・csend（鍵ごとのリバーブ・コーラスの送り）。
コモン（46 xx）: 音量とピッチベンドの幅（46 00 00）、リバーブ・コーラス・インサーション 1・2（46 01〜04）、つなぎ方と Ins-EF の送り（46 00 40）、
コントロールセット（46 05 00）、アルペジオ（46 70 00 の 0x14〜。ノーマルボイスと同じ並び）。
実機で測ったこと（2026-10-25）: ドラムの鍵は雛形の鍵 60 と同じ音（mr_drum_root.py、相関 1.0000）、フィルターは LPF12（mr_drum_cal.py、7 組で差 0.37dB）、
レベルのベロシティ感度はカーブ 3 と同じ、ノートオフを受ける鍵は離すとすぐ消える（リリース 0）。
サンプルは mr_drum_capture.py が鍵 60 + コースチューンで録り直したもの（capture/samples_drum。mr_drum_extract.py）を先に使い、無ければふつうの下録りを使う。
録ったドラムのデータとサンプルはヤマハのものなので配らないこと。"""
import csv
import os
import sys

import mr_patch
import mr_voice

DNOTE = 60          # ドラムの鍵を鳴らす鍵（雛形のこの鍵と同じ音）
FTYPE = 4           # フィルターの型（LPF12）。ドラムの鍵には型の欄が無い
LCURVE = 3          # レベルのベロシティカーブ（ドラムの鍵には欄が無い。カーブ 3 と 20 点で 0.4dB 以内）
RELEASE = 0         # ノートオフを受ける鍵のリリースの時間（離すと 0.1 秒以内に消えた。CC72 を 0〜127 にしても変わらない）
ATTACK_ADD = 8      # ドラムの鍵のアタックは、ふつうのエレメントの値 + 8 と同じ時間（2 倍）。サイン波でアタック 48〜88 の 6 点（mr_drum_cc.py、2026-10-27）。
                    # ディケイ 1・2 はふつうと同じ目盛り
FGAIN = 252         # ドラムの鍵は、雛形から組み立てた音より 0.8〜1.0dB 大きい（4 キット 292 鍵で、キットごとの差 1.0・0.8・0.8・0.8dB。
                    # mr_drum_compare.py）。Filter Gain の 2 目盛り（+0.75dB）で合わせる
BANK_NAMES = {0x20: "DR-PRE", 0x21: "DR-GM", 0x28: "DR-USR"}


def common_lines(b):
    name = bytes(b.get((0x46, 0x70, 0x00), b"")[:10]).decode("ascii", "replace").rstrip()
    lines = ["# %s" % name]
    c = b.get((0x46, 0x00, 0x00), bytes(8))
    con = b.get((0x46, 0x00, 0x40), bytes(4))
    fx = {}
    rows = __import__("mr_params").COMMON_BLOCKS
    for key, blk, rk in (("reverb", (0x46, 0x01, 0x00), (0x40, 0x01, 0x00)), ("chorus", (0x46, 0x02, 0x00), (0x40, 0x02, 0x00)),
                         ("insa", (0x46, 0x03, 0x00), (0x40, 0x03, 0x00)), ("insb", (0x46, 0x04, 0x00), (0x40, 0x04, 0x00))):
        if blk not in b:
            continue
        d = __import__("mr_params").decode_rows(rows[rk], b[blk])
        vals = [d["type"]] + [d.get("param%d" % i, 0) for i in range(1, 17)]
        if key in ("reverb", "chorus"):
            vals += [d.get("return", 96), 127]           # 送りは鍵ごと（rsend・csend）なので、リバーブ・コーラスは送り 127 で作る
        fx[key] = ",".join(str(x) for x in vals)
    a = b.get((0x46, 0x70, 0x00), bytes(0x34))
    tempo = (a[0x28] << 7 | a[0x29]) if len(a) > 0x29 else 120
    fx["connect"] = con[0]
    fx["tempo"] = tempo
    fx["drum"] = 1
    fx["insrev"] = con[1]             # インサーションを通った音のリバーブ・コーラスへの送り（Ins-EF Reverb / Chorus Send）
    fx["inscho"] = con[2]
    lines.append("fx " + " ".join("%s=%s" % kv for kv in fx.items()))
    lines.append("voice volume=%d mono=1 same=1 porta=0 pmode=0 ptime=0 ptmode=0" % c[0])
    if len(a) >= 0x34:
        w = lambda i: a[i] << 7 | a[i + 1]
        ar = {"type": w(0x14), "sw": a[0x16], "hold": a[0x17], "unit": a[0x18], "note": "%d,%d" % (a[0x19], a[0x1A]),
              "vel": "%d,%d" % (a[0x1B], a[0x1C]), "key": a[0x1D], "vmode": a[0x1E], "change": a[0x1F], "qv": a[0x20], "qs": a[0x21],
              "vrate": w(0x22), "grate": w(0x24), "swing": w(0x26) - 128, "tempo": w(0x28),
              "sf": ",".join(str(w(i)) for i in range(0x2A, 0x34, 2))}
        lines.append("arp " + " ".join("%s=%s" % kv for kv in ar.items()))
    x = b.get((0x46, 0x05, 0x00))
    sets = ",".join("%d:%d:%d" % (x[5 * i], x[5 * i + 1] << 7 | x[5 * i + 2], x[5 * i + 3]) for i in range(6)) if x and len(x) >= 30 else "0:0:64"
    lines.append("ctrl sets=%s pbup=%d pbdown=%d" % (sets, c[2], c[3]))
    return lines


def key_element(b, kk):
    o, a, p, f, e = (b.get((blk, kk, sub)) for blk, sub in ((0x47, 0x00), (0x47, 0x20), (0x48, 0x00), (0x49, 0x00), (0x4A, 0x00)))
    if not o or not a or o[0] == 0 or o[1] != 0:
        return None, None
    wave = o[6] << 7 | o[7]
    hold = a[5] >= 127
    d1 = a[9]
    p = p or bytes([64, 64, 64])
    f = f or bytes([0, 1, 127, 64, 0, 0, 0, 0])
    e = e or bytes([5, 0, 0, 54, 64, 1, 103, 64])
    note = 24 + kk
    fl = {
        "wave": wave, "ins": o[0x11], "lbox": 0, "cbox": 63, "lratio": "0,127,127,127", "kdelay": 0, "ksync": 0, "klen": 11,
        "note_lo": note, "note_hi": note, "vel_lo": 1, "vel_hi": 127, "pan": o[0x0B], "span": 64, "apan": o[0x0D], "rpan": o[0x0C], "rpitch": 0,
        "level": a[0], "fgain": FGAIN, "release": RELEASE,
        "aegt": "%d,%d,%d,%d" % (min(127, a[3] + ATTACK_ADD), a[4], 0 if hold else a[5], RELEASE),
        "aegl": "0,127,%d,%d" % (d1, d1 if hold else 0),
        "aegtv": 64, "aegtvs": 0, "aegkf": 64, "aegkc": 60, "lvel": a[1], "lcurve": LCURVE, "lkf": 64,
        "lbp": "24,48,72,96", "loff": "128,128,128,128",
        "lfow": 1, "lfosync": 1, "lfodelay": 0, "lfospeed": 32, "lfoa": 0, "lfop": 0, "lfof": 0, "lfofade": 0,
        "coarse": p[0] - 64, "fine": p[1] - 64,
        "ftype": FTYPE, "cutoff": f[1] << 7 | f[2], "fvel": f[3], "reso": f[4], "rvel": 64, "hpf": f[6] << 7 | f[7], "distance": 0,
        "fkf": 64, "hkf": 64, "bp": "24,48,72,96", "off": "128,128,128,128",
        "fegt": "0,0,0,0,0", "fegl": "0,0,0,0,0", "fegd": 64, "fegtv": 64, "fegtvs": 0, "feglv": 64, "feglc": 0, "fegkf": 64, "fegkc": 60,
        "pegt": "0,0,0,0,0", "pegl": "0,0,0,0,0", "pegd": 64, "pegtv": 64, "pegtvs": 0, "peglv": 64, "peglc": 0, "pegkf": 64, "pegkc": 60,
        "pkf": 64, "pkc": 60, "pvel": p[2],
        "eqtype": e[0], "eqres": e[1], "eqf1": e[2] << 7 | e[3], "eqg1": e[4], "eqf2": e[5] << 7 | e[6], "eqg2": e[7],
        "dnote": DNOTE, "noff": o[3], "alt": o[5], "assign": o[4], "rsend": o[0x0E], "csend": o[0x0F],
    }
    return wave, fl


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    cap = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "..", "roms", "motif-rack-es", "capture")
    odir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(cap, "patches_all")
    sdir = os.path.join(cap, "samples")
    samples = list(csv.DictReader(open(os.path.join(sdir, "samples.csv"))))
    alias = {}
    ap = os.path.join(cap, "wave_alias.csv")
    if os.path.exists(ap):
        alias = {int(r["wave"]): int(r["same_as"]) for r in csv.DictReader(open(ap))}
    by_wave = {}
    for s in samples:
        by_wave.setdefault(int(s["wave"]), []).append(s)
    # 鍵 60 + コースチューンで録り直したもの（その鍵だけの区間）。あれば、その鍵ではこちらを使う
    ddir = os.path.join(cap, "samples_drum")
    exact = {}
    if os.path.exists(os.path.join(ddir, "samples.csv")):
        for s in csv.DictReader(open(os.path.join(ddir, "samples.csv"))):
            exact.setdefault(int(s["wave"]), []).append(s)
    n = 0
    for fn in sorted(os.listdir(os.path.join(cap, "drum"))):
        if not fn.endswith(".syx") or int(fn[:2], 16) not in BANK_NAMES:
            continue
        b = mr_voice.blocks(os.path.join(cap, "drum", fn))
        lines = common_lines(b)
        missing = []
        for kk in range(73):
            w, fl = key_element(b, kk)
            if fl is None:
                continue
            lines.append("element " + " ".join("%s=%s" % kv for kv in fl.items()))
            zs = by_wave.get(w, []) or by_wave.get(alias.get(w, -1), [])
            if not zs:
                missing.append(w)
            zs = sorted(zs, key=lambda s: (int(s["key_hi"]) - int(s["key_lo"]), int(s["vel_hi"]) - int(s["vel_lo"])))
            # 録り直したもののうち、この鍵が鳴らす鍵（60 + コース）のものを先に（芯は最初に合った区間を使う）
            k0 = max(0, min(127, DNOTE + fl["coarse"]))
            ex = [dict(s, _dir=ddir) for s in exact.get(w, []) if int(s["key"]) == k0]
            # 鳴らすのはこの鍵だけなので、その鍵を含む区間だけを書く（全部の区間を書くと、キット 1 つで 700〜850 区間・120MB を読み込んでいた）。
            # 録り直しがあればそれだけ、無ければ鍵を含む区間（ベロシティの層ごと）、それも無ければいちばん近い区間
            if not ex:
                inz = [s for s in zs if int(s["key_lo"]) <= k0 <= int(s["key_hi"])]
                if not inz and zs:
                    near = min(zs, key=lambda s: abs((int(s["key_lo"]) + int(s["key_hi"])) / 2 - k0))
                    inz = [s for s in zs if (s["key_lo"], s["key_hi"]) == (near["key_lo"], near["key_hi"])]
                zs = inz
            else:
                zs = []
            for s in ex + zs:
                name = s["file"].replace(".npz", ".smpx")
                sp, np_ = os.path.join(odir, name), os.path.join(s.get("_dir", sdir), s["file"])
                if not os.path.exists(sp) or os.path.getmtime(sp) < os.path.getmtime(np_):
                    mr_patch.pack(np_, sp, int(s["key"]))
                lines.append("zone key_lo=%s key_hi=%s vel_lo=%s vel_hi=%s file=%s" % (s["key_lo"], s["key_hi"], s["vel_lo"], s["vel_hi"], name))
        out = os.path.join(odir, fn[:-4] + ".patch")
        open(out, "w", newline="\n").write("\n".join(lines) + "\n")
        n += 1
        if missing:
            print("%s: サンプルの無い波形 %s" % (fn, missing))
    print("ドラム %d 個を %s に書いた" % (n, odir))


if __name__ == "__main__":
    main()
