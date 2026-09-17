// license:BSD-3-Clause
//
// 起動後の状態を取っておいて、次からはそれを読み込む。
//
// firmware の起動は音の時間で 5〜8 秒かかる。1 台なら待てるが、DAW に
// 何枚も挿すとそのたびに黙るので、実用上いちばん痛い所になる。
//
// 中身は mu2000::save_state() がそのまま。**戻した状態は起動し切った姿と
// 1 ビットも違わない**（make test の statetest が毎回確かめている）ので、
// 音は起動を回したときとまったく同じになる。
//
// 置き場は <設定>/S-MU2000/boot/<鍵>.bin。鍵は
//   ・プログラム ROM（firmware）
//   ・起動に使ったワーク RAM（NVRAM。設定が違えば起動後の姿も違う）
//   ・波形 ROM（大きいので飛び飛びに拾う）
//   ・状態の形の版
// から作る。どれかが変われば鍵が変わるので、古い写しを読むことはない。
//
// 使い方は nvram と同じ並びで、reset() の直前に load()、起動し切った所で save()。
// カードを差したまま起動するときは使わない（カードの中身は状態に入らない）。

#ifndef S_MU2000_BOOTCACHE_H
#define S_MU2000_BOOTCACHE_H

#pragma once

#include "mu2000.h"
#include "nvram.h"

