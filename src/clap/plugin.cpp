// license:BSD-3-Clause
//
// S-MU2000 の CLAP プラグイン。
//
// CLAP の口の定義（third_party/clap, MIT）だけを使い、中身は VST3 版と同じ
// smu2000::vst3::engine（ROM 探し・起動・周波数の変換）と plug_view（パネルの画面）を
// そのまま借りている。VST3 と違うのは口の形だけ:
//
//   ・MIDI はバイト列のまま届く（CLAP_EVENT_MIDI / MIDI_SYSEX）。VST3 のように
//     コントロールチェンジをパラメータに化けさせる必要が無い
//   ・ノートは CLAP 流（CLAP_EVENT_NOTE_ON 等）で来ることもあるので、MIDI に直す
//   ・パラメータは出力レベルと、XG の値（パートの音量・フィルタ・EG・EQ、マスター EQ など。
//     VST3 と同じ表と番号。automation.h、doc/automation.md）。画面で触った値は
//     ジェスチャーの始まり・値・終わりのイベントでホストへ伝える
//
// ノートの入力は 4 本。実機の MIDI IN A-D（パート 1-16 / 17-32 / 33-48 / 49-64）。VST3 版と同じ。
// 状態の保存の形は VST3 版の getState と同じにしてある。

#include "vst3/automation.h"
#include "vst3/automation_host.h"
#include "vst3/engine.h"
#include "vst3/plug_window.h"
#include "vst3/view.h"
#include "state.h"
#include "ui/snapshot.h"

#include "clap/clap.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using smu2000::vst3::engine;
using smu2000::vst3::plug_view;
namespace autom = smu2000::automation;

// ---- このプラグインを表す名前。一度決めたら変えられない
//      （変えるとホストが別物とみなし、保存した曲から見つからなくなる）
constexpr const char *kPlugId = "io.github.tarboh.s-mu2000";

const char *const kFeatures[] = {
	CLAP_PLUGIN_FEATURE_INSTRUMENT,
	CLAP_PLUGIN_FEATURE_SYNTHESIZER,
	CLAP_PLUGIN_FEATURE_STEREO,
	nullptr,
};

// 同じ本体を**エフェクトとして**も名乗る（issue #191。VST3 の「S-MU2000 FX」と同じ）。音声入力がメインの口に
// なり、挿した直後に A/D パートが鳴るようにしてある（engine::want_ad_through）
constexpr const char *kFxPlugId = "io.github.tarboh.s-mu2000.fx";

const char *const kFxFeatures[] = {
	CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
	CLAP_PLUGIN_FEATURE_MULTI_EFFECTS,
	CLAP_PLUGIN_FEATURE_STEREO,
	nullptr,
};

const clap_plugin_descriptor_t kFxDescriptor = {
	CLAP_VERSION_INIT,
	kFxPlugId,
	"S-MU2000 FX",
	"tarboh",
	"https://github.com/tarboh/S-MU2000",
	"https://github.com/tarboh/S-MU2000",
	"https://github.com/tarboh/S-MU2000/issues",
	"0.1.0",
	"Yamaha MU2000 emulation, as an effect on its A/D input",
	kFxFeatures,
};

const clap_plugin_descriptor_t kDescriptor = {
	CLAP_VERSION_INIT,
	kPlugId,
	"S-MU2000",
	"tarboh",
	"https://github.com/tarboh/S-MU2000",
	"https://github.com/tarboh/S-MU2000",
	"https://github.com/tarboh/S-MU2000/issues",
	"0.1.0",
	"Yamaha MU2000 emulation",
	kFeatures,
};

// MIDI IN A-D。VST3 と同じ 4 口（C・D は実機では USB だけの口で、パート 33-64 に届く）
constexpr int kPorts = mu2000::MIDI_PORTS;
// ノートの口は、その後ろに口 E（マルチパートのプラグインボードだけが聞く。issue #221）
constexpr int kNotePorts = kPorts + 1;

int port_of(uint16_t index)
{
	return index < kNotePorts ? int(index) : 0;
}
constexpr clap_id kGainId = 0;

#if defined(_WIN32)
constexpr const char *kWindowApi = CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
constexpr const char *kWindowApi = CLAP_WINDOW_API_COCOA;
#else
constexpr const char *kWindowApi = CLAP_WINDOW_API_X11;
#endif

// 状態の保存は途中までしか書けない・読めないことがある。全部済むまで回す
bool write_all(const clap_ostream_t *s, const void *p, size_t n)
{
	const auto *b = static_cast<const uint8_t *>(p);
	while (n) {
		const int64_t w = s->write(s, b, n);
		if (w <= 0)
			return false;
		b += w;
		n -= size_t(w);
	}
	return true;
}

bool read_all(const clap_istream_t *s, void *p, size_t n)
{
	auto *b = static_cast<uint8_t *>(p);
	while (n) {
		const int64_t r = s->read(s, b, n);
		if (r <= 0)
			return false;
		b += r;
		n -= size_t(r);
	}
	return true;
}

// MIDI 1 メッセージの長さ（smu2000::vst3::midi_length）は VST3 版と同じものを使う
using smu2000::vst3::midi_length;


// ---- 本体

