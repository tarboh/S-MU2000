// license:BSD-3-Clause
// 実機の dry を芯の block_rt に通す。初めに組のパラメーターで作り、すぐに p=v を set_param で替える（つまみで動いた値の代わり）。
//   fxsim ブロック fxd dry.wav 出口.wav [p=v …] [tempo=n]
// ブロックは「msb × 128 + lsb,p1,…,p16」。tools/motif/mr_fx_ltv_wet.py が .ltv の揺れの線の直しを測るのに使う。
//   g++ -std=c++20 -O2 -I src -I src/compat src/motif/fxsim.cpp -o build/fxsim.exe
#include <algorithm>
#include "motif/sampler_fx_rt.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static bool read_wav(const char *path, std::vector<float> &L, std::vector<float> &R)
{
	FILE *f = std::fopen(path, "rb");
	if (!f)
		return false;
	std::vector<unsigned char> b;
	unsigned char buf[65536];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
		b.insert(b.end(), buf, buf + n);
	std::fclose(f);
	size_t p = 12;
	int fmt = 0, ch = 2, bits = 32;
	while (p + 8 <= b.size()) {
		const unsigned sz = b[p + 4] | b[p + 5] << 8 | b[p + 6] << 16 | unsigned(b[p + 7]) << 24;
		if (!std::memcmp(&b[p], "fmt ", 4)) {
			fmt = b[p + 8] | b[p + 9] << 8;
			ch = b[p + 10];
			bits = b[p + 22];
		} else if (!std::memcmp(&b[p], "data", 4)) {
			const size_t frames = sz / (ch * bits / 8);
			for (size_t i = 0; i < frames; i++) {
				float v[2];
				for (int c = 0; c < 2; c++) {
					const unsigned char *q = &b[p + 8 + (i * ch + std::min(c, ch - 1)) * bits / 8];
					if (fmt == 3)
						std::memcpy(&v[c], q, 4);
					else if (bits == 24)
						v[c] = float(int(unsigned(q[0]) << 8 | unsigned(q[1]) << 16 | unsigned(q[2]) << 24) / 2147483648.0);
					else
						v[c] = short(q[0] | q[1] << 8) / 32768.0f;
				}
				L.push_back(v[0]);
				R.push_back(v[1]);
			}
			return true;
		}
		p += 8 + sz + (sz & 1);
	}
	return false;
}

static void write_wav(const char *path, const std::vector<float> &L, const std::vector<float> &R)
{
	FILE *f = std::fopen(path, "wb");
	const unsigned data = unsigned(L.size() * 8);
	auto u32 = [&](unsigned x) { std::fwrite(&x, 4, 1, f); };
	auto u16 = [&](unsigned short x) { std::fwrite(&x, 2, 1, f); };
	std::fwrite("RIFF", 1, 4, f); u32(36 + data); std::fwrite("WAVEfmt ", 1, 8, f);
	u32(16); u16(3); u16(2); u32(44100); u32(44100 * 8); u16(8); u16(32);
	std::fwrite("data", 1, 4, f); u32(data);
	for (size_t i = 0; i < L.size(); i++) {
		std::fwrite(&L[i], 4, 1, f);
		std::fwrite(&R[i], 4, 1, f);
	}
	std::fclose(f);
}

int main(int argc, char **argv)
{
	if (argc < 5)
		return 1;
	motif_fx::block b = motif_fx::parse_block(argv[1]);
	std::vector<float> dL, dR;
	if (!read_wav(argv[3], dL, dR))
		return 2;
	int tempo = 120;
	for (int i = 5; i < argc; i++)
		std::sscanf(argv[i], "tempo=%d", &tempo);
	motif_fx_rt::block_rt bk;
	bk.init(b, tempo, argv[2]);
	if (!bk.supported())
		return 3;
	for (int i = 5; i < argc; i++) {
		int p = 0, v = 0;
		if (std::sscanf(argv[i], "%d=%d", &p, &v) == 2)
			bk.set_param(p, v);
	}
	const int lat = bk.latency();
	std::vector<float> oL(dL.size()), oR(dL.size());
	for (size_t i = 0; i < dL.size(); i++) {
		double l = dL[i], r = dR[i];
		bk.tick(l, r);
		if (i >= size_t(lat)) {
			oL[i - lat] = float(l);
			oR[i - lat] = float(r);
		}
	}
	write_wav(argv[4], oL, oR);
	return 0;
}
