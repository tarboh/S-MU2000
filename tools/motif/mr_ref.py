"""MOTIF-RACK ES の基準の録音をする道具。doc/motif-rack-es-capture.md の「3. 基準の録音」。
送るのは、Data List に載っているパラメーターチェンジ・モードチェンジ・ふつうの演奏（ノートとコントロールチェンジ）だけ。

  python mr_ref.py <テストの名前> --midi "MOTIF-RACK ES-1" --rec ../../build/rec.exe --dev 0 --dir 置き場

ボイスモードの「今の音色」をその場で作り変える（保存はしない）。終わったら元のモードへは戻さないので、
呼ぶ側で mode を戻すこと（`python mr_ref.py mode3 ...` でマルチへ）。

ほかのスクリプトから使う部品（Ref クラス）も兼ねる。"""
import math
import os
import struct
import subprocess
import sys
import time

import mr_midi


def read_wav(path):
    d = open(path, "rb").read()
    p = d.find(b"data")
    sz = struct.unpack("<I", d[p + 4:p + 8])[0]
    n = min(sz, len(d) - p - 8) // 4
    x = struct.unpack("<%df" % n, d[p + 8:p + 8 + n * 4])
    return x[0::2], x[1::2]


def rms_db(x):
    if not x:
        return -999.0
    r = math.sqrt(sum(v * v for v in x) / len(x))
    return 20 * math.log10(r) if r > 0 else -999.0


def peak(x):
    return max((abs(v) for v in x), default=0.0)


def freq_zero_cross(x, rate=44100):
    """上向きのゼロ交差の間隔から周波数を出す（端は直線で補う）"""
    zs = []
    for i in range(1, len(x)):
        if x[i - 1] < 0 <= x[i]:
            zs.append(i - 1 + (-x[i - 1]) / (x[i] - x[i - 1]))
    if len(zs) < 3:
        return 0.0
    return rate * (len(zs) - 1) / (zs[-1] - zs[0])


