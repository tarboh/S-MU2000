"""MOTIF-RACK ES のエフェクト（ボイスのリバーブ・コーラス）を測る録音。
  python mr_fx.py --dir 置き場 [--block reverb|chorus] [--types 1:0,1:2,…] [--tail 8] [--set p1=30,ret=64] [--send 127] [--cc 127] [--tag 名前]

ノイズの波形 705 を 50ms ほどの短い音にして鳴らし、エフェクトへの送りを 0 にした録音（_dry）と 127 にした録音（_wet）を 1 本ずつ録る。
2 本の比から、エフェクトのインパルス応答を逆畳み込みで出す（mr_fx_fit.py）。
型は「MSB:LSB」（Data List の Effect Type List の MIDI (HEX) を 10 進で）。型を変えたあと、エディットバッファ（0E 0F 00）を一括ダンプで受け、
実機が入れたパラメーター（型の既定値）を <名前>.json に控える。アドレスは 40 01 00（リバーブ）・40 02 00（コーラス）、送りは 40 00 0B・0C と CC91・93。
--set は型を入れたあとに変えるパラメーター（pN = パラメーター N、ret = Return、pan = Pan）、--send は wet の音色の送り（40 00 0B・0C、既定 127）、--cc は wet の CC91・93（既定 127）、
--tag は名前の後ろに付ける印。
送るのはパラメーターチェンジ・ダンプの要求・ノートと CC だけ（保存しない）。終わったらマルチへ戻す。録った音は配らないこと。"""
import json
import os
import sys
import time

import mr_midi
import mr_params
import mr_ref

REVERBS = [(1, 0x00, "Hall1"), (1, 0x01, "Hall2"), (1, 0x10, "Room1"), (1, 0x11, "Room2"), (1, 0x12, "Room3"), (1, 0x20, "Stage1"),
           (1, 0x21, "Stage2"), (1, 0x30, "Plate"), (1, 0x40, "WhiteRoom"), (1, 0x41, "Tunnel"), (1, 0x42, "Basement"),
           (1, 0x43, "Canyon"), (1, 0x02, "R3Hall"), (1, 0x13, "R3Room"), (1, 0x31, "R3Plate"), (1, 0x08, "LargeHall"),
           (1, 0x09, "MediumHall"), (1, 0x18, "WarmRoom"), (1, 0x19, "WoodyRoom"), (1, 0x38, "RichPlate")]
BLOCKS = {"reverb": (0x01, 0x0B, 91), "chorus": (0x02, 0x0C, 93)}


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", ".")
    block = mr_ref.opt(a, "--block", "reverb")
    tail = float(mr_ref.opt(a, "--tail", "8"))
    am, send_off, cc = BLOCKS[block]
    tl = mr_ref.opt(a, "--types")
    if tl:
        types = [(int(t.split(":")[0]), int(t.split(":")[1]), "t%s_%s" % tuple(t.split(":"))) for t in tl.split(",")]
    else:
        types = REVERBS
    sets = []
    st = mr_ref.opt(a, "--set")
    for kv in (st.split(",") if st else []):
        k, v = kv.split("=")
        v = int(v)
        if k == "ret":
            sets.append((0x22, (v,)))
        elif k == "pan":
            sets.append((0x23, (v,)))
        else:
            sets.append((0x02 + 2 * (int(k[1:]) - 1), (v >> 7, v & 0x7F)))
    send = int(mr_ref.opt(a, "--send", "127"))
    ccv = int(mr_ref.opt(a, "--cc", "127"))          # wet のときの CC91 / 93 の値
    tag = mr_ref.opt(a, "--tag", "")
    os.makedirs(d, exist_ok=True)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
    mi = mr_midi.MidiIn(mr_ref.opt(a, "--midi-in", "MOTIF-RACK ES-1"))

    def common(am_, al, *v):
        r.param(0x40, am_, al, *v)

    def take(name):
        if not os.path.exists(os.path.join(d, name + ".wav")):
            r.record(name, 0.3 + 0.1 + tail, [(0.3, lambda: r.note_on(60, 100)), (0.4, lambda: r.note_off(60))])

    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        r.template(705)
        r.el(0, 0x43, 0x00, 21)                     # フィルター THRU
        r.el(0, 0x43, 0x01, 1, 127)
        r.el(0, 0x43, 0x22, 64)
        r.el(0, 0x41, 0x25, 40)                     # AEG: ディケイ 1 を 40（約 50ms）で 0 へ
        r.el(0, 0x41, 0x2A, 0)
        r.el(0, 0x41, 0x2B, 0)
        r.el(0, 0x41, 0x27, 0)
        for e in range(4):
            r.el(e, 0x41, 0x16, 0)                  # インサーションを通さない
        common(0x00, 0x0B, 0)
        common(0x00, 0x0C, 0)
        time.sleep(0.1)
        for msb, lsb, name in types:
            n = "%s_%s%s" % (block, name, "_" + tag if tag else "")
            common(am, 0x00, msb, lsb)
            time.sleep(0.3)
            for off, v in sets:
                common(am, off, *v)
            time.sleep(0.1)
            got = mr_midi.request_block(mi, r.mo, 0, 0x0F, 0x00)
            blk = None
            for m in got or []:
                if m[7:10] == bytes([0x40, am, 0x00]):
                    blk = list(m[10:-2])
            if blk:
                info = mr_params.decode_rows(mr_params.COMMON_BLOCKS[(0x40, am, 0x00)], blk)
                json.dump(info, open(os.path.join(d, n + ".json"), "w"), indent=1)
            common(0x00, send_off, 0)
            r.cc(cc, 0)
            time.sleep(0.1)
            take(n + "_dry")
            common(0x00, send_off, send)
            r.cc(cc, ccv)
            time.sleep(0.1)
            take(n + "_wet")
            common(0x00, send_off, 0)
            r.cc(cc, 0)
            print(n, "done", flush=True)
        r.all_off()
        r.mode(3)
    finally:
        mi.close()
        r.close()
    print("DONE", flush=True)


if __name__ == "__main__":
    main()
