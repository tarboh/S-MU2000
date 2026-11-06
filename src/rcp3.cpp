// license:BSD-3-Clause
// RCP3.0 structural parsing. Command semantics and loop expansion are deliberately separate.
#include "rcp3.h"
#include <cstring>

namespace rcp3 {
namespace {
constexpr char signature[] = "COME ON MUSIC RECOMPOSER RCP3.0";
constexpr size_t signature_size = sizeof(signature) - 1;
u16 le16(const u8 *p) { return static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8); }
u32 le32(const u8 *p) { return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
    (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24); }
std::string fixed_string(const u8 *p, size_t n) {
    while (n && (p[n - 1] == 0 || p[n - 1] == ' ')) --n;
    return std::string(reinterpret_cast<const char *>(p), n);
}
}
bool inspect_header(const u8 *data, size_t size, std::string &err) {
    err.clear();
    if (!data) { err = "RCP3.0 data is null"; return false; }
    if (size < 0x20) { err = "RCP3.0 signature is truncated"; return false; }
    if (std::memcmp(data, signature, signature_size) != 0 || data[0x1f] != 0) {
        err = "RCP3.0 signature does not match"; return false;
    }
    return true;
}
bool parse_header(const u8 *data, size_t size, header_info &out, std::string &err) {
    out = {};
    if (!inspect_header(data, size, err)) return false;
    if (size < fixed_header_size) { err = "RCP3.0 fixed header is truncated"; return false; }
    out.title = fixed_string(data + 0x020, 0x80);
    out.comment = fixed_string(data + 0x0a0, 0x168);
    out.track_count = le16(data + 0x208);
    out.ticks_per_quarter = le16(data + 0x20a);
    out.tempo_bpm = le16(data + 0x20c);
    out.beat_numerator = data[0x20e];
    out.beat_denominator = data[0x20f];
    out.key_signature = data[0x210];
    out.play_bias = data[0x211];
    // Eight 0x30-byte user SysEx definitions follow the 0x800-byte rhythm table.
    // Each slot contains an 0x18-byte name followed by 0x18 bytes of template data.
    constexpr size_t user_sysex_offset = 0x0b18;
    for (size_t i = 0; i < out.user_sysexes.size(); ++i) {
        const size_t slot = user_sysex_offset + i * 0x30;
        out.user_sysexes[i].name = fixed_string(data + slot, 0x18);
        const u8 *payload = data + slot + 0x18;
        size_t length = 0x18;
        while (length > 0 && payload[length - 1] == 0xf7) --length;
        if (length < 0x18) ++length; // retain one terminating F7
        out.user_sysexes[i].data.assign(payload, payload + length);
    }
    if (out.track_count == 0) out.track_count = 18; // legacy default used by the reference converter
    if (out.track_count > 36) { err = "RCP3.0 track count exceeds the documented 36-track maximum"; out = {}; return false; }
    return true;
}
bool parse_track_header(const u8 *data, size_t size, track_header &out, std::string &err) {
    out = {}; err.clear();
    if (!data) { err = "RCP3.0 track header is null"; return false; }
    if (size < track_header_size) { err = "RCP3.0 track header is truncated"; return false; }
    out.encoded_length = le32(data);
    out.track_id = data[4]; out.rhythm_mode = data[5]; out.midi_channel = data[6];
    out.key = data[7]; out.tick_offset = data[8]; out.mute = data[9];
    out.name = fixed_string(data + 10, 0x24);
    if (out.encoded_length < track_header_size || out.encoded_length > size) {
        err = "RCP3.0 track length is outside the available data"; out = {}; return false;
    }
    return true;
}
bool parse_sequence(const u8 *data, size_t size, std::vector<sequence_event> &out,
                    size_t &bytes_consumed, std::string &err) {
    out.clear(); bytes_consumed = 0; err.clear();
    if (!data) { err = "RCP3.0 sequence data is null"; return false; }
    for (size_t pos = 0; pos + event_size <= size; pos += event_size) {
        sequence_event e;
        e.code = data[pos]; e.p2 = data[pos + 1];
        e.p0 = le16(data + pos + 2); e.p1 = le16(data + pos + 4);
        out.push_back(e); bytes_consumed = pos + event_size;
        if (e.code == 0xfe) return true;
    }
    out.clear(); bytes_consumed = 0;
    err = (size % event_size) ? "RCP3.0 sequence ends inside a 6-byte record" :
                               "RCP3.0 sequence has no FE Track End";
    return false;
}
bool parse_tracks(const u8 *data, size_t size, const header_info &header,
                  std::vector<track> &out, std::string &err) {
    out.clear(); err.clear();
    if (!data || size < fixed_header_size) { err = "RCP3.0 track data is unavailable"; return false; }
    size_t pos = fixed_header_size;
    for (u16 i = 0; i < header.track_count; ++i) {
        track t;
        if (pos > size || size - pos < track_header_size) {
            err = "RCP3.0 track header is missing at index " + std::to_string(i); out.clear(); return false;
        }
        if (!parse_track_header(data + pos, size - pos, t.header, err)) {
            err = "RCP3.0 track " + std::to_string(i) + ": " + err; out.clear(); return false;
        }
        const size_t track_size = t.header.encoded_length;
        size_t consumed = 0;
        if (!parse_sequence(data + pos + track_header_size, track_size - track_header_size,
                            t.sequence, consumed, err)) {
            err = "RCP3.0 track " + std::to_string(i) + ": " + err; out.clear(); return false;
        }
        out.push_back(std::move(t));
        pos += track_size;
    }
    return true;
}
}