class mu_plugin
{
public:
	explicit mu_plugin(const clap_host_t *host, bool fx = false) : m_host(host), m_fx(fx)
	{
		m_plugin.desc = m_fx ? &kFxDescriptor : &kDescriptor;
		if (m_fx)
			m_engine.want_ad_through();
		m_plugin.plugin_data = this;
		m_plugin.init             = [](const clap_plugin *p) { return self(p)->init(); };
		m_plugin.destroy          = [](const clap_plugin *p) { delete self(p); };
		m_plugin.activate         = [](const clap_plugin *p, double rate, uint32_t, uint32_t) { return self(p)->activate(rate); };
		m_plugin.deactivate       = [](const clap_plugin *p) { self(p)->report_ports(); };
		m_plugin.start_processing = [](const clap_plugin *p) { return self(p)->start_processing(); };
		m_plugin.stop_processing  = [](const clap_plugin *p) { self(p)->stop_processing(); };
		m_plugin.reset            = [](const clap_plugin *p) { self(p)->m_hush.store(true); };
		m_plugin.process          = [](const clap_plugin *p, const clap_process_t *pr) { return self(p)->process(pr); };
		m_plugin.get_extension    = [](const clap_plugin *p, const char *id) { return self(p)->extension(id); };
		m_plugin.on_main_thread   = [](const clap_plugin *) {};
		m_engine.panel().set_gain(1.0f);
		m_engine.set_output_rate(smu2000::vst3::NATIVE_RATE);
		// 画面で XG の値を触ったら、ホストへ伝える
		m_engine.set_edit_handlers(
			[this](const xg::param &p, int part, int value) { on_gui_edit(p, part, value); },
			[this](bool closing) { on_gui_idle(closing); },
			[this](u32 addr, int, int value) { on_gui_edit_raw(addr, value); });
	}

	~mu_plugin()
	{
		report_ports();
		gui_destroy();
		m_engine.set_edit_handlers(nullptr, nullptr);
	}

	const clap_plugin_t *plugin() const { return &m_plugin; }

private:
	static mu_plugin *self(const clap_plugin *p) { return static_cast<mu_plugin *>(p->plugin_data); }

	// ---- clap_plugin

	bool init()
	{
		m_host_params = static_cast<const clap_host_params_t *>(
			m_host->get_extension(m_host, CLAP_EXT_PARAMS));
		// ROM 読みと起動（音にして 4 秒ぶんの空回し）は時間がかかるので、
		// ここでは走らせるだけ。終わるまでは無音を返す
		m_engine.start();
		return true;
	}

	bool activate(double rate)
	{
		m_rate = rate;
		m_engine.set_output_rate(rate);
		// **ここで起動を待ちきる。**activate は本スレッドで呼ばれ、時間がかかってよい
		// ところなので、ここで待たないとホストは起動中の機械へ MIDI を流し始める。
		// 流された分は溜めてあとでまとめて出すので、曲の頭が崩れる（issue #19）
		if (!m_engine.wait_ready(30000))
			m_engine.log_line("起動が終わらないまま演奏に入る");
		return true;
	}

	bool start_processing()
	{
		// 動いているあいだ、機械に触れてよいのは音声スレッドだけ
		m_engine.set_processing(true);
		return true;
	}

	void stop_processing()
	{
		m_hush.store(true);
		m_engine.set_processing(false);
	}

	clap_process_status process(const clap_process_t *pr);

	// 音を作りながら 1 イベントを音源へ流す（出力レベルは流さない）
	void event(const clap_event_header_t *h);

