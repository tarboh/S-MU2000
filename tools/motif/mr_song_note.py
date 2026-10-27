"""曲（SMF）の頭のマルチの設定のまま、1 つのチャンネルの 1 鍵を実機と芯で鳴らして比べる（2026-10-27）。
  python mr_song_note.py 置き場 曲.mid チャンネル（1〜16） 鍵 [--vel 100] [--hold 0.3] [--name 名前] [--norpn] [--analyze-only]
曲の中の SysEx（マルチの編集バッファへの一括ロード、43 0n 7F 00 … 5F 00。MOTIF-RACK の形 43 0n 6F は ES が受けないので使わない）と、そのチャンネルの最初のノートより前の CC・プログラムチェンジを取り出す。
  実機: 編集バッファ（5F 00）を控え、マルチで SysEx とそのチャンネルの CC・PC を送ってから鍵を鳴らす。終わったら控えを送り返す
  芯: SysEx を mr_multi_export.export で .multi にして（capture/multis/zz_song_tmp.multi、終わったら消す）、motif_play --selftest で
      同じ CC・PC（--seq）を送ってから鳴らす
--norpn は RPN 2・1（Coarse・Fine Tune）を送らない組。送るのは一括ロード・ダンプの要求・CC・PC・ノートだけ（保存しない）。録った音は配らないこと。"""
import os
import subprocess
import sys
import time

import numpy as np

import mr_bulk
import mr_midi
import mr_multi_export
import mr_ref
import mr_voice

HERE = os.path.dirname(os.path.abspath(__file__))
CAP = os.path.join(HERE, "../../roms/motif-rack-es/capture")
FS = 44100


def read_smf(path, ch):
    """(SysEx の並び, そのチャンネルの最初のノートより前の CC・PC の並び)"""
    d = open(path, "rb").read()
    pos = 14
    sx, ev = [], []
    for _ in range(int.from_bytes(d[10:12], "big")):
        ln = int.from_bytes(d[pos + 4:pos + 8], "big")
        p, end, run, tick = pos + 8, pos + 8 + ln, 0, 0
        pos = end
        seen_note = False

        def vlq():
            nonlocal p
            v = 0
            while True:
                b = d[p]
                p += 1
                v = v << 7 | b & 0x7F
                if b < 0x80:
                    return v
        while p < end:
            tick += vlq()
            b = d[p]
            if b >= 0x80:
                run = b
                p += 1
            s = run
            if s == 0xFF:
                p += 1
                n = vlq()
                p += n
                continue
            if s in (0xF0, 0xF7):
                n = vlq()
                body = d[p:p + n]
                p += n
                # MOTIF-RACK ES の一括ロードは 43 0n 7F 00。43 0n 6F（MOTIF-RACK の形）は ES が受けない（曲のあとの編集バッファに名前が入っていなかった）
                if s == 0xF0 and body[:3] == bytes([0x43, body[1] & 0x0F, 0x7F]):
                    sx.append((tick, b"\xf0" + body if body[-1:] == b"\xf7" else b"\xf0" + body + b"\xf7"))
                continue
            n = 1 if s & 0xF0 in (0xC0, 0xD0) else 2
            a = d[p:p + n]
            p += n
            if (s & 15) != ch - 1:
                continue
            if s & 0xF0 == 0x90 and a[1]:
                seen_note = True
            if not seen_note and s & 0xF0 in (0xB0, 0xC0):
                ev.append((tick, bytes([s]) + a))
    sx.sort(key=lambda x: x[0])
    ev.sort(key=lambda x: x[0])
    return [m for _, m in sx], [m for _, m in ev]


def drop_rpn12(ev):
    """RPN 0・1・2 のうち 1 と 2 のデータ（CC6・38）を落とす"""
    out, msb, lsb = [], 127, 127
    for m in ev:
        if m[0] & 0xF0 == 0xB0:
            if m[1] == 101:
                msb = m[2]
            elif m[1] == 100:
                lsb = m[2]
            elif m[1] in (6, 38) and msb == 0 and lsb in (1, 2):
                continue
        out.append(m)
    return out