class Ref:
    def __init__(self, midi_name, rec_exe, rec_dev, out_dir, dev=0):
        self.mo = mr_midi.MidiOut(midi_name)
        self.rec_exe = rec_exe
        self.rec_dev = str(rec_dev)
        self.dir = out_dir
        self.dev = dev
        os.makedirs(out_dir, exist_ok=True)

    def close(self):
        self.all_off()
        self.mo.close()

    # ---- 送る ----
    def param(self, ah, am, al, *data):
        self.mo.sysex(bytes([0xF0, 0x43, 0x10 | self.dev, 0x7F, 0x00, ah, am, al]) + bytes(data) + b"\xf7")
        time.sleep(0.02)

    def mode(self, m):                 # 0 = ボイス、1 = パフォーマンス、3 = マルチ
        self.param(0x0A, 0x00, 0x01, m)
        time.sleep(1.0)

    def cc(self, n, v, ch=0):
        self.mo.short(0xB0 | ch, n, v)

    def program(self, msb, lsb, pc, ch=0):
        self.cc(0, msb, ch)
        self.cc(32, lsb, ch)
        self.mo.short(0xC0 | ch, pc)
        time.sleep(0.5)

    def note_on(self, n, v=100, ch=0):
        self.mo.short(0x90 | ch, n, v)

    def note_off(self, n, ch=0):
        self.mo.short(0x80 | ch, n, 0)

    def all_off(self):
        for ch in range(16):
            self.cc(120, 0, ch)        # All Sound Off
            self.cc(123, 0, ch)        # All Notes Off

    # ---- 今の音色（ボイスモードのノーマルボイス）を作り変える ----
    def el(self, e, block, off, *data):   # block: 0x41 発振とアンプ、0x42 音程、0x43 フィルター、0x44 EQ と LFO
        self.param(block, e, off, *data)

    def template(self, wave=704, e=0, others_off=True):
        """エレメント 1 だけ、EG は平ら、ベロシティは効かせない、LFO・EQ は切る（e で別のエレメントにも同じ雛形を作れる。others_off なら残りを切る）"""
        if others_off:
            for o in range(4):
                if o != e:
                    self.el(o, 0x41, 0x00, 0)              # ほかのエレメントを切る
        self.el(e, 0x41, 0x00, 1)
        self.el(e, 0x41, 0x03, wave >> 7, wave & 0x7F)     # 波形
        self.el(e, 0x41, 0x08, 64)                         # パン中央
        self.el(e, 0x41, 0x09, 0)                          # ランダムパン 0
        self.el(e, 0x41, 0x0A, 64)                         # オルタネートパン 0（64 が真ん中。0 にすると 1 音ごとに左右へ振り切る）
        self.el(e, 0x41, 0x0B, 64)                         # スケーリングパン 0
        self.el(e, 0x41, 0x0C, 0)
        self.el(e, 0x41, 0x0D, 127)                        # 全部の鍵
        self.el(e, 0x41, 0x0E, 1)
        self.el(e, 0x41, 0x0F, 127)                        # 全部のベロシティ
        self.el(e, 0x41, 0x11, 0)                          # キーオンディレイ 0
        self.el(e, 0x41, 0x16, 0)                          # インサーションへ送らない（thru）
        self.el(e, 0x41, 0x20, 127)                        # エレメントの音量
        self.el(e, 0x41, 0x21, 64)                         # 音量のベロシティ感度 0
        self.el(e, 0x41, 0x24, 0)                          # AEG: アタック 0
        self.el(e, 0x41, 0x25, 127)
        self.el(e, 0x41, 0x26, 127)
        self.el(e, 0x41, 0x27, 0)                          # リリース 0
        for off in (0x28, 0x29, 0x2A, 0x2B):
            self.el(e, 0x41, off, 127)                     # 高さは全部 127
        self.el(e, 0x41, 0x2E, 64)                         # AEG の時間のベロシティ感度 0
        self.el(e, 0x41, 0x2F, 64)                         # 音量のキーフォロー 0
        for off in (0x34, 0x36, 0x38, 0x3A):
            self.el(e, 0x41, off, 1, 0)                    # レベルスケーリングのずらし 0（128）。直さないと鍵ごとに音量が変わる
        self.el(e, 0x41, 0x3C, 64)                         # AEG の時間のキーフォロー 0
        self.el(e, 0x42, 0x00, 0x40)                       # コースチューン 0
        self.el(e, 0x42, 0x01, 64)                         # ファインチューン 0
        self.el(e, 0x42, 0x02, 64)                         # 音程のベロシティ感度 0
        self.el(e, 0x42, 0x04, 0)                          # ランダムピッチ 0
        self.el(e, 0x42, 0x14, 64)                         # PEG の深さ 0
        self.el(e, 0x43, 0x01, 1, 127)                     # カットオフ 255
        self.el(e, 0x43, 0x03, 64)                         # カットオフのベロシティ感度 0
        self.el(e, 0x43, 0x05, 0)                          # レゾナンス 0
        self.el(e, 0x43, 0x1D, 64)                         # FEG の深さ 0
        self.el(e, 0x44, 0x00, 5)                          # EQ thru
        for off in (0x44, 0x45, 0x46):
            self.el(e, 0x44, off, 0)                       # エレメントの LFO の深さ 0
        self.cc(91, 0)
        self.cc(93, 0)                                     # リバーブ・コーラスへの送り 0
        self.cc(7, 127)
        self.cc(11, 127)
        self.cc(10, 64)
        self.cc(1, 0)
        time.sleep(0.3)

    # ---- 録る ----
    def record(self, name, seconds, events):
        """events = [(時刻の秒, 関数), …]。録り始めてからの時刻で呼ぶ。録った左右を返す"""
        path = os.path.join(self.dir, name + ".wav")
        p = subprocess.Popen([self.rec_exe, self.rec_dev, path, str(seconds), "--float"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        t0 = time.time()
        for t, fn in sorted(events, key=lambda e: e[0]):
            while time.time() - t0 < t:
                time.sleep(0.001)
            fn()
        p.wait(timeout=seconds + 20)
        return read_wav(path)

    def one_note(self, name, note=69, vel=100, hold=1.5, tail=1.0, pre=1.0):
        return self.record(name, pre + hold + tail, [(pre, lambda: self.note_on(note, vel)),
                                                     (pre + hold, lambda: self.note_off(note))])


def opt(a, name, default=None):
    if name in a:
        i = a.index(name)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return default


def main():
    a = sys.argv[1:]
    test = a.pop(0)
    r = Ref(opt(a, "--midi", "MOTIF-RACK ES-1"), opt(a, "--rec", "../../build/rec.exe"),
            opt(a, "--dev", "0"), opt(a, "--dir", "."))
    try:
        if test.startswith("mode"):
            r.mode(int(test[4:]))
        elif test == "alloff":
            r.all_off()
        else:
            raise SystemExit("知らないテスト: " + test)
    finally:
        r.close()


if __name__ == "__main__":
    main()