	const void *extension(const char *id)
	{
		if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &s_audio_ports;
		if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS))  return &s_note_ports;
		if (!std::strcmp(id, CLAP_EXT_PARAMS))      return &s_params;
		if (!std::strcmp(id, CLAP_EXT_STATE))       return &s_state;
		if (!std::strcmp(id, CLAP_EXT_LATENCY))     return &s_latency;
		if (!std::strcmp(id, CLAP_EXT_TAIL))        return &s_tail;
		if (!std::strcmp(id, CLAP_EXT_GUI))         return &s_gui;
		return nullptr;
	}

	// ---- 音の口。出力 1 つと、A/D INPUT（サンプリングで録る音）の入力 1 つ

	static const clap_plugin_audio_ports_t s_audio_ports;

	// fx: エフェクトとして作られた。音声入力がメインの口になる
	static bool audio_port(uint32_t index, bool is_input, clap_audio_port_info_t *info, bool fx)
	{
		if (index != 0 || !info)
			return false;
		std::memset(info, 0, sizeof(*info));
		info->id            = is_input ? 1 : 0;
		info->flags         = (!is_input || fx) ? CLAP_AUDIO_PORT_IS_MAIN : 0;
		info->channel_count = 2;
		info->port_type     = CLAP_PORT_STEREO;
		info->in_place_pair = CLAP_INVALID_ID;
		std::snprintf(info->name, sizeof(info->name), "%s", is_input ? "A/D Input" : "Stereo Out");
		return true;
	}

	// ---- ノートの口。MIDI IN A-D と E

	static const clap_plugin_note_ports_t s_note_ports;

	static bool note_port(uint32_t index, bool is_input, clap_note_port_info_t *info)
	{
		if (!is_input || index >= kNotePorts || !info)
			return false;
		std::memset(info, 0, sizeof(*info));
		info->id = index;
		info->supported_dialects = CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_CLAP;
		info->preferred_dialect  = CLAP_NOTE_DIALECT_MIDI;
		static const char *NAMES[kNotePorts] = { "MIDI In A (Part 1-16)", "MIDI In B (Part 17-32)",
		                                         "MIDI In C (Part 33-48)", "MIDI In D (Part 49-64)",
		                                         "MIDI In E (Board)" };
		std::snprintf(info->name, sizeof(info->name), "%s", NAMES[index]);
		return true;
	}

	// ---- パラメータ。出力レベル（音源の外で掛ける素の掛け算）と、XG の値

	static const clap_plugin_params_t s_params;

	// 音を作っていないときに来たパラメータ（と、画面で触った値の知らせを出す口）
	void params_flush(const clap_input_events_t *in, const clap_output_events_t *out)
	{
		if (in) {
			// process の前にもパラメータは来る。種がまだならここで仕込む
			// （機械が ready でなければ seed_values は何もしないで、次の機会に）
			if (!m_xg.seeded())
				m_xg.seed_values();
			m_xg.begin_block();
			const uint32_t n = in->size(in);
			for (uint32_t i = 0; i < n; i++) {
				const clap_event_header_t *h = in->get(in, i);
				if (!gain_event(h))
					xg_event(h);
			}
		}
		push_out(out);
	}

	// XG の値のイベントなら、値が変わったところだけ音源へ流して true
	bool xg_event(const clap_event_header_t *h)
	{
		if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE)
			return false;
		const auto *e = reinterpret_cast<const clap_event_param_value_t *>(h);
		const int i = autom::index_of(e->param_id);
		if (i < 0)
			return false;
		const autom::entry &en = autom::entries()[size_t(i)];
		m_xg.host_value(i, autom::clamp_value(en, e->value), [&](int port, const uint8_t *bytes, int n) {
			m_engine.midi(bytes, size_t(n), port);
		});
		return true;
	}

	// ---- 画面で触った値をホストへ（ジェスチャーの始まり・値・終わり）
	//
	// 画面の糸で溜め、ホストに request_flush を頼む。ホストは flush か process の出口で受け取る

	struct out_event { clap_id id; double value; uint16_t type; };

	void on_gui_edit(const xg::param &p, int part, int value)
	{
		bool began = false;
		const int i = m_xg.gui_edit(p, part, value, began);
		if (i >= 0)
			tell_host(i, value, began);
	}

	void on_gui_edit_raw(u32 addr, int raw)
	{
		bool began = false;
		int value = 0;
		const int i = m_xg.gui_edit_raw(addr, raw, value, began);
		if (i >= 0)
			tell_host(i, value, began);
	}

	void tell_host(int i, int value, bool began)
	{
		const clap_id id = autom::entries()[size_t(i)].id;
		{
			std::lock_guard<std::mutex> lock(m_out_mutex);
			if (began)
				m_out.push_back({ id, 0.0, CLAP_EVENT_PARAM_GESTURE_BEGIN });
			m_out.push_back({ id, double(value), CLAP_EVENT_PARAM_VALUE });
		}
		if (m_host_params)
			m_host_params->request_flush(m_host);
	}

	void on_gui_idle(bool closing)
	{
		bool any = false;
		m_xg.gui_idle(closing, [&](int i) {
			std::lock_guard<std::mutex> lock(m_out_mutex);
			m_out.push_back({ autom::entries()[size_t(i)].id, 0.0, CLAP_EVENT_PARAM_GESTURE_END });
			any = true;
		});
		if (any && m_host_params)
			m_host_params->request_flush(m_host);
	}

	// 溜めた知らせを出す。音声の糸からも呼ぶので、錠が取れなければ次に回す
	void push_out(const clap_output_events_t *out)
	{
		if (!out)
			return;
		std::unique_lock<std::mutex> lock(m_out_mutex, std::try_to_lock);
		if (!lock.owns_lock() || m_out.empty())
			return;
		for (const out_event &ev : m_out) {
			if (ev.type == CLAP_EVENT_PARAM_VALUE) {
				clap_event_param_value_t e{};
				e.header.size = sizeof(e);
				e.header.time = 0;
				e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
				e.header.type = CLAP_EVENT_PARAM_VALUE;
				e.param_id = ev.id;
				e.cookie = nullptr;
				e.note_id = -1;
				e.port_index = -1;
				e.channel = -1;
				e.key = -1;
				e.value = ev.value;
				out->try_push(out, &e.header);
			} else {
				clap_event_param_gesture_t g{};
				g.header.size = sizeof(g);
				g.header.time = 0;
				g.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
				g.header.type = ev.type;
				g.param_id = ev.id;
				out->try_push(out, &g.header);
			}
		}
		m_out.clear();
	}

	// 出力レベルのイベントなら受け取って true
	bool gain_event(const clap_event_header_t *h)
	{
		if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE)
			return false;
		const auto *e = reinterpret_cast<const clap_event_param_value_t *>(h);
		if (e->param_id != kGainId)
			return false;                      // XG の値（xg_event）
		m_engine.panel().set_gain(float(std::clamp(e->value, 0.0, 1.0)));
		return true;
	}

	// ---- 状態。VST3 版の getState / setState と同じ並び
	//
	//   版（3）、出力レベル、詰めた機械まるごとの長さと中身、SmartMedia のファイル名の長さと中身

	static const clap_plugin_state_t s_state;

	bool save(const clap_ostream_t *s)
	{
		const int32_t version = 5;
		const float gain = m_engine.panel().gain();
		m_engine.card_flush();   // プロジェクトを保存するときに、カードのファイルも揃える
		if (!write_all(s, &version, sizeof(version)) || !write_all(s, &gain, sizeof(gain)))
			return false;
		const std::vector<uint8_t> blob = m_engine.save_state();
		if (blob.empty())
			return true;                  // まだ起動中など
		const std::vector<u8> packed = state_pack(blob);
		const int32_t n = int32_t(packed.size());
		if (!write_all(s, &n, sizeof(n)) || !write_all(s, packed.data(), packed.size()))
			return false;
		// 中身はプロジェクトに入れない（16MB から 128MB あるので）
		const std::string card = m_engine.card_path();
		const int32_t len = int32_t(card.size());
		if (!write_all(s, &len, sizeof(len)) || (len && !write_all(s, card.data(), card.size())))
			return false;
		// 版 4: XG の値だけの控え（VST3 版と同じ）
		const std::vector<uint8_t> setup = m_engine.save_xg_setup();
		const int32_t sn = int32_t(setup.size());
		if (!write_all(s, &sn, sizeof(sn)) || (sn && !write_all(s, setup.data(), setup.size())))
			return false;
		// 版 5: パネルの A/D INPUT のつまみ（0〜1。VST3 版と同じ）
		const float ad_knob = m_engine.panel().ad_knob();
		return write_all(s, &ad_knob, sizeof(ad_knob));
	}

	bool load(const clap_istream_t *s)
	{
		int32_t version = 0;
		float gain = 1.0f;
		if (!read_all(s, &version, sizeof(version)))
			return true;   // 空でも困らない
		if (read_all(s, &gain, sizeof(gain)) && gain >= 0.0f && gain <= 1.0f)
			m_engine.panel().set_gain(gain);
		if (m_host_params)
			m_host_params->rescan(m_host, CLAP_PARAM_RESCAN_VALUES);
		if (version < 2)
			return true;

		// 機械まるごとの状態（起動中に保存された曲では長さが 0）
		int32_t packed_size = 0;
		std::vector<u8> blob;
		if (read_all(s, &packed_size, sizeof(packed_size)) && packed_size > 0 && packed_size <= (64 << 20)) {
			std::vector<uint8_t> packed(static_cast<size_t>(packed_size));
			if (!read_all(s, packed.data(), packed.size()) || !state_unpack(packed.data(), packed.size(), blob))
				blob.clear();
		}

		// 版 3 から: 差していた SmartMedia のファイル（UTF-8）。無くなっていたら差さない
		std::string card;
		if (version >= 3) {
			int32_t len = 0;
			if (read_all(s, &len, sizeof(len)) && len > 0 && len < 4096) {
				std::string path(size_t(len), '\0');
				if (read_all(s, path.data(), path.size()))
					card = path;
			}
		}
		// 版 4 から: XG の値だけの控え。機械まるごとの状態が読めないときに使う
		std::vector<uint8_t> setup;
		if (version >= 4) {
			int32_t len = 0;
			if (read_all(s, &len, sizeof(len)) && len > 0 && len < (1 << 20)) {
				setup.resize(size_t(len));
				if (!read_all(s, setup.data(), setup.size()))
					setup.clear();
			}
		}
		// 版 5 から: パネルの A/D INPUT のつまみ。無ければ等倍の位置
		{
			float ad_knob = ui::bridge::AD_KNOB_DEFAULT, v = 0;
			if (version >= 5 && read_all(s, &v, sizeof(v)) && v >= 0.0f && v <= 1.0f)
				ad_knob = v;
			m_engine.panel().set_ad_knob(ad_knob);
		}

		// 起動が終わっていないと戻せない。終わるまで待つ
		m_engine.wait_ready(3000);
		if (!blob.empty() || !setup.empty())
			m_engine.load_state(blob.empty() ? nullptr : blob.data(), blob.size(),
			                    setup.empty() ? nullptr : setup.data(), setup.size());
		// 戻したばかりの値を bridge にも載せ直す。载せないと音声の糸が read_xg で
		// 戻す前の古い写しを引いて、ホストの照合値と食い違う（flood の温床）
		m_engine.publish_xg_now();

		if (!card.empty()) {
			std::string err;
			if (!m_engine.card_insert(card, err))
				m_engine.log_line(("SmartMedia を差せない: " + err).c_str());
		} else if (version < 3 && !m_engine.card_path().empty()) {
			m_engine.card_eject();
		}
		// XG の値が替わったので、ホストに読み直させる
		m_xg.forget_recent();
		if (m_host_params)
			m_host_params->rescan(m_host, CLAP_PARAM_RESCAN_VALUES);
		return true;
	}

	// ---- 遅れと残響

	static const clap_plugin_latency_t s_latency;
	static const clap_plugin_tail_t s_tail;

	// ---- 画面。VST3 版の plug_view をそのまま、ホストの窓の中に入れる

	static const clap_plugin_gui_t s_gui;

	bool gui_create(const char *api, bool floating)
	{
		if (!api || std::strcmp(api, kWindowApi) || floating)
			return false;
		if (!m_view)
			m_view = new plug_view(m_engine);
		return true;
	}

	void gui_destroy()
	{
		if (!m_view)
			return;
		m_view->removed();
		m_view->release();
		m_view = nullptr;
	}

	bool gui_adjust(uint32_t *w, uint32_t *h)
	{
		if (!m_view || !w || !h)
			return false;
		Steinberg::ViewRect r(0, 0, Steinberg::int32(*w), Steinberg::int32(*h));
		m_view->checkSizeConstraint(&r);
		*w = uint32_t(r.getWidth());
		*h = uint32_t(r.getHeight());
		return true;
	}

	clap_plugin_t          m_plugin{};
	const clap_host_t     *m_host = nullptr;
	const bool             m_fx = false;        // エフェクトとして作られた（kFxPlugId）
	const clap_host_params_t *m_host_params = nullptr;
	engine                 m_engine;
	autom::host            m_xg{m_engine};
	std::mutex             m_out_mutex;
	std::vector<out_event> m_out;           // 画面で触った値の、ホストへの知らせ（m_out_mutex）
	plug_view             *m_view = nullptr;
	double                 m_rate = smu2000::vst3::NATIVE_RATE;
	// 出力レベルは bridge が持つ。ここは 1 サンプルずつ寄せる途中の値
	float                  m_gain_now = 1.0f;
	std::atomic<bool>      m_hush{false};
	// **口ごとの内訳**（report_ports でログへ。issue #59）。ホストがノートを
	// どの形（MIDI・CLAP のノート）・どの口番号で渡してくるかを数える。
	// [形][口]。形 0 = MIDI のノートオン、1 = CLAP のノートオン、2 = MIDI（ノートオン以外）、
	// 3 = SysEx。口 4 は E、5 は範囲の外（-1 など）
	std::atomic<uint32_t>  m_seen[4][kNotePorts + 1] = {};
	void count_port(int kind, uint16_t index)
	{
		m_seen[kind][index < kNotePorts ? index : kNotePorts].fetch_add(1, std::memory_order_relaxed);
	}
	// 本スレッド（deactivate・消えるとき）から呼ぶ。何も来ていなければ書かない
	void report_ports()
	{
		static const char *const KIND[4] = { "MIDI のノートオン", "CLAP のノートオン", "MIDI（ほか）", "SysEx" };
		std::string line = "口ごとの内訳:";
		bool any = false;
		for (int k = 0; k < 4; k++) {
			char buf[128];
			uint32_t v[kNotePorts + 1];
			for (int i = 0; i <= kNotePorts; i++) {
				v[i] = m_seen[k][i].exchange(0, std::memory_order_relaxed);
				any = any || v[i];
			}
			std::snprintf(buf, sizeof(buf), " %s [A %u / B %u / C %u / D %u / E %u / 範囲外 %u]",
			              KIND[k], v[0], v[1], v[2], v[3], v[4], v[5]);
			line += buf;
		}
		if (const uint64_t dropped = m_bcast_dropped.exchange(0, std::memory_order_relaxed)) {
			char buf[128];
			std::snprintf(buf, sizeof(buf), "。全部の口に配られていたので口 B-D の %llu 件を捨てた",
			              (unsigned long long)dropped);
			line += buf;
			any = true;
		}
		if (any)
			m_engine.log_line(line.c_str());
	}
	// 音を出したチャンネル（口ごとに 16 ビット）。止めるときに流す先を絞る
	std::atomic<uint16_t>  m_sounded[kNotePorts] = {};

	// process で時刻順（同じ時刻はリセットを先）に並べ直すための入れ物
	struct ordered { uint32_t time, rank, index; };
	std::vector<ordered> m_order;

	// **全部の口に配られた同じメッセージは、口 A の 1 つだけ使う**（issue #59）。
	// REAPER は CLAP のノートの口を 4 つ持つプラグインに、トラックの MIDI を
	// 4 つの口すべてへ同じものとして配る（ログで確かめた: 1 音で A・B・C・D に 6 件ずつ）。
	// そのままでは口 B-D のパート（初期のピアノ）も鳴って、音色が重なって聞こえる。
	// 同じ時刻・同じ中身のものが**4 つの口すべて**に来たときだけ口 A に寄せるので、
	// 口ごとに違うものを渡すホストや、2-3 口に重ねる使い方はそのまま
	struct bcast { uint64_t key; uint32_t index; uint8_t port; };
	std::vector<bcast>   m_bcast;
	std::vector<uint8_t> m_drop;
	std::atomic<uint64_t> m_bcast_dropped{0};

	// そのイベントの「口を除いた中身」の指紋。口の無い・数えないものは 0
	static uint64_t event_key(const clap_event_header_t *h, uint32_t time, int &port)
	{
		uint64_t k = 1469598103934665603ull;
		auto mix = [&k](const void *p, size_t n) {
			const uint8_t *b = static_cast<const uint8_t *>(p);
			for (size_t i = 0; i < n; i++) {
				k ^= b[i];
				k *= 1099511628211ull;
			}
		};
		mix(&time, sizeof(time));
		mix(&h->type, sizeof(h->type));
		switch (h->type) {
		case CLAP_EVENT_MIDI: {
			const auto *e = reinterpret_cast<const clap_event_midi_t *>(h);
			port = e->port_index;
			mix(e->data, 3);
			break;
		}
		case CLAP_EVENT_MIDI_SYSEX: {
			const auto *e = reinterpret_cast<const clap_event_midi_sysex_t *>(h);
			port = e->port_index;
			if (e->buffer)
				mix(e->buffer, e->size);
			mix(&e->size, sizeof(e->size));
			break;
		}
		case CLAP_EVENT_NOTE_ON:
		case CLAP_EVENT_NOTE_OFF:
		case CLAP_EVENT_NOTE_CHOKE: {
			const auto *e = reinterpret_cast<const clap_event_note_t *>(h);
			port = e->port_index;
			mix(&e->channel, sizeof(e->channel));
			mix(&e->key, sizeof(e->key));
			mix(&e->velocity, sizeof(e->velocity));
			break;
		}
		default:
			port = -1;
			return 0;
		}
		return k ? k : 1;
	}

	// 口 A-D の 4 つすべてに同じものが来ていたら、口 B-D（と E）のものに印を付ける（m_drop）
	void mark_broadcast(const clap_input_events_t *ev, uint32_t count, uint32_t n)
	{
		m_drop.assign(count, 0);
		m_bcast.clear();
		for (uint32_t i = 0; i < count; i++) {
			const clap_event_header_t *h = ev->get(ev, i);
			if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID)
				continue;
			int port = -1;
			const uint64_t k = event_key(h, std::min(h->time, n), port);
			if (k && port >= 0 && port < kNotePorts)
				m_bcast.push_back({ k, i, uint8_t(port) });
		}
		if (m_bcast.size() < size_t(kPorts))
			return;
		std::sort(m_bcast.begin(), m_bcast.end(), [](const bcast &a, const bcast &b) {
			return a.key != b.key ? a.key < b.key : a.index < b.index;
		});
		for (size_t g = 0; g < m_bcast.size();) {
			size_t e = g;
			unsigned mask = 0;
			while (e < m_bcast.size() && m_bcast[e].key == m_bcast[g].key)
				mask |= 1u << m_bcast[e++].port;
			if ((mask & ((1u << kPorts) - 1)) == (1u << kPorts) - 1) {
				for (size_t j = g; j < e; j++)
					if (m_bcast[j].port != 0) {
						m_drop[m_bcast[j].index] = 1;
						m_bcast_dropped.fetch_add(1, std::memory_order_relaxed);
					}
			}
			g = e;
		}
	}

	// 音を作る途中の入れ物。process の間だけ有効
	float       *m_left = nullptr, *m_right = nullptr;
	const float *m_in_l = nullptr, *m_in_r = nullptr;
	uint32_t     m_done = 0;

	void fill_to(uint32_t at)
	{
		if (at <= m_done)
			return;
		m_engine.fill(m_left + m_done, m_right + m_done, int(at - m_done),
		              m_in_l ? m_in_l + m_done : nullptr, m_in_r ? m_in_r + m_done : nullptr);
		m_done = at;
	}
};


