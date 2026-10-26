// license:BSD-3-Clause
// sampler_fx_rt.h（その場で回すエフェクト）を、sampler_fx.h（音全体をまとめて処理する形）と比べる試験（2026-10-25）。
//   fxrt_test capture の置き場 音色.patch [音色.patch …]
// 音色を鍵 60 で 1 秒鳴らした音に、両方の形でインサーション（組とつなぎ方ごと）・リバーブ・コーラスの枠を掛け、差（最大の振幅に対する比）を出す。
// リバーブは尾を切ったので 2e-4、インサーションの EQ は 4096 タップに切ったので 1e-4、ほかは 1e-6 を越えたら失敗。
// オフラインが録った応答（.ltv）を使う型は、こちらが式で回す近似なので数に入れない。歪みの枠は、オフラインの LPF の応答に
// 先の音を使う尾（角がナイキストに近いと 1/n でゆっくり減る）があり、こちらは 64 サンプルだけ先読みするので 1e-2 まで許す（V-Distortion で 1.3e-3）。
// 揺れる型を録った応答で回す所（.ltv・.dyn・.mod、2026-10-27）は、差を二乗平均の比で見る（下）。
#include "sampler_engine.h"
#include "sampler_fx_rt.h"

#include <cstdio>

int main(int argc, char **argv)
{
	if (argc < 3) {
		std::fprintf(stderr, "usage: fxrt_test capture の置き場 音色.patch …\n");
		return 1;
	}
	const std::string cap = argv[1];
	int fails = 0;
	for (int a = 2; a < argc; a++) {
		motif_sampler::patch pt;
		if (!pt.load(argv[a]))
			continue;
		motif_sampler::engine eg(pt);
		const int n = 44100 * 4;
		std::vector<float> l(n), r(n), voff(n);
		std::vector<float> gl[3] = {std::vector<float>(n), std::vector<float>(n), std::vector<float>(n)};
		std::vector<float> gr[3] = {std::vector<float>(n), std::vector<float>(n), std::vector<float>(n)};
		float *gp[3] = {gl[0].data(), gl[1].data(), gl[2].data()}, *rp[3] = {gr[0].data(), gr[1].data(), gr[2].data()};
		eg.note_on(60, 100);
		eg.render(44100, l.data(), r.data(), gp, rp, voff.data());
		eg.note_off(60);
		for (int s = 0; s < 3; s++)
			gp[s] += 44100, rp[s] += 44100;
		eg.render(n - 44100, l.data() + 44100, r.data() + 44100, gp, rp, voff.data() + 44100);
		auto get = [&](const char *k) { auto it = pt.fxline.find(k); return it == pt.fxline.end() ? motif_fx::block() : motif_fx::parse_block(it->second); };
		const motif_fx::block rv = get("reverb"), ch = get("chorus"), ia = get("insa"), ib = get("insb");
		const int tempo = motif_sampler::geti(pt.fxline, "tempo", 120), connect = motif_sampler::geti(pt.fxline, "connect");
		// インサーション（共通の音量を掛ける前の組の音）
		motif_fx::buf g[3][2], IL, IR;
		for (int s = 0; s < 3; s++) {
			g[s][0].assign(gl[s].begin(), gl[s].end());
			g[s][1].assign(gr[s].begin(), gr[s].end());
		}
		motif_fx::insertion_groups(g, ia, ib, connect, cap + "/fxd", tempo, IL, IR);
		motif_fx_rt::insertion_rt irt;
		irt.init(ia, ib, connect, tempo, cap + "/fxd");
		double ei = 0, mi = 0, se = 0, ss = 0;
		motif_fx::buf L(n), R(n);
		for (int i = 0; i < n; i++) {
			const double xl[3] = {gl[0][i], gl[1][i], gl[2][i]}, xr[3] = {gr[0][i], gr[1][i], gr[2][i]};
			double a1, b1;
			irt.tick(xl, xr, a1, b1);
			// 歪みの枠があると、その場の出口は latency() サンプル遅れる
			const int li = i - irt.latency();
			if (li >= 0) {
				ei = std::max(ei, std::max(std::fabs(a1 - IL[li]), std::fabs(b1 - IR[li])));
				se += (a1 - IL[li]) * (a1 - IL[li]) + (b1 - IR[li]) * (b1 - IR[li]);
				ss += IL[li] * IL[li] + IR[li] * IR[li];
			}
			mi = std::max(mi, std::max(std::fabs(IL[i]), std::fabs(IR[i])));
			// リバーブ・コーラスの枠へは、インサーションを通して共通の音量を掛けた音（オフラインと同じ）
			const double vg = eg.volume_gain(voff[i]);
			L[i] = IL[i] * vg;
			R[i] = IR[i] * vg;
		}
		motif_fx::buf orL(n, 0.0), orR(n, 0.0), ocL(n, 0.0), ocR(n, 0.0);
		motif_fx::reverb(L, R, rv, cap + "/fx", orL, orR, n);
		motif_fx::system_chorus(L, R, ch, ocL, ocR, cap + "/fxd", tempo);
		motif_fx_rt::reverb_rt rrt;
		motif_fx_rt::chorus_rt crt;
		const bool rok = rrt.init(rv, cap + "/fx"), cok = crt.init(ch, tempo, cap + "/fxd");
		double er = 0, mr = 0, ec = 0, mc = 0;
		for (int i = 0; i < n; i++) {
			double a1 = 0, b1 = 0, a2 = 0, b2 = 0;
			if (rok)
				rrt.tick(L[i], R[i], a1, b1);
			if (cok)
				crt.tick(L[i], R[i], a2, b2);
			er = std::max(er, std::max(std::fabs(a1 - orL[i]), std::fabs(b1 - orR[i])));
			mr = std::max(mr, std::max(std::fabs(orL[i]), std::fabs(orR[i])));
			ec = std::max(ec, std::max(std::fabs(a2 - ocL[i]), std::fabs(b2 - ocR[i])));
			mc = std::max(mc, std::max(std::fabs(ocL[i]), std::fabs(ocR[i])));
		}
		const double rr = mr > 0 ? er / mr : 0, rc = mc > 0 ? ec / mc : 0, ri = irt.recorded() ? (ss > 0 ? std::sqrt(se / ss) : 0) : mi > 0 ? ei / mi : 0;
		// 近似（オフラインは録った応答の STFT、こちらは式）は数に入れない
		// 録った応答をその場の形に直した型（.ltv・.dyn・.mod）は STFT と違うので、差は二乗平均の比で見て 0.7 まで
		// （ノイズで Vintage Wah 0.13、Phaser 0.03、Rotary 0.01）。中に間引きや雑音のある Lo-Fi 系（0D xx）は時間ごとの応答で表せず、
		// オフラインとも合わない（Lo-Res はノイズで 0.65、音程のある音で 5dB 大きい）ので数に入れない
		const bool lofi = irt.recorded() && (ia.msb == 13 || ib.msb == 13);
		const bool ins_ok = irt.supported() && !irt.approx() && !lofi, cho_ok = cok && !crt.approx();
		const bool bad = (rok && rr > 2e-4) || (cho_ok && rc > 1e-6) || (!cok && mc > 0 && ch.send) || (ins_ok && ri > (irt.recorded() ? 0.7 : irt.latency() ? 1e-2 : 1e-4));
		fails += bad;
		std::printf("%s %s: インサーション %02X%02X→%02X%02X つなぎ %d %s（差 %.1e）・リバーブ %02X%02X %s（差 %.1e）・コーラスの枠 %02X%02X %s（差 %.1e）\n",
		            bad ? "失敗" : "ok", pt.name.c_str(), ia.msb, ia.lsb, ib.msb, ib.lsb, connect,
		            irt.approx() ? "近似" : lofi ? "録った応答（Lo-Fi 系、数えない）" : irt.recorded() && ins_ok ? "録った応答" : ins_ok ? "回せる" : "まだ", ri, rv.msb, rv.lsb, rok ? "回せる" : "無し", rr, ch.msb, ch.lsb,
		            crt.approx() ? "近似" : cok ? "回せる" : (ch.send ? "まだ" : "送り 0"), rc);
	}
	std::printf("fxrt_test: %s\n", fails ? "失敗あり" : "OK");
	return fails ? 1 : 0;
}
