"""プリセットの音色を実機で鳴らして録り、サンプリング方式の芯（sampler_render）で同じ鍵・ベロシティを鳴らして比べる。
  python mr_preset_compare.py --voices 00_011,06_006 [--notes 60:100,60:40] [--hold 2] [--dir 置き場]
                              [--capture 取ったものの置き場] [--render sampler_render.exe] [--analyze-only]

音色は voice_json/<mm>_<nn>.json（mr_voice.py --json）で選ぶ。mm がバンク（00〜05 = PRE1〜6 → MSB 63・LSB mm、06 = GM → MSB 0・LSB 0、
08〜0A = USR1〜3 → MSB 63・LSB 8〜10）、nn が番号。音色を選んだあと、エディットバッファ（0E 0F 00）を一括ダンプで受けて、
名前が JSON と同じか確かめてから録る（違えば飛ばす）。
録る前に、リバーブ・コーラスへの送り（CC91・CC93）を 0 にし、エレメント 1〜4 のインサーションのスイッチ（41 ee 16）を切る
（エディットバッファを変えるだけで保存しない）。共通の音量やマスター EQ はそのまま。
実機の録音は <置き場>/<音色>_k<鍵>_v<ベロシティ>_hw.wav、芯で鳴らしたものは _sm.wav。
比べるのは、10ms ごとの音量（dB）の動きと、0.1〜0.6 秒の平均のスペクトル（1/3 オクターブ）。絶対の音量は共通の音量などでずれるので、
音色ごとに「ベロシティ 100・鍵 60」の差を引いてから比べる。終わったらマルチへ戻す。録った音は配らないこと（ヤマハの音）。"""
import json
import os
import subprocess
import sys
import time

import numpy as np

import mr_midi
import mr_patch
import mr_ref

FS = 44100


def bank_select(mm):
    if mm == 6:
        return 0, 0
    return 63, mm


def edit_name(mi, mo):
    got = mr_midi.request_block(mi, mo, 0, 0x0F, 0x00)
    if not got:
        return None
    for m in got:
        if m[7:10] == bytes([0x40, 0x70, 0x00]):
            return bytes(m[10:20]).decode("ascii", "replace").rstrip()
    return None


def env_db(path, win=0.01):
    L, R = mr_ref.read_wav(path)
    x = np.asarray(L) + np.asarray(R)
    n = int(win * FS)
    m = len(x) // n
    return 20 * np.log10(np.sqrt(np.mean(x[:m * n].reshape(m, n) ** 2, axis=1)) + 1e-9), x


def onset(x):
    return int(np.argmax(np.abs(x) > 1e-4))


def third_octave(x, on):
    seg = x[on + int(0.1 * FS):on + int(0.6 * FS)]
    if len(seg) < 4096:
        return None
    S = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) ** 2
    f = np.fft.rfftfreq(len(seg), 1 / FS)
    edges = 50 * 2 ** (np.arange(0, 29) / 3)
    return np.array([10 * np.log10(np.sum(S[(f >= a) & (f < b)]) + 1e-12) for a, b in zip(edges[:-1], edges[1:])])


def compare(hw, sm):
    eh, xh = env_db(hw)
    es, xs = env_db(sm)
    oh, os_ = onset(xh) // 441, onset(xs) // 441
    n = min(len(eh) - oh, len(es) - os_)
    a, b = eh[oh:oh + n], es[os_:os_ + n]
    mask = (a > a.max() - 60) | (b > b.max() - 60)
    th, ts = third_octave(xh, onset(xh)), third_octave(xs, onset(xs))
    return a, b, mask, th, ts