const clap_plugin_audio_ports_t mu_plugin::s_audio_ports = {
	[](const clap_plugin_t *, bool) -> uint32_t { return 1; },
	[](const clap_plugin_t *p, uint32_t index, bool is_input, clap_audio_port_info_t *info) {
		return audio_port(index, is_input, info, self(p)->m_fx);
	},
};

const clap_plugin_note_ports_t mu_plugin::s_note_ports = {
	[](const clap_plugin_t *, bool is_input) -> uint32_t { return is_input ? kNotePorts : 0; },
	[](const clap_plugin_t *, uint32_t index, bool is_input, clap_note_port_info_t *info) {
		return note_port(index, is_input, info);
	},
};

// 並びは出力レベル、XG の値（automation.h の表の順）。XG の値は XG の整数のまま（min〜max）
const clap_plugin_params_t mu_plugin::s_params = {
	[](const clap_plugin_t *) -> uint32_t { return 1 + uint32_t(autom::entries().size()); },
	[](const clap_plugin_t *, uint32_t index, clap_param_info_t *info) {
		if (!info || index > autom::entries().size())
			return false;
		std::memset(info, 0, sizeof(*info));
		if (index == 0) {
			info->id            = kGainId;
			info->flags         = CLAP_PARAM_IS_AUTOMATABLE;
			info->min_value     = 0.0;
			info->max_value     = 1.0;
			info->default_value = 1.0;
			std::snprintf(info->name, sizeof(info->name), "Output");
			return true;
		}
		const autom::entry &e = autom::entries()[index - 1];
		info->id            = e.id;
		// インサーションのパラメータは種類で範囲が変わるので、割合（0-1000）の連続の値
		info->flags         = CLAP_PARAM_IS_AUTOMATABLE |
		                      (e.k == autom::kind::insertion ? 0 : CLAP_PARAM_IS_STEPPED);
		info->min_value     = autom::lo(e);
		info->max_value     = autom::hi(e);
		info->default_value = autom::def(e);
		std::snprintf(info->name, sizeof(info->name), "%s", e.name.c_str());
		std::snprintf(info->module, sizeof(info->module), "%s",
		              e.k == autom::kind::insertion ? e.group.c_str() : e.is_part ? ("Parts/" + e.group).c_str() : "Master");
		return true;
	},
	[](const clap_plugin_t *p, clap_id id, double *out) {
		if (!out)
			return false;
		if (id == kGainId) {
			*out = self(p)->m_engine.panel().gain();
			return true;
		}
		const int i = autom::index_of(id);
		if (i < 0)
			return false;
		*out = double(self(p)->m_xg.shown_value(i));
		return true;
	},
	[](const clap_plugin_t *p, clap_id id, double v, char *buf, uint32_t cap) {
		if (!buf || !cap)
			return false;
		if (id == kGainId) {
			std::snprintf(buf, cap, "%.0f %%", v * 100.0);
			return true;
		}
		const int i = autom::index_of(id);
		if (i < 0)
			return false;
		const autom::entry &e = autom::entries()[size_t(i)];
		std::snprintf(buf, cap, "%s", autom::text(e, autom::clamp_value(e, v), self(p)->m_xg.view_ram()).c_str());
		return true;
	},
	[](const clap_plugin_t *p, clap_id id, const char *text, double *out) {
		if (!text || !out)
			return false;
		if (id == kGainId) {
			*out = std::clamp(std::atof(text) / 100.0, 0.0, 1.0);
			return true;
		}
		const int i = autom::index_of(id);
		int value = 0;
		if (i < 0 || !autom::parse(autom::entries()[size_t(i)], text, value, self(p)->m_xg.view_ram()))
			return false;
		*out = double(value);
		return true;
	},
	[](const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out) {
		self(p)->params_flush(in, out);
	},
};

