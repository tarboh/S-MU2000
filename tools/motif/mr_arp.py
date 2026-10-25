"""MOTIF-RACK ES のアルペジオの型を、実機のアルペジオの MIDI 出力（Utility の ArpOutSw、システムの 00 00 34）で受け取る（2026-10-25）。
  python mr_arp.py --probe [--type 0] [--keys 60,64,67] [--tempo 120] [--seconds 4] [--out 置き場.json]
  python mr_arp.py --dir 置き場 --types 0-1786 [--tempo 300] [--unit 0]（--unit は 40 70 18 の値。0 = 50% でテンポが倍）
雛形の音色（ボイスモード）で、アルペジオの型（40 70 14）・スイッチ（40 70 16）・テンポ（40 70 28）を書き、鍵を押さえたまま
アルペジオが出すノートを、時刻（ms）つきで受ける。押さえる鍵の組（PROBES）ごとに 1 回。
送るのは Data List のパラメーターチェンジ（システムの 00 00 34・35 と音色の 40 70 xx）とノートだけ（保存しない）。
終わったら ArpOutSw を切り（off）、マルチへ戻す。受けた MIDI はヤマハのデータなので配らないこと。"""
import ctypes
import ctypes.wintypes as wt
import json
import os
import sys
import time

import mr_midi
import mr_ref

winmm = ctypes.windll.winmm
MIM_DATA = 0x3C3
CALLBACK_FUNCTION = 0x30000
MidiInProc = ctypes.WINFUNCTYPE(None, wt.HANDLE, wt.UINT, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)


class ShortIn:
    """短いメッセージ（ノートなど）を、midiInStart からの ms つきで受ける"""

    def __init__(self, name):
        self.h = wt.HANDLE()
        self.events = []
        self.cb = MidiInProc(self._cb)
        idx = mr_midi.find(mr_midi.in_ports(), name)
        r = winmm.midiInOpen(ctypes.byref(self.h), idx, self.cb, 0, CALLBACK_FUNCTION)
        if r:
            raise SystemExit("MIDI 入力が開けない（%d）" % r)
        self.name = mr_midi.in_ports()[idx]
        winmm.midiInStart(self.h)
        self.t0 = time.perf_counter()

    def _cb(self, h, msg, inst, p1, p2):
        if msg == MIM_DATA:
            d = int(p1 or 0)
            st, a, b = d & 0xFF, (d >> 8) & 0xFF, (d >> 16) & 0xFF
            if st < 0xF8:
                self.events.append((time.perf_counter() - self.t0, int(p2 or 0), st, a, b))

    def take(self):
        e, self.events = self.events, []
        return e

    def close(self):
        winmm.midiInStop(self.h)
        winmm.midiInReset(self.h)
        winmm.midiInClose(self.h)


def keys_on(r, keys, vel):
    """鍵をまとめて 1 つのバッファ（ランニングステータス）で送る。1 つずつ送ると、6 音を超えたとき上の鍵がアルペジオの最初の拍に間に合わない"""
    r.mo.sysex(bytes([0x90]) + bytes(b for k in keys for b in (k, vel)))


UNIT = [0.5, 2 / 3, 0.75, 1.0, 4 / 3, 1.5, 2.0]    # 40 70 18 の値ごとの長さの倍率


def setup(r, typ, tempo, unit=3):
    r.template(704)
    r.param(0x40, 0x70, 0x14, typ >> 7, typ & 0x7F)    # 型
    r.param(0x40, 0x70, 0x16, 1)                       # スイッチ
    r.param(0x40, 0x70, 0x17, 1)                       # ホールド off
    r.param(0x40, 0x70, 0x18, unit)                    # 長さの倍率（3 = 100%、0 = 50% でテンポが倍）
    r.param(0x40, 0x70, 0x19, 0)
    r.param(0x40, 0x70, 0x1A, 127)
    r.param(0x40, 0x70, 0x1B, 1)
    r.param(0x40, 0x70, 0x1C, 127)
    r.param(0x40, 0x70, 0x1D, 0)                       # キーモード sort
    r.param(0x40, 0x70, 0x1E, 0)                       # ベロシティ original
    r.param(0x40, 0x70, 0x21, 0)                       # クオンタイズの強さ 0
    r.param(0x40, 0x70, 0x22, 0, 100)                  # ベロシティ 100%
    r.param(0x40, 0x70, 0x24, 0, 100)                  # ゲートタイム 100%
    r.param(0x40, 0x70, 0x28, tempo >> 7, tempo & 0x7F)
    time.sleep(0.1)


# 押さえる鍵の組（鍵, ベロシティ）。1〜8 音（順位の決まりを見る）・移した 1 音・マイナー・7th・強さ違い。
# DrPc（ドラム）は音の数より押さえた順を見るので、組を変える
PROBES = [("n1", [60], 100), ("n2", [60, 64], 100), ("n3", [60, 64, 67], 100), ("n4", [60, 62, 64, 67], 100),
          ("n5", [60, 62, 64, 65, 67], 100), ("n6", [60, 62, 64, 65, 67, 69], 100),
          ("n8", [60, 62, 64, 65, 67, 69, 71, 72], 100), ("d1", [62], 100), ("cmin", [60, 63, 67], 100),
          ("c7", [60, 64, 67, 70], 100), ("n3v40", [60, 64, 67], 40), ("n3v127", [60, 64, 67], 127)]
