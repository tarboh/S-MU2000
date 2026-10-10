// license:BSD-3-Clause
// DirectSound/ASIO share PortAudio's tested driver adapters. WASAPI remains
// native. All PortAudio lifecycle calls belong to this output's owner thread.
#include "audio_out.h"
#include "portaudio.h"
#include "pa_asio.h"
#include <windows.h>
#include <mmreg.h>
#include <dsound.h>
#include <algorithm>
#include <cstdio>

namespace ui {
namespace {
std::string utf8(const wchar_t *value)
{
	const int n = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
	if (n <= 1) return {};
	std::string result(size_t(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), n, nullptr, nullptr);
	result.pop_back(); return result;
}
BOOL CALLBACK ds_device(LPGUID id, LPCWSTR name, LPCWSTR, LPVOID context)
{
	if (id) static_cast<std::vector<std::string> *>(context)->push_back(utf8(name));
	return TRUE;
}
std::string pa_error(PaError error)
{
	std::string result = Pa_GetErrorText(error);
	if (error == paUnanticipatedHostError) {
		const auto *host = Pa_GetLastHostErrorInfo();
		if (host && host->errorText) result += std::string(": ") + host->errorText;
	}
	return result;
}
PaDeviceIndex find_output(audio_driver driver, const std::string &wanted, bool exact)
{
	const auto api = Pa_HostApiTypeIdToHostApiIndex(driver == audio_driver::asio ? paASIO : paDirectSound);
	if (api < 0) return paNoDevice;
	const auto *host = Pa_GetHostApiInfo(api);
	if (wanted.empty()) return host->defaultOutputDevice;
	for (int pass = 0; pass < (exact ? 1 : 2); pass++)
		for (int i = 0; i < host->deviceCount; i++) {
			const auto index = Pa_HostApiDeviceIndexToDeviceIndex(api, i);
			const auto *info = Pa_GetDeviceInfo(index);
			if (!info || info->maxOutputChannels < 1) continue;
			const std::string name = info->name;
			if (pass == 0 ? name == wanted : name.find(wanted) != std::string::npos) return index;
		}
	return paNoDevice;
}

struct float_capture {
	std::FILE *file = nullptr;
	u64 frames = 0;
	unsigned rate = 0, channels = 0;
	~float_capture()
	{
		if (!file) return;
		std::rewind(file);
		const auto w32 = [&](u32 v) { u8 b[] = {u8(v), u8(v >> 8), u8(v >> 16), u8(v >> 24)}; std::fwrite(b, 1, 4, file); };
		const auto w16 = [&](u16 v) { u8 b[] = {u8(v), u8(v >> 8)}; std::fwrite(b, 1, 2, file); };
		const u32 bytes = u32(frames * channels * sizeof(float));
		std::fwrite("RIFF", 1, 4, file); w32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, file);
		w32(16); w16(3); w16(u16(channels)); w32(rate); w32(rate * channels * 4);
		w16(u16(channels * 4)); w16(32); std::fwrite("data", 1, 4, file); w32(bytes);
		std::fclose(file);
	}
};
} // namespace

// Enumerate native APIs, rather than PortAudio's cached initialization list,
// so hotplug updates never terminate a playing PortAudio stream.
std::vector<std::string> portaudio_output_list(audio_driver driver)
{
	std::vector<std::string> names;
	if (driver == audio_driver::directsound) {
		DirectSoundEnumerateW(ds_device, &names);
	} else if (driver == audio_driver::asio) {
		HKEY root = nullptr;
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root) != ERROR_SUCCESS) return names;
		for (DWORD i = 0;; i++) {
			wchar_t key[256]; DWORD n = 256;
			if (RegEnumKeyExW(root, i, key, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
			HKEY entry = nullptr;
			if (RegOpenKeyExW(root, key, 0, KEY_READ, &entry) != ERROR_SUCCESS) continue;
			wchar_t desc[256] = {}; DWORD bytes = sizeof(desc);
			if (RegQueryValueExW(entry, L"description", nullptr, nullptr, reinterpret_cast<BYTE *>(desc), &bytes) != ERROR_SUCCESS) wcsncpy(desc, key, 255);
			desc[255] = 0;
			names.push_back(utf8(desc));
			RegCloseKey(entry);
		}
		RegCloseKey(root);
	}
	return names;
}