const clap_plugin_state_t mu_plugin::s_state = {
	[](const clap_plugin_t *p, const clap_ostream_t *s) { return s && self(p)->save(s); },
	[](const clap_plugin_t *p, const clap_istream_t *s) { return s && self(p)->load(s); },
};

const clap_plugin_latency_t mu_plugin::s_latency = {
	[](const clap_plugin_t *p) -> uint32_t { return self(p)->m_engine.latency_samples(); },
};

// 残響がある。4 秒みておく
const clap_plugin_tail_t mu_plugin::s_tail = {
	[](const clap_plugin_t *p) -> uint32_t { return uint32_t(self(p)->m_rate * 4.0); },
};

const clap_plugin_gui_t mu_plugin::s_gui = {
	// is_api_supported
	[](const clap_plugin_t *, const char *api, bool floating) {
		return api && !std::strcmp(api, kWindowApi) && !floating;
	},
	// get_preferred_api
	[](const clap_plugin_t *, const char **api, bool *floating) {
		if (api) *api = kWindowApi;
		if (floating) *floating = false;
		return true;
	},
	// create / destroy
	[](const clap_plugin_t *p, const char *api, bool floating) { return self(p)->gui_create(api, floating); },
	[](const clap_plugin_t *p) { self(p)->gui_destroy(); },
	// set_scale: 大きさはホストの窓の大きさで決まる（パネルは描くときに合わせる）
	[](const clap_plugin_t *, double) { return false; },
	// get_size
	[](const clap_plugin_t *p, uint32_t *w, uint32_t *h) {
		plug_view *v = self(p)->m_view;
		if (!v || !w || !h)
			return false;
		*w = uint32_t(v->reported_width());
		*h = uint32_t(v->reported_height());
		return true;
	},
	// can_resize
	[](const clap_plugin_t *) { return true; },
	// get_resize_hints。横に長い機械なので、縦横比はこちらで決める。ただし比はホストに保たせない:
	// 高さは「幅 × 400/1000 ＋ 上の帯」で、単純な比ではない。1000:400 を保たせると、ホストが比に
	// 合わせて幅を広げ、adjust_size が帯の分だけ高さを足し、また幅を広げ…と、開くたび・つまむたびに
	// 大きくなっていった（issue #246）。比は adjust_size（checkSizeConstraint）だけが決める
	[](const clap_plugin_t *, clap_gui_resize_hints_t *hints) {
		if (!hints)
			return false;
		hints->can_resize_horizontally = true;
		hints->can_resize_vertically   = true;
		hints->preserve_aspect_ratio   = false;
		hints->aspect_ratio_width      = uint32_t(ui::LOGICAL_W);
		hints->aspect_ratio_height     = uint32_t(ui::LOGICAL_H);
		return true;
	},
	// adjust_size
	[](const clap_plugin_t *p, uint32_t *w, uint32_t *h) { return self(p)->gui_adjust(w, h); },
	// set_size
	[](const clap_plugin_t *p, uint32_t w, uint32_t h) {
		plug_view *v = self(p)->m_view;
		if (!v)
			return false;
		Steinberg::ViewRect r(0, 0, Steinberg::int32(w), Steinberg::int32(h));
		return v->onSize(&r) == Steinberg::kResultOk;
	},
	// set_parent
	[](const clap_plugin_t *p, const clap_window_t *win) {
		plug_view *v = self(p)->m_view;
		if (!v || !win || !win->api || std::strcmp(win->api, kWindowApi))
			return false;
		return v->attached(win->ptr, smu2000::vst3::plug_window_type()) == Steinberg::kResultOk;
	},
	// set_transient: 浮いた窓は出さない
	[](const clap_plugin_t *, const clap_window_t *) { return false; },
	// suggest_title
	[](const clap_plugin_t *, const char *) {},
	// show / hide: 子窓はホストの窓と一緒に見え隠れする
	[](const clap_plugin_t *) { return true; },
	[](const clap_plugin_t *) { return true; },
};