#include "compat/paths.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace smu2000 {
namespace bootcache {

// 鍵。プログラム ROM・ワーク RAM・波形 ROM・状態の版から作る。
// **reset() の前に、起動に使う RAM が入った状態で呼ぶこと**
inline u64 key(const mu2000 &mu)
{
	auto mix = [](u64 h, u8 b) {
		h ^= b;
		return h * 0x100000001b3ull;
	};
	u64 h = nvram::rom_key(mu);          // プログラム ROM 4MB
	const std::vector<u8> ram = mu.nvram();
	for (u8 b : ram)
		h = mix(h, b);
	// 波形 ROM は 32MB あって毎回なぞるには重い。頭・真ん中・終わりの
	// 4KB ずつと大きさだけ見る。差し替えに気づければ足りる
	if (const auto wave = mu.wave_rom()) {
		const size_t n = wave->size();
		for (int k = 0; k < 8; k++)
			h = mix(h, u8(n >> (k * 8)));
		const size_t spots[3] = { 0, n / 2, n > 4096 ? n - 4096 : 0 };
		for (size_t at : spots)
			for (size_t i = 0; i < 4096 && at + i < n; i++)
				h = mix(h, (*wave)[at + i]);
	}
	for (int k = 0; k < 4; k++)
		h = mix(h, u8(mu2000::state_version() >> (k * 8)));
	// HOST SELECT（USB か MIDI か）でも起動後の姿が変わる
	h = mix(h, u8(mu.usb_host() ? 1 : 0));
	return h;
}

// 置き場。作れなければ空
inline std::string path(u64 k)
{
	const std::string base = smu2000::ensure_config_dir();
	if (base.empty())
		return {};
	const std::string dir = smu2000::join(base, "boot");
	if (!smu2000::ensure_dir(dir))
		return {};
	char name[32];
	std::snprintf(name, sizeof(name), "%016llx.bin", (unsigned long long)k);
	return smu2000::join(dir, name);
}

// A copy baked into the bundle, read only. "" if there is none.
//
// **A sandboxed plug-in has nothing at path().** An AUv3 extension has $HOME
// redirected into a container, so the boot/ inside it is empty the first time
// and every first insert would sit through a boot. Baking one in at build time
// (make auv3 AUV3_ROMS=roms) means it does not. Returns "" when not running
// from inside a bundle
inline std::string baked_path(u64 k)
{
	const std::string dir =
	    smu2000::module_dir(reinterpret_cast<const void *>(&baked_path));
	if (dir.empty())
		return {};
	const std::string res = smu2000::full_path(smu2000::join(dir, "../Resources/boot"));
	if (!smu2000::is_dir(res))
		return {};
	char name[32];
	std::snprintf(name, sizeof(name), "%016llx.bin", (unsigned long long)k);
	return smu2000::join(res, name);
}

// Try one file. True if it read and loaded as a state
inline bool load_from(mu2000 &mu, const std::string &p)
{
	if (p.empty())
		return false;
	std::FILE *f = std::fopen(p.c_str(), "rb");
	if (!f)
		return false;
	std::fseek(f, 0, SEEK_END);
	const long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::vector<u8> buf(size > 0 ? size_t(size) : 0);
	const bool read_ok = !buf.empty() && std::fread(buf.data(), 1, buf.size(), f) == buf.size();
	std::fclose(f);
	if (!read_ok)
		return false;
	std::string err;
	if (!mu.load_state(buf.data(), buf.size(), err)) {
		std::fprintf(stderr, "起動の写しを読めない: %s\n", err.c_str());
		return false;
	}
	return true;
}

// Read the state of a booted machine. If it succeeds the machine is already in
// the shape it would have booted into. **Call it instead of reset()** (calling
// it after reset() is fine too). The baked copy is tried first, because inside a
// sandbox it is the only one there is. `used` receives whichever file was
// actually read, so a log line can name it
inline bool load(mu2000 &mu, u64 k, std::string *used = nullptr)
{
	for (const std::string &p : { baked_path(k), path(k) })
		if (load_from(mu, p)) {
			if (used)
				*used = p;
			return true;
		}
	return false;
}

// 起動し切った所で残す。**起動に成功したときだけ呼ぶこと**
inline bool save(const mu2000 &mu, u64 k)
{
	const std::string p = path(k);
	if (p.empty())
		return false;
	const std::vector<u8> st = mu.save_state();
	// 書いている途中で落ちても壊れた写しを残さないよう、別名で書いてから置き換える
	const std::string tmp = p + ".new";
	std::FILE *f = std::fopen(tmp.c_str(), "wb");
	if (!f)
		return false;
	const bool ok = std::fwrite(st.data(), 1, st.size(), f) == st.size();
	std::fclose(f);
	if (!ok) {
		std::remove(tmp.c_str());
		return false;
	}
	return smu2000::replace_file(tmp, p);
}

// 設定（ワーク RAM）を書き戻したあとに呼ぶ。その設定で起動した写しがまだ
// 無ければ、まっさらな機械を 1 台起こして作っておく。**次の起動が速いまま**になる
// （やらないと、設定をいじった次の 1 回だけ起動が遅くなる）。
//
// live は終わるときの機械。ROM はそのまま借り、ワーク RAM だけ写して起こす。
// 音は出さないので別スレッドにもしない。作れたら true
inline bool refresh(const mu2000 &live)
{
	const std::vector<u8> &ram = live.nvram();
	if (ram.empty() || !live.program_rom())
		return false;

	mu2000 fresh;
	fresh.set_program_rom(live.program_rom());
	fresh.set_wave_rom(live.wave_rom());
	fresh.set_sintab_rom(live.sintab_rom());
	fresh.set_usb_host(live.usb_host());
	if (!fresh.set_nvram(ram.data(), ram.size()))
		return false;

	const u64 k = key(fresh);
	// もう有るなら何もしない
	const std::string p = path(k);
	if (p.empty())
		return false;
	if (std::FILE *f = std::fopen(p.c_str(), "rb")) {
		std::fclose(f);
		return false;
	}

	fresh.reset();
	const u64 limit = 30 * 44100;
	u64 i = 0;
	for (; i < limit && !fresh.midi_ready(); i++) {
		s32 l = 0, r = 0;
		fresh.run_sample(l, r);
	}
	if (i >= limit)
		return false;
	return save(fresh, k);
}

// 写しは 1 つ 6MB ほどある。設定を変えるたびに鍵が変わるので、
// 放っておくと溜まる。新しいほうから keep 個だけ残す
inline void prune(int keep = 4)
{
	const std::string base = smu2000::config_dir();
	if (base.empty())
		return;
	const std::string dir = smu2000::join(base, "boot");
	if (!smu2000::is_dir(dir))
		return;
	std::vector<smu2000::dir_entry> files;
	for (const smu2000::dir_entry &e : smu2000::list_dir(dir)) {
		// 鍵の名前のものだけ。人が置いた物は触らない
		if (e.name.size() == 20 && e.name.compare(16, 4, ".bin") == 0 &&
		    e.name.find_first_not_of("0123456789abcdef") == 16)
			files.push_back(e);
	}
	if (int(files.size()) <= keep)
		return;
	std::sort(files.begin(), files.end(),
	          [](const smu2000::dir_entry &a, const smu2000::dir_entry &b) {
		          return a.mtime > b.mtime;   // 新しい順
	          });
	for (size_t i = size_t(keep); i < files.size(); i++)
		std::remove(smu2000::join(dir, files[i].name).c_str());
}

} // namespace bootcache
} // namespace smu2000

#endif // S_MU2000_BOOTCACHE_H
