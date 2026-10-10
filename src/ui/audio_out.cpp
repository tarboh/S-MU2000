// license:BSD-3-Clause

#include "audio_out.h"
#include "compat/cli_text.h"
#include "resampler.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ui {

namespace {

bool is_float(const WAVEFORMATEX *f)
{
	if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
		return true;
	if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
		const WAVEFORMATEXTENSIBLE *e = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(f);
		return e->SubFormat.Data1 == 3;   // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
	}
	return false;
}

bool is_pcm16(const WAVEFORMATEX *f)
{
	if (f->wBitsPerSample != 16)
		return false;
	if (f->wFormatTag == WAVE_FORMAT_PCM)
		return true;
	if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
		const WAVEFORMATEXTENSIBLE *e = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(f);
		return e->SubFormat.Data1 == 1;   // KSDATAFORMAT_SUBTYPE_PCM
	}
	return false;
}

// デバイスへ渡す形。24bit は 32bit の器に左詰めで入れる（RME の本来の形式）
enum class devfmt { f32, i16, i24in32 };

// 独り占めモードで試す形式を組む
WAVEFORMATEXTENSIBLE make_format(u32 rate, bool flt, int bits, int container, u32 channels = 2, DWORD mask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
{
	WAVEFORMATEXTENSIBLE e{};
	e.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
	e.Format.nChannels       = WORD(channels);
	e.Format.nSamplesPerSec  = rate;
	e.Format.wBitsPerSample  = WORD(container);
	e.Format.nBlockAlign     = WORD(channels * container / 8);
	e.Format.nAvgBytesPerSec = rate * e.Format.nBlockAlign;
	e.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
	e.Samples.wValidBitsPerSample = WORD(bits);
	e.dwChannelMask          = mask;
	// KSDATAFORMAT_SUBTYPE_{PCM,IEEE_FLOAT}
	e.SubFormat.Data1 = flt ? 3 : 1;
	e.SubFormat.Data2 = 0x0000;
	e.SubFormat.Data3 = 0x0010;
	static const BYTE tail[8] = { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 };
	std::memcpy(e.SubFormat.Data4, tail, 8);
	return e;
}

// PKEY_Device_FriendlyName。INITGUID を持ち込むと他と衝突するので直に書く
//   {a45c254e-df1c-4efd-8020-67d146a850e0} の 14 番
const PROPERTYKEY kFriendlyName = {
	{ 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

// 口の名前を取る
std::string endpoint_name(IMMDevice *d)
{
	std::string out;
	IPropertyStore *ps = nullptr;
	if (FAILED(d->OpenPropertyStore(STGM_READ, &ps)))
		return out;
	PROPVARIANT v;
	PropVariantInit(&v);
	if (SUCCEEDED(ps->GetValue(kFriendlyName, &v)) && v.pwszVal) {
		char buf[256] = {};
		WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
		out = buf;
	}
	PropVariantClear(&v);
	ps->Release();
	return out;
}

} // namespace


std::vector<std::string> audio_out::list(audio_driver driver)
{
	if (driver != audio_driver::native) {
#if defined(SMU2000_ASIO)
		return portaudio_output_list(driver);
#else
		return {};
#endif
	}
	std::vector<std::string> out;
	const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
	IMMDeviceEnumerator *en = nullptr;
	if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
	                               __uuidof(IMMDeviceEnumerator), (void **)&en))) {
		IMMDeviceCollection *all = nullptr;
		if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all))) {
			UINT n = 0;
			all->GetCount(&n);
			for (UINT i = 0; i < n; i++) {
				IMMDevice *d = nullptr;
				if (SUCCEEDED(all->Item(i, &d))) {
					out.push_back(endpoint_name(d));
					d->Release();
				}
			}
			all->Release();
		}
		en->Release();
	}
	if (com)
		CoUninitialize();
	return out;
}