clap_process_status mu_plugin::process(const clap_process_t *pr)
{
	const uint32_t n = pr->frames_count;
	clap_audio_buffer_t *out = pr->audio_outputs_count > 0 ? &pr->audio_outputs[0] : nullptr;
	// 64bit 浮動小数は受けないと答えてある。それでも来たら音を出さない
	if (!out || !out->data32 || out->channel_count < 1) {
		params_flush(pr->in_events, pr->out_events);
		return CLAP_PROCESS_CONTINUE;
	}
	m_left  = out->data32[0];
	m_right = out->channel_count > 1 ? out->data32[1] : m_left;
	// A/D INPUT。繋がっていなければ無し
	const clap_audio_buffer_t *in = pr->audio_inputs_count > 0 ? &pr->audio_inputs[0] : nullptr;
	m_in_l = (in && in->data32 && in->channel_count > 0) ? in->data32[0] : nullptr;
	m_in_r = (in && in->data32 && in->channel_count > 1) ? in->data32[1] : m_in_l;
	m_done = 0;

	// 止められたときは、鳴らしたチャンネルだけを黙らせる。全チャンネルへ流すと
	// 1 口につき 192 バイト＝61ms ぶんの直列になり、次の最初の音がそのぶん遅れる（issue #15）
	if (m_hush.exchange(false)) {
		uint16_t mask[kNotePorts];
		bool any = false;
		for (int p = 0; p < kNotePorts; p++) {
			mask[p] = m_sounded[p].exchange(0);
			any = any || mask[p];
		}
		if (any)
			m_engine.all_notes_off(mask, kNotePorts);
	}

	// 最初の区間（と状態を戻した直後）の頭で、RAM から「音源の今の値」を
	// 種に仕込む。ホストが再生頭で流す 1,300 個近いパラメータの再送は
	// ほとんどがこの値と一致するので、直列に載る前に弾ける（seed_values）
	if (!m_xg.seeded())
		m_xg.seed_values();

	// イベントは時刻順に来る。その時刻まで音を作ってから流す
	m_xg.begin_block();
	if (const clap_input_events_t *ev = pr->in_events) {
		// 同じ時刻ならリセットの SysEx を先に（engine.h の is_reset_sysex。issue #51）。
		// ホストによっては SysEx をチャンネルメッセージの後ろにまとめて渡してくる
		const uint32_t count = ev->size(ev);
		m_order.clear();
		mark_broadcast(ev, count, n);
		for (uint32_t i = 0; i < count; i++) {
			const clap_event_header_t *h = ev->get(ev, i);
			if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID || m_drop[i])
				continue;
			bool reset = false;
			if (h->type == CLAP_EVENT_MIDI_SYSEX) {
				const auto *e = reinterpret_cast<const clap_event_midi_sysex_t *>(h);
				reset = e->buffer && smu2000::vst3::is_reset_sysex(e->buffer, e->size);
			}
			m_order.push_back({ std::min(h->time, n), reset ? 0u : 1u, i });
		}
		std::sort(m_order.begin(), m_order.end(), [](const ordered &a, const ordered &b) {
			if (a.time != b.time)
				return a.time < b.time;
			if (a.rank != b.rank)
				return a.rank < b.rank;
			return a.index < b.index;
		});
		for (const ordered &o : m_order) {
			fill_to(o.time);
			event(ev->get(ev, o.index));
		}
	}
	fill_to(n);

	// 出力レベル。一気に変えると音が跳ねるので 1 サンプルずつ寄せる
	const float target = m_engine.panel().gain();
	if (target != m_gain_now || target != 1.0f) {
		const float step = 1.0f / 512.0f;
		for (uint32_t i = 0; i < n; i++) {
			if (m_gain_now < target) m_gain_now = std::min(target, m_gain_now + step);
			else if (m_gain_now > target) m_gain_now = std::max(target, m_gain_now - step);
			m_left[i] *= m_gain_now;
			if (m_right != m_left)
				m_right[i] *= m_gain_now;
		}
	}
	for (uint32_t c = 2; c < out->channel_count; c++)
		std::memset(out->data32[c], 0, n * sizeof(float));
	out->constant_mask = 0;

	m_left = m_right = nullptr;
	m_in_l = m_in_r = nullptr;
	// 画面で触った値の知らせ
	push_out(pr->out_events);
	return CLAP_PROCESS_CONTINUE;
}