def merged(saved, sx):
    """実機の編集バッファの控え（saved_5F.syx）に、曲の一括ロードをバイト単位で上書きしたもの。曲のブロックが短い（マルチ共通が 88 バイトで、
    インサーションのパート 36 00 60〜63 を含まない）と、実機ではその先が前のまま残るので、芯にも同じ形を渡す"""
    blk = {}
    for src in ([open(saved, "rb").read()] if os.path.exists(saved) else []) + [b"".join(sx)]:
        tmp = saved + ".part.syx"
        open(tmp, "wb").write(src)
        for addr, data in mr_voice.blocks(tmp).items():
            old = bytearray(blk.get(addr, b""))
            if len(old) < len(data):
                old += bytes(len(data) - len(old))
            old[:len(data)] = data
            blk[addr] = bytes(old)
        os.remove(tmp)
    out = [mr_bulk.make_bulk(0, (0x0E, 0x5F, 0x00), b"")]
    for addr in sorted(a for a in blk if a[0] in (0x36, 0x37)):
        out.append(mr_bulk.make_bulk(0, addr, blk[addr]))
    out.append(mr_bulk.make_bulk(0, (0x0F, 0x5F, 0x00), b""))
    return b"".join(bytes(m) for m in out)


def record_hw(d, a, name, sx, ev, ch, key, vel, hold):
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
        for m in sx:
            if len(m) > 4 and m[1] == 0x43 and m[3] == 0x6F:
                r.mo.sysex(m)
                time.sleep(0.05)
        time.sleep(1.0)
        # RPN 1・2 はチャンネルの状態で、編集バッファを戻しても残る（前の録音の値が残っていた）ので、初めに真ん中へ戻す
        for num in (1, 2):
            for c, v in ((101, 0), (100, num), (6, 64), (38, 0), (101, 127), (100, 127)):
                r.cc(c, v, ch - 1)
        for m in ev:
            r.mo.short(m[0], m[1], m[2] if len(m) > 2 else 0)
            time.sleep(0.01)
            if m[0] & 0xF0 == 0xC0:
                time.sleep(1.5)
        time.sleep(0.5)
        r.all_off()
        time.sleep(0.2)
        c = ch - 1
        r.record(name + "_hw", 0.3 + hold + 3.0, [(0.3, lambda: r.note_on(key, vel, c)), (0.3 + hold, lambda: r.note_off(key, c))])
    finally:
        r.all_off()
        if saved:
            for m in saved:
                r.mo.sysex(bytes(m))
                time.sleep(0.03)
        r.mode(3)
        r.close()
        mi.close()