def main():
    a = sys.argv[1:]
    d = mr_ref.opt(a, "--dir", "../../roms/motif-rack-es/capture/presets")
    cap = mr_ref.opt(a, "--capture", "../../roms/motif-rack-es/capture")
    render = mr_ref.opt(a, "--render", "sampler_render.exe")
    voices = mr_ref.opt(a, "--voices", "00_011").split(",")
    notes = [tuple(int(v) for v in s.split(":")) for s in mr_ref.opt(a, "--notes", "60:100,60:40,60:127,48:100,72:100").split(",")]
    hold = float(mr_ref.opt(a, "--hold", "2"))
    os.makedirs(d, exist_ok=True)
    pdir = os.path.join(d, "patches")
    os.makedirs(pdir, exist_ok=True)

    if "--analyze-only" not in a:
        r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"),
                       mr_ref.opt(a, "--dev", "0"), d)
        mi = mr_midi.MidiIn(mr_ref.opt(a, "--midi-in", "MOTIF-RACK ES-1"))
        try:
            r.all_off()
            r.mode(0)
            for v in voices:
                js = json.load(open(os.path.join(cap, "voice_json", v + ".json"), encoding="utf-8"))
                mm, nn = int(v[:2], 16), int(v[3:6])
                msb, lsb = bank_select(mm)
                r.program(msb, lsb, nn)
                name = edit_name(mi, r.mo)
                if name != js["name"]:
                    print("%s: 名前が違う（実機 %r、JSON %r）ので飛ばす" % (v, name, js["name"]), flush=True)
                    continue
                r.cc(91, 0)
                r.cc(93, 0)
                for e in range(4):
                    r.el(e, 0x41, 0x16, 0)
                time.sleep(0.1)
                for k, vel in notes:
                    nm = "%s_k%03d_v%03d_hw" % (v, k, vel)
                    if not os.path.exists(os.path.join(d, nm + ".wav")):
                        r.one_note(nm, note=k, vel=vel, hold=hold, tail=2.5, pre=0.3)
                print("%s %s 録った" % (v, name), flush=True)
            r.all_off()
            r.mode(3)
        finally:
            mi.close()
            r.close()

    for v in voices:
        jp = os.path.join(cap, "voice_json", v + ".json")
        sys.argv = ["mr_patch.py", jp, os.path.join(cap, "samples"), pdir]
        mr_patch.main()
        patch = os.path.join(pdir, v + ".patch")
        rows = []
        for k, vel in notes:
            hw = os.path.join(d, "%s_k%03d_v%03d_hw.wav" % (v, k, vel))
            sm = os.path.join(d, "%s_k%03d_v%03d_sm.wav" % (v, k, vel))
            if not os.path.exists(hw):
                continue
            subprocess.run([render, patch, str(k), str(vel), str(hold), sm], check=True, stdout=subprocess.DEVNULL)
            rows.append((k, vel) + compare(hw, sm))
        if not rows:
            continue
        ref = [r_ for r_ in rows if r_[0] == 60 and r_[1] == 100] or rows[:1]
        a0, b0 = ref[0][2], ref[0][3]
        offset = float(np.median((a0 - b0)[ref[0][4]][:50]))      # 出だし 0.5 秒の差の中央値を音色の差とする
        print("%s（実機と芯の差 %.1fdB を引いて比べる）" % (v, offset))
        print("   鍵 ベロ  0.05s  0.2s  0.5s  1.0s  1.9s  離して0.3s  1.0s | 音量の差の中央値・最大 | スペクトルの差（1/3 オクターブ、平均・最大）")
        for k, vel, ah, bs, mask, th, ts in rows:
            diff = ah - (bs + offset)
            pts = []
            for t in (0.05, 0.2, 0.5, 1.0, 1.9, hold + 0.3, hold + 1.0):
                i = int(t / 0.01)
                pts.append("%+5.1f" % diff[i] if i < len(diff) and mask[i] else "   ・")
            sp = ""
            if th is not None and ts is not None:
                ds = (th - th.max()) - (ts - ts.max())
                ok = th > th.max() - 50
                sp = "%4.1f %4.1f" % (np.mean(np.abs(ds[ok])), np.max(np.abs(ds[ok])))
            m = diff[mask]
            print("  %3d %3d %s | %5.1f %5.1f | %s" % (k, vel, " ".join(pts), np.median(np.abs(m)), np.max(np.abs(m)), sp))


if __name__ == "__main__":
    main()