PROBES_DRUM = [("n1", [60], 100), ("n2", [60, 64], 100), ("n3", [60, 64, 67], 100), ("n3rev", [67, 64, 60], 100),
               ("d1", [62], 100), ("n3v40", [60, 64, 67], 40), ("n3v127", [60, 64, 67], 127)]
PROBES_CNTR = [("n1", [60], 100), ("n3", [60, 64, 67], 100)]


def capture(r, mi, a, tempo, unit):
    """型ごとに PROBES の組を押さえ、型の長さ（arp_types.json の小節 × 拍）＋ 1 拍だけ待って、出てきたメッセージを残す。
    時刻は鍵を押した（送った）時刻からの秒。置き場の t0000.json …（もうあれば飛ばす）"""
    d = mr_ref.opt(a, "--dir")
    os.makedirs(d, exist_ok=True)
    types = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "roms", "motif-rack-es", "capture", "arp_types.json"), encoding="utf-8"))
    lo, hi = (int(x) for x in mr_ref.opt(a, "--types", "0-1786").split("-"))
    beat = 60.0 / tempo * UNIT[unit]
    for typ in range(lo, hi + 1):
        path = os.path.join(d, "t%04d.json" % typ)
        if os.path.exists(path):
            continue
        info = types[typ]
        length = info["measures"] * info["beats"] * (4.0 / info["unit"]) * beat
        setup(r, typ, tempo, unit)
        # tempo は長さの倍率を掛けたあとの（実際の）テンポ
        out = {"type": typ, "info": info, "tempo": tempo / UNIT[unit], "probes": {}}
        for name, keys, vel in {"DrPc": PROBES_DRUM, "Cntr": PROBES_CNTR}.get(info["cat"], PROBES):
            r.all_off()
            time.sleep(0.12)
            mi.take()
            t0 = time.perf_counter() - mi.t0
            keys_on(r, keys, vel)
            time.sleep(length + beat)
            t1 = time.perf_counter() - mi.t0
            for k in keys:
                r.note_off(k)
            time.sleep(0.15)
            ev = [(round(e[0] - t0, 4), e[2], e[3], e[4]) for e in mi.take()]
            out["probes"][name] = {"keys": keys, "vel": vel, "off": round(t1 - t0, 4), "events": ev}
        json.dump(out, open(path, "w", encoding="utf-8"))
        n = sum(1 for p in out["probes"].values() for e in p["events"] if e[1] & 0xF0 == 0x90 and e[3] > 0)
        print("型 %d %s %s: ノート %d" % (typ, info["cat"], info["name"], n), flush=True)


def main():
    a = sys.argv[1:]
    midi_out = mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1")
    midi_in = mr_ref.opt(a, "--midi-in", "MOTIF-RACK ES-1")
    tempo = int(mr_ref.opt(a, "--tempo", "120"))
    unit = int(mr_ref.opt(a, "--unit", "3"))
    r = mr_ref.Ref(midi_out, mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), ".")
    mi = ShortIn(midi_in)
    print("MIDI 入力:", mi.name, flush=True)
    try:
        r.all_off()
        r.mode(0)
        r.program(63, 0, 0)
        time.sleep(0.3)
        r.param(0x00, 0x00, 0x35, 0)                   # ArpOutCh = 1
        r.param(0x00, 0x00, 0x34, 1)                   # ArpOutSw = on
        if "--probe" in a:
            # 型ごと・鍵の組（; で区切る）ごとに、ノートオンだけを「時刻（拍）:鍵:ベロシティ」で出す
            sec = float(mr_ref.opt(a, "--seconds", "4"))
            saved = {}
            groups = mr_ref.opt(a, "--keys", "60").split(";")
            for typ in [int(x) for x in mr_ref.opt(a, "--type", "0").split(",")]:
                setup(r, typ, tempo, unit)
                for kg in groups:
                    keys = [int(x) for x in kg.split(",")]
                    mi.take()
                    t0 = time.perf_counter() - mi.t0
                    keys_on(r, keys, 100)
                    time.sleep(sec)
                    for k in keys:
                        r.note_off(k)
                    time.sleep(0.4)
                    allev = mi.take()
                    saved.setdefault(str(typ), {})[kg] = {"keys": keys, "vel": 100, "events": [(round(e[0] - t0, 4), e[2], e[3], e[4]) for e in allev]}
                    ev = [e for e in allev if e[2] & 0xF0 == 0x90 and e[4] > 0]
                    beat = 60.0 / tempo * UNIT[unit]
                    print("型 %d 鍵 %s:" % (typ, kg), " ".join("%.2f:%d:%d" % ((e[0] - t0) / beat, e[3], e[4]) for e in ev[:40]), flush=True)
                    r.all_off()
                    time.sleep(0.2)
            if "--out" in a:
                json.dump({"tempo": tempo / UNIT[unit], "types": saved}, open(mr_ref.opt(a, "--out"), "w", encoding="utf-8"))
        if "--dir" in a:
            capture(r, mi, a, tempo, unit)
    finally:
        r.param(0x00, 0x00, 0x34, 0)                   # ArpOutSw = off（戻す）
        r.all_off()
        r.mode(3)
        r.close()
        mi.close()


if __name__ == "__main__":
    main()
