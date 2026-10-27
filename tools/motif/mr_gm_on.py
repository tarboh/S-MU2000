"""GM Mode On（F0 7E 7F 09 01 F7）をマルチのときに受けると、編集バッファがどう変わるかを実機で見る（2026-10-28）。
  python mr_gm_on.py 置き場 [--start capture/multi/41_nnn.syx]
マルチの編集バッファ（5F 00）を控え、GM Mode On を送って 1 秒待ち、もう一度取って、変わった欄を並べる。終わったら控えを送り返す。
送るのはユニバーサルの GM Mode On・ダンプの要求と送り返しだけ（保存しない）。
結果（2026-10-28）: GM Mode On は、マルチの編集バッファを決まった GM 用の状態にそっくり置き換える（New R&B から始めても 41_005 から始めても、
名前の 7 文字を除く 1266 バイトが同じ）。そのあとの編集バッファを capture/gm_on.multi に書く（motif_play が GM Mode On を受けたときに読む）。"""
import os
import sys
import time

import mr_midi
import mr_ref
import mr_voice


def main():
    a = sys.argv[1:]
    d = a[0]
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
        start = mr_ref.opt(a, "--start", None)          # 初めに編集バッファへ一括ロードするマルチ（capture/multi/41_nnn.syx）
        if start:
            import mr_multi_hw
            for m in mr_multi_hw.to_edit_buffer(start):
                r.mo.sysex(m)
                time.sleep(0.03)
            time.sleep(1.0)
            before = mr_midi.request_block(mi, r.mo, 0, 0x5F, 0x00)
        else:
            before = saved
        open(os.path.join(d, "before_5F.syx"), "wb").write(b"".join(bytes(m) for m in before))
        r.mo.sysex(bytes([0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7]))
        time.sleep(1.5)
        after = mr_midi.request_block(mi, r.mo, 0, 0x5F, 0x00)
        if not after:
            print("GM Mode On のあと、編集バッファが取れなかった")
        else:
            open(os.path.join(d, "after_5F.syx"), "wb").write(b"".join(bytes(m) for m in after))
    finally:
        r.all_off()
        if saved:
            for m in saved:
                r.mo.sysex(bytes(m))
                time.sleep(0.03)
        r.mode(3)
        r.close()
        mi.close()
    b = mr_voice.blocks(os.path.join(d, "before_5F.syx"))
    c = mr_voice.blocks(os.path.join(d, "after_5F.syx")) if os.path.exists(os.path.join(d, "after_5F.syx")) else {}
    n = 0
    for k in sorted(set(b) | set(c)):
        x, y = b.get(k, b""), c.get(k, b"")
        diffs = [(i, x[i] if i < len(x) else None, y[i] if i < len(y) else None) for i in range(max(len(x), len(y)))
                 if (x[i] if i < len(x) else None) != (y[i] if i < len(y) else None)]
        if diffs:
            n += 1
            print(" ".join("%02X" % v for v in k), "変わった欄（位置: 前 → 後）:", " ".join("%02X:%s→%s" % t for t in diffs[:24]), "…" if len(diffs) > 24 else "")
    print("変わったブロック", n)
    if c:
        import mr_multi_export
        out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../roms/motif-rack-es/capture/gm_on.multi")
        mr_multi_export.export(os.path.join(d, "after_5F.syx"), out)
        print("書いた", out)


if __name__ == "__main__":
    main()