def render_sm(d, exe, name, sx, ev, ch, key, vel, hold):
    syx = os.path.join(d, "song_setup.syx")
    open(syx, "wb").write(merged(os.path.join(d, "saved_5F.syx"), sx))
    mdir = os.path.join(CAP, "multis")
    tmp = os.path.join(mdir, "zz_song_tmp.multi")
    try:
        if not mr_multi_export.export(syx, tmp):
            raise SystemExit("編集バッファの控えと曲の SysEx からマルチを作れない")
        idx = sorted(f for f in os.listdir(mdir) if f.endswith(".multi")).index("zz_song_tmp.multi")
        c = ch - 1
        seq = [m.hex(" ") for m in ev if m[0] & 0xF0 == 0xB0]
        pcs = [m for m in ev if m[0] & 0xF0 == 0xC0]
        bank = {0: 0, 32: 0}
        for m in ev:
            if m[0] & 0xF0 == 0xB0 and m[1] in (0, 32):
                bank[m[1]] = m[2]
        n_hold = max(1, int(round(hold / 0.1)))
        seq += ["%02x %02x %02x" % (0x90 | c, key, vel)] + ["B0 03 00"] * (n_hold - 1) + ["%02x %02x 00" % (0x80 | c, key)] + ["B0 03 00"] * 30
        args = [exe, "--nomidi", "--selftest", os.path.join(d, name + "_sm.wav"), "--multi", str(idx), "--seq", ";".join(seq)]
        if pcs:
            args += ["--pc", "%d:%d:%d:%d" % (ch, bank[0], bank[32], pcs[-1][1])]
        subprocess.run(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def load(p):
    if open(p, "rb").read(22)[20] == 3:
        L, R = mr_ref.read_wav(p)
        return np.array([np.asarray(L, float), np.asarray(R, float)])
    import wave
    w = wave.open(p)
    return np.frombuffer(w.readframes(w.getnframes()), "<i2").reshape(-1, 2).T / 32768.0


def feats(x):
    m = x.mean(axis=0)
    on = int(np.argmax(np.abs(m) > np.abs(m).max() * 0.02))
    y = x[:, on:on + int(1.5 * FS)]
    mm = y.mean(axis=0)
    lv = 10 * np.log10((y ** 2).mean() + 1e-30)
    edges = 20 * 2 ** (np.arange(0, 31) / 3)
    X = np.abs(np.fft.rfft(mm[:int(0.5 * FS)] * np.hanning(int(0.5 * FS)))) ** 2
    f = np.fft.rfftfreq(int(0.5 * FS), 1 / FS)
    sp = np.array([10 * np.log10(X[(f >= lo) & (f < hi)].sum() + 1e-30) for lo, hi in zip(edges[:-1], edges[1:])])
    n = FS // 50
    env = np.array([10 * np.log10((mm[i:i + n] ** 2).mean() + 1e-30) for i in range(0, len(mm) - n, n)])
    return lv, sp, edges, env


def analyze(d, name):
    h = feats(load(os.path.join(d, name + "_hw.wav")))
    s = feats(load(os.path.join(d, name + "_sm.wav")))
    print("%s: 音量 実機 %.1f・芯 %.1fdB（差 %+.1f）" % (name, h[0], s[0], s[0] - h[0]))
    ds = (s[1] - s[1].max()) - (h[1] - h[1].max())
    print("  帯（Hz）と、いちばん大きい帯から測った差（芯 − 実機、dB。最初の 0.5 秒）")
    print("  " + " ".join("%5.0f" % e for e in h[2][:-1]))
    print("  " + " ".join("%5.1f" % v for v in ds))
    print("  実機 " + " ".join("%5.1f" % v for v in (h[1] - h[1].max())))
    k = min(len(h[3]), len(s[3]))
    print("  20ms ごとの大きさ（実機 / 芯、dB）: " + " ".join("%.0f/%.0f" % (h[3][i], s[3][i]) for i in range(0, min(k, 60), 3)))


def main():
    a = sys.argv[1:]
    d = a[0]
    vel = int(mr_ref.opt(a, "--vel", "100"))
    hold = float(mr_ref.opt(a, "--hold", "0.3"))
    exe = mr_ref.opt(a, "--exe", os.path.join(HERE, "../../build/motif_play.exe"))
    name = mr_ref.opt(a, "--name", None)
    song, ch, key = a[1], int(a[2]), int(a[3])
    name = name or "ch%d_k%d%s" % (ch, key, "_norpn" if "--norpn" in a else "")
    os.makedirs(d, exist_ok=True)
    sx, ev = read_smf(song, ch)
    if "--norpn" in a:
        ev = drop_rpn12(ev)
    if "--analyze-only" not in a:
        if not os.path.exists(os.path.join(d, name + "_hw.wav")):
            record_hw(d, a, name, sx, ev, ch, key, vel, hold)
        render_sm(d, exe, name, sx, ev, ch, key, vel, hold)
    analyze(d, name)


if __name__ == "__main__":
    main()
