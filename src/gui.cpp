// license:BSD-3-Clause
//
// 実機のフロントパネル風の画面で MU2000 を動かす。
//
//   gui <rom ディレクトリ> [--midi 番号] [--midi-b 番号] [--fast-midi]
//       [--midiout 番号] [--midiout-b 番号] [--midiout-mu 番号] [--latency ミリ秒]
//   gui --list                             MIDI の入口と出口の一覧
//   gui <rom ディレクトリ> --shot 絵.png    窓を出さずに絵だけ書き出す（見た目の確認用）
//
// 入口と出口は**動かしたまま画面から選べる**。パネルの MIDI IN A の
// ジャックを押すか、どこでも右クリックすると品書きが出る。選んだものは
// %LOCALAPPDATA%\S-MU2000\gui.ini に覚えておいて、次から使う。
//
// MU2000 の設定（ワーク RAM）は終わるときに残し、次の起動で使う（src/nvram.h）。
// --factory か右クリックの「工場出荷状態に戻す」で捨てられる。
//
// 音の作り方は live.exe と同じ。**時計を自分で持たない**（doc/design.md）。
// 画面は別スレッドで、音源とは ui::bridge 越しにしか触れ合わない。
//
// マウスホイールはダイヤルに割り当ててある。実機にもロータリー
// エンコーダがあり、VALUE -/+ のボタンと同じ働きをする。

#include "mu2000.h"
#include "bootcache.h"
#include "nvram.h"
#include "voicecache.h"
#include "smf.h"
#include "ui/audio_out.h"
#include "ui/audio_in.h"
#include "ui/bridge.h"
#include "ui/driver.h"
#include "ui/engine.h"
#include "ui/midi_in.h"
#include "ui/midi_guard.h"
#include "ui/midi_out.h"
#include "ui/layout.h"
#include "ui/panel.h"
#include "ui/fx_editor.h"
#include "ui/overview.h"
#include "ui/master_editor.h"
#include "ui/menu.h"
#include "ui/menu_win.h"
#include "ui/keymap.h"
#include "ui/keymap_win.h"
#include "ui/options.h"
#include "ui/settings.h"
#include "ui/part_shapes.h"
#include "ui/pc_editor.h"
#include "ui/pc_host.h"
#include "ui/pc_window.h"
#include "ui/player.h"
#include "ui/text.h"
#include "ui/png.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>

namespace {

constexpr u32 RATE = ui::AUDIO_RATE;

// ---- 音源側
//
// The engine itself now lives in ui/engine.h: the macOS front end needs the
// same boot sequence and, more to the point, the same MIDI routing, and one
// copy is the only way to keep the two from drifting. Only the name is pulled
// in here, so the rest of this file reads as it always did.

using ui::engine;

// Menu lines and builders are shared with gui_mac.cpp in ui/menu.h; the
// names below are unqualified for the choice dispatch (WM_COMMAND).
using namespace ui;


// ---- 窓

struct window_state {
	ui::panel   panel;
	bool lcd_only = false;
	ui::pc_window pc{ std::make_unique<ui::pc_editor>() };    // PC エディタ（F2 か右クリック）
	ui::pc_window list{ std::make_unique<ui::overview>() };   // 一覧（F3 か右クリック）
	ui::pc_window fx{ std::make_unique<ui::fx_editor>() };    // インサーションの設定（一覧でダブルクリック）
	ui::pc_window shapes{ std::make_unique<ui::part_shapes>() };   // パートの音色（一覧の絵をダブルクリック）
	ui::pc_window master{ std::make_unique<ui::master_editor>() }; // マスター（一覧のマスターの行をダブルクリック）
	ui::bridge *br = nullptr;
	engine     *eng = nullptr;
	ui::audio_out *out = nullptr;
	ui::audio_in  *ain = nullptr;      // A/D INPUT（録音デバイス）
	std::string ain_name;              // 選んだ録音デバイスの名前。空なら使わない
	std::string card_path;             // 差している SmartMedia のファイル。空なら差していない

	std::string audio_name;            // 音の出口（名前の一部）。空なら既定
	std::string layout_path;           // 読んでいる panel.txt。F5 で読み直す
	ui::player    play_file;
	// MIDI IN A-D。A・B は実機の DIN、C・D は USB だけの口（パート 33-64）
	ui::midi_in  *midi[mu2000::MIDI_PORTS] = {};
	ui::midi_out *mout = nullptr;      // MIDI THRU A
	ui::midi_out *mout_b = nullptr;    // MIDI THRU B
	ui::midi_out *mout_mu = nullptr;   // MIDI OUT（MU2000 が送り出すもの）
	std::thread   reboot;              // 工場出荷状態に戻す作業
	int  in_dev[mu2000::MIDI_PORTS] = { -1, -1, -1, -1 };  // いま開いている番号。-1 は使っていない
	int  out_dev = -1;
	int  out_dev_b = -1;
	int  out_dev_mu = -1;
	std::string in_name[mu2000::MIDI_PORTS];
	std::string out_name, out_name_b, out_name_mu;
	// 起動したときに開けなかった口の名前。**選び直すまで gui.ini に残す**。
	// 残さないと、loopMIDI を起動し忘れた・機器を挿していなかっただけで、
	// 選んでおいた口を忘れてしまう
	std::string in_keep[mu2000::MIDI_PORTS];
	std::string out_keep, out_keep_b, out_keep_mu;
	std::string last_error;            // 品書きから選んで開けなかったときの理由
	u64 reported_drops = 0;            // 画面の糸が最後に知らせた、捨てた MIDI の量
	bool keep_settings = false;        // gui.ini を書き換えない（--nomidi）


