// アルペジオ（sampler_arp.h）を鳴らさずに回し、出したノートを書き出す（2026-10-25）。tools/motif/mr_arp_check.py が実機の録音と比べる。
//   arp_dump 表.txt 型 鍵,鍵,... [ベロシティ] [拍の数] [テンポ] [長さの倍率の値] [キーモード]
// 出力は 1 行に 1 つ「拍 鍵 ベロシティ 長さ（拍）」（ノートオン順）。鍵は拍 0 で押し、拍の数のところで離す。
#include "sampler_arp.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>

int main(int argc, char **argv)
{
	if (argc < 4) {
		std::fprintf(stderr, "arp_dump prog.txt type keys [vel] [beats] [tempo] [unit] [keymode]\n");
		return 2;
	}
	motif_arp::library lib;
	if (!lib.load(argv[1])) {
		std::fprintf(stderr, "cannot read %s\n", argv[1]);
		return 1;
	}
	motif_arp::settings s;
	s.type = std::atoi(argv[2]);
	s.sw = true;
	const int vel = argc > 4 ? std::atoi(argv[4]) : 100;
	const double beats = argc > 5 ? std::atof(argv[5]) : 4;
	s.tempo = argc > 6 ? std::atoi(argv[6]) : 120;
	s.unit = argc > 7 ? std::atoi(argv[7]) : 3;
	s.key = argc > 8 ? std::atoi(argv[8]) : 0;
	s.qs = 0;
	motif_arp::arpeggiator a;
	a.set_library(&lib);
	a.set(s);
	if (!a.program()) {
		std::fprintf(stderr, "no type %d\n", s.type);
		return 1;
	}
	std::istringstream ks(argv[3]);
	std::string k;
	std::vector<int> keys;
	while (std::getline(ks, k, ','))
		keys.push_back(std::atoi(k.c_str()));
	for (int x : keys)
		a.key_on(x, vel);
	const double rate = 44100;
	const int block = 64;
	const double spb = 60.0 / s.tempo * motif_arp::unit_factor(s.unit) * rate;   // 1 拍のサンプル数
	const long total = long(beats * spb);
	std::map<int, std::pair<long, int>> on;   // 鍵 → （始まり、ベロシティ）
	struct note { long t; int key, vel; long len; };
	std::vector<note> out;
	long pos = 0;
	bool released = false;
	while (pos < total + long(spb)) {
		if (!released && pos >= total) {
			for (int x : keys)
				a.key_off(x);
			released = true;
		}
		a.process(block, rate, [&](int off, int type, int key, int v) {
			const long t = pos + off;
			if (type == motif_arp::OUT_ON) {
				on[key] = {t, v};
			} else if (type == motif_arp::OUT_OFF) {
				auto it = on.find(key);
				if (it != on.end()) {
					out.push_back({it->second.first, key, it->second.second, t - it->second.first});
					on.erase(it);
				}
			}
		});
		pos += block;
	}
	for (auto &x : on)
		out.push_back({x.second.first, x.first, x.second.second, -1});
	std::sort(out.begin(), out.end(), [](const note &a, const note &b) { return a.t != b.t ? a.t < b.t : a.key < b.key; });
	for (auto &n : out)
		std::printf("%.4f %d %d %.4f\n", n.t / spb, n.key, n.vel, n.len < 0 ? -1.0 : n.len / spb);
	return 0;
}
