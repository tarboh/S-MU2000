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

#include "compat/console.h"
#include "compat/crash_log.h"

#include "mu2000.h"
#include "ui/app_win.h"
#include "ui/lang.h"
#include "ui/options.h"
#include "ui/rom_locate.h"
#include "ui/tool_args.h"
#include "ui/window_win.h"

#include <cstdio>
#include <string>

namespace {

// ---- 音源側
//
// The engine itself now lives in ui/engine.h: the macOS front end needs the
// same boot sequence and, more to the point, the same MIDI routing, and one
// copy is the only way to keep the two from drifting. Only the name is pulled
// in here, so the rest of this file reads as it always did.

// The menu names below are unqualified for the choice dispatch (WM_COMMAND).
using namespace ui;

} // namespace


int main(int argc, char **argv)
{
	smu2000::init_console_utf8();
	// abort() や std::terminate で止まったら、呼び出し元を <設定>/S-MU2000/crash.txt に残す
	smu2000::crash_log::install();

	ui::tool_args a;
	a.latency = 20;        // 溜める目標 (per-backend default; the shared parser keeps it)
	ui::window_options win_opts;
	ui::engine_options eng_opts;
	ui::output_options out_opts;

	// The flags are shared (ui/tool_args.h); only latency above stays per side
	const int parsed = ui::parse_tool_args(argc, argv, a, eng_opts, out_opts, win_opts);
	// The language resolves here, from the parsed --lang (then editor.ini,
	// then the locale), before any texts() use below
	ui::init_lang(a.lang.c_str());
	if (parsed >= 0)
		return parsed;

	static ui::bridge br;
	static ui::midi_in  midi_ports[mu2000::MIDI_PORTS];
	static ui::midi_out mout, mout_b, mout_mu;
	static win_app gui(br, midi_ports, mout, mout_b, mout_mu);
	g_win = &gui;

	// 絵だけ欲しい場合。ROM が無くても中身が空の画面は出せる
	if (!a.shot_path.empty() && (a.dir.empty() || !a.boot_for_shot))
		return ui::empty_shot(br, a, win_opts);

	// ROM の場所を渡されなければ（ダブルクリックなど）、プラグインと同じ順番で探し、無ければ選んでもらう
	// （ui/rom_locate.h、issue #89）
	if (a.dir.empty())
		a.dir = ui::locate_roms_for_gui(smu2000::exe_dir());
	if (a.dir.empty()) {
		ui::print_usage();
		return 1;
	}

	static ui::engine eng(br, midi_ports[0]);
	gui.wire_engine(eng, eng_opts);
	gui.eng = &eng;
	gui.state = &eng.state;
	if (!gui.load_machine(eng, a))
		return 1;
	const int shot = gui.run_boot_shot(eng, br, a, win_opts);
	if (shot >= 0)
		return shot;

	// MIDI ファイルを窓に落とせば流す（本体の窓も、エディタや一覧の窓も）。
	// The window itself is made and pumped by ui::app::run
	ui::pc_window::set_drop_handler(play_dropped_file);

	return gui.run(a, eng_opts, out_opts, win_opts);
}
