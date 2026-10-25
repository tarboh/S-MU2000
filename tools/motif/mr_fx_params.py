"""エフェクトのパラメーターの名前と範囲の表（capture/fx_params.json）を、motif_play が読む文字の形（capture/fx_params.txt）にする（2026-10-26）。
  python mr_fx_params.py [capture の置き場]
fx_params.json は Data List の Effect Parameter List を型ごとに読み起こしたもの: {"型の番号（MSB × 128 + LSB）": {"name": …, "params": [{"no", "name", "min", "max", "range", …}]}}。
出力は 1 行に 1 つ「型の番号 番号 下 上 名前|表示の範囲」。Hall 1〜Plate のコーラス・インサーションの枠のものは、型の番号 + 100000。Data List から作ったものなので、roms/ の下に置いて配らない。"""
import json
import os
import sys


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    cap = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "..", "roms", "motif-rack-es", "capture")
    d = json.load(open(os.path.join(cap, "fx_params.json"), encoding="utf-8"))
    lines = ["# MOTIF-RACK ES effect parameters (from the Data List's Effect Parameter List): code no min max name|range. Do not distribute."]
    n = 0
    # Hall 1〜Plate はリバーブの枠と、コーラス・インサーションの枠でパラメーターが違う。後者は型の番号 + 100000 で書く（motif_play が枠で引き分ける）
    rows = []
    for code, t in d.items():
        rows.append((int(code), t.get("params", [])))
        if t.get("params_chorus_insertion"):
            rows.append((int(code) + 100000, t["params_chorus_insertion"]))
    for code, params in sorted(rows):
        for p in params:
            if p.get("no") is None or p.get("min") is None or p.get("max") is None:
                continue
            rng = str(p.get("range", "")).replace("|", "/")
            if p.get("uncertain"):
                rng += "（読み取りが不確か）"
            lines.append("%d %d %d %d %s|%s" % (code, int(p["no"]), int(p["min"]), int(p["max"]), str(p["name"]).replace("|", "/"), rng))
            n += 1
    open(os.path.join(cap, "fx_params.txt"), "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    print("型 %d・パラメーター %d 個を書いた" % (len(d), n))


if __name__ == "__main__":
    main()