void mu_plugin::event(const clap_event_header_t *h)
{
	if (gain_event(h) || xg_event(h))
		return;
	switch (h->type) {
	case CLAP_EVENT_MIDI: {
		const auto *e = reinterpret_cast<const clap_event_midi_t *>(h);
		const int port = port_of(e->port_index);
		count_port((e->data[0] & 0xf0) == 0x90 && e->data[2] ? 0 : 2, e->port_index);
		if ((e->data[0] & 0xf0) == 0x90 && e->data[2])
			m_sounded[port] |= uint16_t(1u << (e->data[0] & 15));
		m_engine.midi(e->data, size_t(midi_length(e->data[0])), port);
		break;
	}
	case CLAP_EVENT_MIDI_SYSEX: {
		const auto *e = reinterpret_cast<const clap_event_midi_sysex_t *>(h);
		count_port(3, e->port_index);
		if (e->buffer && e->size)
			m_engine.midi(e->buffer, e->size, port_of(e->port_index));
		break;
	}
	// CLAP 流のノート。チャンネルやキーが「どれでも」（-1）なら MIDI にできないので捨てる
	case CLAP_EVENT_NOTE_ON:
	case CLAP_EVENT_NOTE_OFF:
	case CLAP_EVENT_NOTE_CHOKE: {
		const auto *e = reinterpret_cast<const clap_event_note_t *>(h);
		if (h->type == CLAP_EVENT_NOTE_ON)
			count_port(1, uint16_t(e->port_index));
		if (e->channel < 0 || e->channel > 15 || e->key < 0 || e->key > 127)
			break;
		const bool on = h->type == CLAP_EVENT_NOTE_ON;
		int v = int(std::lround(e->velocity * 127.0));
		v = on ? std::clamp(v, 1, 127) : std::clamp(v, 0, 127);
		const uint8_t msg[3] = { uint8_t((on ? 0x90 : 0x80) | e->channel), uint8_t(e->key), uint8_t(v) };
		const int port = port_of(e->port_index);
		if (on && v)
			m_sounded[port] |= uint16_t(1u << (e->channel & 15));
		m_engine.midi(msg, 3, port);
		break;
	}
	default:
		break;
	}
}


