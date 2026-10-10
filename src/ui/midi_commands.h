// license:BSD-3-Clause
#pragma once
#include "bridge.h"
#include "menu.h"

namespace ui {
inline void send_midi_command(int id, bridge &br)
{
	if (id == ID_RESET_GM) br.ask({0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7});
	else if (id == ID_RESET_GS) br.ask({0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0, 0x7f, 0, 0x41, 0xf7});
	else if (id == ID_RESET_XG) br.ask({0xf0, 0x43, 0x10, 0x4c, 0, 0, 0x7e, 0, 0xf7});
	else if (id == ID_MIDI_PANIC)
		for (int port = 0; port <= mu2000::MIDI_PORTS; port++) for (int ch = 0; ch < 16; ch++) {
			const u8 message[] = {u8(0xb0 | ch), 120, 0, u8(0xb0 | ch), 123, 0};
			br.send_port(port, message, sizeof(message));
		}
}
} // namespace ui
