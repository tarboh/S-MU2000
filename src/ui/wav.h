// license:BSD-3-Clause
//
// One WAV header, written once.
//
// This tree grew five copies of the same 44 bytes - the Apple and Linux output
// backends, the AU probe, live --wav and render - each of them looking local to
// whatever needed a file written. Two of them wrote the sizes in host byte
// order, which works here and would not survive a big-endian port. They all
// agree on the bytes, so one function does, explicitly little-endian.
//
// Nothing platform-specific lives here: the rate is an argument rather than a
// constant pulled from the audio interface, because a file format does not care
// what the machine's audio device is doing.

#ifndef S_MU2000_UI_WAV_H
#define S_MU2000_UI_WAV_H

#include "compat/cli_text.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ui {

// Canonical RIFF/WAVE, written little-endian on purpose.
inline void wav_u32(std::FILE *f, uint32_t v)
{
	const uint8_t b[4] = { uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24) };
	std::fwrite(b, 1, 4, f);
}

inline void wav_u16(std::FILE *f, uint16_t v)
{
	const uint8_t b[2] = { uint8_t(v), uint8_t(v >> 8) };
	std::fwrite(b, 1, 2, f);
}

// The header alone, for callers that write the samples themselves. frames is a
// frame count, not a sample count, so the sizes come out right for stereo.
// format 1 is PCM integer and 3 is IEEE float, which is the only other shape
// here (render's --float).
inline void write_wav_header(std::FILE *f, uint32_t frames, uint32_t rate,
                             uint16_t channels = 2, uint16_t format = 1, uint16_t bits = 16)
{
	const uint16_t sample_bytes = format == 3 ? 4 : uint16_t(bits / 8);
	const uint16_t align = uint16_t(channels * sample_bytes);
	const uint32_t data = frames * align;
	std::fwrite("RIFF", 1, 4, f);
	wav_u32(f, 36 + data);
	std::fwrite("WAVEfmt ", 1, 8, f);
	wav_u32(f, 16);
	wav_u16(f, format);
	wav_u16(f, channels);
	wav_u32(f, rate);
	wav_u32(f, rate * align);
	wav_u16(f, align);
	wav_u16(f, bits);
	std::fwrite("data", 1, 4, f);
	wav_u32(f, data);
}

// Header and samples in one call, which is what every caller except the
// streaming ones wanted. False means the file could not be written or the
// writing stopped early, with the reason in err.
inline bool write_wav(const std::string &path, const std::vector<int16_t> &pcm,
                      std::string &err, uint32_t rate, uint16_t channels = 2)
{
	std::FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) {
		err = CLI_T("Cannot write: ", "書けない: ") + path;
		return false;
	}
	write_wav_header(f, uint32_t(pcm.size() / (channels ? channels : 1)), rate, channels);
	if (!pcm.empty())
		std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), f);
	if (std::fclose(f) != 0) {
		err = CLI_T("The writing stopped part way: ", "書き込みが途中で終わった: ") + path;
		return false;
	}
	return true;
}

// The float shape: render's --float, 32 bit IEEE at full scale.
inline bool write_wav_float(const std::string &path, const std::vector<float> &pcm,
                            std::string &err, uint32_t rate, uint16_t channels = 2)
{
	std::FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) {
		err = CLI_T("Cannot write: ", "書けない: ") + path;
		return false;
	}
	write_wav_header(f, uint32_t(pcm.size() / (channels ? channels : 1)), rate, channels, 3, 32);
	if (!pcm.empty())
		std::fwrite(pcm.data(), sizeof(float), pcm.size(), f);
	if (std::fclose(f) != 0) {
		err = CLI_T("The writing stopped part way: ", "書き込みが途中で終わった: ") + path;
		return false;
	}
	return true;
}

} // namespace ui

#endif // S_MU2000_UI_WAV_H
