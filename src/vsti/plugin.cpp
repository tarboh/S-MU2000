// license:BSD-3-Clause
//
// Legacy VST 2.4 instrument wrapper.  The emulation, ROM loading, resampling,
// state, and panel are shared with the VST3 and CLAP builds; only this ABI shim
// is format-specific.

#include "vst2_abi.h"

#include "state.h"
#include "vst3/engine.h"
#include "vst3/plug_window.h"
#include "vst3/view.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <new>
#include <vector>

namespace smu2000::vsti {

namespace {

constexpr std::uint8_t chunk_magic[8] = { 'S', 'M', 'U', '2', 'V', 'S', 'T', '2' };
constexpr std::uint32_t chunk_version = 1;

void copy_text(void *dst, const char *text, std::size_t capacity)
{
	if (!dst || !capacity)
		return;
	char *out = static_cast<char *>(dst);
	std::snprintf(out, capacity, "%s", text ? text : "");
}

template<typename T> void append(std::vector<std::uint8_t> &out, const T &value)
{
	const auto *p = reinterpret_cast<const std::uint8_t *>(&value);
	out.insert(out.end(), p, p + sizeof(value));
}

class plugin
{
public:
	explicit plugin(host_callback host) : m_host(host)
	{
		std::memset(&m_effect, 0, sizeof(m_effect));
		m_effect.magic = effect_magic;
		m_effect.dispatcher = dispatch_thunk;
		m_effect.process = process_thunk;
		m_effect.set_parameter = set_parameter_thunk;
		m_effect.get_parameter = get_parameter_thunk;
		m_effect.num_programs = 1;
		m_effect.num_params = 1;
		m_effect.num_inputs = 0;
		m_effect.num_outputs = 2;
		m_effect.flags = has_editor | can_replacing | program_chunks | is_synth;
		m_effect.object = this;
		m_effect.unique_id = plugin_id;
		m_effect.version = 1000;
		m_effect.process_replacing = process_thunk;
		m_engine.panel().set_gain(1.0f);
		set_rate(smu2000::vst3::NATIVE_RATE);
		m_events.reserve(1024);
	}

	~plugin()
	{
		close_editor();
	}

	effect *interface() { return &m_effect; }

private:
	struct queued_event {
		vint32 offset = 0;
		std::uint32_t sequence = 0;
		std::vector<std::uint8_t> bytes;
	};

	static plugin *self(effect *e)
	{
		return e ? static_cast<plugin *>(e->object) : nullptr;
	}

	static vintptr SMU_VSTCALLBACK dispatch_thunk(effect *e, vint32 opcode,
	                                              vint32 index, vintptr value,
	                                              void *ptr, float opt)
	{
		try {
			plugin *p = self(e);
			return p ? p->dispatch(opcode, index, value, ptr, opt) : 0;
		} catch (...) {
			return 0;
		}
	}

	static void SMU_VSTCALLBACK process_thunk(effect *e, float **inputs,
	                                          float **outputs, vint32 frames)
	{
		plugin *p = self(e);
		if (!p)
			return;
		try {
			p->process(inputs, outputs, frames);
		} catch (...) {
			if (outputs && frames > 0) {
				if (outputs[0]) std::memset(outputs[0], 0, std::size_t(frames) * sizeof(float));
				if (outputs[1]) std::memset(outputs[1], 0, std::size_t(frames) * sizeof(float));
			}
		}
	}

	static void SMU_VSTCALLBACK set_parameter_thunk(effect *e, vint32 index, float value)
	{
		if (plugin *p = self(e); p && index == 0)
			p->m_engine.panel().set_gain(std::clamp(value, 0.0f, 1.0f));
	}

	static float SMU_VSTCALLBACK get_parameter_thunk(effect *e, vint32 index)
	{
		plugin *p = self(e);
		return p && index == 0 ? p->m_engine.panel().gain() : 0.0f;
	}

	void set_rate(double rate)
	{
		if (!(rate > 1000.0))
			return;
		m_rate = rate;
		m_engine.set_output_rate(rate);
		m_effect.initial_delay = vint32(m_engine.latency_samples());
	}

	void set_processing(bool on)
	{
		if (!on)
			m_hush.store(true, std::memory_order_release);
		m_engine.set_processing(on);
	}

	void close_editor()
	{
		if (!m_view)
			return;
		m_view->removed();
		m_view->release();
		m_view = nullptr;
	}

