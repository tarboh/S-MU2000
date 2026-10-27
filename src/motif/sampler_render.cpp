// license:BSD-3-Clause
// サンプリング方式の芯（sampler.h・sampler_engine.h）で、パッチ（tools/motif/mr_patch.py が作る文字ファイル）の音を鳴らして WAV に書く。
//
//   g++ -std=c++17 -O2 -o sampler_render src/motif/sampler_render.cpp
//   sampler_render 音色.patch 鍵 ベロシティ 押さえる秒 出力.wav [--fx capture の置き場] [--no-ins] [--no-reverb] [--no-chorus] [--ins-majority]
//                  [--ctl ソース=値@秒,…] [--note-index n] [--seed s] [--events on@秒:鍵:ベロシティ,off@秒:鍵,…] [--threads n]
//
// --fx を付けると、パッチの fx の行のエフェクト（sampler_fx.h）を掛ける。置き場の fx/*.irx（リバーブ）と fxd/*.dsx（歪み系）を使う
// （tools/motif/mr_fx_export.py が書く）。インサーションのスイッチは、鳴ったエレメントでいちばん多いもの。
//
// --ctl は、コントロールセットのソース（0 PB・1 MW（CC1）・2/3 AT・6 RB（CC22）・7 BC（CC2）・8 AS1（CC16）・9 AS2（CC17）・10〜12 FC（CC4））を
// その時刻から値（0〜127）にする。初めは PB・RB・AS1・AS2 が 64（中心）、ほかは 0。PB はピッチベンドの幅（ctrl の行の pbup・pbdown）でも音程を動かす。
//
// --events を付けると、鍵・ベロシティ・押さえる秒の代わりにその並びを鳴らす（和音・モノ・ポルタメント・同じ鍵の重ね・発音数の奪い合い。
// 音色の voice の行の mono・same・porta・pmode・ptime・ptmode を使う。tools/motif/mr_multi.py、2026-10-25）。出力はいちばん遅い出来事から 3 秒。
//
// --threads は、エレメントを描く糸の数（結果は変わらない。速くなるだけ）。
//
// --note-index は、音色を選んでから何音目か（0 から。オルタネートパンの向き）。--seed を付けると、ランダムパンとランダムピッチをその種で振る
// （付けなければ 0。実機は毎回違う並びなので、1 本ずつは合わせられない）。
//
// パッチのエレメントのうち、鍵とベロシティの範囲に入るものを全部鳴らす。サンプルは、そのエレメントの区間のうち
// 鍵とベロシティを含むもの（無ければ鍵がいちばん近いもの）。出力は 44.1kHz・32bit float のステレオ。

#include "sampler_engine.h"
#include "sampler_fx.h"

#include <cmath>

namespace {

void write_wav(const char *path, const std::vector<float> &l, const std::vector<float> &r)
{
	FILE *f = std::fopen(path, "wb");
	if (!f)
		return;
	const uint32_t n = uint32_t(l.size()), data = n * 8;
	auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
	auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
	std::fwrite("RIFF", 1, 4, f); u32(36 + data); std::fwrite("WAVEfmt ", 1, 8, f);
	u32(16); u16(3); u16(2); u32(44100); u32(44100 * 8); u16(8); u16(32);
	std::fwrite("data", 1, 4, f); u32(data);
	for (uint32_t i = 0; i < n; i++) {
		std::fwrite(&l[i], 4, 1, f);
		std::fwrite(&r[i], 4, 1, f);
	}
	std::fclose(f);
}

} // namespace