std::string audio_out::default_device_name(audio_driver driver)
{
	if (driver == audio_driver::asio) {
		const auto devices = list(driver);
		return devices.empty() ? std::string() : devices.front();
	}
	const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
	IMMDeviceEnumerator *en = nullptr;
	IMMDevice *dev = nullptr;
	std::string name;
	if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
	                               __uuidof(IMMDeviceEnumerator), (void **)&en))) {
		if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
			name = endpoint_name(dev);
			dev->Release();
		}
		en->Release();
	}
	if (com) CoUninitialize();
	return name;
}

bool audio_out::start(int latency_ms, fill_fn fill, std::string &err, bool exclusive,
                      const std::string &device, bool raw, bool exact)
{
	if (!valid_audio_request(m_stream)) { err = "Invalid audio stream settings"; return false; }
	if (m_thread.joinable())
		return true;
	m_want_raw = raw;
	m_exact_dev = exact;

	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	m_qpc_freq = f.QuadPart;

	m_fill = std::move(fill);
	m_quit.store(false);
	m_err.clear();
	m_dev_name.clear();
	// These measurements describe one stream. A new device has a new clock
	// and possibly a new sample rate, so old frame counts cannot carry over.
	m_produced.store(0); m_late.store(0);
	m_busy_ticks.store(0); m_worst_ticks.store(0);
	m_slack_min.store(~u64(0));
	m_queue_sum.store(0); m_queue_n.store(0); m_queue_worst.store(0);
	m_inflight_sum.store(0); m_inflight_n.store(0); m_inflight_worst.store(0);
	m_cpu_meter.reset();
	m_mmcss.store(false);
	m_period_ms.store(0); m_stream_ms.store(0);

	m_want_dev = device;
	m_start_state.store(0);
	m_thread = std::thread([this, latency_ms, exclusive] {
		if (m_stream.driver == audio_driver::native) run(latency_ms, exclusive);
#if defined(SMU2000_ASIO)
		else run_portaudio(latency_ms);
#endif
		m_start_state.store(m_running.load() ? 1 : 2);
	});

	// 開始に失敗したかどうかだけ待つ。だめなら理由を返す
	for (int i = 0; (m_control_panel || i < (m_stream.driver == audio_driver::asio ? 2000 : 400)) && m_start_state.load() == 0 && !m_running.load(); i++)
		Sleep(5);
	if (!m_running.load()) {
		stop();
		err = m_err.empty() ? CLI_T("Cannot open the audio device", "音声デバイスを開けない") : m_err;
		return false;
	}
	return true;
}

void audio_out::stop()
{
	m_quit.store(true);
	if (m_thread.joinable())
		m_thread.join();
	m_running.store(false);
}

double audio_out::cpu_percent() const
{
	const u64 done = m_produced.load();
	if (!done)
		return 0.0;
	const double audio = double(done) / double(m_dev_rate.load());
	const double busy  = double(m_busy_ticks.load()) / double(m_qpc_freq);
	return 100.0 * busy / audio;
}

double audio_out::worst_ms() const
{
	return 1000.0 * double(m_worst_ticks.load()) / double(m_qpc_freq);
}

double audio_out::buffer_ms() const
{
	const u32 r = m_dev_rate.load();
	return r ? 1000.0 * double(m_buffer_frames.load()) / double(r) : 0.0;
}

double audio_out::queue_ms() const
{
	const u64 n = m_queue_n.load();
	const u32 r = m_dev_rate.load();
	if (!n || !r)
		return 0.0;
	return 1000.0 * (double(m_queue_sum.load()) / double(n)) / double(r);
}

double audio_out::queue_worst_ms() const
{
	const u32 r = m_dev_rate.load();
	return r ? 1000.0 * double(m_queue_worst.load()) / double(r) : 0.0;
}

double audio_out::target_ms() const
{
	const u32 r = m_dev_rate.load();
	return r ? 1000.0 * double(m_target_frames.load()) / double(r) : 0.0;
}

double audio_out::slack_min_ms() const
{
	const u64 v = m_slack_min.load();
	const u32 r = m_dev_rate.load();
	return (v == ~u64(0) || !r) ? 0.0 : 1000.0 * double(v) / double(r);
}

double audio_out::inflight_ms() const
{
	const u64 n = m_inflight_n.load();
	const u32 r = m_dev_rate.load();
	if (!n || !r)
		return 0.0;
	return 1000.0 * (double(m_inflight_sum.load()) / double(n)) / double(r);
}

