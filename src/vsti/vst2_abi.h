// license:BSD-3-Clause
//
// Minimal VST 2.4 binary interface used by S-MU2000.
//
// This is an independent declaration of the public host/plugin ABI.  It keeps
// the build self-contained; no discontinued VST2 SDK source or header is used.

#ifndef S_MU2000_VSTI_VST2_ABI_H
#define S_MU2000_VSTI_VST2_ABI_H

#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define SMU_VSTCALLBACK __cdecl
#else
#define SMU_VSTCALLBACK
#endif

namespace smu2000::vsti {

using vint32 = std::int32_t;
using vintptr = std::intptr_t;

struct effect;

using host_callback = vintptr (SMU_VSTCALLBACK *)(effect *, vint32, vint32,
                                                  vintptr, void *, float);
using dispatcher_proc = vintptr (SMU_VSTCALLBACK *)(effect *, vint32, vint32,
                                                    vintptr, void *, float);
using process_proc = void (SMU_VSTCALLBACK *)(effect *, float **, float **, vint32);
using set_parameter_proc = void (SMU_VSTCALLBACK *)(effect *, vint32, float);
using get_parameter_proc = float (SMU_VSTCALLBACK *)(effect *, vint32);

struct effect {
	vint32 magic;
	dispatcher_proc dispatcher;
	process_proc process;
	set_parameter_proc set_parameter;
	get_parameter_proc get_parameter;
	vint32 num_programs;
	vint32 num_params;
	vint32 num_inputs;
	vint32 num_outputs;
	vint32 flags;
	vintptr reserved1;
	vintptr reserved2;
	vint32 initial_delay;
	vint32 real_qualities;
	vint32 off_qualities;
	float io_ratio;
	void *object;
	void *user;
	vint32 unique_id;
	vint32 version;
	process_proc process_replacing;
	process_proc process_double_replacing;
	char future[56];
};

struct event {
	vint32 type;
	vint32 byte_size;
	vint32 delta_frames;
	vint32 flags;
	char data[16];
};

struct midi_event {
	vint32 type;
	vint32 byte_size;
	vint32 delta_frames;
	vint32 flags;
	vint32 note_length;
	vint32 note_offset;
	std::uint8_t midi_data[4];
	std::int8_t detune;
	std::uint8_t note_off_velocity;
	std::uint8_t reserved1;
	std::uint8_t reserved2;
};

struct sysex_event {
	vint32 type;
	vint32 byte_size;
	vint32 delta_frames;
	vint32 flags;
	vint32 dump_bytes;
	vintptr reserved1;
	char *dump;
	vintptr reserved2;
};

struct events {
	vint32 count;
	vintptr reserved;
	event *items[2];
};

struct rect {
	std::int16_t top;
	std::int16_t left;
	std::int16_t bottom;
	std::int16_t right;
};

constexpr vint32 fourcc(char a, char b, char c, char d)
{
	return (vint32(std::uint8_t(a)) << 24) | (vint32(std::uint8_t(b)) << 16) |
	       (vint32(std::uint8_t(c)) << 8) | vint32(std::uint8_t(d));
}

constexpr vint32 effect_magic = fourcc('V', 's', 't', 'P');
constexpr vint32 plugin_id = fourcc('S', 'M', 'U', '2');

enum effect_flags : vint32 {
	has_editor = 1 << 0,
	can_replacing = 1 << 4,
	program_chunks = 1 << 5,
	is_synth = 1 << 8,
};

enum event_type : vint32 {
	midi_type = 1,
	sysex_type = 6,
};

enum dispatcher_opcode : vint32 {
	eff_open = 0,
	eff_close = 1,
	eff_set_program = 2,
	eff_get_program = 3,
	eff_set_program_name = 4,
	eff_get_program_name = 5,
	eff_get_param_label = 6,
	eff_get_param_display = 7,
	eff_get_param_name = 8,
	eff_set_sample_rate = 10,
	eff_set_block_size = 11,
	eff_mains_changed = 12,
	eff_edit_get_rect = 13,
	eff_edit_open = 14,
	eff_edit_close = 15,
	eff_edit_idle = 19,
	eff_get_chunk = 23,
	eff_set_chunk = 24,
	eff_process_events = 25,
	eff_can_be_automated = 26,
	eff_string_to_parameter = 27,
	eff_get_program_name_indexed = 29,
	eff_get_plug_category = 35,
	eff_get_effect_name = 45,
	eff_get_vendor_string = 47,
	eff_get_product_string = 48,
	eff_get_vendor_version = 49,
	eff_can_do = 51,
	eff_get_tail_size = 52,
	eff_get_vst_version = 58,
	eff_start_process = 71,
	eff_stop_process = 72,
	eff_set_process_precision = 77,
	eff_get_midi_input_channels = 78,
	eff_get_midi_output_channels = 79,
};

enum host_opcode : vint32 {
	host_version = 1,
	host_want_midi = 6,
};

enum plugin_category : vint32 {
	category_synth = 2,
};

static_assert(sizeof(event) == 32, "VST event ABI mismatch");
static_assert(sizeof(midi_event) == 32, "VST MIDI event ABI mismatch");
static_assert(sizeof(sysex_event) == (sizeof(void *) == 8 ? 48 : 32),
              "VST SysEx event ABI mismatch");
static_assert(offsetof(events, items) == (sizeof(void *) == 8 ? 16 : 8),
              "VST event list ABI mismatch");
static_assert(offsetof(effect, dispatcher) == (sizeof(void *) == 8 ? 8 : 4),
              "VST effect ABI mismatch");
static_assert(sizeof(effect) == (sizeof(void *) == 8 ? 192 : 144),
              "VST effect size mismatch");

} // namespace smu2000::vsti

#endif // S_MU2000_VSTI_VST2_ABI_H
