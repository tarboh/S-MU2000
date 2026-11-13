// license:BSD-3-Clause
// Independent structural reader for Recomposer RCP3.0 (G36).
#ifndef S_MU2000_RCP3_H
#define S_MU2000_RCP3_H
#pragma once
#include "compat/mamecompat.h"
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace rcp3 {
constexpr size_t fixed_header_size = 0x0c98;
constexpr size_t track_header_size = 0x2e;
constexpr size_t event_size = 6;

struct user_sysex {
    std::string name;
    std::vector<u8> data;
};
struct header_info {
    std::string title;
    std::string comment;
    u16 track_count = 0;
    u16 ticks_per_quarter = 0;
    u16 tempo_bpm = 0;
    u8 beat_numerator = 0;
    u8 beat_denominator = 0;
    u8 key_signature = 0;
    u8 play_bias = 0;
    std::array<user_sysex, 8> user_sysexes;
};
struct track_header {
    u32 encoded_length = 0; // includes this 4-byte field and 0x2e-byte header
    u8 track_id = 0;
    u8 rhythm_mode = 0;
    u8 midi_channel = 0;
    u8 key = 0;
    u8 tick_offset = 0;
    u8 mute = 0;
    std::string name;
};
struct sequence_event {
    u8 code = 0;
    u8 p2 = 0;
    u16 p0 = 0;
    u16 p1 = 0;
};
struct track {
    track_header header;
    std::vector<sequence_event> sequence; // includes FE Track End
};

bool inspect_header(const u8 *data, size_t size, std::string &err);
bool parse_header(const u8 *data, size_t size, header_info &out, std::string &err);
bool parse_track_header(const u8 *data, size_t size, track_header &out, std::string &err);
bool parse_sequence(const u8 *data, size_t size, std::vector<sequence_event> &out,
                    size_t &bytes_consumed, std::string &err);
bool parse_tracks(const u8 *data, size_t size, const header_info &header,
                  std::vector<track> &out, std::string &err);
}
#endif
