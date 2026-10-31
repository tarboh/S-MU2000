// license:BSD-3-Clause
//
// Roland RCP シーケンサーファイルの解析入口。
// RCP2 の固定ヘッダー、トラックヘッダー、未解釈のシーケンスレコードを解析する。

#ifndef S_MU2000_RCP_H
#define S_MU2000_RCP_H

#pragma once

#include "compat/mamecompat.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace rcp {

enum class format {
	unknown,
	rcp2,
};

struct header_info {
	format type = format::unknown;
	size_t minimum_header_size = 0;
};

// RCP ファイルの形式を識別する。成功してもイベント解析・再生を保証しない。
// data はファイル先頭、size はバイト数。
bool inspect_header(const u8 *data, size_t size, header_info &out, std::string &err);

// RCP2 固定ヘッダーを読む。文字列は文字コード変換せず、末尾の空白/NUL のみ除去する。
// 現段階ではトラックデータの存在やイベント列の正当性は検証しない。
struct user_sysex_template {
	std::string name;
	std::vector<u8> data; // raw template bytes; trailing F7 padding is removed
};

struct document_header {
	std::string title;
	std::string comment;
	u16 ticks_per_quarter = 0;
	u8 tempo_bpm = 0;
	u8 beat_numerator = 0;
	u8 beat_denominator = 0;
	u8 key_signature = 0;
	u8 play_bias = 0; // raw signed 8-bit document transpose (reference-compatible interpretation)
	u8 track_count = 0;
	std::array<user_sysex_template, 8> user_sysex{};
};

bool parse_header(const u8 *data, size_t size, document_header &out, std::string &err);

// RCP2 トラックヘッダー（0x2c bytes）。意味が確定していない値は生バイトで保持する。
// encoded_length は下位2 bitを16/17 bitへ展開した、ヘッダーを含む実効トラック長。
struct track_header {
	u32 encoded_length = 0;
	u8 track_id = 0;
	u8 rhythm_mode = 0;
	u8 midi_channel = 0;
	u8 key = 0; // raw track transposition byte (including rhythm-channel flag)
	u8 tick_offset = 0;
	u8 mute = 0;
	std::string name;
};

bool parse_track_header(const u8 *data, size_t size, track_header &out, std::string &err);

// RCP2 のシーケンスレコードを生データのまま読む。
// 各レコードは 4 bytes。FE (Track End) を含めて返し、FE までの消費 byte 数を返す。
// コマンドの意味・p0 の時間解釈・ループ展開はこの段階では行わない。
// FE がない、または途中でレコードが切れている入力は失敗とする。
struct sequence_event {
	u8 code = 0; // 00..7F: note height, 80..FF: command ID
	u8 p0 = 0;   // delay field; interpretation depends on command
	u8 p1 = 0;
	u8 p2 = 0;
};

bool parse_sequence(const u8 *data, size_t size,
                    std::vector<sequence_event> &out,
                    size_t &bytes_consumed, std::string &err);

// Experimental F6 comment-data collector based on ValleyBell/MidiConverters' RCP2
// handler, not an official-format guarantee. Starting at start_index (which must
// name F6), concatenate p1/p2 from F6 and immediately following F7 records.
// Stop before any other record, including FE; records_consumed counts only F6/F7.
// Requires a Track End (FE) somewhere at or after start_index. Failure clears out
// and sets records_consumed to zero.
bool extract_comment_data(const std::vector<sequence_event> &sequence,
                          size_t start_index, std::vector<u8> &out,
                          size_t &records_consumed, std::string &err);

// Experimental helper matching the referenced converter's trailing-ASCII-space trim.
// Preserves leading/interior spaces and all other bytes (including NUL). Returns new size.
size_t trim_comment_trailing_spaces(std::vector<u8> &data);

// Experimental F5 decoder based on ValleyBell/MidiConverters' RCP2 handler.
// p0 is interpreted as key signature, not delay; delay_advance is always zero.
struct key_signature_event {
	int accidentals = 0; // negative = flats, positive = sharps
	bool minor = false;
	u8 delay_advance = 0;
};

bool decode_key_signature_event(const sequence_event &raw,
                                key_signature_event &out, std::string &err);