double audio_out::inflight_worst_ms() const
{
	const u32 r = m_dev_rate.load();
	return r ? 1000.0 * double(m_inflight_worst.load()) / double(r) : 0.0;
}

std::string audio_out::format_line() const
{
	char buf[200];
	if (m_stream.driver != audio_driver::native) {
		std::snprintf(buf, sizeof(buf), "%s / %u Hz %u ch float / period %.1f ms", audio_driver_name(m_stream.driver), device_rate(), device_channels(), period_ms());
		return buf;
	}
	std::snprintf(buf, sizeof(buf),
	              CLI_T("%s / %u Hz %u ch %s / period %.1f ms / resampling %s", "%s / %u Hz %u ch %s / 周期 %.1f ms / 変換 %s"),
	              m_exclusive.load() ? CLI_T("exclusive", "独り占め") : CLI_T("shared", "共有"),
	              m_dev_rate.load(), m_dev_channels.load(),
	              m_dev_bits.load() == 24 ? "24bit(32)"
	              : (m_dev_float.load() ? "float" : "16bit"),
	              m_period_ms.load(),
	              m_converting.load() ? (m_stream.quality == resampler_quality::sinc ? "sinc" : m_stream.quality == resampler_quality::linear ? "linear" : "nearest") : CLI_T("none (44100 as is)", "無し（44100 のまま）"));
	if (m_raw.load())
		std::strncat(buf, " / RAW", sizeof(buf) - std::strlen(buf) - 1);
	return buf;
}

std::string audio_out::latency_line() const
{
	char buf[220];
	std::snprintf(buf, sizeof(buf),
	              CLI_T("queue: target %.1f / measured mean %.1f worst %.1f ms. **not yet played: mean %.1f worst %.1f ms** (GetStreamLatency %.1f / buffer %.1f ms)",
	                    "溜め 目標 %.1f / 実測 平均 %.1f 最悪 %.1f ms。**まだ鳴っていない量 平均 %.1f 最悪 %.1f ms**（GetStreamLatency %.1f / 器 %.1f ms）"),
	              target_ms(), queue_ms(), queue_worst_ms(),
	              inflight_ms(), inflight_worst_ms(), device_ms(), buffer_ms());
	return buf;
}

