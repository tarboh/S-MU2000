"""取ったマルチを実機の編集バッファに読み込ませ、MIDI の並びを流して録る（芯の motif_play --selftest --multi --seq と比べる）（2026-10-27）。
  python mr_multi_hw.py 置き場 --multi 41_000 --seq "94 3C 64;94 40 64;…" [--name 名前]
始める前に編集バッファ（5F 00）を一括ダンプで控え、取ったマルチ（capture/multi/41_nnn.syx）の番地の頭を 5F 00 に付け替えて送る（ユーザーの場所には書かない）。
並びは 0.1 秒おきに送り（録音の 0.3 秒から）、送り終えて 3 秒録る。終わったら控えを送り返す。
送るのはダンプの要求・編集バッファへのバルク・ノートと CC だけ（保存しない）。録った音は配らないこと（ヤマハの音）。"""
import os
import sys
import time

import mr_bulk
import mr_midi
import mr_ref


def to_edit_buffer(path, dev=0):
    """41_nnn.syx のメッセージを、番地の頭（0E 41 nn・0F 41 nn）を編集バッファ（0E 5F 00・0F 5F 00）に付け替えて作り直す"""
    out = []
    for _, m, closed in mr_bulk.split_sysex(open(path, "rb").read()):
        if not closed or len(m) < 12:
            continue
        body = m[5:-2]
        addr = tuple(body[2:5])
        payload = bytes(body[5:])
        if addr[0] in (0x0E, 0x0F):
            addr = (addr[0], 0x5F, 0x00)
        out.append(mr_bulk.make_bulk(dev, addr, payload))
    return out


def main():
    a = sys.argv[1:]
    d = a[0]
    here = os.path.dirname(os.path.abspath(__file__))
    cap = os.path.join(here, "..", "..", "roms", "motif-rack-es", "capture")
    multi = mr_ref.opt(a, "--multi", "41_000")
    seq = [bytes(int(x, 16) for x in s.split()) for s in mr_ref.opt(a, "--seq", "").split(";") if s.strip()]
    name = mr_ref.opt(a, "--name", multi)
    os.makedirs(d, exist_ok=True)
    r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
    mi = mr_midi.MidiIn(mr_ref.opt(a, "--midi-in", "MOTIF-RACK ES-1"))
    saved = None
    try:
        r.all_off()
        r.param(0x00, 0x00, 0x34, 0)
        r.mode(3)
        saved = mr_midi.request_block(mi, r.mo, 0, 0x5F, 0x00)
        if not saved:
            raise SystemExit("マルチの編集バッファを控えられない（ダンプが返らない）ので止める")
        open(os.path.join(d, "saved_5F.syx"), "wb").write(b"".join(bytes(m) for m in saved))
        for m in to_edit_buffer(os.path.join(cap, "multi", multi + ".syx")):
            r.mo.sysex(m)
            time.sleep(0.03)
        time.sleep(2.0)                                # 音色を読む間
        ev = [(0.3 + 0.1 * i, (lambda m=m: r.mo.short(m[0], m[1] if len(m) > 1 else 0, m[2] if len(m) > 2 else 0))) for i, m in enumerate(seq)]
        r.record(name + "_hw", 0.3 + 0.1 * len(seq) + 3.0, ev)
        print("録った: %s_hw.wav" % name, flush=True)
    finally:
        r.all_off()
        if saved:
            for m in saved:
                r.mo.sysex(bytes(m))
                time.sleep(0.03)
        r.mode(3)
        r.close()
        mi.close()


if __name__ == "__main__":
    main()