// ---- 工場

const clap_plugin_factory_t g_factory = {
	// 0 = 音源、1 = 同じ本体をエフェクトとして
	[](const clap_plugin_factory *) -> uint32_t { return 2; },
	[](const clap_plugin_factory *, uint32_t index) -> const clap_plugin_descriptor_t * {
		return index == 0 ? &kDescriptor : index == 1 ? &kFxDescriptor : nullptr;
	},
	[](const clap_plugin_factory *, const clap_host_t *host, const char *id) -> const clap_plugin_t * {
		if (!host || !id || !clap_version_is_compatible(host->clap_version))
			return nullptr;
		const bool fx = !std::strcmp(id, kFxPlugId);
		if (!fx && std::strcmp(id, kPlugId))
			return nullptr;
		return (new mu_plugin(host, fx))->plugin();
	},
};

} // namespace


// ---- DLL の出口。ホストはまずこれを取りに来る

namespace {

bool entry_init(const char *) { return true; }
void entry_deinit() {}
const void *entry_factory(const char *id)
{
	return (id && !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID)) ? &g_factory : nullptr;
}

} // namespace

extern "C" {

CLAP_EXPORT extern const clap_plugin_entry_t clap_entry = {
	CLAP_VERSION_INIT,
	entry_init,
	entry_deinit,
	entry_factory,
};

}
