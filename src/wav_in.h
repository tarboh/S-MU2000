// license:BSD-3-Clause
//
// WAV を読んで、サンプリングに使える形（16bit・44.1kHz・1ch）にする。
// サンプリングの窓が「WAV ファイルから録る」に使う。PCM 8/16/24/32bit と 32bit 浮動小数、
// 何チャンネルでも、どのサンプリング周波数でも受ける（周波数は直線補間で 44.1kHz に直す）。
#ifndef S_MU2000_WAV_IN_H
#define S_MU2000_WAV_IN_H
#pragma once

#include "compat/mamecompat.h"
#include "sampling.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace smu2000 {

struct wav_data
{
	u32 rate = 0;
	std::vector<std::vector<float>> ch;     // チャンネルごと、-1..1
	size_t frames() const { return ch.empty() ? 0 : ch[0].size(); }
};

inline bool parse_wav(const std::vector<u8> &b, wav_data &out, std::string &err)
{
	auto u16le = [&](size_t o) { return u32(b[o] | b[o + 1] << 8); };
	auto u32le = [&](size_t o) { return u32(b[o]) | u32(b[o + 1]) << 8 | u32(b[o + 2]) << 16 | u32(b[o + 3]) << 24; };
	if (b.size() < 12 || std::memcmp(&b[0], "RIFF", 4) || std::memcmp(&b[8], "WAVE", 4)) {
		err = "not a WAV file";
		return false;
	}
	u32 fmt = 0, chans = 0, rate = 0, bits = 0;
	size_t data = 0, data_len = 0;
	for (size_t o = 12; o + 8 <= b.size();) {
		const u32 len = u32le(o + 4);
		if (!std::memcmp(&b[o], "fmt ", 4) && o + 8 + 16 <= b.size()) {
			fmt = u16le(o + 8);
			chans = u16le(o + 10);
			rate = u32le(o + 12);
			bits = u16le(o + 22);
			if (fmt == 0xfffe && len >= 26 && o + 8 + 26 <= b.size())
				fmt = u16le(o + 8 + 24);          // WAVE_FORMAT_EXTENSIBLE の中身の形式
		} else if (!std::memcmp(&b[o], "data", 4)) {
			data = o + 8;
			data_len = std::min<size_t>(len, b.size() - data);
		}
		o += 8 + len + (len & 1);
	}
	if (!data || !chans || !rate) {
		err = "no audio data in the WAV file";
		return false;
	}
	const bool flt = fmt == 3 && bits == 32;
	if (!(fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) && !flt) {
		err = "unsupported WAV format (PCM 8/16/24/32-bit or 32-bit float only)";
		return false;
	}
	const size_t bpf = size_t(bits / 8) * chans;
	const size_t n = data_len / bpf;
	out.rate = rate;
	out.ch.assign(chans, std::vector<float>(n));
	for (size_t i = 0; i < n; i++)
		for (u32 c = 0; c < chans; c++) {
			const size_t o = data + i * bpf + static_cast<size_t>(c) * (bits / 8);
			float v;
			if (flt) {
				const u32 w = u32le(o);
				std::memcpy(&v, &w, 4);
			} else if (bits == 8)
				v = (float(b[o]) - 128.0f) / 128.0f;
			else if (bits == 16)
				v = float(s16(u16le(o))) / 32768.0f;
			else if (bits == 24)
				v = float(s32(u32le(o - 1) & 0xffffff00) >> 8) / 8388608.0f;
			else
				v = float(double(s32(u32le(o))) / 2147483648.0);
			out.ch[c][i] = v;
		}
	return true;
}

// 選んだ入力（AD1 = 1 つ目のチャンネル、AD2 = 2 つ目、AD1+2 = 足したもの）を 44.1kHz・16bit に。
// 1ch のファイルは AD1 も AD2 も同じものとする
inline std::vector<s16> wav_for_sampling(const wav_data &w, sampling::source src, size_t max_frames)
{
	std::vector<s16> out;
	if (w.ch.empty() || !w.rate)
		return out;
	const std::vector<float> &a = w.ch[0];
	const std::vector<float> &b = w.ch.size() > 1 ? w.ch[1] : w.ch[0];
	auto at = [&](size_t i) {
		return src == sampling::source::ad1 ? a[i] : src == sampling::source::ad2 ? b[i] : a[i] + b[i];
	};
	const double step = double(w.rate) / double(sampling::SAMPLE_RATE);
	const size_t n = std::min(max_frames, size_t(double(w.frames()) / step));
	out.resize(n);
	for (size_t i = 0; i < n; i++) {
		const double pos = double(i) * step;
		const size_t k = size_t(pos);
		const double f = pos - double(k);
		const double v = k + 1 < w.frames() ? at(k) * (1.0 - f) + at(k + 1) * f : at(std::min(k, w.frames() - 1));
		out[i] = s16(std::clamp(std::lround(v * 32767.0), -32768L, 32767L));
	}
	return out;
}

} // namespace smu2000

#endif
