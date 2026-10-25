"""プリセットの音色で、コントローラー（CC・アフタータッチ・ピッチベンド）を途中で動かした音を録る（2026-10-25。コントロールセットの番号の意味を、
実際に組まれた音色で確かめる）。
  python mr_ctrl_preset.py --dir 置き場 --voices 04_063,00_011 [--msgs auto | auto-pre | CC1,AT,CC2,CC16,CC16lo,CC16pre,…] [--key 60]
音色ごとに「何も動かさない」1 本と、メッセージごとに「1.0 秒で初め → 最大（lo は最小）」の 1 本を（auto は音色のコントロールセットが使うソースだけ）、2.4 秒押さえて録る。リバーブ・コーラスの送りは 0、
インサーションのスイッチは切る（mr_preset_compare.py と同じ）。送るのはプログラムチェンジ・パラメーターチェンジ・CC・アフタータッチ・ピッチベンド・ノートだけ
（保存しない）。終わったら戻してマルチへ。録った音は配らないこと（ヤマハの音）。"""
import os
import sys
import time

import mr_preset_compare as P
import mr_ref

# 名前 → （ステータス・番号・1.0 秒で送る値）。CC16・CC17・CC22（AS1・AS2・RB）は中心から両側へ振れるソースなので、初めは 64 にして、
# 「…」は 127 へ、「…lo」は 0 へ動かす（2026-10-25、mr_ctrl.py --sources。前は初めを 0 にしていて、最小まで回した音になっていた）
MSG = {"CC1": (0xB0, 1, 127), "CC2": (0xB0, 2, 127), "CC4": (0xB0, 4, 127), "CC16": (0xB0, 16, 127), "CC16lo": (0xB0, 16, 0),
       "CC17": (0xB0, 17, 127), "CC17lo": (0xB0, 17, 0), "CC22": (0xB0, 22, 127), "CC22lo": (0xB0, 22, 0),
       "AT": (0xD0, None, 127), "PB": (0xE0, None, 127)}
CENTER = {16, 17, 22}
# コントロールセットのソースの番号 → 動かすメッセージ（--msgs auto で使う）
SRC_MSGS = {1: ["CC1"], 2: ["AT"], 3: ["AT"], 6: ["CC22", "CC22lo"], 7: ["CC2"], 8: ["CC16", "CC16lo"], 9: ["CC17", "CC17lo"], 10: ["CC4"], 11: ["CC4"], 12: ["CC4"]}


def reset(r):
    for st, n, _ in MSG.values():
        if st == 0xB0:
            r.mo.short(0xB0, n, 64 if n in CENTER else 0)
        elif st == 0xD0:
            r.mo.short(0xD0, 0, 0)
        else:
            r.mo.short(0xE0, 0, 64)
    time.sleep(0.05)


def send(r, m):
    st, n, v = MSG[m[:-3] if m.endswith("pre") else m]
    if st == 0xB0:
        r.mo.short(0xB0, n, v)
    elif st == 0xD0:
        r.mo.short(0xD0, v, 0)
    else:
        r.mo.short(0xE0, 127, 127)


def auto_msgs(v):
    """音色のコントロールセットで使われているソースのメッセージ"""
    import mr_voice
    x = mr_voice.blocks(os.path.join(os.path.dirname(__file__), "..", "..", "roms", "motif-rack-es", "capture", "voice", v + ".syx")).get((0x40, 0x05, 0x00))
    out = []
    for i in range(6):
        s_, d_, dp = x[5 * i], x[5 * i + 1] << 7 | x[5 * i + 2], x[5 * i + 3]
        if d_ and dp != 64:
            for m in SRC_MSGS.get(s_, []):
                if m not in out:
                    out.append(m)
    return out


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    voices = mr_ref.opt(a, "--voices", "04_063").split(",")
    msgs = mr_ref.opt(a, "--msgs", "auto")
    key = int(mr_ref.opt(a, "--key", "60"))
    os.makedirs(d, exist_ok=True)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
    try:
        r.all_off()
        r.mode(0)
        for v in voices:
            mm, nn = int(v[:2], 16), int(v[3:6])
            msb, lsb = P.bank_select(mm)
            r.program(msb, lsb, nn)
            time.sleep(0.3)
            r.param(0x40, 0x70, 0x16, 0)
            r.cc(91, 0)
            r.cc(93, 0)
            for e in range(4):
                r.el(e, 0x41, 0x16, 0)
            time.sleep(0.1)
            # auto-pre: 音色が使うソースを、ノートの前（0.1 秒）に動かす（EG の行き先はキーオンのときの値で決まる）
            ms = auto_msgs(v) if msgs == "auto" else [m + "pre" for m in auto_msgs(v)] if msgs == "auto-pre" else msgs.split(",")
            for m in ["none"] + ms + (["nonez"] if msgs == "auto-pre" else []):     # nonez: 最後にもう 1 本、何も動かさない（1 本目とそろうかを見る）
                name = "cp_%s_k%03d_%s" % (v, key, m)
                if os.path.exists(os.path.join(d, name + ".wav")):
                    continue
                reset(r)
                # 前の本の音の尾を切る（リリースの長い音色は、0.7 秒の間では尾が次の本の頭に残り、音の頭の読みがずれた。2026-10-25）
                r.all_off()
                time.sleep(0.2)
                ev = [(0.3, lambda: r.note_on(key, 100)), (2.7, lambda: r.note_off(key))]
                if m not in ("none", "nonez"):
                    ev.insert(0 if m.endswith("pre") else 1, (0.1 if m.endswith("pre") else 1.3, lambda m=m: send(r, m)))
                r.record(name, 3.4, ev)
            reset(r)
            print(v, "録った", flush=True)
    finally:
        reset(r)
        r.all_off()
        r.mode(3)
        r.close()


if __name__ == "__main__":
    main()