	// 二重書き用
	HDC     mem_dc = nullptr;
	HBITMAP mem_bmp = nullptr;
	int     mem_w = 0, mem_h = 0;
};

window_state g_win;

// ---- パネルの配置。作り直さずに文字ファイルで直せる（doc/panel-editing.md）

void apply_layout(const std::string &path, bool quiet)
{
	g_win.panel.lay() = ui::layout();          // まず既定値に戻す
	std::string err;
	if (!path.empty() && g_win.panel.lay().load(path, err)) {
		if (!quiet)
			std::printf("配置: %s\n", path.c_str());
	} else if (!path.empty() && !quiet) {
		std::printf("配置: %s を開けない。組み込みの配置を使う\n", path.c_str());
	}
	if (!err.empty())
		std::fprintf(stderr, "%s", err.c_str());
	std::fflush(stdout);
	g_win.panel.resize(g_win.panel.width(), g_win.panel.height());
}

// ---- 選んだ口を覚えておく
//
// 番号ではなく**名前**で覚える。USB の機器を挿し直すと番号がずれるので、
// 番号で覚えると次に開いたとき別の機器に繋がってしまう。

std::string settings_path()
{
	const char *base = std::getenv("LOCALAPPDATA");
	if (!base || !*base)
		return {};
	std::string dir = std::string(base) + "\\S-MU2000";
	CreateDirectoryA(dir.c_str(), nullptr);
	return dir + "\\gui.ini";
}

// gui.ini keys live in ui/settings.h as ui::SET_* (shared with gui_mac.cpp).

void load_settings(std::string *in_name,
                   std::string &out_name, std::string &out_name_b,
                   std::string &audio_name, float *volume = nullptr,
                   std::string *out_name_mu = nullptr, bool *fold_ports34 = nullptr,
                   std::string *ain_name = nullptr, std::string *card_path = nullptr,
                   bool *analog = nullptr)
{
	const std::string path = settings_path();
	if (path.empty())
		return;
	settings_map kv;
	if (!read_settings_file(path, kv))
		return;
	for (int p = 0; p < mu2000::MIDI_PORTS; p++)
		if (const std::string *v = find_setting(kv, SET_IN_KEYS[p]))
			in_name[p] = *v;
	if (const std::string *v = find_setting(kv, SET_OUT))   out_name   = *v;
	if (const std::string *v = find_setting(kv, SET_OUT_B)) out_name_b = *v;
	if (const std::string *v = find_setting(kv, SET_OUT_MU)) {
		if (out_name_mu) *out_name_mu = *v;
	}
	if (const std::string *v = find_setting(kv, SET_AUDIO_OUT))  audio_name = *v;
	if (const std::string *v = find_setting(kv, SET_AUDIO_IN)) {
		if (ain_name) *ain_name = *v;
	}
	if (const std::string *v = find_setting(kv, SET_CARD)) {
		if (card_path) *card_path = *v;
	}
	if (const std::string *v = find_setting(kv, SET_PORTS34)) {
		if (fold_ports34) *fold_ports34 = *v != "drop";
	}
	if (const std::string *v = find_setting(kv, SET_OUTPUT)) {
		if (analog) *analog = *v == "analog";
	}
	if (const std::string *v = find_setting(kv, SET_VOLUME)) {
		if (volume && !v->empty())
			*volume = std::clamp(float(std::atof(v->c_str())), 0.0f, 1.0f);
	}
}

void save_settings()
{
	if (g_win.keep_settings)                 // --nomidi。覚えている口を消さない
		return;
	const std::string path = settings_path();
	if (path.empty())
		return;
	auto pick = [](const std::string &now, const std::string &keep) {
		return (now.empty() ? keep : now).c_str();
	};
	settings_map kv;
	for (int p = 0; p < mu2000::MIDI_PORTS; p++)
		kv.emplace_back(SET_IN_KEYS[p], pick(g_win.in_name[p], g_win.in_keep[p]));
	kv.emplace_back(SET_OUT,    pick(g_win.out_name,    g_win.out_keep));
	kv.emplace_back(SET_OUT_B,  pick(g_win.out_name_b,  g_win.out_keep_b));
	kv.emplace_back(SET_OUT_MU, pick(g_win.out_name_mu, g_win.out_keep_mu));
	kv.emplace_back(SET_AUDIO_OUT, g_win.audio_name);
	kv.emplace_back(SET_AUDIO_IN, g_win.ain_name);
	kv.emplace_back(SET_CARD, g_win.card_path);
	// パネルの VOLUME のつまみ。実機でも DAC の後ろのアナログのつまみで、
	// firmware の RAM には入らないので、こちらで覚える
	if (g_win.br) {
		char vol[32];
		std::snprintf(vol, sizeof(vol), "%.3f", g_win.br->gain());
		kv.emplace_back(SET_VOLUME, vol);
	}
	kv.emplace_back(SET_PORTS34, g_win.play_file.fold_extra_ports() ? "fold" : "drop");
	// 音の出口。digital（S/PDIF と同じ）か analog（直流を切る。src/analog_out.h）
	if (g_win.eng)
		kv.emplace_back(SET_OUTPUT, g_win.eng->analog.load() ? "analog" : "digital");
	write_settings_file(path, kv);
}

// ---- 口を選ぶ品書き

// Menu command numbers, labels and builders are shared with gui_mac.cpp
// in ui/menu.h (Windows is the reference), so a menu added on one side
// cannot be missed on the other. Only rendering (below) and acting on the
// choice (WM_COMMAND) stay here.

// Popups render the shared ui/menu.h content through ui/menu_win.h.
// Only gathering this window's state (below) stays here.

// What the shared builders show, from this window's state
menu_state menu_snapshot()
{
	menu_state s;
	s.midi_ins = ui::midi_in::list();
	s.midi_outs = ui::midi_out::list();
	s.audio_ins = ui::audio_in::list();
	for (int p = 0; p < mu2000::MIDI_PORTS; p++)
		s.in_dev[p] = g_win.in_dev[p];
	s.out_dev = g_win.out_dev;
	s.out_dev_b = g_win.out_dev_b;
	s.out_dev_mu = g_win.out_dev_mu;
	s.ain_name = g_win.ain_name;
	s.card_path = g_win.card_path;
	s.playing = g_win.play_file.playing();
	s.play_name = g_win.play_file.name();
	s.fold34 = g_win.play_file.fold_extra_ports();
	s.ready = g_win.eng && g_win.eng->state.load() == 1;
	s.native_fx = g_win.eng && g_win.eng->native_fx.load();
	s.native_engine = g_win.eng && g_win.eng->native_engine.load();
	return s;
}

void show_ain_menu(HWND hwnd, POINT screen)
{
	track_menu(hwnd, screen,
	           render_menu(ui::menu_ain_only(ui::audio_in::list(), g_win.ain_name)));
}

// 録音デバイスを選ぶ。-1 は使わない
void choose_ain(int dev)
{
	if (!g_win.ain)
		return;
	g_win.ain->stop();
	g_win.last_error.clear();
	if (dev < 0) {
		g_win.ain_name.clear();
	} else {
		const auto names = ui::audio_in::list();
		if (dev < int(names.size())) {
			std::string err;
			if (!g_win.ain->start(names[size_t(dev)], err)) {
				g_win.last_error = err;
				std::fprintf(stderr, "A/D INPUT: %s\n", err.c_str());
			} else {
				std::printf("A/D INPUT: %s（%s）\n", g_win.ain->device_name().c_str(), g_win.ain->format_line().c_str());
				std::fflush(stdout);
			}
			g_win.ain_name = names[size_t(dev)];
		}
	}
	save_settings();
}

void show_port_menu(HWND hwnd, POINT screen)
{
	track_menu(hwnd, screen, render_menu(ui::menu_ports(menu_snapshot())));
}

// ---- PHONES のジャック。音の出口を選ぶ
//
// デジタルは S/PDIF の出口と同じで、一部の DPCM のサンプルが持つ直流もそのまま出る（実機で確かめた）。
// アナログは LINE OUT・PHONES のつもりで直流を切る（src/analog_out.h。切れる周波数は仮）
void show_output_menu(HWND hwnd, POINT screen)
{
	const bool analog = g_win.eng && g_win.eng->analog.load();
	track_menu(hwnd, screen, render_menu(ui::menu_phones(analog)));
}

// ---- カードの差し込み口。SmartMedia を差す・MIDI ファイルを流す

// 書き換えたブロックをファイルへ書き戻す。写すときだけ音声の糸を止める
void flush_card()
{
	if (!g_win.eng || g_win.card_path.empty())
		return;
	std::vector<smu2000::smartmedia::block> blocks;
	{
		const std::lock_guard<std::mutex> hold(g_win.eng->card_lock);
		g_win.eng->mu.card().take_dirty_blocks(blocks);
	}
	std::string err;
	if (!smu2000::smartmedia::write_blocks(g_win.card_path, blocks, err)) {
		std::fprintf(stderr, "SmartMedia: %s\n", err.c_str());
		std::fflush(stderr);
	}
}

void eject_card()
{
	if (!g_win.eng)
		return;
	flush_card();
	{
		const std::lock_guard<std::mutex> hold(g_win.eng->card_lock);
		g_win.eng->mu.card().eject();
	}
	if (!g_win.card_path.empty())
		std::printf("SmartMedia を抜いた: %s\n", g_win.card_path.c_str());
	std::fflush(stdout);
	g_win.card_path.clear();
}

// ファイルの SmartMedia を差す。読むのは糸を止めずに済ませ、差し替えるときだけ止める
bool insert_card(const std::string &path, bool quiet = false)
{
	if (!g_win.eng)
		return false;
	smu2000::smartmedia card;
	std::string err;
	if (!card.load(path, err)) {
		if (!quiet)
			g_win.last_error = err;
		std::fprintf(stderr, "SmartMedia: %s\n", err.c_str());
		return false;
	}
	eject_card();
	{
		const std::lock_guard<std::mutex> hold(g_win.eng->card_lock);
		g_win.eng->mu.card() = std::move(card);
	}
	g_win.card_path = path;
	std::printf("SmartMedia を差した: %s（%uMB）\n", path.c_str(), g_win.eng->mu.card().megabytes());
	std::fflush(stdout);
	return true;
}

std::string ask_card_path(HWND hwnd, bool create)
{
	wchar_t file[MAX_PATH] = {};
	if (create)
		wcscpy(file, L"smartmedia.img");
	OPENFILENAMEW o{};
	o.lStructSize = sizeof(o);
	o.hwndOwner = hwnd;
	o.lpstrFilter = L"SmartMedia の中身 (*.img)\0*.img\0すべて (*.*)\0*.*\0";
	o.lpstrFile = file;
	o.nMaxFile = MAX_PATH;
	o.lpstrDefExt = L"img";
	if (create) {
		o.lpstrTitle = L"新しい SmartMedia の保存先";
		o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
		if (!GetSaveFileNameW(&o))
			return {};
	} else {
		o.lpstrTitle = L"差す SmartMedia";
		o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
		if (!GetOpenFileNameW(&o))
			return {};
	}
	return ui::to_utf8(file);
}

// 空の SmartMedia（物理の書式だけ）を作って差す。使う前に UTIL → CARD → Format で書式化する
void new_card(HWND hwnd, u32 megabytes)
{
	const std::string path = ask_card_path(hwnd, true);
	if (path.empty())
		return;
	smu2000::smartmedia card;
	card.create(megabytes);
	std::string err;
	if (!card.save(path, err)) {
		g_win.last_error = err;
		return;
	}
	if (insert_card(path)) {
		save_settings();
		MessageBoxW(hwnd, L"空の SmartMedia を差しました。\n"
		                  L"使う前に、本体の UTIL → CARD → Format で書式化してください。",
		            L"S-MU2000", MB_OK | MB_ICONINFORMATION);
	}
}

void show_card_menu(HWND hwnd, POINT screen)
{
	track_menu(hwnd, screen, render_menu(ui::menu_card(menu_snapshot())));
}

// MIDI ファイルを流す（品書きから選んだとき・窓に落とされたとき）。鳴っていれば止めて流し直す
void play_midi_file(HWND hwnd, const std::string &path)
{
	if (!g_win.br)
		return;
	std::string err;
	if (!g_win.play_file.start(path, *g_win.br, err)) {
		const std::wstring w = ui::to_wide("開けない: " + err);
		MessageBoxW(hwnd, w.c_str(), L"S-MU2000", MB_OK | MB_ICONWARNING);
		return;
	}
	std::printf("再生: %s（%.1f 秒）\n", path.c_str(), g_win.play_file.length());
	if (g_win.play_file.ports_used() > 2)
		std::printf("  この曲は %d 口ぶん。C・D は未対応なので、口 3 以降は%s\n", g_win.play_file.ports_used(),
		            g_win.play_file.fold_extra_ports() ? " A・B に重ねて鳴らす" : "鳴らさない");
	std::fflush(stdout);
}

// 窓に落とされたファイル（エディタや一覧の窓から）。本体の窓は WM_DROPFILES で受ける
void play_dropped_file(const std::wstring &path)
{
	play_midi_file(GetForegroundWindow(), ui::to_utf8(path.c_str()));
}

void choose_midi_file(HWND hwnd)
{
	wchar_t file[MAX_PATH] = {};
	OPENFILENAMEW o{};
	o.lStructSize = sizeof(o);
	o.hwndOwner = hwnd;
	o.lpstrFilter = L"MIDI ファイル (*.mid;*.midi)\0*.mid;*.midi\0すべて (*.*)\0*.*\0";
	o.lpstrFile = file;
	o.nMaxFile = MAX_PATH;
	o.lpstrTitle = L"流す MIDI ファイル";
	o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (!GetOpenFileNameW(&o))
		return;

	play_midi_file(hwnd, ui::to_utf8(file));
}

// 覚えている設定を捨てて、電源を入れ直す
void choose_factory_reset(HWND hwnd)
{
	if (!g_win.eng || g_win.eng->state.load() != 1)
		return;
	if (MessageBoxW(hwnd,
	                L"MU2000 を工場出荷状態に戻して、電源を入れ直します。\n"
	                L"ユーティリティの設定や、覚えている音量・音色の設定はすべて消えます。",
	                L"S-MU2000", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
		return;
	g_win.play_file.stop();
	if (g_win.reboot.joinable())
		g_win.reboot.join();
	g_win.reboot = std::thread([] { g_win.eng->factory_reset(); });
}

// 品書きで選ばれたものを開く。開けなかったら「使わない」に戻す
// keep が true なのは起動したとき。開けなくても、覚えていた名前を残す
bool choose_in(int port, int dev, bool keep = false)
{
	if (port < 0 || port >= mu2000::MIDI_PORTS || !g_win.midi[port])
		return false;
	if (!keep)
		g_win.in_keep[port].clear();
	std::string err;
	g_win.last_error.clear();
	if (!g_win.midi[port]->open(dev, err)) {
		g_win.last_error = err;
		std::fprintf(stderr, "%s: %s\n", ui::IN_LABELS[port], err.c_str());
		g_win.midi[port]->open(-1, err);
		dev = -1;
	}
	g_win.in_dev[port]  = g_win.midi[port]->is_open() ? dev : -1;
	g_win.in_name[port] = g_win.midi[port]->device_name();
	save_settings();
	return g_win.last_error.empty();
}

// keep が true なのは起動したとき。開けなくても、覚えていた名前を残す
bool choose_out(int dev, bool keep = false)
{
	if (!g_win.mout)
		return false;
	if (!keep)
		g_win.out_keep.clear();
	std::string err;
	g_win.last_error.clear();
	if (!g_win.mout->open(dev, err)) {
		g_win.last_error = err;
		std::fprintf(stderr, "MIDI 出力: %s\n", err.c_str());
		g_win.mout->open(-1, err);
		dev = -1;
	}
	g_win.out_dev  = g_win.mout->is_open() ? dev : -1;
	g_win.out_name = g_win.mout->device_name();
	save_settings();
	return g_win.last_error.empty();
}

// keep が true なのは起動したとき。開けなくても、覚えていた名前を残す
bool choose_out_mu(int dev, bool keep = false)
{
	if (!g_win.mout_mu)
		return false;
	if (!keep)
		g_win.out_keep_mu.clear();
	std::string err;
	g_win.last_error.clear();
	if (!g_win.mout_mu->open(dev, err)) {
		g_win.last_error = err;
		std::fprintf(stderr, "MIDI 出力（本体の OUT）: %s\n", err.c_str());
		g_win.mout_mu->open(-1, err);
		dev = -1;
	}
	g_win.out_dev_mu  = g_win.mout_mu->is_open() ? dev : -1;
	g_win.out_name_mu = g_win.mout_mu->device_name();
	save_settings();
	return g_win.last_error.empty();
}

// keep が true なのは起動したとき。開けなくても、覚えていた名前を残す
bool choose_out_b(int dev, bool keep = false)
{
	if (!g_win.mout_b)
		return false;
	if (!keep)
		g_win.out_keep_b.clear();
	std::string err;
	g_win.last_error.clear();
	if (!g_win.mout_b->open(dev, err)) {
		g_win.last_error = err;
		std::fprintf(stderr, "MIDI 出力 B: %s\n", err.c_str());
		g_win.mout_b->open(-1, err);
		dev = -1;
	}
	g_win.out_dev_b  = g_win.mout_b->is_open() ? dev : -1;
	g_win.out_name_b = g_win.mout_b->device_name();
	save_settings();
	return g_win.last_error.empty();
}

void ensure_backing(HDC dc, int w, int h)
{
	if (g_win.mem_dc && g_win.mem_w == w && g_win.mem_h == h)
		return;
	if (g_win.mem_bmp) DeleteObject(g_win.mem_bmp);
	if (g_win.mem_dc)  DeleteDC(g_win.mem_dc);
	g_win.mem_dc = CreateCompatibleDC(dc);
	g_win.mem_bmp = CreateCompatibleBitmap(dc, w, h);
	SelectObject(g_win.mem_dc, g_win.mem_bmp);
	g_win.mem_w = w;
	g_win.mem_h = h;
}

// キーボードからも押せるように。並びは MAME の mu2000 と同じ
// The table itself is shared (ui/keymap.h); only VK codes become characters here
mu2000::button key_to_button(WPARAM vk, bool &ok)
{
	mu2000::button b = mu2000::button::count;
	ok = ui::button_for_char(ui::key_char_of_vk(int(vk)), b);
	return b;
}
void open_window(HWND hwnd, ui::pc_window &w)
{
	std::string err;
	if (!w.show(GetModuleHandleA(nullptr), err))
		MessageBoxW(hwnd, ui::to_wide(err).c_str(), L"S-MU2000", MB_OK | MB_ICONWARNING);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_CREATE:
		SetTimer(hwnd, 1, 33, nullptr);        // 30 コマ／秒で描き直す
		return 0;

	case WM_TIMER: {
		// パラメータの層: 音源の返事を読み、見えている面の読み返しを頼む
		if (g_win.br) {
			g_win.panel.tick(*g_win.br);
			// PC の窓（一覧の上の帯）に CPU の負荷を出すため
			if (g_win.out && g_win.out->produced())
				g_win.br->set_cpu(float(g_win.out->cpu_percent()));
			// いまどちらの口で鳴らしているか（F4 で切り替わる）を一覧の帯へ
			g_win.br->set_engine(g_win.eng ? g_win.eng->native_engine.load() : -1);
			ui::pc_frame_all(g_win.list, g_win.pc, g_win.fx, g_win.shapes, g_win.master,
			                 g_win.panel.xg(), g_win.panel.ram(), *g_win.br,
			                 [&](ui::pc_window &w) { open_window(hwnd, w); });
		}
		InvalidateRect(hwnd, nullptr, FALSE);
		// SmartMedia に書いたものを 2 秒ごとにファイルへ書き戻す（抜いたとき・閉じたときも）
		static DWORD last_card = 0;
		if (GetTickCount() - last_card > 2000) {
			last_card = GetTickCount();
			flush_card();
		}
		// MIDI の輪などで溢れて捨てたものがあれば、1 秒に 1 回だけ知らせる
		static DWORD last = 0;
		if (g_win.eng && GetTickCount() - last > 1000) {
			last = GetTickCount();
			const u64 drops = g_win.eng->guard_a.dropped() + g_win.eng->guard_b.dropped() +
			                  g_win.eng->mu.midi_dropped();
			if (drops != g_win.reported_drops) {
				std::fprintf(stderr,
				             "MIDI が多すぎるので捨てた: THRU A %llu / THRU B %llu / 受信 %llu バイト"
				             "（MIDI の輪ができていないか確かめる）\n",
				             (unsigned long long)g_win.eng->guard_a.dropped(),
				             (unsigned long long)g_win.eng->guard_b.dropped(),
				             (unsigned long long)g_win.eng->mu.midi_dropped());
				g_win.reported_drops = drops;
			}
		}
		return 0;
	}

	case WM_DROPFILES: {
		// 窓に落とされたファイルの 1 つ目を流す
		const HDROP drop = HDROP(wp);
		wchar_t path[MAX_PATH * 4] = {};
		const bool got = DragQueryFileW(drop, 0, path, UINT(sizeof(path) / sizeof(path[0]))) > 0;
		DragFinish(drop);
		if (got)
			play_midi_file(hwnd, ui::to_utf8(path));
		return 0;
	}

	case WM_SIZE:
		g_win.panel.resize(LOWORD(lp), HIWORD(lp));
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;

	case WM_ERASEBKGND:
		return 1;                               // 全部自分で描く

	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		RECT cr;
		GetClientRect(hwnd, &cr);
		const int w = cr.right, h = cr.bottom;
		ensure_backing(dc, w, h);

		ui::snapshot s;
		g_win.br->read(s);
		u64 pressed = g_win.br->buttons();
		char status[320] = {};
		if (g_win.out && g_win.out->produced())
			std::snprintf(status, sizeof(status),
			              "発音 %d/128  CPU %.0f%%  最悪 %.1f ms  待ち %.0f ms  遅れ %llu   IN: %s   OUT: %s"
			              "   （MIDI IN A のジャックか右クリックで口を選ぶ）",
			              s.voices_master + s.voices_slave,
			              g_win.out->cpu_percent(), g_win.out->worst_ms(),
			              g_win.out->output_ms(),
			              (unsigned long long)g_win.out->late(),
			              g_win.in_name[0].empty()  ? "なし" : g_win.in_name[0].c_str(),
			              g_win.out_name.empty() ? "なし" : g_win.out_name.c_str());
		else
			std::snprintf(status, sizeof(status), "起動中...");
		g_win.panel.set_volume(g_win.br->gain());
		g_win.panel.paint(g_win.mem_dc, s, pressed, status);

		BitBlt(dc, 0, 0, w, h, g_win.mem_dc, 0, 0, SRCCOPY);
		EndPaint(hwnd, &ps);
		return 0;
	}

	case WM_LBUTTONDOWN: {
		if (g_win.lcd_only)
			return 0;
		const int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
		// パネルの MIDI IN A のジャックを押したら、口を選ぶ品書きを出す
		if (g_win.panel.on_midi_jack(mx, my)) {
			POINT pt{ mx, my };
			ClientToScreen(hwnd, &pt);
			show_port_menu(hwnd, pt);
			return 0;
		}
		// A/D INPUT のジャックは録音デバイス
		if (g_win.panel.on_ad_input(mx, my)) {
			POINT pt{ mx, my };
			ClientToScreen(hwnd, &pt);
			show_ain_menu(hwnd, pt);
			return 0;
		}
		// PHONES のジャックは音の出口
		if (g_win.panel.on_phones(mx, my)) {
			POINT pt{ mx, my };
			ClientToScreen(hwnd, &pt);
			show_output_menu(hwnd, pt);
			return 0;
		}
		// カードの差し込み口は MIDI ファイル
		if (g_win.panel.on_card_slot(mx, my)) {
			POINT pt{ mx, my };
			ClientToScreen(hwnd, &pt);
			show_card_menu(hwnd, pt);
			return 0;
		}
		SetCapture(hwnd);
		if (g_win.panel.press(mx, my, *g_win.br))
			InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}

	case WM_RBUTTONUP: {
		if (g_win.lcd_only)
			return 0;
		const int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
		POINT pt{ mx, my };
		ClientToScreen(hwnd, &pt);
		if (g_win.panel.on_card_slot(mx, my))
			show_card_menu(hwnd, pt);
		else if (g_win.panel.on_phones(mx, my))
			show_output_menu(hwnd, pt);
		else
			show_port_menu(hwnd, pt);
		return 0;
	}

	case WM_COMMAND: {
		const UINT id = LOWORD(wp);
		g_win.last_error.clear();
		bool in_done = false;
		for (int p = 0; p < mu2000::MIDI_PORTS && !in_done; p++) {
			const UINT none = ID_IN_NONE + p * ID_IN_STRIDE, base = ID_IN_BASE + p * ID_IN_STRIDE;
			if (id == none)                            { choose_in(p, -1); in_done = true; }
			else if (id >= base && id < base + 256)    { choose_in(p, int(id - base)); in_done = true; }
		}
		if (in_done)                     {}
		else if (id == ID_OUT_NONE)      choose_out(-1);
		else if (id >= ID_OUT_BASE && id < ID_OUT_BASE + 256) choose_out(int(id - ID_OUT_BASE));
		else if (id == ID_OUTB_NONE)     choose_out_b(-1);
		else if (id >= ID_OUTB_BASE && id < ID_OUTB_BASE + 256) choose_out_b(int(id - ID_OUTB_BASE));
		else if (id == ID_OUTMU_NONE)    choose_out_mu(-1);
		else if (id >= ID_OUTMU_BASE && id < ID_OUTMU_BASE + 256) choose_out_mu(int(id - ID_OUTMU_BASE));
		else if (id == ID_AIN_NONE)      choose_ain(-1);
		else if (id >= ID_AIN_BASE && id < ID_AIN_BASE + 256) choose_ain(int(id - ID_AIN_BASE));
		else if (id >= ID_CARD_NEW16 && id <= ID_CARD_NEW128) new_card(hwnd, 16u << (id - ID_CARD_NEW16));
		else if (id == ID_CARD_OPEN) {
			const std::string path = ask_card_path(hwnd, false);
			if (!path.empty() && insert_card(path))
				save_settings();
		}
		else if (id == ID_CARD_EJECT) { eject_card(); save_settings(); }
		else if (id == ID_PLAY_FILE) choose_midi_file(hwnd);
		else if (id == ID_STOP_FILE) g_win.play_file.stop();
		else if (id == ID_PORTS34_FOLD || id == ID_PORTS34_DROP) {
			g_win.play_file.set_fold_extra_ports(id == ID_PORTS34_FOLD);
			save_settings();
		}
		else if (id == ID_NATIVE_FX)
			g_win.eng->want_native_fx.store(g_win.eng->native_fx.load() ? 0 : 2);
		else if (id == ID_NATIVE_ENGINE)
			g_win.eng->want_native_engine.store(g_win.eng->native_engine.load() ? 0 : 1);
		else if (id == ID_FACTORY) choose_factory_reset(hwnd);
		else if (id == ID_PC_EDITOR) open_window(hwnd, g_win.pc);
		else if (id == ID_OVERVIEW) open_window(hwnd, g_win.list);
		else if ((id == ID_OUTPUT_DIGITAL || id == ID_OUTPUT_ANALOG) && g_win.eng) {
			g_win.eng->analog.store(id == ID_OUTPUT_ANALOG);
			std::printf("音の出口: %s\n", id == ID_OUTPUT_ANALOG ? "アナログ（直流を切る）" : "デジタル");
			std::fflush(stdout);
			save_settings();
		}
		if (!g_win.last_error.empty()) {
			const std::wstring w = ui::to_wide(g_win.last_error);
			MessageBoxW(hwnd, w.c_str(), L"S-MU2000", MB_OK | MB_ICONWARNING);
			g_win.last_error.clear();
		}
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}

	case WM_SETCURSOR: {
		// ジャックの上では指の形にして、押せることを見せる
		POINT pt;
		GetCursorPos(&pt);
		ScreenToClient(hwnd, &pt);
		if (LOWORD(lp) == HTCLIENT &&
		    (g_win.panel.on_midi_jack(pt.x, pt.y) ||
		     g_win.panel.on_ad_input(pt.x, pt.y) ||
		     g_win.panel.on_phones(pt.x, pt.y) ||
		     g_win.panel.on_card_slot(pt.x, pt.y))) {
			SetCursor(LoadCursor(nullptr, IDC_HAND));
			return TRUE;
		}
		break;
	}

	case WM_MOUSEMOVE:
		if (g_win.lcd_only)
			return 0;
		if (g_win.panel.drag(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), *g_win.br))
			InvalidateRect(hwnd, nullptr, FALSE);
		return 0;

	case WM_LBUTTONUP:
		if (g_win.lcd_only)
			return 0;
		g_win.panel.release(*g_win.br);
		ReleaseCapture();
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;

	case WM_MOUSEWHEEL: {
		if (g_win.lcd_only)
			return 0;
		POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
		ScreenToClient(hwnd, &pt);
		const int delta = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
		if (delta && g_win.panel.wheel_at(pt.x, pt.y, delta, *g_win.br))
			InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}

	case WM_KEYDOWN: {
		if (g_win.lcd_only)
			return 0;
		if (lp & (1 << 30))                     // 押しっぱなしの繰り返しは無視
			return 0;
		if (wp == VK_F2) {                      // PC エディタ
			open_window(hwnd, g_win.pc);
			return 0;
		}
		if (wp == VK_F3) {                      // 一覧
			open_window(hwnd, g_win.list);
			return 0;
		}
		if (wp == VK_F4 && g_win.eng) {         // firmware を走らせない口の入切
			g_win.eng->want_native_engine.store(g_win.eng->native_engine.load() ? 0 : 1);
			return 0;
		}
		if (wp == VK_F5) {                      // 配置を読み直す
			apply_layout(g_win.layout_path, false);
			InvalidateRect(hwnd, nullptr, FALSE);
			return 0;
		}
		bool ok = false;
		const mu2000::button b = key_to_button(wp, ok);
		if (ok) g_win.br->press(b, true);
		return 0;
	}

	case WM_KEYUP: {
		bool ok = false;
		const mu2000::button b = key_to_button(wp, ok);
		if (ok) g_win.br->press(b, false);
		return 0;
	}

	case WM_KILLFOCUS:
		g_win.br->release_all();                // 窓から離れたら全部離す
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcA(hwnd, msg, wp, lp);
}


// ---- 窓を出さずに絵だけ書き出す。見た目を直すときに使う

int shot(const std::string &path, int w, int h, ui::bridge &br, bool grid,
         bool lcd_only, const std::string &layout_path)
{
	ui::panel p;
	std::string lerr;
	if (!layout_path.empty() && !p.lay().load(layout_path, lerr))
		std::fprintf(stderr, "配置: %s を開けない\n", layout_path.c_str());
	if (!lerr.empty())
		std::fprintf(stderr, "%s", lerr.c_str());
	p.set_lcd_only(lcd_only);
	p.resize(w, h);
	p.set_grid(grid);

	BITMAPINFO bi{};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;                 // 上から下へ
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;

	void *bits = nullptr;
	HDC screen = GetDC(nullptr);
	HDC dc = CreateCompatibleDC(screen);
	HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	SelectObject(dc, bmp);

	ui::snapshot s;
	br.read(s);
	p.set_volume(0.8);
	p.paint(dc, s, 0, "");
	GdiFlush();

	const bool ok = ui::write_png(path, static_cast<const u8 *>(bits), w, h, w * 4);

	DeleteObject(bmp);
	DeleteDC(dc);
	ReleaseDC(nullptr, screen);

	std::printf(ok ? "書き出した: %s（%d×%d）\n" : "書き出せない: %s\n", path.c_str(), w, h);
	return ok ? 0 : 1;
}

} // namespace


