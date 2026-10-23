"""プリセットの音色を実機で鳴らして録り、サンプリング方式の芯（sampler_render）で同じ鍵・ベロシティを鳴らして比べる。
  python mr_preset_compare.py --voices 00_011,06_006 [--notes 60:100,60:40] [--hold 2] [--dir 置き場]
                              [--capture 取ったものの置き場] [--render sampler_render.exe] [--analyze-only] [--tweak nofeg] [--reverb] [--fx] [--ins]

音色は voice_json/<mm>_<nn>.json（mr_voice.py --json）で選ぶ。mm がバンク（00〜05 = PRE1〜6 → MSB 63・LSB mm、06 = GM → MSB 0・LSB 0、
08〜0A = USR1〜3 → MSB 63・LSB 8〜10）、nn が番号。音色を選んだあと、エディットバッファ（0E 0F 00）を一括ダンプで受けて、
名前が JSON と同じか確かめてから録る（違えば飛ばす）。
録る前に、リバーブ・コーラスへの送り（CC91・CC93）を 0 にし、エレメント 1〜4 のインサーションのスイッチ（41 ee 16）を切る
（エディットバッファを変えるだけで保存しない）。アルペジオ（40 70 16）も切る。共通の音量やマスター EQ はそのまま。
実機の録音は <置き場>/<音色>_k<鍵>_v<ベロシティ>_hw.wav、芯で鳴らしたものは _sm.wav。
--tweak を付けると、実機と芯の両方で部品を 1 つ切って録る（nofeg = FEG の深さ 0、noscale = カットオフのスケーリング 0、nofilter = フィルター THRU）。
名前の後ろに _<tweak> が付く。--reverb は、実機の CC91 を音色の送りにして（リバーブを残して）録り、芯の側も mr_fx_apply.py で
リバーブを掛けて比べる（名前の後ろに _rv）。--fx はコーラスも残す（CC93 も音色の送り、名前の後ろに _fx）。
--ins はインサーションを残し（エレメントのスイッチを切らない）、芯の側も sampler_render --fx で掛ける（エレメントをスイッチごとの組に分ける。名前の後ろに _ins）。
比べるのは、10ms ごとの音量（dB）の動きと、0.1〜0.6 秒の平均のスペクトル（1/3 オクターブ）。絶対の音量は共通の音量などでずれるので、
音色ごとに「ベロシティ 100・鍵 60」の差を引いてから比べる。終わったらマルチへ戻す。録った音は配らないこと（ヤマハの音）。"""
import json
import os
import subprocess
import sys
import time

import numpy as np

import re

import mr_midi
import mr_patch
import mr_ref

FS = 44100