	vintptr dispatch(vint32 opcode, vint32 index, vintptr value, void *ptr, float opt)
	{
		switch (opcode) {
		case eff_open:
			m_engine.start();
			if (m_host)
				m_host(&m_effect, host_want_midi, 0, 1, nullptr, 0);
			return 1;
		case eff_close:
			delete this;
			return 1;
		case eff_set_program:
		case eff_set_program_name:
		case eff_set_block_size:
		case eff_edit_idle:
			return 1;
		case eff_get_program:
			return 0;
		case eff_get_program_name:
		case eff_get_program_name_indexed:
			copy_text(ptr, "Default", 24);
			return 1;
		case eff_get_param_name:
			if (index == 0) copy_text(ptr, "Output", 8);
			return index == 0;
		case eff_get_param_display:
			if (index == 0) {
				char text[16];
				std::snprintf(text, sizeof(text), "%.0f", m_engine.panel().gain() * 100.0f);
				copy_text(ptr, text, 8);
			}
			return index == 0;
		case eff_get_param_label:
			if (index == 0) copy_text(ptr, "%", 8);
			return index == 0;
		case eff_set_sample_rate:
			set_rate(opt);
			return 1;
		case eff_mains_changed:
			set_processing(value != 0);
			return 1;
		case eff_start_process:
			set_processing(true);
			return 1;
		case eff_stop_process:
			set_processing(false);
			return 1;
		case eff_process_events:
			return queue_events(static_cast<const events *>(ptr));
		case eff_get_chunk:
			return get_chunk(ptr);
		case eff_set_chunk:
			return set_chunk(ptr, value);
		case eff_can_be_automated:
			return index == 0;
		case eff_string_to_parameter:
			if (index == 0 && ptr) {
				const float gain = std::clamp(float(std::atof(static_cast<const char *>(ptr)) / 100.0), 0.0f, 1.0f);
				m_engine.panel().set_gain(gain);
				return 1;
			}
			return 0;
		case eff_edit_get_rect:
			if (!ptr) return 0;
			*static_cast<rect **>(ptr) = &m_rect;
			return 1;
		case eff_edit_open:
			if (!ptr) return 0;
			close_editor();
			m_view = new smu2000::vst3::plug_view(m_engine);
			if (m_view->attached(ptr, smu2000::vst3::plug_window_type()) != Steinberg::kResultOk) {
				m_view->release();
				m_view = nullptr;
				return 0;
			}
			return 1;
		case eff_edit_close:
			close_editor();
			return 1;
		case eff_get_plug_category:
			return category_synth;
		case eff_get_effect_name:
			copy_text(ptr, "S-MU2000", 64);
			return 1;
		case eff_get_vendor_string:
			copy_text(ptr, "S-MU2000", 64);
			return 1;
		case eff_get_product_string:
			copy_text(ptr, "S-MU2000", 64);
			return 1;
		case eff_get_vendor_version:
			return 1000;
		case eff_can_do:
			if (!ptr) return 0;
			if (!std::strcmp(static_cast<const char *>(ptr), "receiveVstEvents") ||
			    !std::strcmp(static_cast<const char *>(ptr), "receiveVstMidiEvent") ||
			    !std::strcmp(static_cast<const char *>(ptr), "receiveVstSysexEvent"))
				return 1;
			return -1;
		case eff_get_tail_size:
			return vintptr(m_rate * 4.0);
		case eff_get_vst_version:
			return 2400;
		case eff_set_process_precision:
			return value == 0; // 32-bit floating point only
		case eff_get_midi_input_channels:
			return 16;
		case eff_get_midi_output_channels:
			return 0;
		default:
			return 0;
		}
	}

	vintptr queue_events(const events *list)
	{
		if (!list || list->count < 0 || list->count > 65536)
			return 0;
		for (vint32 i = 0; i < list->count; i++) {
			const event *base = list->items[i];
			if (!base)
				continue;
			queued_event q;
			q.offset = std::max<vint32>(0, base->delta_frames);
			q.sequence = m_sequence++;
			if (base->type == midi_type && base->byte_size >= vint32(sizeof(midi_event))) {
				const auto *m = reinterpret_cast<const midi_event *>(base);
				const int n = smu2000::vst3::midi_length(m->midi_data[0]);
				q.bytes.assign(m->midi_data, m->midi_data + n);
			} else if (base->type == sysex_type && base->byte_size >= vint32(sizeof(sysex_event))) {
				const auto *s = reinterpret_cast<const sysex_event *>(base);
				if (s->dump && s->dump_bytes > 0 && s->dump_bytes <= 1024 * 1024)
					q.bytes.assign(reinterpret_cast<const std::uint8_t *>(s->dump),
					               reinterpret_cast<const std::uint8_t *>(s->dump) + s->dump_bytes);
			}
			if (!q.bytes.empty()) {
				if ((q.bytes[0] & 0xf0) == 0x90 && q.bytes.size() >= 3 && q.bytes[2])
					m_sounded.fetch_or(std::uint16_t(1u << (q.bytes[0] & 15)),
					                   std::memory_order_relaxed);
				m_events.push_back(std::move(q));
			}
		}
		return 1;
	}

