// license:BSD-3-Clause
#pragma once

#include "resampler.h"
#include "audio_driver.h"
#include <array>
#include <functional>
#include <string>
#include <vector>

namespace ui {

struct audio_stream_options {
	int sample_rate = 0; // 0: device/default rate
	int buffer_frames = 0; // 0: use the latency target
	int left = 0, right = 1;
	audio_driver driver = audio_driver::native;
	resampler_quality quality = resampler_quality::sinc;
	bool strict = false; // settings changes must not silently change access mode
	bool operator==(const audio_stream_options &) const = default;
};

struct audio_stream_info {
	std::vector<int> rates;
	std::vector<std::string> channels;
	int rate = 44100;
	std::vector<int> buffers;
	bool control_panel = false;
	int buffer_rate = 0; // 0: stream rate; CoreAudio periods use the hardware clock
	bool manual_buffer = true;
	// Whether the rate and the channel pair can be chosen at all. False where the
	// platform owns both (iOS), so the window offers nothing it would have to
	// refuse later.
	bool manual_format = true;
};

inline bool custom_audio_format(const audio_stream_options &s)
{
	return s.sample_rate != 0 || s.left != 0 || s.right != 1;
}

inline bool valid_audio_request(const audio_stream_options &s)
{
	return supported_audio_driver(s.driver) && int(s.quality) >= 0 && int(s.quality) <= 2 && (s.sample_rate == 0 || (s.sample_rate >= 8000 && s.sample_rate <= 192000)) &&
	       s.buffer_frames >= 0 && s.buffer_frames <= 8192 && s.left >= 0 && s.right >= 0 &&
	       s.left < 64 && s.right < 64 && s.left != s.right;
}

inline bool valid_audio_route(const audio_stream_options &s, unsigned channels)
{
	// The original mono path played the left side of the default stereo pair.
	if (channels == 1 && s.left == 0 && s.right == 1) return true;
	return s.left >= 0 && s.right >= 0 && unsigned(s.left) < channels &&
	       unsigned(s.right) < channels && s.left != s.right;
}

// Converts the fixed MU clock to a host stream. Bounded chunks fit the sinc
// converter's ring and all scratch is allocated before playback starts.
class audio_stream_renderer {
public:
	void configure(int rate, resampler_quality quality = resampler_quality::sinc)
	{
		m_rs.configure(44100, rate, quality);
		// Leave room for the sinc history even on low-rate host devices.
		m_chunk = unsigned(std::clamp((int(m_native.size() / 2) - 64) * double(rate > 0 ? rate : 44100) / 44100, 1.0, 512.0));
	}
	template <typename Sample, typename Fill, typename Convert>
	void render(Sample *dst, unsigned frames, unsigned channels,
	            int left, int right, Fill &&fill, Convert convert)
	{
		while (frames) {
			const unsigned n = std::min(frames, m_chunk);
			const int need = m_rs.input_needed(int(n));
			if (need) {
				fill(m_native.data(), unsigned(need));
				m_rs.push(m_native.data(), need);
			}
			m_rs.pull(m_stereo.data(), int(n));
			std::fill_n(dst, size_t(n) * channels, Sample{});
			for (unsigned i = 0; i < n; i++) {
				dst[size_t(i) * channels + left] = convert(m_stereo[i * 2]);
				if (channels > 1) dst[size_t(i) * channels + right] = convert(m_stereo[i * 2 + 1]);
			}
			dst += size_t(n) * channels;
			frames -= n;
		}
	}
	static s16 pcm16(float v) { return s16(std::lrint(std::clamp(v, -1.0f, 32767.0f / 32768) * 32768)); }
	static s16 pcm16_truncate(float v) { return s16(std::clamp(v, -1.0f, 1.0f) * 32767); }
private:
	resampler m_rs;
	unsigned m_chunk = 512;
	std::array<s16, 4096> m_native{};
	std::array<float, 1024> m_stereo{};
};

} // namespace ui
