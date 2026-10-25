"""ドラムボイスを実機と芯（sampler_render）で鍵ごとに鳴らして比べる（2026-10-25）。
  python mr_drum_compare.py 置き場 [--kits 0,1] [--bank 20] [--vel 100] [--render sampler_render.exe] [--analyze-only]
ボイスモードでドラム（20 = PRE → 63/32、21 = GM → 127/0、28 = USER → 63/40）を選び、鳴る鍵を 1 つずつ鳴らして録る（<置き場>/<bank>_<nnn>_k<鍵>_hw.wav）。
CC91・CC93 は 0（リバーブ・コーラスへ送らない）、インサーションは鍵ごとのスイッチ（47 ee 11）を thru にする（編集バッファだけ。保存しない）。
芯の側は capture/patches_all/<bank>_<nnn>.patch を --fx 無しで鳴らす。比べるのは、0〜1 秒の音量（音色ごとの差の中央値を引く）・
1/3 オクターブのスペクトルの差の平均・左右の差。終わったらマルチへ戻す。録った音は配らないこと（ヤマハの音）。"""
import os
import statistics
import subprocess
import sys
import time

import numpy as np

import mr_ref
import mr_voice

BANKS = {"20": (63, 32), "21": (127, 0), "28": (63, 40)}


def feats(path, skip):
    x = np.array(mr_ref.read_wav(path))
    m = (x[0] + x[1]) / 2
    a = np.abs(m)
    on = int(np.argmax(a > max(a.max() * 0.01, 1e-6))) if skip else 0
    y = x[:, on:on + 44100]
    lv = 10 * np.log10((y ** 2).mean() + 1e-30)
    lr = 10 * np.log10(((y[0] ** 2).mean() + 1e-30) / ((y[1] ** 2).mean() + 1e-30))
    s = (y[0] + y[1]) / 2
    n = len(s)
    X = np.abs(np.fft.rfft(s * np.hanning(n))) ** 2
    f = np.fft.rfftfreq(n, 1 / 44100)
    edges = 31.5 * 2 ** (np.arange(0, 30) / 3)
    sp = np.array([10 * np.log10(X[(f >= lo) & (f < hi)].mean() + 1e-30) for lo, hi in zip(edges[:-1], edges[1:]) if hi < 18000])
    return lv, lr, sp


def main():
    a = sys.argv[1:]
    d = a[0]
    bank = mr_ref.opt(a, "--bank", "20")
    kits = [int(k) for k in mr_ref.opt(a, "--kits", "0").split(",")]
    vel = int(mr_ref.opt(a, "--vel", "100"))
    here = os.path.dirname(os.path.abspath(__file__))
    cap = os.path.join(here, "..", "..", "roms", "motif-rack-es", "capture")
    render = mr_ref.opt(a, "--render", os.path.join(here, "..", "..", "build", "sampler_render.exe"))
    os.makedirs(d, exist_ok=True)
    keysets = {}
    for kit in kits:
        b = mr_voice.blocks(os.path.join(cap, "drum", "%s_%03d.syx" % (bank, kit)))
        keysets[kit] = [24 + kk for kk in range(73) if b.get((0x47, kk, 0)) and b[(0x47, kk, 0)][0] and b[(0x47, kk, 0)][1] == 0]
    if "--analyze-only" not in a:
        r = mr_ref.Ref(mr_ref.opt(a, "--midi", "MOTIF-RACK ES-1"), mr_ref.opt(a, "--rec", "../../build/rec.exe"), mr_ref.opt(a, "--dev", "0"), d)
        try:
            r.all_off()
            r.param(0x00, 0x00, 0x34, 0)
            r.mode(0)
            for kit in kits:
                r.program(*BANKS[bank], kit)
                r.param(0x46, 0x70, 0x16, 0)          # アルペジオを切る（編集バッファ）
                r.cc(91, 0)
                r.cc(93, 0)
                for k in keysets[kit]:
                    r.param(0x47, k - 24, 0x11, 0)    # インサーション thru
                time.sleep(0.2)
                for k in keysets[kit]:
                    nm = "%s_%03d_k%03d_hw" % (bank, kit, k)
                    if not os.path.exists(os.path.join(d, nm + ".wav")):
                        r.all_off()
                        time.sleep(0.1)
                        r.one_note(nm, note=k, vel=vel, hold=1.2, tail=0.4, pre=0.3)
                print("ドラム %s_%03d 録った（%d 鍵）" % (bank, kit, len(keysets[kit])), flush=True)
        finally:
            r.all_off()
            r.mode(3)
            r.close()
    allv, alls, alllr = [], [], []
    for kit in kits:
        patch = os.path.join(cap, "patches_all", "%s_%03d.patch" % (bank, kit))
        rows = []
        for k in keysets[kit]:
            hw = os.path.join(d, "%s_%03d_k%03d_hw.wav" % (bank, kit, k))
            sm = os.path.join(d, "%s_%03d_k%03d_sm.wav" % (bank, kit, k))
            if not os.path.exists(hw):
                continue
            subprocess.run([render, patch, str(k), str(vel), "1.2", sm], check=True, stdout=subprocess.DEVNULL)
            h, s = feats(hw, True), feats(sm, True)
            rows.append((k, h[0] - s[0], h[1] - s[1], float(np.mean(np.abs((h[2] - h[2].max()) - (s[2] - s[2].max()))[h[2] > h[2].max() - 50]))))
        if not rows:
            continue
        off = statistics.median(r_[1] for r_ in rows)
        print("%s_%03d（実機と芯の差 %.1fdB を引いて）" % (bank, kit, off))
        for k, dv, dlr, dsp in rows:
            print("  鍵 %3d  音量 %+5.1f  左右 %+5.1f  スペクトル %4.1f" % (k, dv - off, dlr, dsp))
            allv.append(abs(dv - off))
            alls.append(dsp)
            alllr.append(abs(dlr))
    if allv:
        print("全部 %d 鍵: 音量の差 中央 %.2f 平均 %.2f・左右の差 中央 %.2f・スペクトルの差 中央 %.2f 平均 %.2f" % (
            len(allv), statistics.median(allv), statistics.mean(allv), statistics.median(alllr), statistics.median(alls), statistics.mean(alls)))


if __name__ == "__main__":
    main()