void audio_out::run_portaudio(int latency_ms)
{
	const PaError initialized = Pa_Initialize();
	if (initialized != paNoError) { m_err = pa_error(initialized); return; }
	struct lifetime { ~lifetime() { Pa_Terminate(); } } guard;
	PaDeviceIndex device = find_output(m_stream.driver, m_want_dev, m_exact_dev);
	const PaDeviceInfo *info = device == paNoDevice ? nullptr : Pa_GetDeviceInfo(device);
	if (!info) { m_err = "No output device with that name: " + m_want_dev; return; }
	const bool asio = m_stream.driver == audio_driver::asio;
	if (asio && m_control_panel) {
		const auto error = PaAsio_ShowControlPanel(device, nullptr);
		if (error != paNoError) { m_err = pa_error(error); return; }
		// The panel may change rate, buffers or channel layout. Re-enumerate
		// after releasing the driver, on the same owner thread.
		const std::string selected = info->name;
		Pa_Terminate();
		const auto reopened = Pa_Initialize();
		if (reopened != paNoError) { m_err = pa_error(reopened); return; }
		device = find_output(m_stream.driver, selected, true);
		info = device == paNoDevice ? nullptr : Pa_GetDeviceInfo(device);
		if (!info) { m_err = "The ASIO driver became unavailable"; return; }
	}
	if (!valid_audio_route(m_stream, unsigned(info->maxOutputChannels))) { m_err = "The selected output channels are unavailable"; return; }
	m_dev_name = !asio && m_want_dev.empty() ? default_device_name(m_stream.driver) : info->name;
	m_info = {};
	m_info.control_panel = asio;
	for (int c = 0; c < std::min(info->maxOutputChannels, 64); c++) {
		const char *name = nullptr;
		if (asio) PaAsio_GetOutputChannelName(device, c, &name);
		m_info.channels.push_back(name && *name ? name : "Output " + std::to_string(c + 1));
	}
	int selectors[2] = {m_stream.left, m_stream.right};
	PaAsioStreamInfo mapping{};
	mapping.size = sizeof(mapping); mapping.hostApiType = paASIO; mapping.version = 1;
	mapping.flags = paAsioUseChannelSelectors; mapping.channelSelectors = selectors;
	PaStreamParameters output{};
	output.device = device;
	output.channelCount = std::min(info->maxOutputChannels, asio ? 2 : std::max(m_stream.left, m_stream.right) + 1);
	output.sampleFormat = paFloat32;
	output.suggestedLatency = asio ? 0.0 : (latency_ms > 0 ? double(latency_ms) / 1000 : info->defaultLowOutputLatency);
	output.hostApiSpecificStreamInfo = asio ? &mapping : nullptr;
	for (int rate : {8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000})
		if (Pa_IsFormatSupported(nullptr, &output, rate) == paFormatIsSupported) m_info.rates.push_back(rate);
	const int rate = m_stream.sample_rate ? m_stream.sample_rate : int(info->defaultSampleRate);
	if (Pa_IsFormatSupported(nullptr, &output, rate) != paFormatIsSupported) { m_err = "The requested sample rate is unavailable"; return; }
	unsigned long frames = unsigned(m_stream.buffer_frames);
	if (asio) {
		long min = 0, max = 0, preferred = 0, step = 0;
		if (PaAsio_GetAvailableBufferSizes(device, &min, &max, &preferred, &step) != paNoError) { m_err = "Cannot read ASIO buffer sizes"; return; }
		long first = std::max(1L, min);
		if (step == -1) { first = 1; while (first < min) first *= 2; }
		if (step == 0) first = preferred;
		for (long n = first; n <= std::min(8192L, max);) {
			if (step != -1 || (n & (n - 1)) == 0) m_info.buffers.push_back(int(n));
			if (step == 0) break;
			n = step == -1 ? n * 2 : n + step;
		}
		if (!frames) frames = static_cast<unsigned long>(preferred);
		if (long(frames) < min || long(frames) > max || (step == -1 && (frames & (frames - 1))) ||
		    (step > 0 && (long(frames) - min) % step) || (step == 0 && long(frames) != preferred)) {
			m_err = "The ASIO driver cannot use the requested buffer size"; return;
		}
	}
	float_capture capture;
	capture.rate = unsigned(rate); capture.channels = unsigned(output.channelCount);
	if (!m_cap_path.empty()) {
		capture.file = std::fopen(m_cap_path.c_str(), "wb");
		if (!capture.file) { m_err = "Cannot open the output capture file"; return; }
		u8 header[44] = {}; std::fwrite(header, 1, sizeof(header), capture.file);
	}
	struct callback_state {
		audio_out *owner;
		audio_stream_renderer renderer;
		int channels, left, right;
		float_capture *capture;
	} state{this, {}, output.channelCount, asio ? 0 : m_stream.left, asio ? 1 : m_stream.right, &capture};
	state.renderer.configure(rate, m_stream.quality);
	const auto callback = [](const void *, void *dst, unsigned long n, const PaStreamCallbackTimeInfo *, PaStreamCallbackFlags flags, void *context) -> int {
		auto &s = *static_cast<callback_state *>(context);
		auto &o = *s.owner;
		LARGE_INTEGER before, after; QueryPerformanceCounter(&before);
		s.renderer.render(static_cast<float *>(dst), unsigned(n), unsigned(s.channels), s.left, s.right, o.m_fill, [](float value) { return value; });
		if (s.capture->file) {
			std::fwrite(dst, sizeof(float) * size_t(s.channels), n, s.capture->file);
			s.capture->frames += n;
		}
		QueryPerformanceCounter(&after);
		const auto ticks = u64(after.QuadPart - before.QuadPart);
		o.m_busy_ticks.fetch_add(ticks);
		o.m_cpu_meter.add(double(ticks) / o.m_qpc_freq, double(n) / o.m_dev_rate.load());
		if (ticks > o.m_worst_ticks.load()) o.m_worst_ticks.store(ticks);
		if (flags & paOutputUnderflow) o.m_late.fetch_add(1);
		o.m_buffer_frames.store(u32(n));
		o.m_produced.fetch_add(n);
		return o.m_quit.load() ? paComplete : paContinue;
	};
	PaStream *stream = nullptr;
	PaError error = Pa_OpenStream(&stream, nullptr, &output, rate, frames, paNoFlag, callback, &state);
	if (error != paNoError) { m_err = pa_error(error); return; }
	m_info.rate = rate;
	m_dev_rate.store(unsigned(rate)); m_dev_channels.store(unsigned(output.channelCount));
	m_dev_float.store(true); m_dev_bits.store(32); m_exclusive.store(asio); m_raw.store(false);
	m_converting.store(rate != AUDIO_RATE);
	m_stream_ms.store(Pa_GetStreamInfo(stream)->outputLatency * 1000);
	m_period_ms.store(frames ? 1000.0 * frames / rate : m_stream_ms.load());
	m_target_frames.store(u32(frames));
	error = Pa_StartStream(stream);
	if (error == paNoError) {
		m_running.store(true);
		while (!m_quit.load() && Pa_IsStreamActive(stream) == 1) Sleep(10);
		Pa_StopStream(stream);
	} else m_err = pa_error(error);
	Pa_CloseStream(stream);
	m_running.store(false);
}
} // namespace ui