// Parsed RCP2 track. raw_length includes the 2-byte length field and the full
// 0x2c-byte track header; sequence contains records through FE.
struct track {
    track_header header;
    std::vector<sequence_event> sequence;
};

bool parse_tracks(const u8 *data, size_t size,
                  std::vector<track> &out, std::string &err);

 
// コマンドの意味を決めつけず、解析済みレコードを後段が扱いやすい形にまとめる。
// record_index はトラック内の 0 始まりのレコード番号。時間/tick への変換はまだ行わない。
// Decoded view of a note record (code 0x00..0x7f). This does not convert
// delay/duration to absolute ticks and does not apply transposition or tempo.
struct note_event {
    u8 note = 0;
    u8 delay = 0;
    u8 duration = 0;
    u8 velocity = 0;
};

// Returns false for command records (code >= 0x80), leaving out zeroed.
bool decode_note_event(const sequence_event &raw, note_event &out, std::string &err);

// Experimental E7 decoder based on ValleyBell/MidiConverters. p0 is kept as
// delay, p1 is the tempo scale, and p2 is the gradual-change parameter (semantics
// beyond the reference converter's warning are not implemented).
struct tempo_event {
    u8 delay = 0;
    u8 scale = 0;
    u8 gradual = 0;
    u32 microseconds_per_quarter = 0;
};
bool decode_tempo_event(const sequence_event &raw, u16 bpm,
                        tempo_event &out, std::string &err);

// Conversion matching ValleyBell/MidiConverters' Tempo2Mid integer formula:
// 60,000,000 * 64 / (BPM * scale). Returns microseconds per quarter note; rejects zero BPM/scale.
bool rcp2_tempo_us_per_quarter(u16 bpm, u8 scale, u32 &out, std::string &err);

// Experimental legacy timeline accepting notes and F5 only. F5 is emitted at
// the current tick and must not advance it; other commands (including E7) are rejected.
enum class timeline_event_kind { note, note_off, key_signature, tempo, control_change, program_change, pitch_bend, channel_aftertouch, poly_aftertouch, sysex, route_change };
struct midi_channel_event {
    u8 delay = 0;
    u8 data1 = 0;
    u8 data2 = 0;
};
struct midi_route_event { u8 channel = 0; u8 port = 0; bool enabled = true; };

struct timed_event {
    u32 tick = 0;
    timeline_event_kind kind = timeline_event_kind::note;
    note_event note;
    key_signature_event key;
    tempo_event tempo;
    midi_channel_event channel;
    std::vector<u8> sysex;
    midi_route_event route;
    // Notes and their generated Note Offs retain the route active at Note On.
    bool route_fixed = false;
};

// Experimental decoder for EA Channel Aftertouch, EB Control Change, EC standard
// Program Change, ED Polyphonic Aftertouch, and EE Pitch Bend. EC values >= 0x80
// use a special instrument encoding and are rejected rather than misrepresented.
bool decode_midi_channel_event(const sequence_event &raw,
                              midi_channel_event &out,
                              timeline_event_kind &kind,
                              std::string &err);

// Encode a timed note/channel event as a MIDI 1.0 channel message.
// channel is explicitly supplied (0..15); RCP track-channel mapping is kept separate
// until its file-format semantics are verified. Metadata events are not channel messages.
bool encode_midi_message(const timed_event &event, u8 channel,
                         std::vector<u8> &out, std::string &err);

bool expand_note_key_signature_timeline(const std::vector<sequence_event> &sequence,
                                        std::vector<timed_event> &out,
                                        std::string &err);

// Experimental timeline for notes, F5 key signatures, E7 tempo changes, and EB/EC/EE MIDI channel events.
// E7 is emitted at the current tick, then p0 advances the cursor; gradual
// tempo curves (p2) are retained but not expanded.
bool expand_note_key_signature_tempo_timeline(
    const std::vector<sequence_event> &sequence, u16 bpm,
    std::vector<timed_event> &out, std::string &err,
    const std::array<user_sysex_template, 8> *user_sysex = nullptr,
    u8 initial_midi_channel = 0);