# 切り分け（--tweak）: 実機のエディットバッファに送るパラメーターチェンジ（エレメント 1〜4 に同じもの）と、パッチの書き換え
TWEAKS = {
    "nofeg": ([(0x43, 0x1D, (64,))], {"fegd": "64"}),
    "noscale": ([(0x43, 0x27 + 2 * i, (1, 0)) for i in range(4)], {"off": "128,128,128,128"}),
    "nofilter": ([(0x43, 0x00, (21,))], {"ftype": "21"}),
}


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
    # 直流を除く（インサーションによっては実機の出力に −57dB ほどの直流が乗り、鳴り終わったあとも残る。耳には聞こえない）。50ms の移動平均を引く
    w = int(0.05 * FS)

    def dc(y):
        y = np.asarray(y, dtype=np.float64)
        c = np.cumsum(np.concatenate([[0.0], np.pad(y, (w // 2, w - w // 2), mode="edge")]))   # 端は端の値で延ばす（0 だと直流が段になって出だしと取り違える）
        return y - (c[w:w + len(y)] - c[:len(y)]) / w
    L, R = dc(L), dc(R)
    x = L + R
    n = int(win * FS)
    m = len(x) // n
    # 音量は左右の電力の和で測る（2 倍して、左右が同じなら L+R と同じ値）。左右に振った同じ高さのエレメントが L+R で打ち消し合うのは、聞こえ方と違う
    p = 2 * (L[:m * n] ** 2 + R[:m * n] ** 2).reshape(m, n).mean(axis=1) / 4
    return 10 * np.log10(4 * p + 1e-18), x


def onset(x):
    # 前の音の名残り（実機の録音で −70〜−80dB）を出だしと取り違えないよう、いちばん大きい所から −40dB も見る
    return int(np.argmax(np.abs(x) > max(1e-4, 0.01 * float(np.max(np.abs(x))))))


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
    tweak = mr_ref.opt(a, "--tweak", "")
    reverb = "--reverb" in a or "--fx" in a        # リバーブを残して録り、芯の側も mr_fx_apply.py で掛けて比べる
    chorus = "--fx" in a                           # コーラスも残す
    ins = "--ins" in a                             # インサーションを残す（エレメントのスイッチを切らない）。芯の側も mr_fx_apply.apply_insertion
    sfx = ("_" + tweak if tweak else "") + ("_fx" if chorus else "_rv" if reverb else "") + ("_ins" if ins else "")
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
                r.param(0x40, 0x70, 0x16, 0)                # アルペジオを切る（1 音で比べる。ARP Switch、エディットバッファだけ）
                r.cc(91, js["common"].get("reverb_send", 0) if reverb else 0)
                r.cc(93, js["common"].get("chorus_send", 0) if chorus else 0)
                for e in range(4):
                    if not ins:
                        r.el(e, 0x41, 0x16, 0)
                    for blk, off, data in (TWEAKS[tweak][0] if tweak else []):
                        r.el(e, blk, off, *data)
                time.sleep(0.1)
                for k, vel in notes:
                    nm = "%s_k%03d_v%03d%s_hw" % (v, k, vel, sfx)
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
        js = json.load(open(jp, encoding="utf-8"))
        js["arp_tempo"] = mr_patch.voice_tempo(jp)
        sys.argv = ["mr_patch.py", jp, os.path.join(cap, "samples"), pdir]
        mr_patch.main()
        patch = os.path.join(pdir, v + ".patch")
        if tweak:
            t = open(patch).read()
            for key, val in TWEAKS[tweak][1].items():
                t = re.sub(r"(?<= )%s=\S+" % key, "%s=%s" % (key, val), t)
            patch = os.path.join(pdir, "%s_%s.patch" % (v, tweak))
            open(patch, "w", newline="\n").write(t)
        rows = []
        for k, vel in notes:
            hw = os.path.join(d, "%s_k%03d_v%03d%s_hw.wav" % (v, k, vel, sfx))
            sm = os.path.join(d, "%s_k%03d_v%03d%s_sm.wav" % (v, k, vel, sfx))
            if not os.path.exists(hw):
                continue
            cmd = [render, patch, str(k), str(vel), str(hold), sm]
            if ins:
                # インサーションは芯（C++）で通す。エレメントをスイッチごとの組に分けて、つなぎ方どおりに通す（mr_fx_apply.apply_insertion は多数決）
                cmd += ["--fx", cap, "--no-reverb", "--no-chorus"]
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
            if reverb:
                import mr_fx_apply
                L, R = mr_ref.read_wav(sm)
                L, R = np.asarray(L), np.asarray(R)
                oL, oR, msg = mr_fx_apply.apply(L, R, js, os.path.join(cap, "fx"))
                if chorus:
                    cL, cR, msg2 = mr_fx_apply.apply_chorus(L, R, js)
                    oL[:len(L)] += cL - L
                    oR[:len(R)] += cR - R
                mr_fx_apply.write_wav(sm, oL, oR)
            rows.append((k, vel) + compare(hw, sm))
        if not rows:
            continue
        ref = [r_ for r_ in rows if r_[0] == 60 and r_[1] == 100] or rows[:1]
        a0, b0 = ref[0][2], ref[0][3]
        offset = float(np.median((a0 - b0)[ref[0][4]][:50]))      # 出だし 0.5 秒の差の中央値を音色の差とする
        print("%s%s（実機と芯の差 %.1fdB を引いて比べる）" % (v, sfx, offset))
        print("   鍵 ベロ  0.05s  0.2s  0.5s  1.0s  1.9s  離して0.3s  1.0s | 音量の差の中央値・最大 | スペクトルの差（1/3 オクターブ、平均・最大）")
        for k, vel, ah, bs, mask, th, ts in rows:
            fl = np.percentile(ah, 1) + 3                         # 実機の録音の床（雑音）より下は床にそろえる（芯は鳴り終わると −200dB まで落ちる）
            if fl > ah.max() - 40:                                # 実機が鳴りやまない録音には床が無い
                fl = -120.0
            diff = np.maximum(ah, fl) - np.maximum(bs + offset, fl)
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
