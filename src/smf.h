// license:BSD-3-Clause
//
// 標準 MIDI ファイル（SMF）を「秒 + バイト列」の並びに開く。
// render（ファイルを WAV に）と midisend（実時間で MIDI 出力へ）で共用する。

#ifndef S_MU2000_SMF_H
#define S_MU2000_SMF_H

#pragma once

#include "compat/mamecompat.h"

#include <string>
#include <vector>

namespace smf {

struct event {
	double time;              // 秒
	std::vector<u8> bytes;
	// どの MIDI の口へ出すか。SMF のメタイベント `FF 21 01 pp`
	// （ポート指定）か、`FF 09`（機器名）が「A」〜「D」「Port 1」〜「Port 4」のものを
	// トラックごとに見る。ヤマハのシーケンサー固有のポート指定（`FF 7F 04 43 00 01 pp`）も同じ扱い。
	// どれも無ければ**トラック名**（`FF 03`）が「PartA」「Part A」
	// 「A01」「A1」「A-1」〜「D16」の形なら、その口（issue #63。MU2000EX 向けの 64 パートの曲）。
	// 何も無ければ 0。MU2000 は口 0-3 が A-D（パート 1-16、17-32、33-48、49-64）
	u8 port = 0;
};

// ファイルの口をエミュの口（0-3 = A-D）に割り当てる。USB の口（usb。gui の既定）なら
// A-D の 4 口がそのまま届く。DIN の口だけ（--host-midi）のときは実機と同じく A・B しか無いので、
// 3 口目以降は fold が真なら A・B に交互に重ね（口 3 → A、口 4 → B）、偽なら鳴らさない（-1）。
// 5 口目以降は USB でも同じ扱い（fold なら 4 口に重ねる）
// board が真（口 E を受け持つマルチパートのプラグインボードが挿さっている）なら、5 口目はそのボードへ（4）
inline int mu_port(u8 port, bool fold, bool usb = false, bool board = false)
{
	if (board && port == 4)
		return 4;
	const int n = usb ? 4 : 2;
	if (port < n)
		return port;
	return fold ? port % n : -1;
}

// トラック名から口を読む（issue #63）。「PartA」「Part A」「Part-A」「A01」「A1」「A-1」「A 16」、
// 2 桁の番号のあとに区切りと名前が続く「A01-FrHorn 2」（大文字小文字は問わない）。読めなければ -1。
// 「A」1 文字だけのものや「B3 Organ」は曲名・楽器名と紛れるので読まない
int port_from_track_name(const std::string &name);

// 曲の見出しと、時間の目盛り（プレイヤーの表示と頭出し用）
struct tempo_point { double time; u64 tick; double bpm; };   // この時刻・tick から、この速さ
struct sig_point { u64 tick; int num, den; };                // この tick から、この拍子
struct song_meta {
	// 曲名。format 0 は最初のトラック、format 1 は最初のトラック（指揮のトラック）の最初のトラック名（`FF 03`）。
	// 口の名前（「PartA」など）や空のものは曲名にしない。UTF-8 でなければ空（Windows は Shift_JIS から直す）
	std::string title;
	int ppq = 480;
	std::vector<tempo_point> tempo;   // 時刻の順。頭に必ず 1 つある（無指定なら 120）
	std::vector<sig_point> sig;       // 拍子の順。空なら 4/4
	// その時刻の小節（1 から）・拍（1 から）・テンポ
	void bar_beat(double sec, int &bar, int &beat, double &bpm) const;
};

// 曲名などのファイル内テキストを UTF-8 に正規化する。
std::string title_text(const std::string &raw);

// format 0/1 に対応。テンポ変化は追う。SMPTE 単位には未対応。meta を渡せば曲名と目盛りも返す
bool load(const std::string &path, std::vector<event> &out, std::string &err, song_meta *meta = nullptr);

// 頭出し: 時刻 at へ飛ぶときに、そこまでの設定を追いかけるために送るもの。
// 音符（ノートオン・オフ）は送らない。SysEx・バンクセレクト・プログラムチェンジ・RPN/NRPN とデータエントリーは
// 順番に意味があるので全部を元の順に、ほかのコントロールチェンジ・ピッチベンド・アフタータッチは最後の値だけを
// その後ろに並べる（同じつまみを何千回も送り直さない）。time は使わない（0）
std::vector<event> chase(const std::vector<event> &events, double at);

// メモリ上の SMF を開く（wasm 用。load(path) と同じものを返す）。
// data/size はファイルの中身そのまま。
bool load_from_memory(const u8 *data, size_t size, std::vector<event> &out, std::string &err, song_meta *meta = nullptr);

} // namespace smf

#endif // S_MU2000_SMF_H