	vintptr get_chunk(void *ptr)
	{
		if (!ptr)
			return 0;
		const float gain = m_engine.panel().gain();
		const std::vector<std::uint8_t> packed = state_pack(m_engine.save_state());
		m_chunk.clear();
		m_chunk.insert(m_chunk.end(), std::begin(chunk_magic), std::end(chunk_magic));
		append(m_chunk, chunk_version);
		append(m_chunk, gain);
		m_chunk.insert(m_chunk.end(), packed.begin(), packed.end());
		*static_cast<void **>(ptr) = m_chunk.data();
		return vintptr(m_chunk.size());
	}

	vintptr set_chunk(const void *ptr, vintptr size)
	{
		constexpr std::size_t header_size = sizeof(chunk_magic) + sizeof(chunk_version) + sizeof(float);
		if (!ptr || size < vintptr(header_size))
			return 0;
		const auto *p = static_cast<const std::uint8_t *>(ptr);
		if (std::memcmp(p, chunk_magic, sizeof(chunk_magic)) != 0)
			return 0;
		std::uint32_t version = 0;
		float gain = 1.0f;
		std::memcpy(&version, p + sizeof(chunk_magic), sizeof(version));
		std::memcpy(&gain, p + sizeof(chunk_magic) + sizeof(version), sizeof(gain));
		if (version != chunk_version || !std::isfinite(gain))
			return 0;
		std::vector<std::uint8_t> raw;
		if (!state_unpack(p + header_size, std::size_t(size) - header_size, raw))
			return 0;
		m_engine.panel().set_gain(std::clamp(gain, 0.0f, 1.0f));
		if (raw.empty())
			return 1;
		return m_engine.load_state(raw.data(), raw.size()) ? 1 : 0;
	}

	void process(float **, float **outputs, vint32 frames)
	{
		if (frames <= 0)
			return;
		float *left = outputs ? outputs[0] : nullptr;
		float *right = outputs ? outputs[1] : nullptr;
		if (!left || !right) {
			m_events.clear();
			return;
		}

		if (m_hush.exchange(false, std::memory_order_acq_rel)) {
			const std::uint16_t sounded = m_sounded.exchange(0, std::memory_order_acq_rel);
			if (sounded)
				m_engine.all_notes_off(sounded, 0);
		}

		std::stable_sort(m_events.begin(), m_events.end(), [](const queued_event &a, const queued_event &b) {
			return a.offset != b.offset ? a.offset < b.offset : a.sequence < b.sequence;
		});
		vint32 done = 0;
		for (const queued_event &e : m_events) {
			const vint32 at = std::clamp(e.offset, done, frames);
			if (at > done)
				m_engine.fill(left + done, right + done, at - done);
			m_engine.midi(e.bytes.data(), e.bytes.size());
			done = at;
		}
		m_events.clear();
		if (done < frames)
			m_engine.fill(left + done, right + done, frames - done);

		const float target = m_engine.panel().gain();
		if (target != m_gain_now || target != 1.0f) {
			constexpr float step = 1.0f / 512.0f;
			for (vint32 i = 0; i < frames; i++) {
				if (m_gain_now < target) m_gain_now = std::min(target, m_gain_now + step);
				else if (m_gain_now > target) m_gain_now = std::max(target, m_gain_now - step);
				left[i] *= m_gain_now;
				right[i] *= m_gain_now;
			}
		}
	}

	effect m_effect{};
	host_callback m_host = nullptr;
	smu2000::vst3::engine m_engine;
	smu2000::vst3::plug_view *m_view = nullptr;
	double m_rate = smu2000::vst3::NATIVE_RATE;
	float m_gain_now = 1.0f;
	std::atomic<bool> m_hush{false};
	std::atomic<std::uint16_t> m_sounded{0};
	std::vector<queued_event> m_events;
	std::vector<std::uint8_t> m_chunk;
	std::uint32_t m_sequence = 0;
	rect m_rect{0, 0, 360, 1400};
};

} // namespace

effect *create_plugin(host_callback host)
{
	return (new plugin(host))->interface();
}

} // namespace smu2000::vsti

extern "C" __declspec(dllexport) smu2000::vsti::effect *SMU_VSTCALLBACK
VSTPluginMain(smu2000::vsti::host_callback host)
{
	if (!host)
		return nullptr;
	try {
		return smu2000::vsti::create_plugin(host);
	} catch (...) {
		return nullptr;
	}
}