// Experimental note-only timeline. start_tick is the current tick before the
// note's delay is applied; after each note, p0 advances the cursor. FE ends the
// sequence and is not emitted. Other commands are rejected because their timing
// semantics are command-specific and not yet implemented. duration=0 is kept.
// This is not a full RCP2 event expander.
// Merged view preserves track identity while ordering by absolute tick.
// Equal-tick events keep lower track index first and source order within each track.
struct timed_track_event {
    size_t track_index = 0;
    timed_event event;
};
bool merge_timed_tracks(const std::vector<std::vector<timed_event>> &tracks,
                        std::vector<timed_track_event> &out, std::string &err);

struct timed_note_event {
    u32 start_tick = 0;
    note_event note;
};

bool expand_note_only_timeline(const std::vector<sequence_event> &sequence,
                               std::vector<timed_note_event> &out,
                               std::string &err);

// Intermediate representation retains the source record and its track-local order.
struct intermediate_event {
    size_t record_index = 0;
    sequence_event raw;
};

struct intermediate_track {
    track_header header;
    std::vector<intermediate_event> events;
};

struct intermediate_document {
    document_header header;
    std::vector<intermediate_track> tracks;
};

// ファイル全体を解析し、ヘッダー・トラック情報・生レコードを中間表現にまとめる。
// 失敗時は out を空にする。未知コマンドも値を変更せず保持する。
// Apply track and document transposition to a note, following the reference converter.
// Track bit 7 marks the rhythm-channel case and suppresses both transpose values.
// Otherwise track transpose is signed 7-bit, document transpose is signed 8-bit,
// and the resulting MIDI note wraps to 7 bits. Returns false for non-note values.
bool apply_note_transposition(u8 note, u8 raw_track_transposition,
                              u8 raw_document_transposition, u8 &out,
                              std::string &err);

// Apply the RCP2 signed 8-bit track-start offset to one absolute tick.
// Negative results clamp to zero; values above the 32-bit tick range fail.
bool apply_signed_track_offset(u32 tick, u8 raw_offset, u32 &out, std::string &err);

// Experimental document-level timeline: expands supported note/F5/E7/EB/EC/EE
// events, applies note transposition, expands note duration to note-off events with
// reference-compatible same-pitch running-note behavior, applies signed track
// start offsets, then merges by absolute tick. Negative ticks clamp to zero;
// gradual tempo curves and special EC instrument handling remain unsupported.
bool expand_intermediate_timeline(const intermediate_document &document,
                                  std::vector<timed_track_event> &out,
                                  std::string &err);

// Absolute-tick events converted to seconds using the document PPQ and E7 tempo changes.
// Channel/port mapping and gradual E7 curves remain separate concerns.
struct timed_seconds_event {
    double time = 0.0;
    timed_track_event source;
};
bool convert_timeline_to_seconds(const document_header &header,
                                 const std::vector<timed_track_event> &timeline,
                                 std::vector<timed_seconds_event> &out,
                                 std::string &err);

// Encoded channel events for the seconds-based playback path. Mapping is explicit
// until RCP track channel/port semantics are verified.
struct midi_output_event { double time = 0.0; std::vector<u8> bytes; u8 port = 0; };

// Track-header MIDI routing decoded from the RCP2 channel byte.
// Low nibble is MIDI channel, high nibble is MIDI port/device.
// Bit 7 marks a dummy/unrouted track in the reference converter.
struct track_midi_mapping { u8 channel = 0; u8 port = 0; bool enabled = true; };
bool map_track_midi_outputs(const intermediate_document &document,
                            std::vector<track_midi_mapping> &out, std::string &err);
bool encode_timeline_to_midi_events(
    const std::vector<timed_seconds_event> &timeline,
    const std::vector<u8> &channel_by_track, const std::vector<u8> &port_by_track,
    std::vector<midi_output_event> &out, std::string &err);
bool encode_timeline_to_midi_events(
    const std::vector<timed_seconds_event> &timeline,
    const std::vector<track_midi_mapping> &mapping,
    std::vector<midi_output_event> &out, std::string &err);

bool build_intermediate(const u8 *data, size_t size,
                        intermediate_document &out, std::string &err);

} // namespace rcp

#endif // S_MU2000_RCP_H
