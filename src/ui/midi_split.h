// license:BSD-3-Clause
//
// The MU2000's MIDI OUT arrives as a stream of bytes -- the same 31250bps
// serial the hardware sends -- but everything it has to be handed to wants one
// whole message at a time: AUv3's MIDIOutputEventBlock, AUv2's MIDIPacketList,
// CoreMIDI's MIDISend. This fills that gap.
//
// **It allocates nothing and takes no lock**, so it can run inside a render
// block. A message too long to hold (a SysEx over 8KB) is dropped on its own,
// without disturbing the stream around it.

#ifndef S_MU2000_UI_MIDI_SPLIT_H
#define S_MU2000_UI_MIDI_SPLIT_H

#pragma once

#include <cstddef>
#include <cstdint>

namespace ui {

class midi_split
{
public:
	// Where a completed message is handed to
	using emit_fn = void (*)(void *ctx, const uint8_t *bytes, size_t n);

	void reset() { m_n = 0; m_want = 0; m_status = 0; m_sysex = false; m_over = false; }

	// Feed it bytes. emit is called once for every whole message that forms
	void feed(const uint8_t *p, size_t n, emit_fn emit, void *ctx)
	{
		for (size_t i = 0; i < n; i++)
			one(p[i], emit, ctx);
	}

private:
	// How many data bytes follow that status byte
	static int data_bytes(uint8_t status)
	{
		switch (status & 0xf0) {
		case 0x80: case 0x90: case 0xa0: case 0xb0: case 0xe0: return 2;
		case 0xc0: case 0xd0: return 1;
		default: break;
		}
		switch (status) {
		case 0xf1: case 0xf3: return 1;   // MTC quarter frame, song select
		case 0xf2: return 2;              // song position
		default: return 0;                // F6 / F7, and F8 and above
		}
	}

	void flush(emit_fn emit, void *ctx)
	{
		if (m_n && !m_over)
			emit(ctx, m_buf, m_n);
		m_n = 0;
		m_over = false;
	}

	void put(uint8_t v)
	{
		if (m_n < SIZE)
			m_buf[m_n++] = v;
		else
			m_over = true;            // too long; this message is dropped
	}

	void one(uint8_t b, emit_fn emit, void *ctx)
	{
		// Realtime (F8-FF) may appear anywhere, so it goes out on its own and
		// **leaves a half-built message alone** -- arriving in the middle of a
		// SysEx is exactly what the spec allows
		if (b >= 0xf8) {
			const uint8_t rt = b;
			emit(ctx, &rt, 1);
			return;
		}

		if (b >= 0x80) {
			// A new status byte. Anything half-built ended right here
			if (m_sysex) {
				if (b == 0xf7)
					put(b);           // ended properly
				flush(emit, ctx);
				m_sysex = false;
				if (b == 0xf7)
					return;
			} else if (m_n) {
				flush(emit, ctx);     // cut short by the next one; emit it anyway
			}

			if (b == 0xf0) {
				m_sysex = true;
				m_status = 0;
				put(b);
				return;
			}
			m_status = (b < 0xf0) ? b : 0;   // F1-F7 carry no running status
			m_want = data_bytes(b);
			put(b);
			if (!m_want)
				flush(emit, ctx);            // F6 and friends, which carry no data
			return;
		}

		// A data byte
		if (m_sysex) {
			put(b);
			return;
		}
		if (!m_n) {
			// Running status: the status byte stays what it was
			if (!m_status)
				return;                      // nothing to attach it to; drop it
			put(m_status);
			m_want = data_bytes(m_status);
		}
		put(b);
		if (m_n >= size_t(m_want) + 1)
			flush(emit, ctx);
	}

	// Big enough for a bulk dump; the MU2000's own TX buffer is 4096 bytes
	static constexpr size_t SIZE = 8192;
	uint8_t m_buf[SIZE] = {};
	size_t  m_n = 0;
	int     m_want = 0;
	uint8_t m_status = 0;
	bool    m_sysex = false;
	bool    m_over = false;
};

} // namespace ui

#endif // S_MU2000_UI_MIDI_SPLIT_H