int main(int argc, char **argv)
{
	using motif_sampler::geti;
	if (argc < 6) {
		std::fprintf(stderr, "usage: sampler_render 音色.patch 鍵 ベロシティ 押さえる秒 出力.wav\n");
		return 1;
	}
	const std::string patch = argv[1];
	const int note = std::atoi(argv[2]), vel = std::atoi(argv[3]);
	const double hold = std::atof(argv[4]);

	std::string fxdir;
	struct ctl_ev { double t; int src, val; };
	std::vector<ctl_ev> ctl;
	int note_index = 0, threads = 1;
	long seed = -1;
	struct note_ev { double t; bool on; int key, vel; };
	std::vector<note_ev> nev;
	bool use_ins = true, use_rev = true, use_cho = true, majority = false;
	for (int i = 6; i < argc; i++) {
		if (!std::strcmp(argv[i], "--fx") && i + 1 < argc)
			fxdir = argv[++i];
		else if (!std::strcmp(argv[i], "--no-ins"))
			use_ins = false;
		else if (!std::strcmp(argv[i], "--no-reverb"))
			use_rev = false;
		else if (!std::strcmp(argv[i], "--no-chorus"))
			use_cho = false;
		else if (!std::strcmp(argv[i], "--ctl") && i + 1 < argc) {
			std::istringstream ss(argv[++i]);
			std::string t;
			while (std::getline(ss, t, ',')) {
				const size_t e = t.find('='), a = t.find('@');
				if (e == std::string::npos)
					continue;
				ctl.push_back({ a == std::string::npos ? 0.0 : std::atof(t.c_str() + a + 1), std::atoi(t.c_str()), std::atoi(t.c_str() + e + 1) });
			}
		}
		else if (!std::strcmp(argv[i], "--events") && i + 1 < argc) {
			std::istringstream ss(argv[++i]);
			std::string t;
			while (std::getline(ss, t, ',')) {
				double tt = 0;
				int k = 60, v = 100;
				if (!std::strncmp(t.c_str(), "on@", 3) && std::sscanf(t.c_str() + 3, "%lf:%d:%d", &tt, &k, &v) >= 2)
					nev.push_back({ tt, true, k, v });
				else if (!std::strncmp(t.c_str(), "off@", 4) && std::sscanf(t.c_str() + 4, "%lf:%d", &tt, &k) == 2)
					nev.push_back({ tt, false, k, 0 });
			}
		}
		else if (!std::strcmp(argv[i], "--note-index") && i + 1 < argc)
			note_index = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--threads") && i + 1 < argc)
			threads = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc)
			seed = std::atol(argv[++i]);
		else if (!std::strcmp(argv[i], "--ins-majority"))      // エレメントのスイッチの多数決で全体を 1 つの経路に通す（前の形。mr_fx_apply と同じ）
			majority = true;
	}
	motif_sampler::patch pt;
	if (!pt.load(patch)) {
		std::fprintf(stderr, "パッチを開けない: %s\n", patch.c_str());
		return 1;
	}
	const motif_sampler::kv &fxline = pt.fxline;
	// インサーションのスイッチは、鳴る鳴らないによらず入っているエレメント全部の多数決（mr_fx_apply.apply_insertion と同じ）
	std::map<int, int> ins_count;
	for (const motif_sampler::element &e : pt.els)
		ins_count[geti(e.f, "ins")]++;
	motif_sampler::engine::options opt;
	opt.note_index = note_index;
	opt.seed = seed;
	motif_sampler::engine eg(pt, opt);
	eg.set_threads(threads);

	// 鳴らす音の並び（--events が無ければ、鍵・ベロシティを 0 秒から押さえる秒まで）
	if (nev.empty()) {
		nev.push_back({ 0.0, true, note, vel });
		nev.push_back({ hold, false, note, 0 });
	}
	std::stable_sort(nev.begin(), nev.end(), [](const note_ev &a, const note_ev &b) { return a.t < b.t; });
	std::stable_sort(ctl.begin(), ctl.end(), [](const ctl_ev &a, const ctl_ev &b) { return a.t < b.t; });
	const int n = int((nev.back().t + 3.0) * 44100);
	std::vector<float> l(n), r(n), voff(n);
	std::vector<float> gl[3] = {std::vector<float>(n), std::vector<float>(n), std::vector<float>(n)};
	std::vector<float> gr[3] = {std::vector<float>(n), std::vector<float>(n), std::vector<float>(n)};
	// 出来事はそのサンプル（時刻 × 44100 を越えた最初のサンプル）で、ソースの値、音の順に効かせ、次の出来事までまとめて描く
	size_t ci = 0, ei = 0;
	for (int i = 0; i < n;) {
		while (ci < ctl.size() && ctl[ci].t * 44100 <= i) {
			eg.set_source(ctl[ci].src, ctl[ci].val);
			ci++;
		}
		while (ei < nev.size() && nev[ei].t * 44100 <= i) {
			if (nev[ei].on)
				eg.note_on(nev[ei].key, nev[ei].vel);
			else
				eg.note_off(nev[ei].key);
			ei++;
		}
		int next = n;
		if (ci < ctl.size())
			next = std::min(next, std::max(i + 1, int(std::ceil(ctl[ci].t * 44100))));
		if (ei < nev.size())
			next = std::min(next, std::max(i + 1, int(std::ceil(nev[ei].t * 44100))));
		float *gp[3] = { gl[0].data() + i, gl[1].data() + i, gl[2].data() + i };
		float *rp[3] = { gr[0].data() + i, gr[1].data() + i, gr[2].data() + i };
		eg.render(next - i, l.data() + i, r.data() + i, gp, rp, voff.data() + i);
		i = next;
	}
	// 共通の音量（0〜127）は 40·log10(値 / 127) dB（プリセット 29 音色の実機と芯の音量の差から）。実機ではインサーションの後で掛かる
	// （強く歪む Still Blue で、前に掛けると 11dB 大きく出た）ので、インサーションを通すときは前で戻して後で掛け直す
	// コントロールセットの Volume は、音量の値をずらしてから同じ式で掛ける
	std::vector<double> vgv(n);
	for (int i = 0; i < n; i++)
		vgv[i] = eg.volume_gain(voff[i]);
	for (int i = 0; i < n; i++) {
		l[i] = float(l[i] * vgv[i]);
		r[i] = float(r[i] * vgv[i]);
	}
	if (!fxdir.empty() && !fxline.empty()) {
		using namespace motif_fx;
		auto get = [&](const char *k) { auto it = fxline.find(k); return it == fxline.end() ? block() : parse_block(it->second); };
		const block rv = get("reverb"), ch = get("chorus"), ia = get("insa"), ib = get("insb");
		int sw = 0, best = -1;
		for (auto &kc : ins_count)
			if (kc.second > best) {
				best = kc.second;
				sw = kc.first;
			}
		buf L(l.begin(), l.end()), R(r.begin(), r.end());
		if (use_ins) {
			for (size_t i = 0; i < L.size(); i++) {
				const double g = i < vgv.size() && vgv[i] > 0 ? vgv[i] : 1.0;
				L[i] /= g;
				R[i] /= g;
			}
			if (majority)
				insertion(L, R, ia, ib, geti(fxline, "connect"), sw, fxdir + "/fxd", geti(fxline, "tempo", 120));
			else {
				// エレメントをスイッチごとの組に分けて、つなぎ方どおりに通す（--ins-majority で前の多数決に戻せる。mr_fx_apply.apply_insertion は多数決）
				buf g[3][2];
				for (int s2 = 0; s2 < 3; s2++)
					for (int c = 0; c < 2; c++) {
						const std::vector<float> &src = c == 0 ? gl[s2] : gr[s2];
						g[s2][c].assign(src.begin(), src.end());      // 組の音は共通の音量を掛ける前のもの
					}
				insertion_groups(g, ia, ib, geti(fxline, "connect"), fxdir + "/fxd", geti(fxline, "tempo", 120), L, R);
			}
			for (size_t i = 0; i < L.size(); i++) {
				const double g = i < vgv.size() ? vgv[i] : vgv.back();
				L[i] *= g;
				R[i] *= g;
			}
		}
		buf oL(L), oR(R);
		if (use_rev)
			reverb(L, R, rv, fxdir + "/fx", oL, oR, L.size());
		// コーラスからリバーブへの送り（fx の chorev、0〜127）: コーラスの Return の前の wet × chorev / 127 を、送り 127 のリバーブにも通す
		// （実機で測った。tools/motif/mr_cho_rev.py、2026-10-28。リバーブは線形なので、元の音の分と分けて足しても同じ）
		const int chorev = geti(fxline, "chorev");
		if (use_rev && use_cho && chorev > 0 && ch.send > 0) {
			block pre = ch;
			pre.ret = 96;
			buf pL(L.size(), 0.0), pR(R.size(), 0.0);
			system_chorus(L, R, pre, pL, pR, fxdir + "/fxd", geti(fxline, "tempo", 120));
			for (size_t i = 0; i < pL.size(); i++) {
				pL[i] *= chorev / 127.0;
				pR[i] *= chorev / 127.0;
			}
			block r127 = rv;
			r127.send = 127;
			reverb(pL, pR, r127, fxdir + "/fx", oL, oR, L.size());
		}
		if (use_cho)
			system_chorus(L, R, ch, oL, oR, fxdir + "/fxd", geti(fxline, "tempo", 120));
		for (size_t i = 0; i < l.size(); i++) {
			l[i] = float(oL[i]);
			r[i] = float(oR[i]);
		}
	}
	// 実機の出口（デジタル出力）は ±1.0 で頭打ち（発振した LPF24D をフィルターゲイン 250 で鳴らすと、山が 0dBFS の四角い波になる）
	for (size_t i = 0; i < l.size(); i++) {
		l[i] = std::fmax(-1.0f, std::fmin(1.0f, l[i]));
		r[i] = std::fmax(-1.0f, std::fmin(1.0f, r[i]));
	}
	write_wav(argv[5], l, r);
	std::printf("鳴らしたエレメント %zu\n", eg.elements_started());
	return 0;
}
