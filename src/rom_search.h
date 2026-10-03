// license:BSD-3-Clause
//
// ROM 置き場を探す順番。プラグイン（src/vst3/engine.cpp）と gui が同じ順番で探す（issue #89）。
//
// 何を「ROM 置き場」と呼ぶか（揃っていなければならないファイル）は roms_dir.h。ここは探す場所の
// 順番と、利用者が選んだ場所を書き残す roms.txt の読み書きだけ。
//
//   1. 環境変数 S_MU2000_ROMS
//   2. 利用者の設定の場所（config_dir）: roms.txt に書かれた場所、roms、その場所そのもの。
//      ~/Documents/S-MU2000/roms
//   3. 実行ファイル（プラグインなら DLL）の近く: ../Resources、../Resources/roms、roms、そこそのもの、
//      そこの roms.txt
//   4. gui だけ: 今の作業フォルダの roms
//   5. 機械全体の場所（shared_config_dir）: roms.txt、roms、そこそのもの

#ifndef S_MU2000_ROM_SEARCH_H
#define S_MU2000_ROM_SEARCH_H

#pragma once

#include "compat/paths.h"
#include "roms_dir.h"

#include <cstdio>
#include <string>
#include <vector>

namespace smu2000 {

// roms.txt に書かれた場所を読む（1 行目だけ）。無ければ空
inline std::string read_roms_pointer(const std::string &path)
{
	std::FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return {};
	char line[1024] = {};
	if (!std::fgets(line, sizeof(line), f)) {
		std::fclose(f);
		return {};
	}
	std::fclose(f);
	std::string s(line);
	// メモ帳などが付ける BOM を落とす。これがあると場所を見失う
	if (s.size() >= 3 && (unsigned char)s[0] == 0xef && (unsigned char)s[1] == 0xbb &&
	    (unsigned char)s[2] == 0xbf)
		s.erase(0, 3);
	while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
		s.pop_back();
	return s;
}

// 利用者が選んだ ROM 置き場を、設定の場所の roms.txt に書く（次からは gui もプラグインもそこを見る）
inline bool write_roms_pointer(const std::string &dir, std::string &err)
{
	const std::string conf = ensure_config_dir();
	if (conf.empty()) {
		err = "no settings directory";
		return false;
	}
	const std::string path = join(conf, "roms.txt");
	std::FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) {
		err = "cannot write " + path;
		return false;
	}
	std::fprintf(f, "%s\n", dir.c_str());
	std::fclose(f);
	return true;
}

// ROM 置き場を探す。見つかった場所を返す。無ければ空で、探した場所が tried に 1 行ずつ入る。
// image_dir は実行ファイル（プラグインなら DLL）の場所。with_cwd なら今の作業フォルダの roms も見る（gui）
inline std::string find_roms(const std::string &image_dir, bool with_cwd, std::string &tried)
{
	std::vector<std::string> cand;

	// 1. 環境変数。一番強い
	const std::string ev = env("S_MU2000_ROMS");
	if (!ev.empty())
		cand.push_back(ev);

	// 2. The user's own files, before the bundle's. A copy the user put there
	//    is the one they chose, while a bundle copy only exists because a
	//    build baked it in -- so theirs wins when both are present.
	const std::string local = config_dir();
	if (!local.empty()) {
		// A note naming the directory. Someone using this from a DAW has nowhere
		// to set an environment variable, so one line here (roms.txt) does it;
		// the GUI writes it when the user picks the folder
		const std::string note = read_roms_pointer(join(local, "roms.txt"));
		if (!note.empty())
			cand.push_back(note);
		cand.push_back(join(local, "roms"));
		cand.push_back(local);
	}
	const std::string home = home_dir();
	if (!home.empty())
		cand.push_back(join(home, "Documents/S-MU2000/roms"));

	// 3. Next to the binary, which is where a checkout works from rather than
	//    an install
	if (!image_dir.empty()) {
		// 3a. バンドルの Resources。<名前>.vst3/Contents/x86_64-win/ に DLL がいるので 1 つ上
		//     (macOS puts the binary in Contents/MacOS, also one level up)
		cand.push_back(join(image_dir, "../Resources"));
		cand.push_back(join(image_dir, "../Resources/roms"));
		// 3b. すぐ横
		cand.push_back(join(image_dir, "roms"));
		cand.push_back(image_dir);
		// 3c. 場所を書いた紙
		const std::string notes[2] = { join(image_dir, "../Resources/roms.txt"), join(image_dir, "roms.txt") };
		for (const std::string &p : notes) {
			const std::string s = read_roms_pointer(p);
			if (!s.empty())
				cand.push_back(s);
		}
	}

	// 4. 今の作業フォルダの roms（gui を roms の横から起動したとき）
	if (with_cwd)
		cand.push_back("roms");

	// 5. The machine-wide places. Put the ROMs here once and every user of the
	//    machine, and every instance of either plug-in, finds them. This program
	//    never writes here: creating it takes the rights to
	const std::string shared = shared_config_dir();
	if (!shared.empty()) {
		const std::string note = read_roms_pointer(join(shared, "roms.txt"));
		if (!note.empty())
			cand.push_back(note);
		cand.push_back(join(shared, "roms"));
		cand.push_back(shared);
	}

	for (const std::string &c : cand) {
		const fs::path p = fs::path(full_path(c));
		if (has_roms(p))
			return p.string();
		tried += "  " + p.string() + "\n";
	}
	return {};
}

// 利用者が選んだ場所が ROM 置き場か。そのものか、その下の roms なら、その場所を返す（違えば空）
inline std::string accept_roms_choice(const std::string &picked)
{
	if (picked.empty())
		return {};
	const fs::path p = fs::path(full_path(picked));
	if (has_roms(p))
		return p.string();
	if (has_roms(p / "roms"))
		return (p / "roms").string();
	return {};
}

} // namespace smu2000

#endif // S_MU2000_ROM_SEARCH_H