void audio_out::run(int latency_ms, bool want_exclusive)
{
	if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
		m_err = CLI_T("Cannot initialise COM", "COM を初期化できない");
		return;
	}

	IMMDeviceEnumerator *en = nullptr;
	IMMDevice *dev = nullptr;
	IAudioClient *client = nullptr;
	IAudioRenderClient *render = nullptr;
	HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	WAVEFORMATEX *mix = nullptr;
	UINT32 buf_frames = 0;
	DWORD  mmcss_index = 0;
	HANDLE mmcss = nullptr;

	IAudioClock *clock = nullptr;
	UINT64 clock_freq = 0;
	std::FILE *cap = nullptr;
	u64 cap_frames = 0;

	resampler rs;
	std::vector<s16>   stage;
	std::vector<float> mixbuf;
	bool dev_float = false, exclusive = false, autoconv = false;
	devfmt fmt = devfmt::f32;
	u32  dev_ch = 2, dev_rate = AUDIO_RATE;
	UINT32 target = 0, period_frames = 0;
	const UINT32 CHUNK = 480;

	auto fail = [this](const char *what, HRESULT hr) {
		char buf[140];
		std::snprintf(buf, sizeof(buf), CLI_T("%s failed (0x%08lx)", "%s に失敗 (0x%08lx)"), what, (unsigned long)hr);
		m_err = buf;
	};

	HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
	                              __uuidof(IMMDeviceEnumerator), (void **)&en);
	if (FAILED(hr)) { fail(CLI_T("Listing the devices", "デバイス一覧の取得"), hr); goto done; }

	// Prefer an exact menu name, then allow a substring for --audio.
	if (!m_want_dev.empty()) {
		IMMDeviceCollection *all = nullptr;
		if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all))) {
			UINT n = 0;
			all->GetCount(&n);
			for (int pass = 0; pass < (m_exact_dev ? 1 : 2) && !dev; pass++)
			for (UINT i = 0; i < n && !dev; i++) {
				IMMDevice *d = nullptr;
				if (FAILED(all->Item(i, &d)))
					continue;
				const std::string name = endpoint_name(d);
				if (pass == 0 ? name == m_want_dev : name.find(m_want_dev) != std::string::npos)
					dev = d;              // 掴んだまま使う
				else
					d->Release();
			}
			all->Release();
		}
		if (!dev) {
			m_err = CLI_T("No playback device with that name: ", "その名前の再生デバイスが無い: ") + m_want_dev;
			goto done; // a vanished selection must not silently open another output
		}
	}
	if (!dev) {
		hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
		if (FAILED(hr)) { fail(CLI_T("Getting the default audio device", "既定の音声デバイスの取得"), hr); goto done; }
	}
	m_dev_name = endpoint_name(dev);

	hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client);
	if (FAILED(hr)) { fail(CLI_T("Activating the audio device", "音声デバイスの起動"), hr); goto done; }
	if (FAILED(client->GetMixFormat(&mix)) || !mix) { fail(CLI_T("Reading the device format", "形式の取得"), E_FAIL); goto done; }

	{
		REFERENCE_TIME def_period = 0, min_period = 0;
		client->GetDevicePeriod(&def_period, &min_period);

		// ---- 独り占めモード。Windows の混ぜ合わせを通さないので一番短い
		m_info = {};
		for (u32 c = 0; c < mix->nChannels; c++)
			m_info.channels.push_back("Output " + std::to_string(c + 1));
		if (!valid_audio_route(m_stream, mix->nChannels)) {
			m_err = "The selected output channels are unavailable";
			goto done;
		}
		const DWORD mask = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE
		    ? reinterpret_cast<WAVEFORMATEXTENSIBLE *>(mix)->dwChannelMask : 0;
		if (want_exclusive) {
			for (int rate : { 8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000 }) {
				for (const auto &f : { make_format(rate, false, 24, 32, mix->nChannels, mask),
				                       make_format(rate, true, 32, 32, mix->nChannels, mask),
				                       make_format(rate, false, 16, 16, mix->nChannels, mask) }) {
					if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &f.Format, nullptr) == S_OK) {
						m_info.rates.push_back(rate);
						break;
					}
				}
			}
		} else {
			m_info.rates.push_back(int(mix->nSamplesPerSec));
			if (m_stream.sample_rate && m_stream.sample_rate != int(mix->nSamplesPerSec)) {
				m_err = "Shared mode uses the system device rate; choose exclusive mode to select a rate";
				goto done;
			}
		}
		if (want_exclusive) {
			// **デバイスが言っている周波数を先に試す。** そこがデバイスの
			// 時計なので、違う周波数を通すと（受け付けられても）速さが
			// 合わずに音が崩れる。
			//
			// 形式は **24bit を 32bit の器に入れたものを先に**。RME のような
			// 業務用の機械の本来の形で、16bit は受け付けても扱いが怪しい
			// ことがある（切り刻まれたような音になった）。44100 が通れば
			// 変換も要らなくなるので、それも試す
			const struct { u32 rate; bool flt; int bits; int container; } cands[] = {
				{ mix->nSamplesPerSec, false, 24, 32 },
				{ mix->nSamplesPerSec, true,  32, 32 },
				{ mix->nSamplesPerSec, false, 16, 16 },
				{ AUDIO_RATE, false, 24, 32 },
				{ AUDIO_RATE, true,  32, 32 },
				{ AUDIO_RATE, false, 16, 16 },
			};
			for (const auto &c : cands) {
				const u32 requested_rate = m_stream.sample_rate ? u32(m_stream.sample_rate) : c.rate;
				WAVEFORMATEXTENSIBLE want = make_format(requested_rate, c.flt, c.bits, c.container, mix->nChannels, mask);
				hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &want.Format, nullptr);
				if (hr != S_OK)
					continue;
				// **周期は 2 の冪のフレーム数にする。**
				//
				// ドライバは何を頼んでも「通った」と言うことがあるが、
				// 業務用の機械は自分の単位（128/256/512/1024…）で動いていて、
				// 割り切れない長さを渡すと切り刻まれたような音になる。
				// RME を 1024 サンプルで走らせている機械に 480 フレーム
				// （10ms）を頼んで、実際にそうなった
				const double want_ms = latency_ms > 0 ? double(latency_ms) : 10.0;
				u32 frames = m_stream.buffer_frames ? u32(m_stream.buffer_frames) : u32(want_ms * requested_rate / 1000.0 + 0.5);
				u32 pow2 = 64;
				while (pow2 < frames && pow2 < 8192)
					pow2 <<= 1;
				if (pow2 > 64 && (pow2 - frames) > (frames - (pow2 >> 1)))
					pow2 >>= 1;          // 下のほうが近ければそちら
				frames = pow2;
				REFERENCE_TIME per = REFERENCE_TIME(10000000.0 * frames / requested_rate + 0.5);
				while (per < min_period && frames < 8192) {
					frames <<= 1;
					per = REFERENCE_TIME(10000000.0 * frames / requested_rate + 0.5);
				}
				hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
				                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
				                        per, per, &want.Format, nullptr);
				if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
					// 揃っていない長さを断られた。教えられた長さで作り直す。
					// **client を作り直すのが決まり**（再 Initialize は通らない）
					UINT32 aligned = 0;
					client->GetBufferSize(&aligned);
					client->Release();
					client = nullptr;
					if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
					                         nullptr, (void **)&client)))
						break;
					per = REFERENCE_TIME(10000.0 * 1000.0 * aligned / requested_rate + 0.5);
					hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
					                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
					                        per, per, &want.Format, nullptr);
				}
				if (SUCCEEDED(hr)) {
					exclusive = true;
					dev_float = c.flt;
					fmt       = c.flt ? devfmt::f32
					          : (c.container == 32 ? devfmt::i24in32 : devfmt::i16);
					dev_ch    = mix->nChannels;
					dev_rate  = requested_rate;
					m_period_ms.store(per / 10000.0);
					break;
				}
				// 失敗したら client は Initialize 前の状態に戻す
				client->Release();
				client = nullptr;
				if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
				                         nullptr, (void **)&client)))
					break;
			}
			if (!exclusive && m_stream.strict) {
				fail(CLI_T("Opening the exclusive audio stream", "排他音声ストリームの開始"), hr);
				goto done;
			}
			if (!exclusive && client)
				m_err = CLI_T("Could not open in exclusive mode (falling back to shared)", "独り占めで開けなかった（共有に落とす）");
			if (!client) { fail(CLI_T("Activating the audio device", "音声デバイスの起動"), E_FAIL); goto done; }
		}

		// ---- 共有モード
		if (!exclusive) {
			// RAW を頼む。エンジンの信号処理（APO）を飛ばす分だけ短くなる
			// ことがある。断られても構わない
			if (m_want_raw) {
				IAudioClient2 *a2 = nullptr;
				if (SUCCEEDED(client->QueryInterface(__uuidof(IAudioClient2),
				                                     (void **)&a2))) {
					AudioClientProperties props{};
					props.cbSize    = sizeof(props);
					props.bIsOffload = FALSE;
					props.eCategory = AudioCategory_Media;
					props.Options   = AUDCLNT_STREAMOPTIONS_RAW;
					m_raw.store(SUCCEEDED(a2->SetClientProperties(&props)));
					a2->Release();
				}
			}
			WAVEFORMATEX fallback{};
			const bool usable = is_float(mix) || is_pcm16(mix);
			if (!usable) {
				fallback.wFormatTag      = WAVE_FORMAT_PCM;
				fallback.nChannels       = 2;
				fallback.nSamplesPerSec  = AUDIO_RATE;
				fallback.wBitsPerSample  = 16;
				fallback.nBlockAlign     = 4;
				fallback.nAvgBytesPerSec = AUDIO_RATE * 4;
				autoconv = true;
			}
			const WAVEFORMATEX *use = usable ? mix : &fallback;
			dev_float = usable && is_float(mix);
			fmt       = dev_float ? devfmt::f32 : devfmt::i16;
			dev_ch    = use->nChannels;
			dev_rate  = use->nSamplesPerSec;
			if (m_stream.sample_rate && m_stream.sample_rate != int(dev_rate)) {
				m_err = "The shared stream cannot use the requested sample rate";
				goto done;
			}
			if (!valid_audio_route(m_stream, dev_ch)) {
				m_err = "The stream cannot use the selected channels";
				goto done;
			}
			m_info.rates = {int(dev_rate)};
			m_info.channels.resize(dev_ch);
			m_period_ms.store(def_period / 10000.0);

			// **器は余裕を持って取り、溜めるのは target だけ。**
			// 器が小さいと、起きるのが少し遅れたときに書く場所が無くなる
			const double want_ms = m_stream.buffer_frames ? 1000.0 * m_stream.buffer_frames / dev_rate
			    : latency_ms > 0 ? double(latency_ms) : 2.0 * def_period / 10000.0;
			const double buf_ms = std::max(want_ms + 2.0 * def_period / 10000.0,
			                               3.0 * def_period / 10000.0);
			const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
			                    (autoconv ? (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
			                                 AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY) : 0u);
			hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
			                        REFERENCE_TIME(buf_ms * 10000.0), 0, use, nullptr);
			if (FAILED(hr)) { fail(CLI_T("Initialising the audio stream", "音声の開始準備"), hr); goto done; }

			// **目標は周期の倍数にする。** エンジンは周期ごとにまとめて
			// 読んでいくので、半端な目標にすると書ける回と書けない回が
			// 交互に来て溜めが振動する（15ms で平均 23 / 最悪 46ms になった）
			{
				const double per_ms = def_period / 10000.0;
				const u32 per_frames = u32(per_ms * dev_rate / 1000.0 + 0.5);
				u32 n = u32(want_ms / per_ms + 0.5);
				if (n < 1)
					n = 1;
				target = per_frames * n;
				period_frames = per_frames;
			}
		}

		m_exclusive.store(exclusive);
		m_dev_rate.store(dev_rate);
		m_info.rate = int(dev_rate);
		m_dev_channels.store(dev_ch);
		m_dev_float.store(dev_float);
		m_dev_bits.store(fmt == devfmt::i24in32 ? 24 : (fmt == devfmt::i16 ? 16 : 32));
		rs.configure(double(AUDIO_RATE), double(dev_rate), m_stream.quality);
		m_converting.store(!rs.direct());
		stage.resize(size_t(CHUNK + 64) * 2);
		mixbuf.resize(size_t(CHUNK) * 2);
	}

	hr = client->SetEventHandle(ev);
	if (FAILED(hr)) { fail(CLI_T("Registering the audio event", "イベントの登録"), hr); goto done; }

	hr = client->GetBufferSize(&buf_frames);
	if (FAILED(hr)) { fail(CLI_T("Reading the buffer size", "バッファ長の取得"), hr); goto done; }

	hr = client->GetService(__uuidof(IAudioRenderClient), (void **)&render);
	if (FAILED(hr)) { fail(CLI_T("Getting the render interface", "書き込み口の取得"), hr); goto done; }

	{
		// デバイス側の取り分。こちらでは短くできない
		REFERENCE_TIME sl = 0;
		if (SUCCEEDED(client->GetStreamLatency(&sl)))
			m_stream_ms.store(sl / 10000.0);
		// 再生位置。書いた量との差が「まだ鳴っていない量」で、
		// **ドライバが抱えている分も入る**
		if (SUCCEEDED(client->GetService(__uuidof(IAudioClock), (void **)&clock)))
			clock->GetFrequency(&clock_freq);
	}

	// デバイスへ渡すものをそのまま書き出す（切り分け用）
	if (!m_cap_path.empty()) {
		cap = std::fopen(m_cap_path.c_str(), "wb");
		if (cap) {
			u8 head[44] = {};
			std::fwrite(head, 1, 44, cap);   // 後で書き直す
		}
	}

	// 独り占めでは毎周期ちょうど器ぶんを書く。共有では target だけ溜める
	if (exclusive || !target || target > buf_frames)
		target = buf_frames;
	m_buffer_frames.store(buf_frames);
	m_target_frames.store(target);

	// 音声を作るスレッドは優先度を上げる。取りこぼすと音が切れる。
	//
	// **SetThreadPriority だけでは足りない**。Windows の割り当ては
	// MMCSS（マルチメディア用の割り当て）が別に持っていて、"Pro Audio" で
	// 登録しておかないと、他の仕事のために数十ミリ秒まとめて止められる
	// ことがある。平均の負荷に余裕があっても、そこで音が途切れる
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_index);
	if (mmcss)
		AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);
	m_mmcss.store(mmcss != nullptr);

	{
		BYTE *data = nullptr;

		auto write_frames = [&](BYTE *dst, UINT32 frames) {
			UINT32 at = 0;
			while (at < frames) {
				const UINT32 n = std::min<UINT32>(CHUNK, frames - at);
				const int need = rs.input_needed(int(n));
				if (need > 0) {
					if (stage.size() < size_t(need) * 2)
						stage.resize(size_t(need) * 2);
					m_fill(stage.data(), u32(need));
					rs.push(stage.data(), need);
				}
				rs.pull(mixbuf.data(), int(n));

				auto route = [&](auto *out, auto convert) {
					std::fill_n(out, size_t(n) * dev_ch, 0);
					for (UINT32 i = 0; i < n; i++) {
						out[size_t(i) * dev_ch + m_stream.left] = convert(mixbuf[i * 2]);
						if (dev_ch > 1) out[size_t(i) * dev_ch + m_stream.right] = convert(mixbuf[i * 2 + 1]);
					}
				};
				if (fmt == devfmt::f32)
					route(reinterpret_cast<float *>(dst) + size_t(at) * dev_ch, [](float v) { return v; });
				else if (fmt == devfmt::i24in32)
					route(reinterpret_cast<s32 *>(dst) + size_t(at) * dev_ch,
					      [](float v) { return s32(std::clamp(v, -1.0f, 1.0f) * 8388607.0f) * 256; });
				else
					route(reinterpret_cast<s16 *>(dst) + size_t(at) * dev_ch, audio_stream_renderer::pcm16_truncate);
				at += n;
			}
		};

		// 走り出しに溜める分。**満杯ではなく target まで**
		if (SUCCEEDED(render->GetBuffer(target, &data))) {
			write_frames(data, target);
			m_produced.fetch_add(target);
			render->ReleaseBuffer(target, 0);
		}

		hr = client->Start();
		if (FAILED(hr)) { fail(CLI_T("Starting playback", "再生の開始"), hr); goto done; }
		m_running.store(true);

		while (!m_quit.load()) {
			if (WaitForSingleObject(ev, 2000) != WAIT_OBJECT_0) {
				m_err = CLI_T("No signal from the audio device", "音声デバイスからの合図が来ない");
				break;
			}

			// **独り占めでは padding を見ない。** 器は表裏の二枚で回っていて、
			// 合図が来たら裏を**まるごと**書くのが作法。書き残すと、そこに
			// 前の音が残って周期ごとに鳴る（切り刻まれたような音になる）
			UINT32 padding = 0;
			if (!exclusive && FAILED(client->GetCurrentPadding(&padding)))
				break;

			// 書いたのに、まだ鳴っていない量。ドライバの分も入る
			if (clock && clock_freq) {
				UINT64 pos = 0;
				if (SUCCEEDED(clock->GetPosition(&pos, nullptr))) {
					const u64 played = u64(double(pos) / double(clock_freq)
					                       * double(dev_rate));
					const u64 wrote = m_produced.load();
					if (wrote > played && m_produced.load() > u64(buf_frames) * 2) {
						const u64 fly = wrote - played;
						m_inflight_sum.fetch_add(fly);
						m_inflight_n.fetch_add(1);
						if (fly > m_inflight_worst.load())
							m_inflight_worst.store(fly);
					}
				}
			}

			// 起きたときに溜まっていた量。これが待ち時間の本体
			m_queue_sum.fetch_add(padding);
			m_queue_n.fetch_add(1);
			if (padding > m_queue_worst.load())
				m_queue_worst.store(padding);

			UINT32 want = buf_frames;
			if (!exclusive) {
				// 溜めは目標まで。満杯にすると、その分そのまま待ち時間になる
				want = padding >= target ? 0 : std::min(buf_frames - padding,
				                                        target - padding);
			}
			if (!want)
				continue;

			if (FAILED(render->GetBuffer(want, &data)))
				break;

			LARGE_INTEGER t0, t1;
			QueryPerformanceCounter(&t0);
			write_frames(data, want);
			QueryPerformanceCounter(&t1);

			if (cap) {
				const size_t bytes = size_t(want) * dev_ch *
				                     (fmt == devfmt::i16 ? 2 : 4);
				std::fwrite(data, 1, bytes, cap);
				cap_frames += want;
			}

			const u64 took = u64(t1.QuadPart - t0.QuadPart);
			m_busy_ticks.fetch_add(took);
			m_cpu_meter.add(double(took) / double(m_qpc_freq),
			                double(want) / double(dev_rate));
			if (took > m_worst_ticks.load())
				m_worst_ticks.store(took);
			m_produced.fetch_add(want);

			// 間に合ったか。デバイスが次の音を要るまでの余裕は、
			// 共有なら「まだ溜まっていた量」、独り占めなら「器ひとつぶん」
			// （表と裏で 表裏の二枚 なので、裏を鳴らしている間に書く）
			// 走り出しの数回は数えない（まだ溜まっていないのは当たり前）
			if (m_produced.load() > u64(buf_frames) * 2) {
				// 締め切りまでの余裕。共有モードのエンジンは周期ごとに
				// 読むので、**残量に周期 1 つを足したもの**が本当の締め切り。
				// 独り占めは表裏の二枚なので器ひとつぶん
				const u64 slack = exclusive ? buf_frames
				                            : (u64(padding) + period_frames);
				if (slack < m_slack_min.load())
					m_slack_min.store(slack);
				if (double(took) / double(m_qpc_freq) > double(slack) / double(dev_rate))
					m_late.fetch_add(1);
			}

			render->ReleaseBuffer(want, 0);
		}

		client->Stop();
	}

