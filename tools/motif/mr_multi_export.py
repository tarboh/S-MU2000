"""取ったマルチのダンプ（capture/multi/41_nnn.syx・multi_edit/5F_000.syx）を、motif_play が読む .multi の文字の形にする（2026-10-25）。
  python mr_multi_export.py [capture の置き場] [出力の置き場（既定は capture/multis）]
1 マルチ 1 ファイル。読むのは Data List の MULTI COMMON（36 00 00）・リバーブ（36 01 00）・コーラス（36 02 00）・MULTI PART（37 pp 00、pp 0〜15）。
  # 名前
  arp type=… sw=… …               マルチのアルペジオ（36 00 00 の 0x14〜0x33。音色の 40 70 00 と同じ並び）
  fx reverb=… chorus=…            パッチの fx 行と同じ形（型,パラメーター 1〜16,Return,送り）。送りはパートごとなので 127 にする
  ins parts=0,3,…                 インサーションを使うパート（36 00 60〜63 のビット。8 つまで）
  part n voice=PRE1:000 bank=63,0 prog=0 ch=0 … パートごとの設定（下の PART の並び）
マスター EQ（36 10 00）とマスターエフェクト（36 11 00）は、まだ芯に無いので書かない。マルチのデータはヤマハのものなので配らないこと。"""
import os
import sys

import mr_params
import mr_patch
import mr_voice

# 37 pp 00 の並び（Data List の MULTI PART）
PART = [(0x00, "bank_msb"), (0x01, "bank_lsb"), (0x02, "prog"), (0x03, "ch"), (0x04, "mode"), (0x05, "vel_lo"), (0x06, "vel_hi"),
        (0x07, "note_lo"), (0x08, "note_hi"), (0x09, "pbup"), (0x0A, "pbdown"), (0x0B, "vsd"), (0x0C, "vso"), (0x0D, "vol"), (0x0E, "pan"),
        (0x12, "rev"), (0x13, "cho"), (0x15, "dry"), (0x16, "shift"), (0x17, "cutoff"), (0x18, "reso"), (0x1A, "porta"), (0x1B, "ptime"),
        (0x1C, "pmode"), (0x1E, "out"), (0x1F, "rx1"), (0x20, "rx2"), (0x21, "rx3"), (0x22, "aeg_sus"), (0x23, "aeg_atk"), (0x24, "voicepan"),
        (0x25, "aeg_dec"), (0x26, "aeg_rel"), (0x27, "feg_sus"), (0x28, "feg_atk"), (0x2A, "feg_dec"), (0x2B, "feg_rel"), (0x2C, "feg_depth")]

# バンクの MSB・LSB → ダンプの番地の mm（パッチのファイル名の頭）と、一覧の名前。ドラムのパッチは mr_drum_patch.py が書く
BANKS = {(63, 0): ("00", "PRE1"), (63, 1): ("01", "PRE2"), (63, 2): ("02", "PRE3"), (63, 3): ("03", "PRE4"), (63, 4): ("04", "PRE5"),
         (63, 5): ("05", "PRE6"), (0, 0): ("06", "GM"), (63, 8): ("08", "USR1"), (63, 9): ("09", "USR2"), (63, 10): ("0A", "USR3"),
         (63, 32): ("20", "PREDR"), (63, 40): ("28", "USRDR"), (127, 0): ("21", "GMDR")}


def fx_line(b, key, rows):
    d = mr_params.decode_rows(rows, b[key])
    vals = [d["type"]] + [d.get("param%d" % i, 0) for i in range(1, 17)] + [d.get("return", 96), 127]
    return ",".join(str(x) for x in vals)


def export(syx, out):
    b = mr_voice.blocks(syx)
    com = b.get((0x36, 0x00, 0x00))
    if not com or len(com) < 0x64:
        return None
    lines = ["# " + bytes(com[:10]).decode("ascii", "replace").rstrip()]
    w = lambda i: com[i] << 7 | com[i + 1]
    ar = {"type": w(0x14), "sw": com[0x16], "hold": com[0x17], "unit": com[0x18], "note": "%d,%d" % (com[0x19], com[0x1A]),
          "vel": "%d,%d" % (com[0x1B], com[0x1C]), "key": com[0x1D], "vmode": com[0x1E], "change": com[0x1F], "qv": com[0x20],
          "qs": com[0x21], "vrate": w(0x22), "grate": w(0x24), "swing": w(0x26) - 128, "tempo": w(0x28),
          "sf": ",".join(str(w(i)) for i in range(0x2A, 0x34, 2))}
    lines.append("arp " + " ".join("%s=%s" % kv for kv in ar.items()))
    fx = []
    if (0x36, 0x01, 0x00) in b:
        fx.append("reverb=" + fx_line(b, (0x36, 0x01, 0x00), mr_params.COMMON_BLOCKS[(0x40, 0x01, 0x00)]))
    if (0x36, 0x02, 0x00) in b:
        fx.append("chorus=" + fx_line(b, (0x36, 0x02, 0x00), mr_params.COMMON_BLOCKS[(0x40, 0x02, 0x00)]))
    fx.append("tempo=%d" % ar["tempo"])
    lines.append("fx " + " ".join(fx))
    # インサーションのパート: 0x60 は予約、0x61 がパート 15・16、0x62 が 8〜14、0x63 が 1〜7（ビット 0 が若い番号）
    ins = []
    for byte, first, count in ((0x63, 0, 7), (0x62, 7, 7), (0x61, 14, 2)):
        for k in range(count):
            if com[byte] >> k & 1:
                ins.append(first + k)
    lines.append("ins parts=%s" % ",".join(str(p) for p in sorted(ins)))
    for p in range(16):
        x = b.get((0x37, p, 0x00))
        if not x or len(x) < 0x2D:
            continue
        f = {name: x[off] for off, name in PART}
        mm, bank = BANKS.get((f["bank_msb"], f["bank_lsb"]), (None, "%d/%d" % (f["bank_msb"], f["bank_lsb"])))
        voice = "%s_%03d" % (mm, f["prog"]) if mm else "-"
        arpsw = x[0x1F] >> 1 & 1
        # パートの EQ（37 pp 2E〜38）: 低域の周波数（2 バイト）・利き、中域の周波数（2 バイト）・利き・幅、高域の周波数（2 バイト）・利き
        if len(x) > 0x38:
            f.update(eqlf=x[0x2E] << 7 | x[0x2F], eqlg=x[0x30], eqmf=x[0x32] << 7 | x[0x33], eqmg=x[0x34], eqmq=x[0x35],
                     eqhf=x[0x36] << 7 | x[0x37], eqhg=x[0x38])
        lines.append("part %d voice=%s bank=%s:%03d arpsw=%d " % (p, voice, bank, f["prog"] + 1, arpsw) + " ".join("%s=%d" % kv for kv in f.items()))
    with open(out, "w", encoding="utf-8", newline="\n") as fo:
        fo.write("\n".join(lines) + "\n")
    return lines[0][2:]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    cap = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "..", "roms", "motif-rack-es", "capture")
    outd = sys.argv[2] if len(sys.argv) > 2 else os.path.join(cap, "multis")
    os.makedirs(outd, exist_ok=True)
    n = 0
    for sub in ("multi", "multi_edit"):
        d = os.path.join(cap, sub)
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            if f.endswith(".syx") and export(os.path.join(d, f), os.path.join(outd, f[:-4] + ".multi")):
                n += 1
    print("マルチ %d 個を %s に書いた" % (n, outd))


if __name__ == "__main__":
    main()