int main(int argc, char **argv)
{
	SetConsoleOutputCP(CP_UTF8);

	std::string dir, shot_path;
	// -2 未指定（覚えているものを使う）/ -1 使わない
	int in_dev[mu2000::MIDI_PORTS] = { -2, -2, -2, -2 };
	bool usb_host = true;              // USB の口で起動する（口 C・D が使える）。--host-midi で切る
	int moutb_dev = -2;                // MIDI OUT B
	int mout_dev = -2;
	int moutmu_dev = -2;               // MIDI OUT（本体）
	int latency = 20;        // 溜める目標
	ui::output_options out_opts;
	ui::window_options win_opts;
	int win_w = 1000, win_h = 400;   // パネルの論理寸法（1000 × 400）と同じ比
	bool size_given = false;
	ui::engine_options eng_opts;
	bool grid = false;
	std::string layout_path, dump_layout, play_path;
	bool boot_for_shot = false;
	std::string shot_mid;
	double shot_secs = 0.0;

	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "--list")) {
			const auto ins = ui::midi_in::list();
			std::printf("MIDI 入力（--midi 番号 / 画面からも選べる）:\n");
			for (size_t k = 0; k < ins.size(); k++)
				std::printf("  %zu: %s\n", k, ins[k].c_str());
			if (ins.empty())
				std::printf("  （なし）\n");
			const auto outs = ui::midi_out::list();
			std::printf("MIDI 出力（--midiout 番号 / 受けたものをそのまま外へ）:\n");
			for (size_t k = 0; k < outs.size(); k++)
				std::printf("  %zu: %s\n", k, outs[k].c_str());
			if (outs.empty())
				std::printf("  （なし）\n");
			const auto aouts = ui::audio_out::list();
			std::printf("音声の出口（--audio に名前の一部）:\n");
			for (size_t k = 0; k < aouts.size(); k++)
				std::printf("  %zu: %s\n", k, aouts[k].c_str());
			return 0;
		}
		else if (!std::strcmp(argv[i], "--midi") && i + 1 < argc) in_dev[0] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midi-b") && i + 1 < argc) in_dev[1] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midi-c") && i + 1 < argc) in_dev[2] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midi-d") && i + 1 < argc) in_dev[3] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midiout") && i + 1 < argc) mout_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midiout-b") && i + 1 < argc) moutb_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midiout-mu") && i + 1 < argc) moutmu_dev = std::atoi(argv[++i]);
		// 入口も出口も開かない。試しに動かすとき、覚えている THRU の先（実機）へ
		// 流れないように。覚えている口は書き換えない
		else if (!std::strcmp(argv[i], "--nomidi")) {
			for (int &d : in_dev) d = -1;
			mout_dev = moutb_dev = moutmu_dev = -1;
			g_win.keep_settings = true;
		}
		else if (!std::strcmp(argv[i], "--latency") && i + 1 < argc) latency = std::atoi(argv[++i]);
		else if (ui::consume_output_option(argv, argc, i, out_opts)) {}
		else if (ui::consume_window_option(argv[i], win_opts)) {}
		else if (ui::consume_engine_option(argv[i], eng_opts)) {}
		else if (!std::strcmp(argv[i], "--usb")) usb_host = true;
		else if (!std::strcmp(argv[i], "--host-midi")) usb_host = false;
		else if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) shot_path = argv[++i];
		else if (!std::strcmp(argv[i], "--boot")) boot_for_shot = true;
		else if (!std::strcmp(argv[i], "--grid")) grid = true;
		else if (!std::strcmp(argv[i], "--layout") && i + 1 < argc) layout_path = argv[++i];
		else if (!std::strcmp(argv[i], "--play") && i + 1 < argc) play_path = argv[++i];
		else if (!std::strcmp(argv[i], "--dump-layout") && i + 1 < argc) dump_layout = argv[++i];
		else if (!std::strcmp(argv[i], "--mid") && i + 2 < argc) {
			shot_mid = argv[++i];
			shot_secs = std::atof(argv[++i]);
			boot_for_shot = true;
		}
		else if (!std::strcmp(argv[i], "--size") && i + 1 < argc) {
			if (std::sscanf(argv[++i], "%dx%d", &win_w, &win_h) != 2) { win_w = 1000; win_h = 400; }
			size_given = true;
		}
		else if (dir.empty()) dir = argv[i];
	}
	if (win_opts.lcd_only && !size_given) {
		win_w = 898;
		win_h = 290;
	}

	// --layout が無ければ、決まった場所を順に探す
	if (layout_path.empty())
		layout_path = ui::layout::find_default();

	if (!dump_layout.empty()) {
		ui::layout l;
		std::string lerr;
		if (!layout_path.empty())
			l.load(layout_path, lerr);
		if (!l.save(dump_layout)) {
			std::fprintf(stderr, "%s に書けない\n", dump_layout.c_str());
			return 1;
		}
		std::printf("いまの配置を書き出した: %s\n", dump_layout.c_str());
		std::printf("直したら --layout で渡すか、窓で F5 を押す\n");
		return 0;
	}

	static ui::bridge br;
	static ui::midi_in  midi_ports[mu2000::MIDI_PORTS];
	static ui::midi_out mout, mout_b, mout_mu;

	// 絵だけ欲しい場合。ROM が無くても中身が空の画面は出せる
	if (!shot_path.empty() && (dir.empty() || !boot_for_shot)) {
		ui::snapshot s;
		std::snprintf(s.message, sizeof(s.message), "S-MU2000");
		br.publish(s);
		return shot(shot_path, win_w, win_h, br, grid, win_opts.lcd_only, layout_path);
	}

	if (dir.empty()) {
		std::fprintf(stderr,
			"使い方: gui <rom ディレクトリ> [--midi 番号] [--midi-b 番号] [--midi-c 番号] [--midi-d 番号]"
			" [--midiout 番号] [--midiout-b 番号] [--midiout-mu 番号]"
			" [--latency ミリ秒] [--exclusive] [--layout panel.txt] [--play 曲.mid] [--lcd] [--fast-midi] [--host-midi]\n"
			"        [--factory]   覚えている設定を捨てて工場出荷状態で起動する\n"
			"        [--editor]    PC エディタも開く（窓では F2 か右クリック）\n"
			"        [--list-window] 一覧の窓も開く（窓では F3 か右クリック）\n"
			"        [--fx-window] インサーションの設定の窓も開く（一覧でインサーションの欄をダブルクリック）\n"
			"        [--shapes-window] パートの音色の窓も開く（一覧で VIB などの絵をダブルクリック）\n"
			"        [--master-window] マスターの窓も開く（一覧でマスターの行をダブルクリック）\n"
			"        gui --dump-layout panel.txt   いまの配置を書き出す\n"
			"        gui --list\n"
			"        gui [<rom ディレクトリ> --boot] --shot 絵.png [--size 1000x400]\n");
		return 1;
	}

	static engine eng(br, midi_ports[0]);
	if (std::getenv("SMU2000_VOICECACHE"))
		eng_opts.voicecache = 1;
	ui::apply_engine_options(eng.mu, eng_opts);
	eng.native_fx.store(eng_opts.native_fx);
	for (int p = 1; p < mu2000::MIDI_PORTS; p++)
		eng.midi_p[p] = &midi_ports[p];
	eng.mout_b = &mout_b;
	eng.mout_mu = &mout_mu;
	eng.mout = &mout;
	if (!eng.load(dir)) {
		std::fprintf(stderr, "%s\n", eng.message.c_str());
		return 1;
	}
	// 一覧の窓で、音色の名前と楽器の絵を利用者の ROM から読む（xg/voices.h）
	ui::xgui::set_voice_rom(eng.mu.program_rom());

	// **既定は USB の口**（実機を PC に繋ぐときと同じ姿）。口 C・D は実機では
	// USB だけの口で、firmware は HOST SELECT が USB のときしか通さない。
	// USB のときは A・B も USB 側を通る（実機で DIN が黙るのと同じ）。
	// --host-midi を付けると DIN の口 A・B だけになる。
	//
	// **起動より前に決めること**。reset() が「ホストが居る」の知らせ
	// （F4 03 01 01 01）を積むかどうかはここで決まる。--shot は下で先に
	// 起動して return するので、この行が後ろにあると絵だけ DIN の姿で
	// 撮れてしまっていた
	eng.mu.set_usb_host(usb_host);
	std::printf(usb_host ? "MIDI は USB の口（A-D の 64 パート）\n"
	                     : "--host-midi: DIN の口 A・B だけ（パート 1-32）\n");

	// 絵だけ、ただし起動後の LCD が欲しい場合
	if (!shot_path.empty()) {
		if (!eng.boot()) { std::fprintf(stderr, "%s\n", eng.message.c_str()); return 1; }
		eng.state.store(1);

		// 起動直後は表示が動いている途中。少し空回しして落ち着かせる
		{
			s32 l, r;
			for (size_t i = 0; i < size_t(2.0 * RATE); i++)
				eng.mu.run_sample(l, r);
		}

		// レベルメータを出したいので、指定があれば MIDI を流しておく
		if (!shot_mid.empty()) {
			std::vector<smf::event> evs;
			std::string err;
			if (!smf::load(shot_mid, evs, err)) {
				std::fprintf(stderr, "%s\n", err.c_str());
			} else {
				std::printf("MIDI %zu 件を %.1f 秒ぶん流す\n", evs.size(), shot_secs);
				size_t at = 0;
				s32 l, r;
				for (size_t i = 0; i < size_t(shot_secs * RATE); i++) {
					const double now = double(i) / RATE;
					while (at < evs.size() && evs[at].time <= now) {
						for (u8 b : evs[at].bytes)
							eng.mu.midi_in(b);
						at++;
					}
					eng.mu.run_sample(l, r);
				}
			}
		}

		eng.publish();
		return shot(shot_path, win_w, win_h, br, grid, win_opts.lcd_only, layout_path);
	}

	// ---- 窓を出す

	const HINSTANCE inst = GetModuleHandleA(nullptr);
	WNDCLASSA wc{};
	wc.lpfnWndProc   = wnd_proc;
	wc.hInstance     = inst;
	wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = "SMU2000Panel";
	wc.hbrBackground = nullptr;
	RegisterClassA(&wc);

	RECT want{ 0, 0, win_w, win_h };
	AdjustWindowRect(&want, WS_OVERLAPPEDWINDOW, FALSE);
	HWND hwnd = CreateWindowA("SMU2000Panel", "S-MU2000", WS_OVERLAPPEDWINDOW,
	                          CW_USEDEFAULT, CW_USEDEFAULT,
	                          want.right - want.left, want.bottom - want.top,
	                          nullptr, nullptr, inst, nullptr);
	if (!hwnd) {
		std::fprintf(stderr, "窓を出せない\n");
		return 1;
	}

	// MIDI ファイルを窓に落とせば流す（本体の窓も、エディタや一覧の窓も）
	DragAcceptFiles(hwnd, TRUE);
	ui::pc_window::set_drop_handler(play_dropped_file);

	g_win.br   = &br;
	g_win.eng  = &eng;
	g_win.lcd_only = win_opts.lcd_only;
	g_win.panel.set_lcd_only(win_opts.lcd_only);
	// 窓を出すときだけ、覚えている設定で起動する（--shot は毎回同じ絵にしたい）
	eng.use_nvram = !out_opts.factory;
	if (out_opts.factory)
		std::printf("工場出荷状態で起動する（覚えていた設定は終わるときに上書きされる）\n");
	g_win.layout_path = layout_path;
	for (int p = 0; p < mu2000::MIDI_PORTS; p++)
		g_win.midi[p] = &midi_ports[p];
	g_win.mout   = &mout;
	g_win.mout_b = &mout_b;
	g_win.mout_mu = &mout_mu;
	g_win.panel.resize(win_w, win_h);
	apply_layout(layout_path, false);
	g_win.panel.resize(win_w, win_h);
	{
		// VOLUME のつまみは前に閉じたときの位置から
		std::string ins[mu2000::MIDI_PORTS], b, c, d;
		float volume = 1.0f;
		bool fold34 = true;
		bool analog = false;
		load_settings(ins, b, c, d, &volume, nullptr, &fold34, nullptr, nullptr, &analog);
		br.set_gain(volume);
		eng.analog.store(analog);
		if (analog)
			std::printf("音の出口: アナログ（直流を切る）\n");
		g_win.play_file.set_fold_extra_ports(fold34);
	}

	eng.publish();
	ShowWindow(hwnd, SW_SHOW);
	if (win_opts.open_editor && !win_opts.lcd_only)
		open_window(hwnd, g_win.pc);
	if (win_opts.open_fx && !win_opts.lcd_only)
		open_window(hwnd, g_win.fx);
	if (win_opts.open_list && !win_opts.lcd_only)
		open_window(hwnd, g_win.list);
	if (win_opts.open_shapes && !win_opts.lcd_only)
		open_window(hwnd, g_win.shapes);
	if (win_opts.open_master && !win_opts.lcd_only)
		open_window(hwnd, g_win.master);
	UpdateWindow(hwnd);

	// 起動は別スレッド。終わったら音を出し始める
	static ui::audio_out out;
	g_win.out = &out;
	static ui::audio_in ain;
	g_win.ain = &ain;
	eng.ain = &ain;
	std::thread boot_thread([&] {
		if (!eng.boot()) {
			eng.state.store(2);
			eng.publish();
			return;
		}
		// 起動が終わってから入れる（起動には firmware が要る）
		if (eng_opts.native_engine) {
			eng.mu.set_native_engine(eng_opts.native_engine);
			eng.native_engine.store(eng_opts.native_engine);
			if (eng_opts.voicecache)
				smu2000::voicecache::load(eng.mu, smu2000::voicecache::key(eng.mu));
		}
		eng.state.store(1);
		eng.publish();

		// 前に選んだ口を名前で探す。--midi / --midiout があればそちらが勝つ
		std::string want_in[mu2000::MIDI_PORTS];
		std::string want_out, want_out_b, want_audio, want_out_mu;
		std::string want_ain, want_card;
		load_settings(want_in, want_out, want_out_b, want_audio, nullptr, &want_out_mu, nullptr, &want_ain, &want_card);
		g_win.ain_name = want_ain;
		// 前に差していた SmartMedia。ファイルが無くなっていたら差さない（覚えている名前も消える）
		if (!want_card.empty())
			insert_card(want_card, true);
		// --audio があればそちらが勝つ。無ければ前に選んだもの
		g_win.audio_name = out_opts.audio_dev ? std::string(out_opts.audio_dev) : want_audio;
		for (int p = 0; p < mu2000::MIDI_PORTS; p++)
			if (in_dev[p] == -2)
				in_dev[p] = find_device(ui::midi_in::list(), want_in[p]);
		if (mout_dev == -2)
			mout_dev = find_device(ui::midi_out::list(), want_out);
		if (moutb_dev == -2)
			moutb_dev = find_device(ui::midi_out::list(), want_out_b);
		if (moutmu_dev == -2)
			moutmu_dev = find_device(ui::midi_out::list(), want_out_mu);

		for (int p = 0; p < mu2000::MIDI_PORTS; p++)
			g_win.in_keep[p] = want_in[p];
		g_win.out_keep    = want_out;
		g_win.out_keep_b  = want_out_b;
		g_win.out_keep_mu = want_out_mu;
		for (int p = 0; p < mu2000::MIDI_PORTS; p++)
			choose_in(p, in_dev[p], true);
		choose_out(mout_dev, true);
		choose_out_b(moutb_dev, true);
		choose_out_mu(moutmu_dev, true);
		// 開けなかった口は、覚えていた名前も出す（選び直すまで覚えている）
		auto show = [](const char *label, const std::string &now, const std::string &keep) {
			if (!now.empty())
				std::printf("%s: %s\n", label, now.c_str());
			else if (!keep.empty())
				std::printf("%s: なし（「%s」が見つからないか開けない。覚えたままにしてある）\n",
				            label, keep.c_str());
			else
				std::printf("%s: なし\n", label);
		};
		for (int p = 0; p < mu2000::MIDI_PORTS; p++)
			show(ui::IN_LABELS[p], g_win.in_name[p], g_win.in_keep[p]);
		show("MIDI OUT",    g_win.out_name_mu, g_win.out_keep_mu);
		show("MIDI THRU A", g_win.out_name,    g_win.out_keep);
		show("MIDI THRU B", g_win.out_name_b,  g_win.out_keep_b);
		std::fflush(stdout);

		std::string err;

		if (!out.start(latency, [](s16 *o, u32 n) { eng.fill(o, n); }, err, out_opts.exclusive,
		               g_win.audio_name)) {
			std::fprintf(stderr, "音声: %s\n", err.c_str());
			eng.message = "音声デバイスを開けない";
			eng.state.store(2);
			eng.publish();
			return;
		}
		// 開けた出口を覚える。**設定を読んで MIDI の口を開いた後でないと
		// いけない**。前はこれを起動直後にやっていて、まだ空の MIDI の名前で
		// gui.ini を上書きしていた（毎回 MIDI が「なし」に戻っていた）
		g_win.audio_name = out.device_name();
		std::printf("音声の出口: %s\n%s\n", out.device_name().c_str(),
		            out.format_line().c_str());
		// A/D INPUT。前に選んだ録音デバイスがあれば開く（開けなくても名前は覚えておく）
		if (!g_win.ain_name.empty()) {
			std::string aerr;
			if (ain.start(g_win.ain_name, aerr))
				std::printf("A/D INPUT: %s（%s）\n", ain.device_name().c_str(), ain.format_line().c_str());
			else
				std::printf("A/D INPUT: なし（%s）\n", aerr.c_str());
		}
		save_settings();
		// --play が付いていれば、鳴り始めたところで流し出す
		if (!play_path.empty()) {
			std::string perr;
			if (!g_win.play_file.start(play_path, br, perr))
				std::fprintf(stderr, "MIDI ファイル: %s\n", perr.c_str());
			else
				std::printf("再生: %s（%.1f 秒）\n", play_path.c_str(),
				            g_win.play_file.length());
		}
		std::printf("鳴らしている（待ち時間 %.1f ms、MMCSS %s）\n",
		            1000.0 * out.buffer_frames() / RATE,
		            out.mmcss() ? "登録できた" : "登録できない（途切れやすい）");
		std::fflush(stdout);
	});

	MSG msg;
	while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageA(&msg);
	}

	// PC の窓に閉じたと知らせる（一覧のミュートを外して受信チャンネルを戻すなど）。
	// 送ったものは音声の糸が流すので、少し待ってから止める
	if (g_win.br) {
		ui::pc_shutdown_all(g_win.list, g_win.pc, g_win.fx, g_win.shapes, g_win.master, *g_win.br);
		Sleep(100);
	}

	// **先に MIDI ファイルを止める。** 止めたときのオールノートオフは音声の糸が THRU から
	// 外へ流すので、音を先に止めると外の機器（実機）に届かず鳴りっぱなしになる。
	// 止めてから、音声の糸が流し終えるのを少し待つ
	if (g_win.play_file.playing()) {
		g_win.play_file.stop();
		Sleep(150);
	}
	out.stop();
	// 念のため、THRU の先へ直にもオールサウンドオフ・オールノートオフを送る。
	// 音声の糸はもう止まっているので、ここから送っても取り合いにならない
	for (ui::midi_out *thru : { &mout, &mout_b }) {
		if (!thru->is_open())
			continue;
		for (int ch = 0; ch < 16; ch++) {
			for (u8 v : { u8(0xb0 | ch), u8(120), u8(0), u8(0xb0 | ch), u8(123), u8(0) })
				thru->send(v);
		}
	}
	if (boot_thread.joinable())
		boot_thread.join();
	if (g_win.reboot.joinable())
		g_win.reboot.join();
	flush_card();      // 音はもう止まっている。SmartMedia に書いたものを残す
	save_settings();   // VOLUME のつまみの位置
	// 音はもう止まっている。起動できていたときだけ残す
	eng.settle_for_save();
	if (eng.state.load() == 1 && !smu2000::nvram::save(eng.mu))
		std::fprintf(stderr, "設定を残せなかった: %s\n", smu2000::nvram::path(eng.mu).c_str());
	// 残した設定で起動した写しも用意しておく（src/bootcache.h）。無いと、
	// 設定をいじった次の 1 回だけ起動が遅くなる。1 秒ほどかかるが、
	// 窓はもう閉じているので待たせない。溜まった古い写しはここで間引く
	if (eng.state.load() == 1) {
		if (smu2000::bootcache::refresh(eng.mu))
			std::printf("次の起動ぶんの写しを作った\n");
		smu2000::bootcache::prune();
	}
	g_win.play_file.stop();
	for (ui::midi_in &m : midi_ports)
		m.close();
	mout.close();
	mout_mu.close();
	mout_b.close();
	ain.stop();

	// 音を出さずに終わったとき（起動に失敗した、音声デバイスを開けなかった）は、どちらも出さない
	if (out.produced()) {
		std::printf("CPU %.1f%%、1 回の最悪 %.2f ms、間に合わなかった %llu 回\n",
		            out.cpu_percent(), out.worst_ms(),
		            (unsigned long long)out.late());
		std::printf("%s\n%s\n", out.format_line().c_str(), out.latency_line().c_str());
	}
	return 0;
}