done:
	if (cap) {
		// WAV の頭を後から書く。形式はデバイスに渡したものそのまま
		const u32 bits = (fmt == devfmt::i16) ? 16 : 32;
		const u32 bps  = dev_rate * dev_ch * bits / 8;
		const u32 data_bytes = u32(cap_frames * dev_ch * bits / 8);
		std::fseek(cap, 0, SEEK_SET);
		auto w32 = [&](u32 v) { u8 b[4] = { u8(v), u8(v >> 8), u8(v >> 16), u8(v >> 24) };
		                        std::fwrite(b, 1, 4, cap); };
		auto w16 = [&](u16 v) { u8 b[2] = { u8(v), u8(v >> 8) }; std::fwrite(b, 1, 2, cap); };
		std::fwrite("RIFF", 1, 4, cap); w32(36 + data_bytes); std::fwrite("WAVE", 1, 4, cap);
		std::fwrite("fmt ", 1, 4, cap); w32(16);
		w16((fmt == devfmt::f32) ? 3 : 1); w16(u16(dev_ch));
		w32(dev_rate); w32(bps); w16(u16(dev_ch * bits / 8)); w16(u16(bits));
		std::fwrite("data", 1, 4, cap); w32(data_bytes);
		std::fclose(cap);
	}
	if (clock) clock->Release();
	if (mmcss)
		AvRevertMmThreadCharacteristics(mmcss);
	m_running.store(false);
	if (mix) CoTaskMemFree(mix);
	if (render) render->Release();
	if (client) client->Release();
	if (dev) dev->Release();
	if (en) en->Release();
	if (ev) CloseHandle(ev);
	CoUninitialize();
}

} // namespace ui
