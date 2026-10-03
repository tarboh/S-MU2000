// license:BSD-3-Clause
//
// gui を ROM の場所なしで起動したとき（ダブルクリックなど）に ROM を見つける（issue #89）。
//
// まずプラグインと同じ順番で探す（src/rom_search.h）。無ければ、吸い出し方の案内と
// 「ROM のフォルダを選ぶ」を出し、選ばれた場所が ROM 置き場なら roms.txt に書いて使う。
// 次からは gui もプラグインもそこを見るので、選ぶのは 1 度だけ。
//
// 案内とフォルダを選ぶ窓は OS ごと（ask_roms_folder: window_win.cpp / window_mac.mm / sdl_popup.cpp）。

#ifndef S_MU2000_UI_ROM_LOCATE_H
#define S_MU2000_UI_ROM_LOCATE_H

#pragma once

#include "rom_search.h"
#include "texts.h"

#include <cstdio>
#include <string>

namespace ui {

// message を見せてから、フォルダを選ぶ窓を出す。選ばれたら true と picked（UTF-8）。
// 案内を閉じた・選ぶのをやめたなら false
bool ask_roms_folder(const std::string &message, std::string &picked);

// 見つかった（選ばれた）ROM 置き場。やめたなら空
inline std::string locate_roms_for_gui(const std::string &exe_dir)
{
	std::string tried;
	const std::string found = smu2000::find_roms(exe_dir, true, tried);
	if (!found.empty())
		return found;
	std::fprintf(stderr, "ROM が見つからない。探した場所:\n%s", tried.c_str());

	std::string message = UI_TEXT(dlg_roms_needed,
	    "S-MU2000 needs the ROMs dumped from your own MU2000.\n"
	    "How to dump them: https://github.com/tarboh/S-MU2000#roms\n\n"
	    "Once you have them, choose the folder that holds mu2000_flash.bin and the dump folder. "
	    "It is remembered, so the GUI and the plug-ins find the ROMs from then on.");
	for (;;) {
		std::string picked;
		if (!ask_roms_folder(message, picked))
			return {};
		const std::string ok = smu2000::accept_roms_choice(picked);
		if (!ok.empty()) {
			std::string err;
			if (!smu2000::write_roms_pointer(ok, err))
				std::fprintf(stderr, "roms.txt: %s\n", err.c_str());
			return ok;
		}
		// 足りないものを並べて、もう一度
		std::string missing;
		for (const smu2000::fs::path &rel : smu2000::roms_missing(smu2000::fs::path(smu2000::full_path(picked))))
			missing += (missing.empty() ? "" : ", ") + rel.generic_string();
		char buf[2048];
		std::snprintf(buf, sizeof(buf),
		              UI_TEXT(dlg_roms_bad_fmt, "%s does not hold the whole set (missing: %s).\n"
		                                        "Choose the folder that holds mu2000_flash.bin and the dump folder."),
		              picked.c_str(), missing.c_str());
		message = buf;
	}
}

} // namespace ui

#endif // S_MU2000_UI_ROM_LOCATE_H
