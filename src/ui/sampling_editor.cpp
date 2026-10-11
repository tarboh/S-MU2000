// license:BSD-3-Clause
#include "sampling_editor.h"

#include "wav_in.h"
#include "user_boards.h"
#include "smartmedia.h"
#include "xg/voices.h"
#include "compat/paths.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <memory>

namespace ui {

namespace sp = smu2000::sampling;

namespace {

// 見出し 1 行。区画の頭に
void heading(const char *text)
{
	ImGui::TextUnformatted(text);
	ImGui::Separator();
}

const char *pan_text(int pan, char *buf, size_t n)
{
	if (pan == 15)
		std::snprintf(buf, n, "%s", UI_TEXT(smp_pan_scaling, "Scaling"));
	else if (pan == 7)
		std::snprintf(buf, n, "C");
	else
		std::snprintf(buf, n, "%c%d", pan < 7 ? 'L' : 'R', pan < 7 ? 7 - pan : pan - 7);
	return buf;
}

// 16bit の絶対値を dBFS に（0 は -90）
float db(s32 peak)
{
	return peak <= 0 ? -90.0f : 20.0f * std::log10(float(peak) / 32768.0f);
}

void meter(const char *label, s32 peak, s32 trigger)
{
	const float fs = ImGui::GetFontSize();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(label);
	ImGui::SameLine(fs * 3.5f);
	const float w = ImGui::GetContentRegionAvail().x - fs * 5;
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float h = ImGui::GetFrameHeight() * 0.6f;
	ImDrawList *dl = ImGui::GetWindowDrawList();
	auto x_of = [&](float d) { return p.x + w * std::clamp((d + 60.0f) / 60.0f, 0.0f, 1.0f); };
	dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg));
	const float d = db(peak);
	const ImU32 col = d > -1.0f ? IM_COL32(230, 70, 60, 255) : d > -12.0f ? IM_COL32(230, 200, 60, 255) : IM_COL32(90, 200, 110, 255);
	dl->AddRectFilled(p, ImVec2(x_of(d), p.y + h), col);
	if (trigger > 0) {
		const float tx = x_of(db(trigger));
		dl->AddLine(ImVec2(tx, p.y - 2), ImVec2(tx, p.y + h + 2), IM_COL32(255, 255, 255, 220), 2.0f);
	}
	ImGui::Dummy(ImVec2(w, h));
	ImGui::SameLine();
	if (peak > 0)
		ImGui::Text("%5.1f dB", d);
	else
		ImGui::TextUnformatted("  -inf");
}

// 拡大した部分の波形を [v0, v1)（サンプルの位置）の枠に描く。shift だけずらして置く（つなぎ目の向こう側を重ねるとき）
void draw_slice(ImDrawList *dl, ImVec2 p, ImVec2 sz, double v0, double v1, const bridge::sampling_view::slice &s,
                double shift, ImU32 col)
{
	const int nb = int(s.hi.size());
	if (nb <= 0 || s.to <= s.from)
		return;
	const double span = v1 - v0, per = double(s.to - s.from) / double(nb);
	const float mid = p.y + sz.y * 0.5f, half = sz.y * 0.5f - 2.0f;
	auto x_of = [&](double f) { return p.x + float((f - v0) / span * double(sz.x)); };
	auto y_of = [&](int v) { return mid - half * float(v) / 32768.0f; };
	const double px_per_bucket = per / span * double(sz.x);
	if (px_per_bucket < 1.0) {
		// 1 列に区切りが 1 つ以上: 列ごとに最小と最大の縦線
		const int cols = std::max(1, int(sz.x));
		for (int x = 0; x < cols; x++) {
			const double fa = v0 + double(x) / double(sz.x) * span - shift, fb = v0 + double(x + 1) / double(sz.x) * span - shift;
			const int b0 = std::max(0, int(std::floor((fa - s.from) / per))), b1 = std::min(nb, int(std::ceil((fb - s.from) / per)));
			if (b1 <= b0)
				continue;
			int lo = 32767, hi = -32768;
			for (int b = b0; b < b1; b++) {
				lo = std::min(lo, int(s.lo[size_t(b)]));
				hi = std::max(hi, int(s.hi[size_t(b)]));
			}
			dl->AddLine(ImVec2(p.x + float(x) + 0.5f, y_of(hi)), ImVec2(p.x + float(x) + 0.5f, std::max(y_of(lo), y_of(hi) + 1.0f)), col);
		}
		return;
	}
	// 1 サンプルが何列にもなる: 点を線でつなぎ、広ければ点も打つ
	ImVec2 prev;
	for (int b = 0; b < nb; b++) {
		const double f = double(s.from) + (double(b) + 0.5) * per + shift;
		const ImVec2 pt(x_of(f), y_of((int(s.lo[size_t(b)]) + int(s.hi[size_t(b)])) / 2));
		if (b)
			dl->AddLine(prev, pt, col, 1.5f);
		if (px_per_bucket > 6.0)
			dl->AddCircleFilled(pt, 2.0f, col);
		prev = pt;
	}
}

// 横のスクロールバー。v0 は表示の左端（サンプル）、span は表示の幅、total は全体。動かしたら true。
// つまみをつかむとその所を保ち、つまみの外を押すとそこへ飛ぶ
bool hscroll(const char *id, double &v0, double span, double total, float w)
{
	const float h = ImGui::GetFontSize() * 0.75f;
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton(id, ImVec2(w, h));
	const double maxv = std::max(0.0, total - span);
	const float tw = maxv > 0 ? std::max(h * 2.0f, w * float(span / total)) : w;
	const double cur = std::clamp(v0, 0.0, maxv);
	const float tx = p.x + (maxv > 0 ? float(cur / maxv) * (w - tw) : 0.0f);
	bool changed = false;
	const ImGuiID key = ImGui::GetItemID();
	ImGuiStorage *st = ImGui::GetStateStorage();
	const float mx = ImGui::GetIO().MousePos.x;
	if (ImGui::IsItemActivated())
		st->SetFloat(key, mx >= tx && mx <= tx + tw ? mx - tx : tw * 0.5f);
	if (ImGui::IsItemActive() && maxv > 0) {
		const float grab = st->GetFloat(key, tw * 0.5f);
		v0 = std::clamp(double(mx - grab - p.x) / double(w - tw) * maxv, 0.0, maxv);
		changed = true;
	}
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImGuiCol_ScrollbarBg), h * 0.5f);
	const ImGuiCol c = ImGui::IsItemActive() ? ImGuiCol_ScrollbarGrabActive
	                   : ImGui::IsItemHovered() ? ImGuiCol_ScrollbarGrabHovered : ImGuiCol_ScrollbarGrab;
	dl->AddRectFilled(ImVec2(tx + 1, p.y + 2), ImVec2(tx + tw - 1, p.y + h - 2), ImGui::GetColorU32(c), h * 0.5f);
	return changed;
}

// 波形の枠: 角を少し丸めた背景と、色の付いた縁（縁は波形を描いた後に、切り抜きの外で）
void wave_frame(ImDrawList *dl, ImVec2 p, ImVec2 sz)
{
	dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(16, 20, 26, 255), 4.0f);
}

void wave_border(ImDrawList *dl, ImVec2 p, ImVec2 sz, ImU32 border)
{
	dl->AddRect(ImVec2(p.x - 1, p.y - 1), ImVec2(p.x + sz.x + 1, p.y + sz.y + 1), border, 4.0f, 0, 1.5f);
}

s32 trigger_level(int trigger_db)
{
	return trigger_db >= 0 ? 0 : s32(std::lround(32768.0 * std::pow(10.0, trigger_db / 20.0)));
}

} // namespace

void sampling_editor::hidden(bridge &br)
{
	if (m_hidden_stopped)
		return;
	m_hidden_stopped = true;
	preset_restore(br);
	m_rw_playing = m_rw_start = m_rw_play_wanted = false;
	br.post([](mu2000 &mu) {
		mu.preview_stop();
		return std::string();
	});
}

void sampling_editor::draw(xg::model &m, const xg_snapshot &ram, bridge &br)
{
	m_hidden_stopped = false;
	(void)m;
	(void)ram;
	br.get_sampling(m_view);
	if (m_view.serial != m_note_serial && !m_view.message.empty()) {
		// 仕事の結果は、次の写しにも同じ文が残るので、変わったときだけ受け取る
		if (m_view.message != m_note)
			m_note = m_view.message;
		m_note_serial = m_view.serial;
	}

	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
	                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::Begin("sampling_editor", nullptr, wf);
	ImGui::PopStyleVar();

	if (!m_view.ready) {
		ImGui::TextUnformatted(UI_TEXT(smp_not_ready, "Waiting for the MU2000 to start..."));
		ImGui::End();
		return;
	}

	pump_sysex(br);

	// WAV のファイルの窓から読んだ中身
	std::vector<u8> opened;
	if (xgui::take_opened_wav(opened)) {
		// 同じ窓で SysEx も選べる。頭が F0 なら SysEx
		if (!opened.empty() && opened[0] == 0xf0)
			load_sysex(opened, br);
		else
			import_wav(opened, br);
	}

	const float fs = ImGui::GetFontSize();
	// 2 列。左 = サンプルの一覧（どの方法で用意したものもここに並ぶ）。右 = タブ 2 つで、それぞれに子のタブ:
	// 音の用意（手段ごと: 録音・取り込み / カード / 波形を作る）、音の加工（サンプル = 波形・トリム・ループ / 音色 = 要素ごとの割り当て）
	const float full_w = ImGui::GetContentRegionAvail().x;
	const float left_w = std::min(fs * 19.0f, full_w * 0.3f);
	if (ImGui::BeginChild("samples", ImVec2(left_w, 0), ImGuiChildFlags_Borders))
		samples_pane(br);
	ImGui::EndChild();
	ImGui::SameLine();
	if (ImGui::BeginChild("right", ImVec2(0, 0))) {
		const int go = m_goto_tab;
		m_rw_drawn = false;
		m_pv_drawn = false;
		m_lib_drawn = false;
		// 「音色」のタブは親のタブの中にある。親がまだ開いていない描画では子のタブまで届かないので、
		// 子のタブを実際に出すまで頼みを持ち越す（1 度で消すと、親だけ替わって子が「サンプル」のままになる）
		if (go != 1)
			m_goto_tab = 0;
		if (ImGui::BeginTabBar("right_tabs")) {
			if (ImGui::BeginTabItem(UI_TEXT(smp_tab_prepare, "Get a sound"))) {
				if (ImGui::BeginTabBar("prepare_tabs")) {
					if (ImGui::BeginTabItem(UI_TEXT(smp_tab_record, "Record / import"))) {
						if (ImGui::BeginChild("inrec", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
							// 入力と録音を横に並べる（狭ければ縦に）
							const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
							const bool side = half > fs * 20.0f;
							ImGui::BeginChild("in", ImVec2(side ? half : 0, 0), ImGuiChildFlags_AutoResizeY);
							input_pane(br);
							ImGui::EndChild();
							if (side)
								ImGui::SameLine();
							else
								ImGui::Separator();
							ImGui::BeginChild("rec", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY);
							record_pane(br);
							ImGui::EndChild();
						}
						ImGui::EndChild();
						ImGui::EndTabItem();
					}
					if (ImGui::BeginTabItem(UI_TEXT(smp_tab_card, "Card"))) {
						if (ImGui::BeginChild("card", ImVec2(0, 0), ImGuiChildFlags_Borders))
							card_pane(br);
						ImGui::EndChild();
						ImGui::EndTabItem();
					}
					if (ImGui::BeginTabItem(UI_TEXT(smp_tab_make, "Make a wave"))) {
						if (ImGui::BeginChild("make", ImVec2(0, 0), ImGuiChildFlags_Borders))
							make_pane(br);
						ImGui::EndChild();
						ImGui::EndTabItem();
					}
					ImGui::EndTabBar();
				}
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem(UI_TEXT(smp_tab_process, "Shape the sound"), nullptr, go == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
				if (ImGui::BeginTabBar("process_tabs")) {
					if (ImGui::BeginTabItem(UI_TEXT(smp_tab_edit, "Sample"))) {
						if (ImGui::BeginChild("wave", ImVec2(0, 0), ImGuiChildFlags_Borders))
							wave_pane(br);
						ImGui::EndChild();
						ImGui::EndTabItem();
					}
					if (ImGui::BeginTabItem(UI_TEXT(smp_tab_voice, "Voice"), nullptr, go == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
						if (go == 1)
							m_goto_tab = 0;
						if (ImGui::BeginChild("assign", ImVec2(0, 0), ImGuiChildFlags_Borders))
							assign_pane(br);
						ImGui::EndChild();
						ImGui::EndTabItem();
					}
					ImGui::EndTabBar();
				}
				ImGui::EndTabItem();
			}
			// 内蔵ウェーブ: 内蔵の波形を 1 つずつ聞いて・見て、何の音色が使っているかを調べる
			if (ImGui::BeginTabItem(UI_TEXT(smp_tab_romwave, "Built-in waves"), nullptr, go == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
				if (ImGui::BeginChild("romwave", ImVec2(0, 0), ImGuiChildFlags_Borders))
					romwave_pane(br);
				ImGui::EndChild();
				ImGui::EndTabItem();
			}
			// 内蔵音色: 内蔵の音色がどの波形をどう重ねているかを見て、要素を選んで鳴らす
			if (ImGui::BeginTabItem(UI_TEXT(smp_tab_presets, "Built-in voices"), nullptr, go == 3 ? ImGuiTabItemFlags_SetSelected : 0)) {
				if (ImGui::BeginChild("presets", ImVec2(0, 0), ImGuiChildFlags_Borders))
					preset_pane(br);
				ImGui::EndChild();
				ImGui::EndTabItem();
			}
			// ライブラリ: 作った音色を PC に取っておき、選んで戻す（電源を切ると消えるメモリの代わりの置き場）
			if (ImGui::BeginTabItem(UI_TEXT(smp_tab_library, "Library"), nullptr, go == 4 ? ImGuiTabItemFlags_SetSelected : 0)) {
				if (ImGui::BeginChild("library", ImVec2(0, 0), ImGuiChildFlags_Borders))
					library_pane(br);
				ImGui::EndChild();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		// 内蔵音色のタブから離れたら、試聴に借りたサンプル音色の枠を元に戻す
		if (!m_pv_drawn && !m_lib_drawn)
			preset_restore(br);            // ライブラリの試聴も同じ枠を借りる
		// 内蔵ウェーブのタブから離れたら、そこで鳴らしていた波形を止める（ループする波形が鳴りっぱなしにならないように）
		if (!m_rw_drawn && (m_rw_playing || m_rw_start || m_rw_play_wanted)) {
			if (m_rw_playing)
				br.post([](mu2000 &mu) {
					mu.preview_stop();
					return std::string();
				});
			m_rw_playing = m_rw_start = m_rw_play_wanted = false;
		}
	}
	ImGui::EndChild();
	ImGui::End();
}

void sampling_editor::input_pane(bridge &br)
{
	heading(UI_TEXT(smp_input, "Input"));
	// 録音デバイス。gui だけ（プラグインはホストの A/D Input バス）
	std::vector<std::string> names;
	std::string current;
	if (br.ain_devices(names, current)) {
		ImGui::TextUnformatted(UI_TEXT(smp_device, "Recording device"));
		ImGui::SetNextItemWidth(-1);
		const char *none = UI_TEXT(smp_device_none, "(none)");
		if (ImGui::BeginCombo("##ain", current.empty() ? none : current.c_str())) {
			br.request_ain_list();
			if (ImGui::Selectable(none, current.empty()))
				br.request_ain(-1);
			for (size_t i = 0; i < names.size(); i++)
				if (ImGui::Selectable(names[i].c_str(), names[i] == current))
					br.request_ain(int(i));
			ImGui::EndCombo();
		}
	} else {
		ImGui::TextWrapped("%s", UI_TEXT(smp_device_host, "Recording comes from the host's A/D Input bus."));
	}

	ImGui::TextUnformatted(UI_TEXT(smp_source, "Record from"));
	ImGui::RadioButton("AD1", &m_source, int(sp::source::ad1));
	ImGui::SameLine();
	ImGui::RadioButton("AD2", &m_source, int(sp::source::ad2));
	ImGui::SameLine();
	ImGui::RadioButton("AD1+2", &m_source, int(sp::source::both));

	const s32 trig = trigger_level(m_trigger_db);
	meter("AD1", m_view.peak[0], m_source != int(sp::source::ad2) ? trig : 0);
	meter("AD2", m_view.peak[1], m_source != int(sp::source::ad1) ? trig : 0);

	ImGui::TextUnformatted(UI_TEXT(smp_trigger, "Trigger"));
	ImGui::SetNextItemWidth(-1);
	ImGui::SliderInt("##trig", &m_trigger_db, -60, 0,
	                 m_trigger_db >= 0 ? UI_TEXT(smp_trigger_off, "Off (start at once)") : "%d dB");
}

void sampling_editor::record_pane(bridge &br)
{
	heading(UI_TEXT(smp_record, "Record"));
	const float fs = ImGui::GetFontSize();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_name, "Name"));
	ImGui::SameLine(fs * 4.5f);
	ImGui::SetNextItemWidth(fs * 8);
	ImGui::InputTextWithHint("##name", "take###", m_name, sizeof(m_name));

	const bool busy = m_view.rec_state != 0;
	const std::string name = m_name;
	if (!busy) {
		const bool can = m_view.free_frames > sp::SAMPLE_RATE / 10;
		ImGui::BeginDisabled(!can);
		ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(170, 50, 50, 255));
		if (ImGui::Button(UI_TEXT(smp_record_start, "Record"), ImVec2(fs * 7, 0))) {
			const sp::source src = sp::source(m_source);
			const int trig = trigger_level(m_trigger_db);
			br.post([src, trig](mu2000 &mu) {
				mu.rec_start(src, trig, mu.sampling_free_frames());
				return std::string();
			});
		}
		ImGui::PopStyleColor();
		ImGui::EndDisabled();
	} else if (ImGui::Button(UI_TEXT(smp_stop, "Stop"), ImVec2(fs * 7, 0))) {
		// 止めたら、録れたものを firmware の表に足す
		std::string nothing = UI_TEXT(smp_nothing, "Nothing was recorded");
		std::string added = UI_TEXT(smp_added_fmt, "Added sample %03d (%.1f s)");
		br.post([name, nothing, added](mu2000 &mu) {
			const std::vector<s16> pcm = mu.rec_take();
			if (pcm.empty())
				return nothing;
			std::string err;
			const int n = mu.sampling_add(pcm.data(), pcm.size(), name, err);
			if (!n)
				return err;
			char buf[160];
			std::snprintf(buf, sizeof(buf), added.c_str(), n, double(pcm.size()) / sp::SAMPLE_RATE);
			return std::string(buf);
		});
	}
	ImGui::SameLine();
	if (m_view.rec_state == 1)
		ImGui::TextUnformatted(UI_TEXT(smp_waiting, "Waiting for the trigger..."));
	else if (m_view.rec_state == 2)
		ImGui::Text(UI_TEXT(smp_recording_fmt, "Recording %.1f s"), double(m_view.rec_frames) / sp::SAMPLE_RATE);
	ImGui::Text(UI_TEXT(smp_free_fmt, "%.1f s free"),
	            double(m_view.free_frames - std::min(m_view.free_frames, m_view.rec_frames)) / sp::SAMPLE_RATE);

	ImGui::Spacing();
	ImGui::TextWrapped("%s", UI_TEXT(smp_wav_note, "A WAV file can be imported instead: the channel picked under \"Record from\" is taken and converted to 44.1 kHz."));
	ImGui::BeginDisabled(busy);
	if (xgui::file_dialogs()) {
		if (ImGui::Button(UI_TEXT(smp_wav, "Import WAV...")))
			xgui::ask_open_wav();
		ImGui::SameLine();
		if (ImGui::Button(UI_TEXT(smp_syx_load, "Load SysEx...")))
			xgui::ask_open_wav();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", UI_TEXT(smp_syx_tip, "Loads a .syx of sampling data (Yamaha model 0x68: one saved here with Save all as SysEx, or bulk dumps read from a real MU2000). Waves and sample tables are written straight into memory, so it takes no time. A file that starts by erasing everything replaces the samples and sample voices here."));
	} else {
		ImGui::SetNextItemWidth(-fs * 6);
		ImGui::InputTextWithHint("##path", UI_TEXT(smp_wav_path, "WAV file path"), m_path, sizeof(m_path));
		ImGui::SameLine();
		if (ImGui::Button(UI_TEXT(smp_wav_load, "Import")) && m_path[0]) {
			std::ifstream f(std::filesystem::path(reinterpret_cast<const char8_t *>(m_path)), std::ios::binary);
			std::vector<u8> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			if (bytes.empty())
				m_note = UI_TEXT(smp_wav_fail, "Could not read the WAV file");
			else if (bytes[0] == 0xf0)
				load_sysex(bytes, br);
			else
				import_wav(bytes, br);
		}
	}
	ImGui::EndDisabled();

	if (!m_note.empty()) {
		ImGui::Spacing();
		ImGui::TextWrapped("%s", m_note.c_str());
	}
}

void sampling_editor::import_wav(const std::vector<u8> &bytes, bridge &br)
{
	smu2000::wav_data w;
	std::string err;
	if (!smu2000::parse_wav(bytes, w, err)) {
		m_note = err;
		return;
	}
	auto pcm = std::make_shared<std::vector<s16>>(
		smu2000::wav_for_sampling(w, sp::source(m_source), m_view.free_frames));
	if (pcm->empty()) {
		m_note = UI_TEXT(smp_nothing, "Nothing was recorded");
		return;
	}
	const std::string name = m_name;
	std::string added = UI_TEXT(smp_added_fmt, "Added sample %03d (%.1f s)");
	br.post([pcm, name, added](mu2000 &mu) {
		std::string e;
		const int n = mu.sampling_add(pcm->data(), pcm->size(), name, e);
		if (!n)
			return e;
		char buf[160];
		std::snprintf(buf, sizeof(buf), added.c_str(), n, double(pcm->size()) / sp::SAMPLE_RATE);
		return std::string(buf);
	});
}

// サンプリングの SysEx（機種 0x68）を読み込む。「全部を消す」が入っていて、いまサンプルがあるなら先に確かめる
void sampling_editor::load_sysex(const std::vector<u8> &bytes, bridge &br)
{
	bool any = false, wipes = false;
	for (size_t i = 0; i + 6 < bytes.size(); i++)
		if (bytes[i] == 0xf0 && bytes[i + 1] == 0x43 && bytes[i + 3] == 0x68) {
			any = true;
			wipes = wipes || ((bytes[i + 2] & 0xf0) == 0x10 && bytes[i + 4] == 0 && bytes[i + 5] == 0 && bytes[i + 6] == 0x7f);
		}
	if (!any) {
		m_note = UI_TEXT(smp_syx_none, "No sampling SysEx (model 0x68) in this file");
		return;
	}
	m_syx = std::make_shared<std::vector<u8>>(bytes);
	m_syx_at = -1;
	if (wipes && !m_view.samples.empty())
		m_syx_confirm = true;                   // 窓は samples_pane で出す
	else
		apply_sysex(br);
}

// 読み込みを進める。1 度目: 「全部を消す」があれば、それだけ MIDI で送って firmware に消させ、少し後でもう 1 度呼ぶ。
// 2 度目（か、消す通が無いとき）: 波形と表を直に書く（mu2000::sampling_load_sysex）
void sampling_editor::apply_sysex(bridge &br)
{
	if (!m_syx)
		return;
	std::shared_ptr<std::vector<u8>> bytes = m_syx;
	if (m_syx_at < 0) {
		bool wipes = false;
		for (size_t i = 0; i + 6 < bytes->size() && !wipes; i++)
			wipes = (*bytes)[i] == 0xf0 && (*bytes)[i + 1] == 0x43 && ((*bytes)[i + 2] & 0xf0) == 0x10 && (*bytes)[i + 3] == 0x68 &&
			        (*bytes)[i + 4] == 0 && (*bytes)[i + 5] == 0 && (*bytes)[i + 6] == 0x7f;
		if (wipes) {
			br.send({ 0xf0, 0x43, 0x10, 0x68, 0x00, 0x00, 0x7f, 0x00, 0xf7 });
			m_syx_at = ImGui::GetTime() + sp::INIT_WAIT_MS / 1000.0 + 0.3;
			return;
		}
	}
	m_syx.reset();
	m_syx_at = -1;
	std::string done = UI_TEXT(smp_syx_done_fmt, "Loaded the SysEx (%d messages written directly)");
	br.post([bytes, done](mu2000 &mu) {
		bool wipes = false;
		std::vector<u8> rest;
		const int n = mu.sampling_load_sysex(*bytes, wipes, rest);
		for (u8 b : rest)
			mu.midi_in(b, 0);
		char buf[200];
		std::snprintf(buf, sizeof(buf), done.c_str(), n);
		return std::string(buf);
	});
}

// 外へ送る SysEx の列を、実機が受けきれる速さで少しずつ渡す（どのタブを開いていても進むように draw から呼ぶ）。
// 速さは shingo45endo さんの M2A to SMF Converter の既定と同じ 1 秒に 2800 バイト
void sampling_editor::pump_sysex(bridge &br)
{
	constexpr double BYTES_PER_S = 2800.0;
	const double now = ImGui::GetTime();
	if (m_syx && m_syx_at >= 0 && now >= m_syx_at)
		apply_sysex(br);
	if (m_sx_job && m_sx_job->done.load(std::memory_order_acquire)) {
		std::shared_ptr<sx_job> job = std::move(m_sx_job);
		if (job->to_file) {
			std::vector<u8> bytes;
			for (const auto &m : job->msgs)
				bytes.insert(bytes.end(), m.begin(), m.end());
			if (!bytes.empty())
				xgui::ask_save_file(std::move(bytes));
		} else {
			m_sx_queue.assign(job->msgs.begin(), job->msgs.end());
			m_sx_total = m_sx_queue.size();
			m_sx_wipe_wait = job->wipes;
			m_sx_next = now;
		}
	}
	while (!m_sx_queue.empty() && now >= m_sx_next) {
		const size_t n = m_sx_queue.front().size();
		if (!xgui::out_send(br, m_sx_queue.front()))
			break;
		m_sx_queue.pop_front();
		// コマが遅れても、取り戻すのは 0.1 秒ぶんまで
		m_sx_next = std::max(m_sx_next, now - 0.1) + double(n) / BYTES_PER_S;
		if (m_sx_wipe_wait) {
			m_sx_next = now + sp::INIT_WAIT_MS / 1000.0;
			m_sx_wipe_wait = false;
		}
	}
}

void sampling_editor::samples_pane(bridge &br)
{
	heading(UI_TEXT(smp_samples, "Samples"));
	if (m_view.samples.empty()) {
		ImGui::TextDisabled("%s", UI_TEXT(smp_none, "No samples yet"));
		m_selected = 0;
		return;
	}
	// 選んだものが消えていたら、最後に足したものを選ぶ（録った直後にすぐ見られるように）
	bool found = false;
	for (const sp::sample &s : m_view.samples)
		found = found || s.number == m_selected;
	if (!found)
		m_selected = m_view.samples.back().number;
	const float fs = ImGui::GetFontSize();
	const float foot = ImGui::GetFrameHeightWithSpacing() * (m_sx_queue.empty() ? 2.0f : 3.0f);
	if (ImGui::BeginTable("samples", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
	                      ImVec2(0, -foot))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn(UI_TEXT(smp_col_no, "No."), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn(UI_TEXT(smp_name, "Name"));
		ImGui::TableSetupColumn(UI_TEXT(smp_col_len, "Length"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn(UI_TEXT(smp_col_peak, "Peak"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableHeadersRow();
		for (const sp::sample &s : m_view.samples) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			char id[16];
			std::snprintf(id, sizeof(id), "%03d", s.number);
			if (ImGui::Selectable(id, s.number == m_selected, ImGuiSelectableFlags_SpanAllColumns))
				m_selected = s.number;
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(s.name.c_str());
			ImGui::TableNextColumn();
			ImGui::Text("%.2f s", double(s.frames()) / double(s.rate ? s.rate : sp::SAMPLE_RATE));
			ImGui::TableNextColumn();
			if (s.peak == 0)
				ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", UI_TEXT(smp_silent, "silent"));
			else if (s.peak > 0)
				ImGui::Text("%.1f dB", double(db(s.peak)));
		}
		ImGui::EndTable();
	}

	// 実機へ: サンプリングの中身まるごと（波形・サンプル・サンプルを鳴らす音色）を SysEx にする（sp::memory_sysex）。
	// 波形 64 バイトが 85 バイトの 1 通になるので、1 秒ぶんの波形（88KB）を送るのに 42 秒かかる
	u32 words = 0;
	for (const sp::sample &s : m_view.samples)
		words = std::max(words, s.end);
	const int secs = int(double(words) * 4.0 / 64.0 * 85.0 / 2800.0) + 2;
	auto make_sysex = [&](bool to_file) {
		auto job = std::make_shared<sx_job>();
		job->to_file = to_file;
		job->wipes = true;
		m_sx_job = job;
		br.post([job](mu2000 &mu) {
			job->msgs = sp::memory_sysex(mu.dram(), mu.sample_ram(), true);
			job->done.store(true, std::memory_order_release);
			return std::string();
		});
	};
	const char *tip = UI_TEXT(smp_mem_tip, "Turns everything here (waves, samples and the voices that play them) into SysEx (Yamaha model 0x68 bulk dumps) that a real MU2000 loads into its sampling memory. The first message erases the samples and sample voices on the receiving unit.");
	ImGui::BeginDisabled(m_sx_job != nullptr || !m_sx_queue.empty());
	if (ImGui::Button(UI_TEXT(smp_mem_save, "Save all as SysEx..."), ImVec2(-1, 0)))
		make_sysex(true);
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", tip);
	ImGui::BeginDisabled(!xgui::out_ready());
	if (ImGui::Button(UI_TEXT(smp_mem_send, "Send all to MIDI out"), ImVec2(-1, 0)))
		m_mem_confirm = true;
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", tip);
	ImGui::EndDisabled();
	if (!m_sx_queue.empty()) {
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled(UI_TEXT(smp_sx_sending_fmt, "Sending %d / %d"), int(m_sx_total - m_sx_queue.size()), int(m_sx_total));
		ImGui::SameLine();
		if (ImGui::SmallButton(UI_TEXT(smp_sx_stop, "Stop")))
			m_sx_queue.clear();
	}
	if (m_mem_confirm) {
		ImGui::OpenPopup("###mem_confirm");
		m_mem_confirm = false;
	}
	const std::string title = std::string(UI_TEXT(smp_mem_send, "Send all to MIDI out")) + "###mem_confirm";
	if (ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextUnformatted(UI_TEXT(smp_mem_warn, "This erases the samples and sample voices on the receiving MU2000 and replaces them with the ones here. Go on?"));
		ImGui::Text(UI_TEXT(smp_mem_time_fmt, "It takes about %d min %02d s. Keep this window open until it ends."), secs / 60, secs % 60);
		if (ImGui::Button("OK", ImVec2(fs * 6, 0))) {
			make_sysex(false);
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(UI_TEXT(dlg_cancel, "Cancel"), ImVec2(fs * 6, 0)))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	// 読み込む SysEx が、いまのサンプルを消すとき
	if (m_syx_confirm) {
		ImGui::OpenPopup("###syx_confirm");
		m_syx_confirm = false;
	}
	const std::string syx_title = std::string(UI_TEXT(smp_syx_load, "Load SysEx...")) + "###syx_confirm";
	if (ImGui::BeginPopupModal(syx_title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextUnformatted(UI_TEXT(smp_syx_warn, "This file erases the samples and sample voices here and replaces them with its own. Go on?"));
		if (ImGui::Button("OK", ImVec2(fs * 6, 0))) {
			apply_sysex(br);
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(UI_TEXT(dlg_cancel, "Cancel"), ImVec2(fs * 6, 0))) {
			m_syx.reset();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

void sampling_editor::wave_pane(bridge &br)
{
	const sp::sample *sel = nullptr;
	for (const sp::sample &s : m_view.samples)
		if (s.number == m_selected)
			sel = &s;
	if (!sel) {
		heading(UI_TEXT(smp_wave, "Waveform"));
		ImGui::TextDisabled("%s", UI_TEXT(smp_wave_pick, "Pick a sample in the list"));
		br.request_overview(0);
		for (int i = 0; i < bridge::DETAIL_SLOTS; i++)
			br.request_detail(i, 0, 0, 0);
		return;
	}
	const int num = sel->number;
	const u32 frames = sel->frames();
	const double rate = double(sp::SAMPLE_RATE);
	char title[64];
	std::snprintf(title, sizeof(title), "%s  %03d %s", UI_TEXT(smp_wave, "Waveform"), num, sel->name.c_str());
	heading(title);
	const float fs = ImGui::GetFontSize();
	const bool busy = m_view.rec_state != 0;

	// 選んだサンプルが替わったら（トリムで長さが変わったときも）、全体を表示し、鳴らす所を表から読む
	if (m_trim_for != num || m_trim_frames != frames) {
		m_trim_for = num;
		m_trim_frames = frames;
		m_start = sel->play_from;
		m_end = sel->play_to ? sel->play_to : frames;
		m_view0 = 0.0;
		m_view1 = double(frames);
		m_drag = 0;
		m_loop_on = sel->loop;
		m_loop_at = sel->loop_from;
		for (u32 &l : m_det_last)
			l = ~0u;   // 拡大の枠も点へ合わせ直す
	}
	// 鳴り始め S・鳴り終わり E・ループの頭 L をそろえる（src/sampling.cpp の fit_points と同じ）。
	// moved は今動かしたもの（1 = S、2 = E、4 = L）。L は S と E の 8 手前のあいだで、S や E に押されて動く
	auto fit = [&](int moved) {
		m_end = std::clamp(m_end, std::min(frames, 8u), frames);
		m_start = std::min(m_start, m_end >= 8 ? m_end - 8 : 0);
		const u32 lo = (m_start + 1) & ~1u, hi = m_end >= 8 ? (m_end - 8) & ~1u : 0;
		m_loop_at = std::clamp(m_loop_at & ~1u, std::min(lo, hi), hi);
		if (moved == 4 && m_loop_at < m_start)
			m_start = m_loop_at;
	};
	// 音源の側でそろえた S・E・L が届いたら窓へ（吸い付けや E を合わせるで動いた分）
	if (m_points && m_points->done.load(std::memory_order_acquire)) {
		if (m_points->number == num && !m_drag && m_det_drag < 0) {
			m_start = m_points->from;
			m_end = m_points->to;
			m_loop_at = m_points->loop_from;
		}
		m_points.reset();
	}
	// 鳴らす所は変えたらすぐ表へ（波形は切らない）。moved は今動かしたもの（1 = S、2 = E、4 = L、8 = E を合わせる）。
	// 吸い付けが入っていれば、動かしたものを近くのゼロクロスへ寄せてから書く
	auto post_points = [&](int moved = 0) {
		const u32 f0 = m_start, f1 = m_end, at = m_loop_at;
		const bool on = m_loop_on, snap = m_snap;
		auto res = std::make_shared<points_result>();
		res->number = num;
		m_points = res;
		std::string matched = UI_TEXT(smp_match_done_fmt, "End moved to %u (%+d)");
		br.post([num, f0, f1, on, at, snap, moved, res, matched](mu2000 &mu) {
			u32 s = f0, e = f1, l = at, o = 0;
			const u32 range = sp::SAMPLE_RATE / 100;   // 10ms 以内
			std::string msg;
			if (moved == 8 && mu.sampling_match_end(num, l, e, sp::SAMPLE_RATE / 20, o)) {
				char buf[80];
				std::snprintf(buf, sizeof(buf), matched.c_str(), o, int(o) - int(e));
				msg = buf;
				e = o;
			}
			if (snap && moved == 1 && mu.sampling_snap(num, s, false, range, o))
				s = o;
			if (snap && moved == 2 && mu.sampling_snap(num, e, false, range, o))
				e = o;
			if (snap && moved == 4 && mu.sampling_snap(num, l, true, range, o))
				l = o;
			mu.sampling_points(num, s, e, on, l);
			for (const sp::sample &x : mu.sampling_list())
				if (x.number == num) {
					res->from = x.play_from;
					res->to = x.play_to;
					res->loop_from = x.loop_from;
				}
			res->done.store(true, std::memory_order_release);
			return msg;
		});
	};
	// 表示の範囲。いちばん細かくて 32 サンプル
	const double min_span = std::min(32.0, double(frames));
	auto clamp_view = [&]() {
		double span = std::clamp(m_view1 - m_view0, min_span, double(frames));
		m_view0 = std::clamp(m_view0, 0.0, double(frames) - span);
		m_view1 = m_view0 + span;
	};
	auto zoom_at = [&](double center, double factor) {
		const double span = std::clamp((m_view1 - m_view0) * factor, min_span, double(frames));
		const double t = (center - m_view0) / (m_view1 - m_view0);
		m_view0 = center - span * t;
		m_view1 = m_view0 + span;
		clamp_view();
	};
	clamp_view();

	// ---- 音量。ノーマライズは最大を -0.5 dB に。書き換えなので元に戻せない
	const int peak = sel->peak;
	std::string done = UI_TEXT(smp_gain_done_fmt, "Sample %03d: peak now %.1f dB");
	auto post_gain = [&](double gain) {
		br.post([num, gain, done](mu2000 &mu) {
			const int p = mu.sampling_gain(num, gain);
			if (p < 0)
				return std::string("no such sample");
			char buf[120];
			std::snprintf(buf, sizeof(buf), done.c_str(), num, p > 0 ? 20.0 * std::log10(p / 32768.0) : -90.0);
			return std::string(buf);
		});
	};
	ImGui::BeginDisabled(busy || peak <= 0);
	if (ImGui::Button(UI_TEXT(smp_normalize, "Normalize")))
		post_gain(32767.0 * std::pow(10.0, -0.5 / 20.0) / double(peak));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 7);
	ImGui::InputFloat("##gain", &m_gain_db, 1.0f, 6.0f, "%+.1f dB");
	m_gain_db = std::clamp(m_gain_db, -40.0f, 40.0f);
	ImGui::SameLine();
	if (ImGui::Button(UI_TEXT(smp_gain_apply, "Change volume")))
		post_gain(std::pow(10.0, double(m_gain_db) / 20.0));
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", UI_TEXT(smp_gain_note, "Rewrites the sample in place. Turning it down and up again loses detail, and turning it up past full scale clips."));

	// ---- 鳴り始めと鳴り終わり（サンプル単位で打ち込める。Shift を押しながら +/- で 10ms）
	ImGui::BeginDisabled(busy);
	const int step_fast = int(sp::SAMPLE_RATE / 100);
	int st = int(m_start), en = int(m_end);
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_trim_start, "Start"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 7);
	if (ImGui::InputInt("##tstart", &st, 1, step_fast)) {
		m_start = u32(std::max(st, 0));
		fit(1);
		post_points();
	}
	ImGui::SameLine();
	ImGui::Text("%.3f s", double(m_start) / rate);
	ImGui::SameLine(0, fs * 1.2f);
	ImGui::TextUnformatted(UI_TEXT(smp_trim_end, "End"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 7);
	if (ImGui::InputInt("##tend", &en, 1, step_fast)) {
		m_end = u32(std::max(en, 0));
		fit(2);
		post_points();
	}
	ImGui::SameLine();
	ImGui::Text("%.3f s (%.3f s)", double(m_end) / rate, double(m_end - m_start) / rate);

	// 前後の無音を除いて選ぶ（全体を音源の側で調べる。結果は次のコマ以降に届く）
	if (m_auto && m_auto->load() != ~u64(0)) {
		const u64 r = m_auto->load();
		if (r != ~u64(1)) {
			m_start = u32(r >> 32);
			m_end = u32(r);
			fit(1);
			post_points();
		}
		m_auto.reset();
	}
	if (ImGui::Button(UI_TEXT(smp_trim_auto, "Select without silence")) && !m_auto) {
		auto res = std::make_shared<std::atomic<u64>>(~u64(0));
		m_auto = res;
		br.post([num, res](mu2000 &mu) {
			u32 a = 0, b = 0;
			res->store(mu.sampling_bounds(num, 0.01, a, b) ? (u64(a) << 32 | b) : ~u64(1));
			return std::string();
		});
	}
	ImGui::SameLine();
	if (ImGui::Button(UI_TEXT(smp_trim_clear, "Select all"))) {
		m_start = 0;
		m_end = frames;
		fit(1);
		post_points();
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(m_start == 0 && m_end == frames);
	const bool trim = ImGui::Button(UI_TEXT(smp_trim, "Trim"));
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", UI_TEXT(smp_trim_note, "Cuts away the sound before the start and after the end to free sampling memory. Start and end already work without trimming."));
	if (trim) {
		const u32 f0 = m_start, f1 = m_end;
		std::string trimmed = UI_TEXT(smp_trimmed_fmt, "Sample %03d trimmed to %.2f s");
		br.post([num, f0, f1, trimmed](mu2000 &mu) {
			std::string e;
			if (!mu.sampling_trim(num, f0, f1, e))
				return e;
			char buf[120];
			std::snprintf(buf, sizeof(buf), trimmed.c_str(), num, double(f1 - f0) / sp::SAMPLE_RATE);
			return std::string(buf);
		});
	}
	ImGui::EndDisabled();

	// ---- ループ。鍵盤を押しているあいだ、ループの頭から鳴り終わり（E）までをくり返す（変えたらすぐ表へ）
	if (ImGui::Checkbox(UI_TEXT(smp_loop, "Loop"), &m_loop_on))
		post_points();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(smp_loop_tip, "While a key is held, repeat from the loop point to the end point. Off plays from the start to the end once."));
	ImGui::SameLine();
	ImGui::BeginDisabled(!m_loop_on);
	ImGui::TextUnformatted(UI_TEXT(smp_loop_at, "Loop point"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 7);
	int la = int(m_loop_at);
	if (ImGui::InputInt("##loopat", &la, 2, step_fast)) {
		m_loop_at = u32(std::max(la, 0));
		fit(4);
		post_points();
	}
	ImGui::SameLine();
	ImGui::Text("%.3f s", double(m_loop_at) / rate);
	ImGui::EndDisabled();

	// ---- つなぎ目の道具
	ImGui::Checkbox(UI_TEXT(smp_snap, "Snap to zero crossings"), &m_snap);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(smp_snap_tip, "When a line is placed with the mouse, move it to the nearest point (within 10 ms) where the wave crosses zero going up. Typed numbers are kept as they are."));
	ImGui::SameLine();
	ImGui::BeginDisabled(!m_loop_on);
	if (ImGui::Button(UI_TEXT(smp_match, "Match end to loop")))
		post_points(8);
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", UI_TEXT(smp_match_tip, "Moves the end point (within 50 ms) to where the wave looks most like the wave around the loop point, so the jump back is smooth. Works best on a steady, pitched part."));
	ImGui::EndDisabled();

	// ループ区間を探す（始点から終点のあいだで、長さが決めた ms 以上の組）。重いので波形を写して別の糸で
	if (m_find && m_find->stage.load(std::memory_order_acquire) == 1 && !m_find_thread.joinable()) {
		auto f = m_find;
		m_find_thread = std::thread([f]() {
			f->ok = sp::find_loop(f->pcm, f->from, f->to, f->min_len, f->loop_from, f->loop_to);
			f->stage.store(2, std::memory_order_release);
		});
	}
	if (m_find && m_find->stage.load(std::memory_order_acquire) == 2) {
		if (m_find_thread.joinable())
			m_find_thread.join();
		if (m_find->number == num && m_find->ok) {
			m_loop_on = true;
			m_loop_at = m_find->loop_from;
			m_end = m_find->loop_to;
			fit(4);
			post_points(0);
			char buf[160];
			std::snprintf(buf, sizeof(buf), UI_TEXT(smp_find_done_fmt, "Loop found: %.3f - %.3f s (%.3f s)"),
			              double(m_loop_at) / rate, double(m_end) / rate, double(m_end - m_loop_at) / rate);
			m_note = buf;
		} else if (m_find->number == num) {
			m_note = UI_TEXT(smp_find_fail, "No loop found: make the range between start and end longer, or the minimum length shorter");
		}
		m_find.reset();
	}
	const bool finding = m_find != nullptr;
	ImGui::BeginDisabled(finding);
	if (ImGui::Button(finding ? UI_TEXT(smp_finding, "Searching...") : UI_TEXT(smp_find, "Find loop"))) {
		auto f = std::make_shared<find_job>();
		f->number = num;
		f->from = m_start;
		f->to = m_end;
		f->min_len = u32(m_find_ms) * sp::SAMPLE_RATE / 1000;
		m_find = f;
		br.post([num, f](mu2000 &mu) {
			mu.sampling_pcm(num, f->pcm);
			f->stage.store(1, std::memory_order_release);
			return std::string();
		});
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", UI_TEXT(smp_find_tip, "Searches between the start and the end for the loop point and end whose waves match best, at least this long. Sets the loop point and the end and turns the loop on."));
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::TextUnformatted(UI_TEXT(smp_find_min, "at least"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 6);
	ImGui::InputInt("##findms", &m_find_ms, 50, 200);
	m_find_ms = std::clamp(m_find_ms, 10, 5000);
	ImGui::SameLine(0, 2);
	ImGui::TextUnformatted("ms");

	ImGui::SameLine(0, fs * 1.2f);
	ImGui::BeginDisabled(!m_loop_on);
	ImGui::SetNextItemWidth(fs * 6);
	ImGui::InputInt("##xfade", &m_xfade_ms, 10, 50);
	m_xfade_ms = std::clamp(m_xfade_ms, 1, 1000);
	ImGui::SameLine(0, 2);
	ImGui::TextUnformatted("ms");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 9);
	const char *curves[2] = { UI_TEXT(smp_xfade_gain, "Matching waves"), UI_TEXT(smp_xfade_power, "Wavering sound") };
	if (ImGui::BeginCombo("##xcurve", curves[m_xfade_power ? 1 : 0])) {
		for (int i = 0; i < 2; i++)
			if (ImGui::Selectable(curves[i], m_xfade_power == (i == 1)))
				m_xfade_power = i == 1;
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(smp_xfade_curve_tip, "Matching waves: for a seam whose waves already line up (the level stays even). Wavering sound: for sound whose pitch or tone drifts, with a long crossfade (the sound does not thin out in the middle)."));
	ImGui::SameLine();
	if (ImGui::Button(UI_TEXT(smp_xfade, "Crossfade"))) {
		const u32 l = m_loop_at, e = m_end, len = u32(m_xfade_ms) * sp::SAMPLE_RATE / 1000;
		const bool power = m_xfade_power;
		std::string done_x = UI_TEXT(smp_xfade_done_fmt, "Sample %03d: crossfaded %.0f ms before the end");
		std::string fail_x = UI_TEXT(smp_xfade_fail, "Nothing to crossfade: the loop point needs sound before it");
		br.post([num, l, e, len, power, done_x, fail_x](mu2000 &mu) {
			const u32 use = std::min({ len, l, e > l ? e - l : 0u });
			if (!mu.sampling_crossfade(num, l, e, len, power))
				return fail_x;
			char buf[120];
			std::snprintf(buf, sizeof(buf), done_x.c_str(), num, double(use) * 1000.0 / sp::SAMPLE_RATE);
			return std::string(buf);
		});
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", UI_TEXT(smp_xfade_tip, "Blends the sound just before the end into the sound just before the loop point, so the loop has no jump in level or tone. This rewrites the sample and cannot be undone. Needs that much sound before the loop point."));
	ImGui::EndDisabled();
	ImGui::EndDisabled();

	// ---- 試聴。始点から終点まで（音源を通さない生の音）。ループが入っていれば止めるまでくり返す
	const bool playing = m_view.preview_number == num;
	if (!playing) {
		if (ImGui::Button(UI_TEXT(smp_play, "Play"))) {
			const u32 f0 = m_start, f1 = m_end;
			const u32 lp = m_loop_on ? m_loop_at : ~0u;
			br.post([num, f0, f1, lp](mu2000 &mu) {
				mu.preview_start(num, f0, f1, lp);
				return std::string();
			});
		}
	} else if (ImGui::Button(UI_TEXT(smp_play_stop, "Stop playing"))) {
		br.post([](mu2000 &mu) {
			mu.preview_stop();
			return std::string();
		});
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(smp_play_tip, "Plays from the start to the end point as recorded, without the voice's level, pan or pitch"));
	ImGui::SameLine(0, fs * 1.2f);

	// ---- 表示の拡大・縮小
	ImGui::TextUnformatted(UI_TEXT(smp_zoom, "Zoom"));
	ImGui::SameLine();
	const double center = (m_view0 + m_view1) * 0.5;
	if (ImGui::SmallButton("-"))
		zoom_at(center, 2.0);
	ImGui::SameLine();
	if (ImGui::SmallButton("+"))
		zoom_at(center, 0.5);
	ImGui::SameLine();
	if (ImGui::SmallButton(UI_TEXT(smp_zoom_all, "All"))) {
		m_view0 = 0.0;
		m_view1 = double(frames);
	}
	ImGui::SameLine();
	if (ImGui::SmallButton(UI_TEXT(smp_zoom_sel, "Selection"))) {
		const double pad = std::max(8.0, double(m_end - m_start) * 0.05);
		m_view0 = double(m_start) - pad;
		m_view1 = double(m_end) + pad;
		clamp_view();
	}
	ImGui::SameLine();
	ImGui::TextDisabled("%.3f - %.3f s  (x%.0f)", m_view0 / rate, m_view1 / rate, double(frames) / (m_view1 - m_view0));

	// ---- 全体の波形（下に始点・終点・ループの頭の拡大を 3 つ並べる）
	const float bar_h = fs * 0.75f + ImGui::GetStyle().ItemSpacing.y;
	const float det_h = std::max(fs * 7, ImGui::GetContentRegionAvail().y * 0.48f);
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const ImVec2 sz(ImGui::GetContentRegionAvail().x, std::max(ImGui::GetContentRegionAvail().y - bar_h - det_h, fs * 3));
	ImGui::InvisibleButton("##wave", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
	                                     ImGuiButtonFlags_MouseButtonMiddle);
	const double span = m_view1 - m_view0;
	auto x_of = [&](double f) { return p.x + float((f - m_view0) / span * double(sz.x)); };
	auto f_of = [&](float x) { return m_view0 + double(x - p.x) / double(sz.x) * span; };
	ImGuiIO &io = ImGui::GetIO();
	if (!busy) {
		if (ImGui::IsItemHovered()) {
			// ホイールでマウスの所を中心に拡大・縮小
			if (io.MouseWheel != 0.0f)
				zoom_at(f_of(io.MousePos.x), io.MouseWheel > 0 ? 0.8 : 1.25);
			ImGui::SetTooltip("%s", UI_TEXT(smp_trim_tip, "Left-click sets the start, right-click the end. Shift-click sets the loop point. Drag a line with either button to move it. Wheel zooms, middle-drag scrolls."));
		}
		// 線（つまみ）の近くを押したら、どのボタンでもその線を動かす。それ以外は左で始点、右で終点をそこへ。
		// 中ボタンで表示を動かす
		if (ImGui::IsItemActivated()) {
			const float xs = x_of(double(m_start)), xe = x_of(double(m_end)), xl = x_of(double(m_loop_at));
			const float mx = io.MousePos.x, grab = fs * 0.5f;
			const float ds = std::fabs(mx - xs), de = std::fabs(mx - xe), dlp = m_loop_on ? std::fabs(mx - xl) : 1e9f;
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
				m_drag = 3;
			else if (io.KeyShift)
				m_drag = 4;   // Shift を押しながらならどのボタンでもループの頭（ループも入れる）
			else if (dlp <= grab && dlp < ds && dlp < de)
				m_drag = 4;
			else if (ds <= grab || de <= grab)
				m_drag = ds <= de ? 1 : 2;
			else
				m_drag = ImGui::IsMouseClicked(ImGuiMouseButton_Right) ? 2 : 1;
		}
		if (m_drag && ImGui::IsItemActive()) {
			if (m_drag == 3) {
				const double df = -double(io.MouseDelta.x) / double(sz.x) * span;
				m_view0 += df;
				m_view1 += df;
				clamp_view();
			} else {
				const long f = std::lround(std::clamp(f_of(io.MousePos.x), 0.0, double(frames)));
				if (m_drag == 1)
					m_start = u32(std::clamp(f, 0L, long(m_end) - 8));
				else if (m_drag == 2)
					m_end = u32(std::clamp(f, long(m_start) + 8, long(frames)));
				else {
					m_loop_at = u32(f);
					m_loop_on = true;
				}
				fit(m_drag);
			}
		}
		// S・E・L は放したときに表へ
		// 吸い付けはマウスで置いたときだけ（数の +/- は 1 サンプルずつ動かせるように）
		if (ImGui::IsItemDeactivated() && (m_drag == 1 || m_drag == 2 || m_drag == 4))
			post_points(m_drag);
		if (!ImGui::IsItemActive())
			m_drag = 0;
	}
	const u32 v0 = u32(std::floor(m_view0)), v1 = u32(std::min(double(frames), std::ceil(m_view1)));
	br.request_overview(num, v0, v1);

	ImDrawList *dl = ImGui::GetWindowDrawList();
	wave_frame(dl, p, sz);
	dl->PushClipRect(p, ImVec2(p.x + sz.x, p.y + sz.y), true);
	const float mid = p.y + sz.y * 0.5f, half = sz.y * 0.5f - 2.0f;
	dl->AddLine(ImVec2(p.x, mid), ImVec2(p.x + sz.x, mid), IM_COL32(80, 90, 110, 255));
	auto y_of = [&](int v) { return mid - half * float(v) / 32768.0f; };
	const int nb = int(m_view.wave_hi.size());
	if (m_view.wave_number == num && nb > 0) {
		// 届いた見取り図の範囲（拡大の途中は前の範囲のこともある）で、区切りごとの位置を出す
		const double d0 = double(m_view.wave_from), d1 = double(m_view.wave_to ? m_view.wave_to : frames);
		const double per = (d1 - d0) / double(nb);
		const int cols = std::max(1, int(sz.x));
		if (double(nb) >= double(cols) * (d1 - d0) / span * 0.999) {
			// 1 列に区切りが 1 つ以上: 列ごとに最小と最大の縦線
			for (int x = 0; x < cols; x++) {
				const double fa = f_of(p.x + float(x)), fb = f_of(p.x + float(x + 1));
				int b0 = int(std::floor((fa - d0) / per)), b1 = int(std::ceil((fb - d0) / per));
				b0 = std::max(b0, 0);
				b1 = std::min(b1, nb);
				if (b1 <= b0)
					continue;
				int lo = 32767, hi = -32768;
				for (int b = b0; b < b1; b++) {
					lo = std::min(lo, int(m_view.wave_lo[size_t(b)]));
					hi = std::max(hi, int(m_view.wave_hi[size_t(b)]));
				}
				const bool clip = hi >= 32767 || lo <= -32768;
				dl->AddLine(ImVec2(p.x + float(x) + 0.5f, y_of(hi)), ImVec2(p.x + float(x) + 0.5f, std::max(y_of(lo), y_of(hi) + 1.0f)),
				            clip ? IM_COL32(235, 80, 70, 255) : IM_COL32(110, 200, 255, 255));
			}
		} else {
			// 拡大して 1 サンプルが何列にもなる: 点を線でつなぎ、点も打つ
			ImVec2 prev;
			for (int b = 0; b < nb; b++) {
				const double f = d0 + (double(b) + 0.5) * per;
				const ImVec2 pt(x_of(f), y_of(int(m_view.wave_hi[size_t(b)])));
				if (b)
					dl->AddLine(prev, pt, IM_COL32(110, 200, 255, 255), 1.5f);
				if (sz.x / float(span) > 6.0f)
					dl->AddCircleFilled(pt, 2.0f, IM_COL32(170, 225, 255, 255));
				prev = pt;
			}
		}
	}
	// 鳴らさない所を暗く。鳴り始めは緑、鳴り終わりは黄の線
	const float xs = x_of(double(m_start)), xe = x_of(double(m_end));
	if (m_start > 0 || m_end < frames) {
		dl->AddRectFilled(p, ImVec2(std::max(p.x, xs), p.y + sz.y), IM_COL32(0, 0, 0, 150));
		dl->AddRectFilled(ImVec2(std::min(p.x + sz.x, xe), p.y), ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(0, 0, 0, 150));
	}
	dl->AddLine(ImVec2(xs, p.y), ImVec2(xs, p.y + sz.y), IM_COL32(110, 230, 120, 255), m_drag == 1 ? 3.0f : 2.0f);
	dl->AddLine(ImVec2(xe, p.y), ImVec2(xe, p.y + sz.y), IM_COL32(255, 210, 90, 255), m_drag == 2 ? 3.0f : 2.0f);
	dl->AddText(ImVec2(xs + 3, p.y + 2), IM_COL32(110, 230, 120, 255), "S");
	dl->AddText(ImVec2(xe - fs * 0.8f, p.y + 2), IM_COL32(255, 210, 90, 255), "E");
	// ループするところ（ループの頭から鳴り終わりまで）を薄く塗り、頭に紫の線
	if (m_loop_on) {
		const float xl = x_of(double(m_loop_at)), xf = xe;
		dl->AddRectFilled(ImVec2(xl, p.y + sz.y - fs * 0.5f), ImVec2(xf, p.y + sz.y), IM_COL32(200, 120, 255, 120));
		dl->AddLine(ImVec2(xl, p.y), ImVec2(xl, p.y + sz.y), IM_COL32(200, 120, 255, 255), m_drag == 4 ? 3.0f : 2.0f);
		dl->AddText(ImVec2(xl + 3, p.y + sz.y - fs * 1.6f), IM_COL32(200, 120, 255, 255), "L");
	}
	if (playing) {
		const float xp = x_of(double(m_view.preview_pos));
		dl->AddLine(ImVec2(xp, p.y), ImVec2(xp, p.y + sz.y), IM_COL32(255, 255, 255, 230), 1.5f);
	}
	dl->PopClipRect();
	wave_border(dl, p, sz, IM_COL32(110, 125, 150, 255));

	// 表示の位置を動かす棒（全体を表示しているときはつまみが端から端まで）
	if (hscroll("##oscroll", m_view0, span, double(frames), sz.x)) {
		m_view1 = m_view0 + span;
		clamp_view();
	}

	// ---- 始点・終点・ループの頭の拡大。それぞれ左クリック（ドラッグ）でその点を置く。ホイールで拡大・縮小。
	// 終点の枠には戻った後に鳴る L の後ろを、ループの頭の枠には戻る前に鳴る E の手前を薄く重ねる（つなぎ目の形）
	ImGui::Dummy(ImVec2(0, fs * 0.3f));
	const float gap = fs * 0.8f;
	const float dw = (ImGui::GetContentRegionAvail().x - gap * 2) / 3.0f;
	auto clamp_req = [&](double a, double b, u32 &ra, u32 &rb) {
		ra = u32(std::clamp(std::floor(a), 0.0, double(frames)));
		rb = u32(std::clamp(std::ceil(b), 0.0, double(frames)));
	};
	for (int w = 0; w < 3; w++) {
		if (w)
			ImGui::SameLine(0, gap);
		ImGui::BeginGroup();
		static const ImU32 cols[3] = { IM_COL32(110, 230, 120, 255), IM_COL32(255, 210, 90, 255), IM_COL32(200, 120, 255, 255) };
		const char *names[3] = { UI_TEXT(smp_det_start, "Start (S)"), UI_TEXT(smp_det_end, "End (E)"), UI_TEXT(smp_det_loop, "Loop point (L)") };
		const u32 at = w == 0 ? m_start : w == 1 ? m_end : m_loop_at;
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(cols[w]), "%s", names[w]);
		ImGui::SameLine();
		if (w == 2 && !m_loop_on)
			ImGui::TextDisabled("%s", UI_TEXT(smp_det_loop_off, "(loop off)"));
		else
			ImGui::TextDisabled("%u  %.4f s", at, double(at) / rate);
		const ImVec2 q = ImGui::GetCursorScreenPos();
		const ImVec2 qs(dw, std::max(ImGui::GetContentRegionAvail().y - bar_h, fs * 3));
		char id[16];
		std::snprintf(id, sizeof(id), "##det%d", w);
		ImGui::InvisibleButton(id, qs);
		// 点が動いたら（ほかの枠や数で）表示の中心をそこへ。スクロールバーで離れて見られる。
		// つまんでいるあいだは動かさない（動かすと押した所がずれる）
		double &dspan = m_det_span[w];
		dspan = std::clamp(dspan, 16.0, std::max(16.0, double(frames)));
		if (m_det_drag != w && at != m_det_last[w])
			m_det_center[w] = double(at);
		m_det_last[w] = at;
		const double c = m_det_center[w], d0 = c - dspan * 0.5, d1 = c + dspan * 0.5;
		auto dx_of = [&](double f) { return q.x + float((f - d0) / dspan * double(qs.x)); };
		auto df_of = [&](float x) { return d0 + double(x - q.x) / double(qs.x) * dspan; };
		if (!busy) {
			if (ImGui::IsItemHovered()) {
				if (io.MouseWheel != 0.0f)
					dspan *= io.MouseWheel > 0 ? 0.8 : 1.25;
				ImGui::SetTooltip("%s", UI_TEXT(smp_det_tip, "Left-click or drag to place this point. Wheel zooms. With the loop on, the faint wave shows what plays on the other side of the jump."));
			}
			if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				m_det_drag = w;
			if (m_det_drag == w && ImGui::IsItemActive()) {
				const u32 f = u32(std::lround(std::clamp(df_of(io.MousePos.x), 0.0, double(frames))));
				if (w == 0)
					m_start = std::min(f, m_end >= 8 ? m_end - 8 : 0);
				else if (w == 1)
					m_end = std::max(f, m_start + 8);
				else {
					m_loop_at = f;
					m_loop_on = true;
				}
				fit(w == 0 ? 1 : w == 1 ? 2 : 4);
			}
			if (ImGui::IsItemDeactivated() && m_det_drag == w) {
				post_points(w == 0 ? 1 : w == 1 ? 2 : 4);
				m_det_drag = -1;
			}
		}
		// 波形を頼む: 自分の範囲と、つなぎ目の向こう側
		u32 ra, rb;
		clamp_req(d0, d1, ra, rb);
		br.request_detail(w, num, ra, rb);
		const bool ghost = m_loop_on && w > 0;
		double shift = 0;
		if (ghost && w == 1) {
			// E より後ろに、戻った後に鳴る L から先を置く
			shift = double(m_end) - double(m_loop_at);
			clamp_req(double(m_loop_at), d1 - shift, ra, rb);
		} else if (ghost) {
			// L より前に、戻る前に鳴る E の手前を置く
			shift = double(m_loop_at) - double(m_end);
			clamp_req(d0 - shift, double(m_end), ra, rb);
		}
		br.request_detail(3 + w, ghost ? num : 0, ra, rb);

		ImDrawList *ddl = ImGui::GetWindowDrawList();
		wave_frame(ddl, q, qs);
		ddl->PushClipRect(q, ImVec2(q.x + qs.x, q.y + qs.y), true);
		const float qmid = q.y + qs.y * 0.5f;
		ddl->AddLine(ImVec2(q.x, qmid), ImVec2(q.x + qs.x, qmid), IM_COL32(80, 90, 110, 255));
		const auto &main = m_view.details[size_t(w)];
		const auto &gh = m_view.details[size_t(3 + w)];
		if (main.number == num)
			draw_slice(ddl, q, qs, d0, d1, main, 0.0, IM_COL32(110, 200, 255, 255));
		// 鳴らさない側を暗く（始点の前・終点の後ろ）
		const float xa = dx_of(double(at));
		if (w == 0)
			ddl->AddRectFilled(q, ImVec2(std::max(q.x, xa), q.y + qs.y), IM_COL32(0, 0, 0, 150));
		else if (w == 1)
			ddl->AddRectFilled(ImVec2(std::min(q.x + qs.x, xa), q.y), ImVec2(q.x + qs.x, q.y + qs.y), IM_COL32(0, 0, 0, 150));
		if (ghost && gh.number == num)
			draw_slice(ddl, q, qs, d0, d1, gh, shift, w == 1 ? IM_COL32(200, 120, 255, 200) : IM_COL32(255, 210, 90, 170));
		if (w < 2 || m_loop_on)
			ddl->AddLine(ImVec2(xa, q.y), ImVec2(xa, q.y + qs.y), cols[w], m_det_drag == w ? 3.0f : 2.0f);
		ddl->PopClipRect();
		// 縁はその点の色（ループが切れているときの L は暗く）
		const ImU32 bc = (w == 2 && !m_loop_on) ? IM_COL32(90, 80, 105, 255) : (cols[w] & 0x00ffffffu) | 0xc8000000u;
		wave_border(ddl, q, qs, bc);
		std::snprintf(id, sizeof(id), "##dsc%d", w);
		double dv0 = m_det_center[w] - dspan * 0.5;
		if (hscroll(id, dv0, dspan, double(frames), qs.x))
			m_det_center[w] = dv0 + dspan * 0.5;
		ImGui::EndGroup();
	}
}

void sampling_editor::assign_pane(bridge &br)
{
	heading(UI_TEXT(smp_assign, "Voice assignment"));
	const float fs = ImGui::GetFontSize();
	float lab = fs * 7.0f;   // 項目の名前の幅（右の列は「レベル 2（サステイン）」が入るよう広げる）
	// 左の列は音色・サンプル・音量・音程、右の列はエンベロープと試聴・書き込み（スクロールしなくても届くように）
	// 幅が狭いとき（3 列の右の列）は、2 つを縦に積む
	const bool stacked = ImGui::GetContentRegionAvail().x < fs * 44.0f;
	const float col_w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
	ImGui::BeginChild("assign_l", ImVec2(stacked ? 0 : col_w, 0), stacked ? ImGuiChildFlags_AutoResizeY : ImGuiChildFlags_None);

	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Bank#");
	ImGui::SameLine(lab);
	ImGui::SetNextItemWidth(fs * 5);
	ImGui::Combo("##bank", &m_bank, "000\0" "001\0");
	ImGui::SameLine();
	ImGui::TextUnformatted(UI_TEXT(smp_pgm, "Program"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 6);
	if (ImGui::InputInt("##pgm", &m_pgm))
		m_pgm = std::clamp(m_pgm, 1, 128);

	const int slot = m_bank * 128 + (m_pgm - 1);
	const sp::voice &cur = m_view.voices[size_t(slot)];
	if (slot != m_loaded_slot || !m_dirty) {
		// 選んだ音色の今の値を編集欄へ（触っている間は上書きしない）
		m_loaded_slot = slot;
		std::snprintf(m_voice_name, sizeof(m_voice_name), "%s", cur.name.c_str());
		m_els = cur.el;
		load_el(m_cur_el);
		m_dirty = false;
	}

	// 音色の名前（音色全体のもの。要素の欄の上に置く）
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_voice_name, "Voice name"));
	ImGui::SameLine(lab);
	ImGui::SetNextItemWidth(fs * 10);
	if (ImGui::InputText("##vname", m_voice_name, sizeof(m_voice_name)))
		m_dirty = true;

	// 要素 1-4。音色は 4 つまで要素を重ねて鳴らせる（鍵と強さの範囲で分けることもできる）。
	// 下の欄は選んでいる要素のもの。使う要素は名前の後ろに ● を付ける
	if (ImGui::BeginTabBar("##elems")) {
		for (int e = 0; e < sp::VOICE_ELEMENTS; e++) {
			const bool on = e == m_cur_el ? m_el_on : m_els[size_t(e)].on;
			char tab[48];
			std::snprintf(tab, sizeof(tab), "%s %d%s###el%d", UI_TEXT(smp_element, "Element"), e + 1, on ? " \xe2\x97\x8f" : "", e);
			if (ImGui::BeginTabItem(tab)) {
				if (e != m_cur_el) {
					stash_el();
					load_el(e);
				}
				ImGui::EndTabItem();
			}
		}
		ImGui::EndTabBar();
	}
	if (ImGui::Checkbox(UI_TEXT(smp_element_on, "Play this element"), &m_el_on))
		m_dirty = true;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(smp_element_tip, "A voice can layer up to four elements. Each has its own wave, level, pan, pitch, envelope and key / velocity range. Elements that are not played keep their settings."));

	// サンプル
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_sample, "Sample"));
	ImGui::SameLine(lab);
	ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 5.5f);
	char label[32];
	const char *none = UI_TEXT(smp_sample_none, "(none)");
	std::string shown = none;
	for (const sp::sample &s : m_view.samples)
		if (s.number == m_sample) {
			std::snprintf(label, sizeof(label), "%03d %s", s.number, s.name.c_str());
			shown = label;
		}
	if (m_sample && shown == none) {
		std::snprintf(label, sizeof(label), "%03d ?", m_sample);
		shown = label;
	}
	// 内蔵の波形の組の名前代わり（その組を使っている音色。無ければドラムの打）。ROM を読めたら 1 度だけ作る
	if (m_wave_labels.empty()) {
		romwave_build();
		if (m_rw_built) {
			m_wave_labels.resize(m_rw_cat.size());
			for (size_t i = 0; i < m_rw_cat.size(); i++)
				m_wave_labels[i] = romwave_label(int(i));
		}
	}
	auto wave_label = [&](int w) {
		return w >= 0 && w < int(m_wave_labels.size()) ? m_wave_labels[size_t(w)] : "W" + std::to_string(w);
	};
	if (!m_sample && m_rom_wave >= 0)
		shown = wave_label(m_rom_wave);
	if (ImGui::BeginCombo("##sample", shown.c_str(), ImGuiComboFlags_HeightLarge)) {
		if (ImGui::Selectable(none, m_sample == 0 && m_rom_wave < 0)) {
			m_sample = 0;
			m_rom_wave = -1;
			m_dirty = true;
		}
		for (const sp::sample &s : m_view.samples) {
			std::snprintf(label, sizeof(label), "%03d %s", s.number, s.name.c_str());
			if (ImGui::Selectable(label, s.number == m_sample)) {
				m_sample = s.number;
				m_rom_wave = -1;
				m_dirty = true;
			}
		}
		// 内蔵ウェーブ（MU2000 の XG の音色が使う波形の組）。名前の一部で絞り込める
		ImGui::SeparatorText(UI_TEXT(smp_rom_waves, "Built-in waves"));
		ImGui::SetNextItemWidth(-1);
		ImGui::InputTextWithHint("##wfind", UI_TEXT(smp_rom_wave_find, "Filter by voice name"), m_wave_find, sizeof(m_wave_find));
		std::string want = m_wave_find;
		for (char &c : want)
			c = char(std::tolower(u8(c)));
		const int count = m_wave_labels.empty() ? sp::ROM_WAVE_SETS : int(m_wave_labels.size());
		for (int w = 0; w < count; w++) {
			std::string l = wave_label(w);
			if (!want.empty()) {
				std::string low = l;
				for (char &c : low)
					c = char(std::tolower(u8(c)));
				if (low.find(want) == std::string::npos)
					continue;
			}
			l += "##w" + std::to_string(w);
			if (ImGui::Selectable(l.c_str(), m_sample == 0 && w == m_rom_wave)) {
				m_sample = 0;
				m_rom_wave = w;
				m_dirty = true;
			}
		}
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(smp_rom_wave_tip, "A sample from the sampling RAM, or one of the built-in waves the MU2000's own voices play (named after the voices that use it). A built-in wave is played the same way as a sample: pitch, level and envelope below apply."));
	ImGui::SameLine();
	if (ImGui::Button(UI_TEXT(smp_rom_wave_browse, "Browse..."), ImVec2(-1, 0))) {
		if (m_rom_wave >= 0) {
			m_rw_sel = m_rom_wave;
			m_rw_zone = 0;
			m_rw_scroll = true;
		}
		m_goto_tab = 2;
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(smp_rom_wave_browse_tip, "Opens the Built-in waves tab: listen to each wave, see its shape and which voices use it, then pick one for this element."));

	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_level, "Level"));
	ImGui::SameLine(lab);
	ImGui::SetNextItemWidth(-1);
	if (ImGui::SliderInt("##level", &m_level, 0, 127))
		m_dirty = true;

	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_pan, "Pan"));
	ImGui::SameLine(lab);
	ImGui::SetNextItemWidth(fs * 8);
	char pbuf[24];
	if (ImGui::BeginCombo("##pan", pan_text(m_pan, pbuf, sizeof(pbuf)))) {
		for (int p = 0; p <= 15; p++)
			if (ImGui::Selectable(pan_text(p, pbuf, sizeof(pbuf)), p == m_pan)) {
				m_pan = p;
				m_dirty = true;
			}
		ImGui::EndCombo();
	}

	// 音程。鍵 60 が録ったときの高さで、そこからずらす
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_coarse, "Pitch (semitones)"));
	ImGui::SameLine(lab);
	ImGui::SetNextItemWidth(-1);
	if (ImGui::SliderInt("##coarse", &m_coarse, -24, 24, "%+d"))
		m_dirty = true;
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_fine, "Fine (cents)"));
	ImGui::SameLine(lab);
	ImGui::SetNextItemWidth(-1);
	if (ImGui::SliderInt("##fine", &m_fine, -64, 63, "%+d"))
		m_dirty = true;

	// この要素を鳴らす鍵と強さの範囲（要素ごとに鍵盤を分けたり、強く弾いたときだけ重ねたりする）
	static const char *const NOTE_NAME[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
	auto key_text = [](int k, char *buf, size_t n) {
		std::snprintf(buf, n, "%s%d", NOTE_NAME[k % 12], k / 12 - 2);   // XG の数え方（60 = C3）
		return buf;
	};
	char klo[8], khi[8];
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_key_range, "Keys"));
	ImGui::SameLine(lab);
	ImGui::SetNextItemWidth(-1);
	// 書式に数の指定が無ければ ImGui はその文字をそのまま出すので、鍵の名前を書式に入れる
	if (ImGui::DragIntRange2("##keys", &m_key_lo, &m_key_hi, 0.25f, 0, 127, key_text(m_key_lo, klo, sizeof(klo)),
	                         key_text(m_key_hi, khi, sizeof(khi)), ImGuiSliderFlags_AlwaysClamp))
		m_dirty = true;
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_vel_range, "Velocity"));
	ImGui::SameLine(lab);
	ImGui::SetNextItemWidth(-1);
	if (ImGui::DragIntRange2("##vels", &m_vel_lo, &m_vel_hi, 0.25f, 1, 127, "%d", "%d", ImGuiSliderFlags_AlwaysClamp))
		m_dirty = true;

	ImGui::Spacing();
	// ---- フィルター・LFO・ピッチ EG・フィルター EG（今の要素の欄を直に書き換える。畳んでおける）
	{
		sp::element &x = m_els[size_t(m_cur_el)];
		const float lab = fs * 12.5f;      // この区画の名前の幅（外の lab より広い）
		auto row = [&](const char *name, const char *id, int &v, int lo, int hi, const char *fmt = "%d") {
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(name);
			ImGui::SameLine(lab);
			ImGui::SetNextItemWidth(-1);
			if (ImGui::SliderInt(id, &v, lo, hi, fmt))
				m_dirty = true;
		};
		// EG の形の目安。レベル 5 つ（始め・アタック・ディケイ 1・ディケイ 2 = 押している間・離した後）を、速さに応じた幅でつなぐ
		auto eg_graph = [&](const int *rate, const int *level, ImU32 col) {
			const float gw = ImGui::GetContentRegionAvail().x, gh = fs * 3.2f;
			const ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(gw, gh));
			ImDrawList *d = ImGui::GetWindowDrawList();
			d->AddRectFilled(p, ImVec2(p.x + gw, p.y + gh), IM_COL32(16, 20, 26, 255), 3.0f);
			d->AddLine(ImVec2(p.x, p.y + gh * 0.5f), ImVec2(p.x + gw, p.y + gh * 0.5f), IM_COL32(70, 80, 95, 255));
			auto y = [&](int lv) { return p.y + gh * (0.5f - 0.46f * float(std::clamp(lv, -64, 63)) / 64.0f); };
			auto seg = [&](int r) { return r <= 0 ? 4.0f : 0.25f + 3.75f * float(63 - std::min(r, 63)) / 63.0f; };
			const float hold = 1.5f;
			const float total = seg(rate[0]) + seg(rate[1]) + seg(rate[2]) + hold + seg(rate[3]);
			const float sx = gw / total;
			float xx = p.x;
			ImVec2 pts[6];
			pts[0] = ImVec2(xx, y(level[0]));
			for (int i = 0; i < 3; i++) {
				xx += seg(rate[i]) * sx;
				pts[i + 1] = ImVec2(xx, y(level[i + 1]));
			}
			xx += hold * sx;
			pts[4] = ImVec2(xx, y(level[3]));
			d->AddLine(ImVec2(xx, p.y), ImVec2(xx, p.y + gh), IM_COL32(90, 100, 120, 255));   // ここで鍵を離す
			pts[5] = ImVec2(p.x + gw, y(level[4]));
			d->AddPolyline(pts, 6, col, 0, 1.5f);
		};
		auto eg_rows = [&](const char *tag, int *rate, int *level) {
			const char *rn[4] = { UI_TEXT(smp_eg_attack, "Attack rate"), UI_TEXT(smp_eg_decay1, "Decay 1 rate"),
			                      UI_TEXT(smp_eg_decay2, "Decay 2 rate"), UI_TEXT(smp_eg_release, "Release rate") };
			const char *ln[5] = { UI_TEXT(smp_eg_l0, "Start level"), UI_TEXT(smp_eg_l1, "Attack level"),
			                      UI_TEXT(smp_eg_l2, "Decay 1 level"), UI_TEXT(smp_eg_l3, "Decay 2 level (held)"),
			                      UI_TEXT(smp_eg_l4, "Release level") };
			ImGui::PushID(tag);
			row(ln[0], "##l0", level[0], -64, 63, "%+d");
			for (int i = 0; i < 4; i++) {
				ImGui::PushID(i);
				row(rn[i], "##r", rate[i], 0, 63);
				row(ln[i + 1], "##l", level[i + 1], -64, 63, "%+d");
				ImGui::PopID();
			}
			ImGui::PopID();
		};

		if (ImGui::CollapsingHeader(UI_TEXT(smp_filter, "Filter and velocity"))) {
			row(UI_TEXT(smp_cutoff, "Cutoff"), "##cut", x.cutoff, 0, 127);
			row(UI_TEXT(smp_resonance, "Resonance"), "##reso", x.resonance, 0, 127);
			row(UI_TEXT(smp_hpf, "High-pass"), "##hpf", x.hpf, 0, 127);
			row(UI_TEXT(smp_vel_curve, "Velocity curve"), "##velc", x.vel_curve, 0, 10);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", UI_TEXT(smp_vel_curve_tip, "How key velocity turns into level. 0 is the normal curve; 1 and 2 keep soft notes louder; 3 to 8 widen the difference; 9 and 10 reach full level by medium velocity."));
		}
		if (ImGui::CollapsingHeader("LFO", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(UI_TEXT(smp_lfo_wave, "Wave"));
			ImGui::SameLine(lab);
			ImGui::SetNextItemWidth(fs * 8);
			const std::string waves = std::string(UI_TEXT(smp_lfo_saw, "Saw")) + '\0' + UI_TEXT(smp_lfo_tri, "Triangle") + '\0' +
			                          UI_TEXT(smp_lfo_sh, "S&H") + '\0';
			if (ImGui::Combo("##lfowave", &x.lfo_wave, waves.c_str()))
				m_dirty = true;
			ImGui::Dummy(ImVec2(0, 0));
			ImGui::SameLine(lab);
			if (ImGui::Checkbox(UI_TEXT(smp_lfo_init, "Phase init"), &x.lfo_phase_init))
				m_dirty = true;
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", UI_TEXT(smp_lfo_init_tip, "On: the LFO starts from the same point at every note-on. Off: it starts from a random point."));
			row(UI_TEXT(smp_lfo_speed, "Speed"), "##lfospeed", x.lfo_speed, 0, 63);
			row(UI_TEXT(smp_lfo_delay, "Delay"), "##lfodelay", x.lfo_delay, 0, 127);
			row(UI_TEXT(smp_lfo_pitch, "Pitch depth"), "##lfop", x.lfo_pitch, 0, 127);
			row(UI_TEXT(smp_lfo_filter, "Filter depth"), "##lfof", x.lfo_filter, 0, 127);
			row(UI_TEXT(smp_lfo_amp, "Amplitude depth"), "##lfoa", x.lfo_amp, 0, 127);
		}
		if (ImGui::CollapsingHeader(UI_TEXT(smp_peg, "Pitch EG"))) {
			row(UI_TEXT(smp_peg_depth, "Depth"), "##pegd", x.peg_depth, 0, 127);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", UI_TEXT(smp_peg_tip, "Levels are offsets from the note's pitch (0 = no change); depth sets how far the largest level goes (64 or more: an octave). The pitch starts at Start level, moves to each level at its rate while the key is held, and goes to Release level after it is let go."));
			eg_rows("peg", x.peg_rate, x.peg_level);
			eg_graph(x.peg_rate, x.peg_level, IM_COL32(120, 230, 150, 255));
		}
		if (ImGui::CollapsingHeader(UI_TEXT(smp_feg, "Filter EG"))) {
			eg_rows("feg", x.feg_rate, x.feg_level);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", UI_TEXT(smp_feg_tip, "Levels move the cutoff up and down from its setting (0 = no change; negative closes the filter). Lower the cutoff first if the filter is fully open."));
			eg_graph(x.feg_rate, x.feg_level, IM_COL32(255, 190, 110, 255));
		}
	}

	ImGui::EndChild();
	if (!stacked)
		ImGui::SameLine();
	ImGui::BeginChild("assign_r", ImVec2(0, 0), stacked ? ImGuiChildFlags_AutoResizeY : ImGuiChildFlags_None);
	lab = fs * 10.0f;

	// エンベロープ。速さは大きいほど速く 0 は動かない、レベルは 127 が最大。値の下に形の目安
	ImGui::TextDisabled("%s", UI_TEXT(smp_env, "Envelope"));
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(smp_env_tip, "Rates run 0-63 (higher is faster, 0 stays put); levels 0-127. A key goes up at the attack rate, falls at decay 1 to level 1, then at decay 2 to level 2 and stays there while held (with the loop on, the sound keeps going). Release fades it out after the key is let go."));
	{
		auto rate = [&](const char *name, const char *id, int &v, int max) {
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(name);
			ImGui::SameLine(lab);
			ImGui::SetNextItemWidth(-1);
			if (ImGui::SliderInt(id, &v, 0, max))
				m_dirty = true;
		};
		rate(UI_TEXT(smp_env_attack, "Attack"), "##ar", m_attack, 63);
		rate(UI_TEXT(smp_env_decay1, "Decay 1"), "##d1r", m_decay1, 63);
		rate(UI_TEXT(smp_env_level1, "Level 1"), "##d1l", m_level1, 127);
		rate(UI_TEXT(smp_env_decay2, "Decay 2"), "##d2r", m_decay2, 63);
		rate(UI_TEXT(smp_env_level2, "Level 2 (sustain)"), "##d2l", m_level2, 127);
		rate(UI_TEXT(smp_env_release, "Release"), "##rr", m_release, 63);

		const float gw = ImGui::GetContentRegionAvail().x, gh = fs * 4;
		const ImVec2 g0 = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(gw, gh));
		ImDrawList *dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(g0, ImVec2(g0.x + gw, g0.y + gh), IM_COL32(16, 20, 26, 255));
		// 形の目安: 区間の幅は (64 - 速さ) に比べる（0 は動かないので長い平らな区間）、高さはレベル（dB を直線に）
		auto seg = [](int rate) { return rate <= 0 ? 0.0f : float(64 - rate) + 2.0f; };
		auto lv = [](int level) { return std::clamp(1.0f - float(127 - level) * 0.77f / 48.0f, 0.0f, 1.0f); };
		const float hold = 24.0f;
		const float wa = seg(m_attack), w1 = m_decay1 ? seg(m_decay1) : hold, w2 = m_decay2 ? seg(m_decay2) : 0.0f, wr = seg(m_release);
		const float l1 = m_decay1 ? lv(m_level1) : 1.0f, l2 = m_decay1 && m_decay2 ? lv(m_level2) : l1;
		const float total = (m_attack ? wa : hold) + w1 + w2 + hold + (m_release ? wr : hold);
		const float sx = (gw - 8) / total, top = g0.y + 4, bot = g0.y + gh - 4, hh = bot - top;
		float x = g0.x + 4;
		std::vector<ImVec2> pts = { ImVec2(x, bot) };
		x += (m_attack ? wa : hold) * sx;
		pts.push_back(ImVec2(x, m_attack ? top : bot));
		const float peak = m_attack ? 1.0f : 0.0f;
		x += w1 * sx;
		pts.push_back(ImVec2(x, bot - hh * peak * l1));
		x += w2 * sx;
		pts.push_back(ImVec2(x, bot - hh * peak * l2));
		x += hold * sx;
		pts.push_back(ImVec2(x, bot - hh * peak * l2));
		const float xoff = x;
		x += (m_release ? wr : hold) * sx;
		pts.push_back(ImVec2(x, m_release ? bot : bot - hh * peak * l2));
		dl->AddPolyline(pts.data(), int(pts.size()), IM_COL32(110, 200, 255, 255), 0, 2.0f);
		dl->AddLine(ImVec2(xoff, g0.y), ImVec2(xoff, g0.y + gh), IM_COL32(255, 210, 90, 160));
	}

	ImGui::Spacing();
	// 編集欄（今の要素）を入れ物へ戻してから、4 つの要素で音色を作る
	auto make_voice = [&]() {
		stash_el();
		sp::voice v;
		v.name = m_voice_name;
		v.el = m_els;
		return v;
	};
	bool any_wave = false;
	for (int e = 0; e < sp::VOICE_ELEMENTS; e++) {
		const bool on = e == m_cur_el ? m_el_on : m_els[size_t(e)].on;
		const bool wave = e == m_cur_el ? (m_sample != 0 || m_rom_wave >= 0)
		                                : (m_els[size_t(e)].assigned || m_els[size_t(e)].rom_wave >= 0);
		any_wave |= on && wave;
	}

	// 試聴（音源を通す）。押しているあいだ、今の値を音色に書いてパート 1 をその音色にし、鍵を鳴らす。
	// 離すとノートオフ（リリースも聞ける）
	ImGui::BeginDisabled(!any_wave);
	ImGui::Button(UI_TEXT(smp_audition, "Hold to play"), ImVec2(fs * 9, 0));
	const bool hold_on = ImGui::IsItemActivated(), hold_off = ImGui::IsItemDeactivated();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", UI_TEXT(smp_audition_tip, "While held, writes the values above to the voice, selects it on part 1 and plays the key through the tone generator (level, pan, pitch and envelope all apply). Letting go sends note-off, so the release is heard too."));
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_audition_key, "Key"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 6);
	if (ImGui::InputInt("##akey", &m_audition_key))
		m_audition_key = std::clamp(m_audition_key, 0, 127);
	ImGui::SameLine();
	static const char *const NOTE[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
	ImGui::TextDisabled("%s%d", NOTE[m_audition_key % 12], m_audition_key / 12 - 2);   // XG の数え方（60 = C3）
	if (hold_on) {
		const sp::voice v = make_voice();
		br.post([slot, v](mu2000 &mu) {
			std::string err;
			mu.sampling_set_voice(slot, v, err);
			return err;
		});
		m_dirty = false;
		m_held_key = m_audition_key;
		const std::vector<u8> msg = { 0xb0, 0x00, 0x10, 0xb0, 0x20, u8(m_bank), 0xc0, u8(m_pgm - 1),
		                              0x90, u8(m_held_key), 0x64 };
		br.send(msg);
	}
	if (hold_off && m_held_key >= 0) {
		const std::vector<u8> msg = { 0x80, u8(m_held_key), 0x40 };
		br.send(msg);
		m_held_key = -1;
	}

	if (ImGui::Button(UI_TEXT(smp_apply, "Apply"), ImVec2(fs * 7, 0))) {
		const sp::voice v = make_voice();
		std::string done = UI_TEXT(smp_voice_set_fmt, "Wrote Bank# %d, program %d");
		const int bank = m_bank, pgm = m_pgm;
		br.post([slot, v, done, bank, pgm](mu2000 &mu) {
			std::string err;
			if (!mu.sampling_set_voice(slot, v, err))
				return err;
			char buf[120];
			std::snprintf(buf, sizeof(buf), done.c_str(), bank, pgm);
			return std::string(buf);
		});
		m_dirty = false;
	}
	ImGui::SameLine();
	// 鳴らしてみる: パート 1 をこの音色にする（バンク MSB 16）。書いた値は音色を選び直したときに効く
	if (ImGui::Button(UI_TEXT(smp_select_part1, "Select on part 1"))) {
		const std::vector<u8> msg = { 0xb0, 0x00, 0x10, 0xb0, 0x20, u8(m_bank), 0xc0, u8(m_pgm - 1) };
		br.send(msg);
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip(UI_TEXT(smp_play_hint_fmt, "Play it with bank MSB 16, LSB %d, program %d. Changes take effect when the voice is selected again."),
		                  m_bank, m_pgm);

	// 実機へ: この音色を SysEx（機種 0x68 のパラメータチェンジ。sp::voice_sysex）にする。今の値を書き込んでから
	// 表の記録をそのまま写すので、ここに出ていない欄も含めて 334 通になる
	auto make_sysex = [&](bool to_file) {
		const sp::voice v = make_voice();
		auto job = std::make_shared<sx_job>();
		job->to_file = to_file;
		m_sx_job = job;
		br.post([job, slot, v](mu2000 &mu) {
			std::string err;
			if (mu.sampling_set_voice(slot, v, err)) {
				const u32 o = sp::TAB_VOICE + sp::VOICE_SIZE * u32(slot) - 0x1000000;
				job->msgs = sp::voice_sysex(slot, mu.dram().data() + o);
			}
			job->done.store(true, std::memory_order_release);
			return err;
		});
		m_dirty = false;
	};
	const char *sx_tip = UI_TEXT(smp_sx_tip, "Writes the values above to the voice, then turns the whole voice into SysEx (Yamaha model 0x68 parameter changes, 334 messages) that a real MU2000 accepts for its sample voices. A voice that plays a sample points at that sample number on the receiving unit.");
	ImGui::BeginDisabled(m_sx_job != nullptr || !m_sx_queue.empty());
	if (ImGui::Button(UI_TEXT(smp_sx_save, "Save as SysEx...")))
		make_sysex(true);
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", sx_tip);
	ImGui::SameLine();
	ImGui::BeginDisabled(!xgui::out_ready());
	if (ImGui::Button(UI_TEXT(smp_sx_send, "Send to MIDI out")))
		make_sysex(false);
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", sx_tip);
	ImGui::EndDisabled();
	if (!m_sx_queue.empty()) {
		ImGui::SameLine();
		ImGui::TextDisabled(UI_TEXT(smp_sx_sending_fmt, "Sending %d / %d"), int(m_sx_total - m_sx_queue.size()), int(m_sx_total));
	}
	if (xgui::out_ready())
		xgui::out_port_combo();
	ImGui::EndChild();
}

// ---- カード（SmartMedia の中身を見る・試聴する・差す・読み込む）

namespace {

// 44.1kHz に直す（直線でつなぐ）。M2A は本体が書けば 44.1kHz なので、たいてい何もしない
std::vector<s16> to_44k(const std::vector<s16> &in, u32 rate)
{
	if (rate == 44100 || rate == 0 || in.empty())
		return in;
	const double step = double(rate) / 44100.0;
	std::vector<s16> out(size_t(double(in.size()) / step));
	for (size_t i = 0; i < out.size(); i++) {
		const double at = double(i) * step;
		const size_t a = size_t(at);
		const double f = at - double(a);
		const double v = in[a] * (1.0 - f) + (a + 1 < in.size() ? in[a + 1] : in[a]) * f;
		out[i] = s16(std::lround(v));
	}
	return out;
}

std::filesystem::path u8path(const std::string &s)
{
	return std::filesystem::path(reinterpret_cast<const char8_t *>(s.c_str()));
}

std::string u8name(const std::filesystem::path &p)
{
	const std::u8string s = p.filename().u8string();
	return std::string(s.begin(), s.end());
}

bool ends_with_m2a(const std::string &path)
{
	if (path.size() < 4)
		return false;
	std::string ext = path.substr(path.size() - 4);
	for (char &c : ext)
		c = char(std::toupper(u8(c)));
	return ext == ".M2A";
}

// カードに置く 8.3 の名前（「NAME.M2A」）。使えない字は _、全部使えなければ FROMPC
std::string short_m2a_name(const std::string &path)
{
	const std::u8string stem8 = u8path(path).stem().u8string();
	std::string stem;
	for (char8_t c8 : stem8) {
		const char c = char(c8);
		if (stem.size() >= 8)
			break;
		if (u8(c) >= 0x80)
			continue;   // 日本語などは落とす（8.3 に入らない）
		const char up = char(std::toupper(u8(c)));
		stem += ((up >= 'A' && up <= 'Z') || (up >= '0' && up <= '9') || up == '_' || up == '-') ? up : '_';
	}
	if (stem.find_first_not_of('_') == std::string::npos)
		stem = "FROMPC";
	return stem + ".M2A";
}

} // namespace

// M2A から作ったカード（m_card_img）を、設定の場所の cards に書く。差すにはファイルが要る
bool sampling_editor::card_materialize(std::string &err)
{
	if (!m_card_file.empty() || m_card_m2a.empty())
		return true;
	std::error_code ec;
	const std::string base = smu2000::config_dir();
	const std::filesystem::path dir = u8path(base.empty() ? std::string(".") : base) / "cards";
	std::filesystem::create_directories(dir, ec);
	const std::string stem = short_m2a_name(m_card_m2a).substr(0, short_m2a_name(m_card_m2a).size() - 4);
	std::filesystem::path p = dir / (stem + ".sm");
	for (int i = 2; std::filesystem::exists(p, ec) && i < 1000; i++)
		p = dir / (stem + "-" + std::to_string(i) + ".sm");
	std::ofstream f(p, std::ios::binary);
	f.write(reinterpret_cast<const char *>(m_card_img.data()), std::streamsize(m_card_img.size()));
	f.close();
	if (!f) {
		err = UI_TEXT(smp_card_write_fail, "Could not write the new card");
		return false;
	}
	const std::u8string s = p.u8string();
	m_card_file.assign(s.begin(), s.end());
	return true;
}

void sampling_editor::card_refresh(bridge &br)
{
	m_card_files.clear();
	m_card_sel = -1;
	m_m2a_lo.clear();
	m_m2a_hi.clear();
	m_m2a.clear();
	m_m2a_waves.clear();
	m_m2a_sel = -1;
	m_m2a_pcm.clear();
	m_card_note.clear();
	m_card_stale = false;
	if (m_card_src == 1) {
		std::string err;
		if (m_card_img.empty())
			return;
		if (!smu2000::cardfs::list(m_card_img, m_card_files, err))
			m_card_note = err;
		return;
	}
	// 差しているカードは音を作る糸で読む
	auto job = std::make_shared<card_job>();
	m_card_job = job;
	br.post([job](mu2000 &mu) {
		if (!mu.card_inserted())
			job->err = "no card in the slot";
		else
			job->ok = smu2000::cardfs::list(mu.card().raw(), job->files, job->err);
		job->done.store(true, std::memory_order_release);
		return std::string();
	});
}

void sampling_editor::card_open_file(bridge &br, int index)
{
	m_card_sel = index;
	m_m2a.clear();
	m_m2a_waves.clear();
	m_m2a_sel = -1;
	m_m2a_pcm.clear();
	if (index < 0 || index >= int(m_card_files.size()))
		return;
	const std::string path = m_card_files[size_t(index)].path;
	if (m_card_src == 1) {
		std::string err;
		if (!smu2000::cardfs::read(m_card_img, path, m_m2a, err))
			m_card_note = err;
		else if (!smu2000::m2a::parse(m_m2a, m_m2a_waves, err))
			m_card_note = err;
		return;
	}
	auto job = std::make_shared<card_job>();
	job->index = index;
	m_card_job = job;
	br.post([job, path](mu2000 &mu) {
		job->ok = mu.card_inserted() && smu2000::cardfs::read(mu.card().raw(), path, job->bytes, job->err);
		job->done.store(true, std::memory_order_release);
		return std::string();
	});
}

void sampling_editor::card_select_wave(int index)
{
	m_m2a_sel = index;
	m_m2a_pcm.clear();
	m_m2a_lo.clear();
	m_m2a_hi.clear();
	if (index < 0 || index >= int(m_m2a_waves.size()))
		return;
	const smu2000::m2a::wave &w = m_m2a_waves[size_t(index)];
	m_m2a_pcm = to_44k(smu2000::m2a::pcm(m_m2a, w), w.rate);
	// 見取り図（1024 区切りの最小と最大）
	const size_t n = m_m2a_pcm.size(), nb = std::min<size_t>(1024, n);
	if (!nb)
		return;
	m_m2a_lo.assign(nb, 32767);
	m_m2a_hi.assign(nb, -32768);
	for (size_t i = 0; i < n; i++) {
		const size_t b = i * nb / n;
		m_m2a_lo[b] = std::min(m_m2a_lo[b], m_m2a_pcm[i]);
		m_m2a_hi[b] = std::max(m_m2a_hi[b], m_m2a_pcm[i]);
	}
}

void sampling_editor::card_pane(bridge &br)
{
	const float fs = ImGui::GetFontSize();
	const std::string slot = br.card_path();

	// 音を作る糸からの答え
	if (m_card_job && m_card_job->done.load(std::memory_order_acquire)) {
		std::shared_ptr<card_job> job = std::move(m_card_job);
		if (!job->ok) {
			m_card_note = job->err;
		} else if (job->index < 0) {
			m_card_files = std::move(job->files);
		} else if (job->index == m_card_sel) {
			std::string err;
			m_m2a = std::move(job->bytes);
			if (!smu2000::m2a::parse(m_m2a, m_m2a_waves, err))
				m_card_note = err;
		}
	}
	// 開いた画像（ファイルの窓から）
	std::string picked;
	if (xgui::take_opened_card(picked)) {
		std::ifstream f(u8path(picked), std::ios::binary);
		std::vector<u8> bytes;
		bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
		m_card_src = 1;
		m_card_stale = true;
		m_card_img.clear();
		m_card_file.clear();
		m_card_m2a.clear();
		if (ends_with_m2a(picked)) {
			// M2A のファイル。入るいちばん小さいカードを作り、一番上に置く（差すまではメモリの中だけ）
			std::vector<smu2000::smartmedia::root_file> put(1);
			put[0].name = short_m2a_name(picked);
			put[0].bytes = std::move(bytes);
			smu2000::smartmedia sm;
			const u32 mb = smu2000::smartmedia::megabytes_for(put[0].bytes.size());
			if (!put[0].bytes.empty() && mb && sm.create(mb) && sm.format(put)) {
				m_card_img = sm.raw();
				m_card_m2a = picked;
			} else {
				m_card_note = put[0].bytes.empty() ? UI_TEXT(smp_card_read_fail, "Could not read the card image")
				                                   : UI_TEXT(smp_card_too_big, "This file does not fit on a 128MB card");
			}
		} else {
			m_card_img = std::move(bytes);
			m_card_file = picked;
			if (m_card_img.empty())
				m_card_note = UI_TEXT(smp_card_read_fail, "Could not read the card image");
		}
	}
	// 差しているカードが変わったら（差す・抜く・読み込みで書かれた）一覧を作り直す
	if (m_card_src == 0 && slot != m_card_seen) {
		m_card_seen = slot;
		m_card_stale = true;
	}
	// 差してから読み込む: 差し終わったら読み込みを始める
	if (!m_load_after_insert.empty() && slot == m_card_file && m_view.card_in && !m_view.macro_busy) {
		br.request_macro(panel_macro::load_m2a(m_load_after_insert), UI_TEXT(smp_card_loaded, "Loaded from the card"));
		m_load_after_insert.clear();
		m_card_src = 0;
		m_card_stale = true;
	}
	if (m_card_stale && !m_card_job)
		card_refresh(br);

	heading(UI_TEXT(smp_card, "SmartMedia"));
	// ---- どのカードを見るか
	const bool was = m_card_src;
	if (ImGui::RadioButton(UI_TEXT(smp_card_slot, "Card in the slot"), m_card_src == 0))
		m_card_src = 0;
	ImGui::SameLine();
	ImGui::TextDisabled("%s", slot.empty() ? UI_TEXT(smp_card_none, "(none)") : u8name(u8path(slot)).c_str());
	if (ImGui::RadioButton(UI_TEXT(smp_card_image, "Card image or M2A file"), m_card_src == 1))
		m_card_src = 1;
	ImGui::SameLine();
	if (xgui::file_dialogs()) {
		if (ImGui::Button(UI_TEXT(smp_card_open, "Open...")))
			xgui::ask_open_card();
	} else {
		ImGui::SetNextItemWidth(-fs * 6);
		ImGui::InputTextWithHint("##cardpath", UI_TEXT(smp_card_path, "Card image path"), m_card_input, sizeof(m_card_input));
		ImGui::SameLine();
		if (ImGui::Button(UI_TEXT(smp_card_open_path, "Open")) && m_card_input[0])
			xgui::give_opened_card(m_card_input);
	}
	if (!m_card_m2a.empty()) {
		ImGui::SameLine();
		ImGui::TextDisabled("%s", u8name(u8path(m_card_m2a)).c_str());
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", UI_TEXT(smp_card_m2a_tip, "Shown as a new card holding this file. Inserting or loading saves that card in the cards folder of the settings folder."));
	} else if (!m_card_file.empty()) {
		ImGui::SameLine();
		ImGui::TextDisabled("%s", u8name(u8path(m_card_file)).c_str());
	}
	if (was != bool(m_card_src))
		m_card_stale = true;
	ImGui::SameLine();
	if (ImGui::SmallButton(UI_TEXT(smp_card_reload, "Reload")))
		m_card_stale = true;
	if (!m_card_note.empty())
		ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f), "%s", m_card_note.c_str());

	// ---- ファイルの一覧と、選んだ M2A の波形の一覧を並べる
	const float list_h = std::max(fs * 6, ImGui::GetContentRegionAvail().y * 0.38f);
	const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
	if (ImGui::BeginChild("card_files", ImVec2(half, list_h), ImGuiChildFlags_Borders)) {
		if (m_card_job && m_card_job->index < 0)
			ImGui::TextDisabled("%s", UI_TEXT(smp_card_reading, "Reading..."));
		else if (m_card_files.empty())
			ImGui::TextDisabled("%s", UI_TEXT(smp_card_empty, "No files"));
		if (!m_card_files.empty() && ImGui::BeginTable("files", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn(UI_TEXT(smp_card_col_file, "File"));
			ImGui::TableSetupColumn(UI_TEXT(smp_card_col_size, "Size"), ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableHeadersRow();
			for (size_t i = 0; i < m_card_files.size(); i++) {
				const smu2000::cardfs::entry &e = m_card_files[i];
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				if (ImGui::Selectable(e.path.c_str(), int(i) == m_card_sel, ImGuiSelectableFlags_SpanAllColumns))
					card_open_file(br, int(i));
				ImGui::TableNextColumn();
				if (e.size >= 1024 * 1024)
					ImGui::Text("%.1f MB", e.size / 1048576.0);
				else
					ImGui::Text("%.0f KB", e.size / 1024.0);
			}
			ImGui::EndTable();
		}
	}
	ImGui::EndChild();
	ImGui::SameLine();
	if (ImGui::BeginChild("card_waves", ImVec2(0, list_h), ImGuiChildFlags_Borders)) {
		if (m_card_sel >= 0 && m_m2a_waves.empty())
			ImGui::TextDisabled("%s", m_card_job ? UI_TEXT(smp_card_reading, "Reading...")
			                                     : UI_TEXT(smp_card_no_waves, "No samples in this file"));
		if (!m_m2a_waves.empty() && ImGui::BeginTable("waves", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn(UI_TEXT(smp_name, "Name"));
			ImGui::TableSetupColumn(UI_TEXT(smp_col_len, "Length"), ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn(UI_TEXT(smp_loop, "Loop"), ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableHeadersRow();
			for (size_t i = 0; i < m_m2a_waves.size(); i++) {
				const smu2000::m2a::wave &w = m_m2a_waves[i];
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				char label[64];
				std::snprintf(label, sizeof(label), "%s##w%zu", w.name.empty() ? "-" : w.name.c_str(), i);
				if (ImGui::Selectable(label, int(i) == m_m2a_sel, ImGuiSelectableFlags_SpanAllColumns))
					card_select_wave(int(i));
				ImGui::TableNextColumn();
				ImGui::Text("%.2f s", w.rate ? double(w.frames) / w.rate : 0.0);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(w.loop ? UI_TEXT(smp_card_yes, "yes") : "-");
			}
			ImGui::EndTable();
		}
	}
	ImGui::EndChild();

	// ---- 選んだ波形: 試聴と見取り図
	const bool playing = m_view.preview_number == -1;
	ImGui::BeginDisabled(m_m2a_pcm.empty());
	if (!playing) {
		if (ImGui::Button(UI_TEXT(smp_play, "Play"))) {
			std::vector<s16> pcm = m_m2a_pcm;
			const smu2000::m2a::wave &w = m_m2a_waves[size_t(m_m2a_sel)];
			const u32 loop = w.loop && w.rate ? u32(u64(w.loop_start) * 44100 / w.rate) : ~0u;
			br.post([pcm, loop](mu2000 &mu) mutable {
				mu.preview_pcm(std::move(pcm), loop);
				return std::string();
			});
		}
	} else if (ImGui::Button(UI_TEXT(smp_play_stop, "Stop playing"))) {
		br.post([](mu2000 &mu) {
			mu.preview_stop();
			return std::string();
		});
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", UI_TEXT(smp_card_play_tip, "Plays the sample straight from the file, without loading it into the MU2000"));
	if (m_m2a_sel >= 0 && m_m2a_sel < int(m_m2a_waves.size())) {
		const smu2000::m2a::wave &w = m_m2a_waves[size_t(m_m2a_sel)];
		ImGui::SameLine();
		ImGui::TextDisabled("%s  %u Hz  %u bit  %s  %s %d", w.name.c_str(), w.rate, w.bits,
		                    w.channels > 1 ? "stereo" : "mono", UI_TEXT(smp_card_key, "key"), w.unity);
	}
	const float btn_h = ImGui::GetFrameHeightWithSpacing() * 2.2f;
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const ImVec2 sz(ImGui::GetContentRegionAvail().x, std::max(fs * 4, ImGui::GetContentRegionAvail().y - btn_h));
	ImGui::Dummy(sz);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(16, 20, 26, 255), 4.0f);
	const float mid = p.y + sz.y * 0.5f, halfh = sz.y * 0.5f - 2.0f;
	dl->AddLine(ImVec2(p.x, mid), ImVec2(p.x + sz.x, mid), IM_COL32(80, 90, 110, 255));
	if (!m_m2a_hi.empty()) {
		const int cols = std::max(1, int(sz.x));
		const size_t nb = m_m2a_hi.size();
		for (int x = 0; x < cols; x++) {
			const size_t b0 = size_t(x) * nb / size_t(cols), b1 = std::max(b0 + 1, size_t(x + 1) * nb / size_t(cols));
			int lo = 32767, hi = -32768;
			for (size_t b = b0; b < b1 && b < nb; b++) {
				lo = std::min(lo, int(m_m2a_lo[b]));
				hi = std::max(hi, int(m_m2a_hi[b]));
			}
			if (hi < lo)
				continue;
			dl->AddLine(ImVec2(p.x + float(x) + 0.5f, mid - halfh * float(hi) / 32768.0f),
			            ImVec2(p.x + float(x) + 0.5f, std::max(mid - halfh * float(lo) / 32768.0f, mid - halfh * float(hi) / 32768.0f + 1.0f)),
			            IM_COL32(110, 200, 255, 255));
		}
		const smu2000::m2a::wave &w = m_m2a_waves[size_t(m_m2a_sel)];
		if (w.loop && !m_m2a_pcm.empty() && w.rate) {
			const float xl = p.x + sz.x * float(double(w.loop_start) * 44100 / w.rate / double(m_m2a_pcm.size()));
			dl->AddLine(ImVec2(xl, p.y), ImVec2(xl, p.y + sz.y), IM_COL32(200, 120, 255, 255), 2.0f);
		}
		if (playing && !m_m2a_pcm.empty()) {
			const float xp = p.x + sz.x * float(double(m_view.preview_pos) / double(m_m2a_pcm.size()));
			dl->AddLine(ImVec2(xp, p.y), ImVec2(xp, p.y + sz.y), IM_COL32(255, 255, 255, 230), 1.5f);
		}
	}
	dl->AddRect(ImVec2(p.x - 1, p.y - 1), ImVec2(p.x + sz.x + 1, p.y + sz.y + 1), IM_COL32(110, 125, 150, 255), 4.0f, 0, 1.5f);

	// ---- 差す・読み込む
	const smu2000::cardfs::entry *file = (m_card_sel >= 0 && m_card_sel < int(m_card_files.size()))
	                                     ? &m_card_files[size_t(m_card_sel)] : nullptr;
	const bool is_m2a = file && file->path.size() > 4 &&
	                    file->path.compare(file->path.size() - 4, 4, ".M2A") == 0;
	const bool at_root = file && file->path.find('/') == std::string::npos;
	const bool busy = m_view.macro_busy || m_view.rec_state != 0 || !m_load_after_insert.empty();
	const bool from_m2a = !m_card_m2a.empty();
	ImGui::BeginDisabled(m_card_src != 1 || (m_card_file.empty() && !from_m2a) || (!m_card_file.empty() && m_card_file == slot) ||
	                     m_card_img.empty() || busy);
	if (ImGui::Button(UI_TEXT(smp_card_insert, "Insert this card"))) {
		std::string err;
		if (card_materialize(err))
			br.request_card(m_card_file);
		else
			m_card_note = err;
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", UI_TEXT(smp_card_insert_tip, "Puts this card image into the MU2000's slot (the card in it now comes out)"));
	ImGui::SameLine();
	ImGui::BeginDisabled(!is_m2a || !at_root || busy || (m_card_src == 0 && !m_view.card_in));
	if (ImGui::Button(m_view.macro_busy ? UI_TEXT(smp_card_loading, "Loading...")
	                                    : UI_TEXT(smp_card_load, "Load this M2A into the MU2000")))
		m_load_confirm = true;
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", UI_TEXT(smp_card_load_tip, "Presses SAMPLING > LOAD > ALL+SEQ on the front panel for you and picks this file, as you would on the real unit. Only files at the top of the card can be picked this way."));
	if (m_load_confirm) {
		ImGui::OpenPopup("###load_confirm");
		m_load_confirm = false;
	}
	// 見出しは訳した文言、ID は ### の後ろで固定
	const std::string confirm_title = std::string(UI_TEXT(smp_card_load, "Load this M2A into the MU2000")) + "###load_confirm";
	if (ImGui::BeginPopupModal(confirm_title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		if (m_card_src == 1 && from_m2a && (m_card_file.empty() || m_card_file != slot))
			ImGui::TextUnformatted(UI_TEXT(smp_card_m2a_warn, "A new card holding this file is made and inserted (the card in the slot comes out)."));
		ImGui::TextUnformatted(UI_TEXT(smp_card_load_warn, "Loading replaces the samples and sample voices in the MU2000 now. Go on?"));
		if (ImGui::Button("OK", ImVec2(fs * 6, 0)) && file) {
			// 8.3 の名前（一番上のファイルなので path そのもの）
			std::string err;
			if (m_card_src == 1 && (m_card_file.empty() || m_card_file != slot)) {
				if (card_materialize(err)) {
					m_load_after_insert = file->path;   // 先に差して、差し終わったら読み込む
					br.request_card(m_card_file);
				} else {
					m_card_note = err;
				}
			} else {
				br.request_macro(panel_macro::load_m2a(file->path), UI_TEXT(smp_card_loaded, "Loaded from the card"));
			}
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(UI_TEXT(dlg_cancel, "Cancel"), ImVec2(fs * 6, 0)))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

// ---- 波形を作る（録る代わりに、PC で作った 1 周期の形をサンプルにする。wavegen.h）

void sampling_editor::make_pane(bridge &br)
{
	namespace wg = smu2000::wavegen;
	const float fs = ImGui::GetFontSize();
	heading(UI_TEXT(smp_make, "Make a wave"));

	// ---- 作り方（幅に入るだけ横に並べ、入らなければ折り返す）
	enum { M_BASIC, M_BARS, M_DRAW, M_NOISE, M_FC, M_FM, M_ORGAN, M_UNISON, M_VOWEL, M_SYNC, M_FOLD, M_PWM, M_PLUCK, M_DRUM, M_PD, M_BELL, M_PAINT, M_COUNT };
	const char *mode_names[M_COUNT] = {
		UI_TEXT(smp_make_basic, "Basic shape"), UI_TEXT(smp_make_bars, "Harmonics"), UI_TEXT(smp_make_draw, "Draw"),
		UI_TEXT(smp_make_noise, "Noise"), UI_TEXT(smp_make_fc, "Famicom"), UI_TEXT(smp_make_fm, "FM"),
		UI_TEXT(smp_make_organ, "Organ"), UI_TEXT(smp_make_unison, "Unison"), UI_TEXT(smp_make_vowel, "Voice"),
		UI_TEXT(smp_make_sync, "Sync"), UI_TEXT(smp_make_fold, "Fold"), UI_TEXT(smp_make_pwm, "PWM"),
		UI_TEXT(smp_make_pluck, "Pluck"), UI_TEXT(smp_make_drum, "Drum"), UI_TEXT(smp_make_pd, "PD"),
		UI_TEXT(smp_make_bell, "Bell"), UI_TEXT(smp_make_paint, "Paint"),
	};
	const int was_mode = m_wm_mode;
	{
		ImGui::PushID("modes");
		const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
		for (int i = 0; i < M_COUNT; i++) {
			const float need = ImGui::CalcTextSize(mode_names[i]).x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
			if (i) {
				ImGui::SameLine();
				if (ImGui::GetCursorScreenPos().x + need > right)
					ImGui::NewLine();
			}
			ImGui::RadioButton(mode_names[i], &m_wm_mode, i);
		}
		ImGui::PopID();
	}
	if (was_mode != m_wm_mode)
		m_wm_stale = true;
	ImGui::Separator();

	const float w = ImGui::GetContentRegionAvail().x;
	ImDrawList *dl = ImGui::GetWindowDrawList();
	auto frame = [&](const ImVec2 &p, const ImVec2 &sz) {
		dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(16, 20, 26, 255), 4.0f);
		dl->AddRect(ImVec2(p.x - 1, p.y - 1), ImVec2(p.x + sz.x + 1, p.y + sz.y + 1), IM_COL32(110, 125, 150, 255), 4.0f, 0, 1.5f);
	};
	auto slider_f = [&](const char *label, float &v, float lo, float hi, const char *fmt = "%.2f") {
		ImGui::SetNextItemWidth(fs * 16);
		if (ImGui::SliderFloat(label, &v, lo, hi, fmt))
			m_wm_stale = true;
	};
	auto slider_i = [&](const char *label, int &v, int lo, int hi, const char *fmt = "%d") {
		ImGui::SetNextItemWidth(fs * 16);
		if (ImGui::SliderInt(label, &v, lo, hi, fmt))
			m_wm_stale = true;
	};
	// 基本の波形の選び（基本の波形とユニゾンで使う）
	auto shape_row = [&]() {
		const char *names[4] = { UI_TEXT(smp_make_sine, "Sine"), UI_TEXT(smp_make_saw, "Sawtooth"),
		                         UI_TEXT(smp_make_square, "Square"), UI_TEXT(smp_make_tri, "Triangle") };
		for (int i = 0; i < 4; i++) {
			if (i)
				ImGui::SameLine();
			if (ImGui::RadioButton(names[i], m_wm_shape == i)) {
				m_wm_shape = i;
				m_wm_stale = true;
			}
		}
		if (m_wm_shape == int(wg::shape::square))
			slider_f(UI_TEXT(smp_make_pulse, "Pulse width"), m_wm_pulse, 0.05f, 0.95f);
	};

	ImGui::PushID("body");
	ImGui::PushID(m_wm_mode);
	if (m_wm_mode == M_BASIC) {
		shape_row();
	} else if (m_wm_mode == M_BARS) {
		// 倍音を足す: 1-32 倍音の棒。縦に引いて高さを決める（棒の上をなぞれば続けて描ける）
		if (ImGui::SmallButton(UI_TEXT(smp_make_bars_one, "Fundamental only"))) {
			std::fill(std::begin(m_wm_bars), std::end(m_wm_bars), 0.0f);
			m_wm_bars[0] = 1.0f;
			m_wm_stale = true;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(UI_TEXT(smp_make_bars_all, "1/n (saw-like)"))) {
			for (int h = 0; h < 32; h++)
				m_wm_bars[h] = 1.0f / float(h + 1);
			m_wm_stale = true;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(UI_TEXT(smp_make_bars_odd, "Odd only (square-like)"))) {
			for (int h = 0; h < 32; h++)
				m_wm_bars[h] = (h & 1) ? 0.0f : 1.0f / float(h + 1);
			m_wm_stale = true;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(UI_TEXT(smp_make_bars_random, "Random"))) {
			const wg::spectrum r = wg::random_harmonics(++m_wm_seed, 32);
			for (int h = 0; h < 32; h++)
				m_wm_bars[h] = std::min(1.0f, r.b[h + 1]);
			m_wm_stale = true;
		}
		const ImVec2 p = ImGui::GetCursorScreenPos(), sz(w, fs * 7);
		ImGui::InvisibleButton("##bars", sz);
		frame(p, sz);
		const float bw = sz.x / 32.0f;
		if (ImGui::IsItemActive()) {
			const ImVec2 m = ImGui::GetIO().MousePos;
			const int h = std::clamp(int((m.x - p.x) / bw), 0, 31);
			m_wm_bars[h] = std::clamp(1.0f - (m.y - p.y) / sz.y, 0.0f, 1.0f);
			m_wm_stale = true;
		}
		for (int h = 0; h < 32; h++) {
			const float x0 = p.x + bw * float(h) + 1.0f, x1 = p.x + bw * float(h + 1) - 1.0f;
			dl->AddRectFilled(ImVec2(x0, p.y + sz.y * (1.0f - m_wm_bars[h])), ImVec2(x1, p.y + sz.y),
			                  IM_COL32(110, 200, 255, 255));
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", UI_TEXT(smp_make_bars_tip, "Drag up and down to set the level of harmonics 1 to 32 (left is the fundamental)"));
	} else if (m_wm_mode == M_DRAW) {
		// 手描き: 1 周期ぶんをマウスでなぞる。前の点とのあいだも埋める
		if (!m_wm_draw_init) {
			for (int i = 0; i < 256; i++)
				m_wm_draw[i] = float(std::sin(2 * wg::PI * (double(i) + 0.5) / 256.0));
			m_wm_draw_init = true;
		}
		if (ImGui::SmallButton(UI_TEXT(smp_make_draw_sine, "Start from a sine"))) {
			for (int i = 0; i < 256; i++)
				m_wm_draw[i] = float(std::sin(2 * wg::PI * (double(i) + 0.5) / 256.0));
			m_wm_stale = true;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(UI_TEXT(smp_make_draw_flat, "Flat"))) {
			std::fill(std::begin(m_wm_draw), std::end(m_wm_draw), 0.0f);
			m_wm_stale = true;
		}
		// 波形メモリ音源の形にする: 段数と bit 数
		ImGui::SameLine();
		ImGui::SetNextItemWidth(fs * 7);
		if (ImGui::Combo(UI_TEXT(smp_make_draw_steps, "Steps"), &m_wm_steps, "256\0" "64\0" "32\0" "16\0" "8\0"))
			m_wm_stale = true;
		ImGui::SameLine();
		ImGui::SetNextItemWidth(fs * 7);
		if (ImGui::Combo(UI_TEXT(smp_make_draw_bits, "Bits"), &m_wm_bits, "-\0" "8\0" "5\0" "4\0" "3\0" "2\0"))
			m_wm_stale = true;
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", UI_TEXT(smp_make_draw_chip_tip, "Fewer steps and bits give the stair-stepped waves of wavetable sound chips: 32 steps and 4 bits is the Game Boy's wave channel, 32 steps and 8 bits the SCC."));
		const ImVec2 p = ImGui::GetCursorScreenPos(), sz(w, fs * 9);
		ImGui::InvisibleButton("##draw", sz);
		frame(p, sz);
		dl->AddLine(ImVec2(p.x, p.y + sz.y * 0.5f), ImVec2(p.x + sz.x, p.y + sz.y * 0.5f), IM_COL32(80, 90, 110, 255));
		if (ImGui::IsItemActive()) {
			const ImVec2 m = ImGui::GetIO().MousePos;
			const int i = std::clamp(int((m.x - p.x) / sz.x * 256.0f), 0, 255);
			const float v = std::clamp(1.0f - 2.0f * (m.y - p.y) / sz.y, -1.0f, 1.0f);
			const int from = m_wm_draw_last < 0 ? i : m_wm_draw_last;
			const float v0 = m_wm_draw[from];
			const int lo = std::min(from, i), hi = std::max(from, i);
			for (int k = lo; k <= hi; k++)
				m_wm_draw[k] = hi == lo ? v : (k == i ? v : v0 + (v - v0) * float(k - from) / float(i - from));
			m_wm_draw_last = i;
			m_wm_stale = true;
		} else {
			m_wm_draw_last = -1;
		}
		for (int i = 0; i + 1 < 256; i++)
			dl->AddLine(ImVec2(p.x + sz.x * (float(i) + 0.5f) / 256.0f, p.y + sz.y * 0.5f * (1.0f - m_wm_draw[i])),
			            ImVec2(p.x + sz.x * (float(i) + 1.5f) / 256.0f, p.y + sz.y * 0.5f * (1.0f - m_wm_draw[i + 1])),
			            IM_COL32(255, 210, 90, 255), 1.5f);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", UI_TEXT(smp_make_draw_tip, "Drag to draw one cycle. What plays is this shape rebuilt from its first harmonics (below), so sharp corners are rounded"));
	} else if (m_wm_mode == M_NOISE) {
		const char *colors[3] = { UI_TEXT(smp_make_noise_white, "White"), UI_TEXT(smp_make_noise_pink, "Pink"),
		                          UI_TEXT(smp_make_noise_brown, "Brown") };
		for (int i = 0; i < 3; i++) {
			if (i)
				ImGui::SameLine();
			if (ImGui::RadioButton(colors[i], m_wm_noise_color == i)) {
				m_wm_noise_color = i;
				m_wm_stale = true;
			}
		}
		ImGui::TextWrapped("%s", UI_TEXT(smp_make_noise_note, "White noise, one second, looped. It has no pitch, so every key plays it faster or slower."));
	} else if (m_wm_mode == M_FC) {
		// ファミコン（2A03）。矩形 4 つと三角は高さのある波形、ノイズ 2 つは高さの無いサンプル
		const char *names[7] = { UI_TEXT(smp_make_fc_p12, "Pulse 12.5%"), UI_TEXT(smp_make_fc_p25, "Pulse 25%"),
		                         UI_TEXT(smp_make_fc_p50, "Pulse 50%"), UI_TEXT(smp_make_fc_p75, "Pulse 75%"),
		                         UI_TEXT(smp_make_fc_tri, "Triangle"), UI_TEXT(smp_make_fc_noise, "Noise (long)"),
		                         UI_TEXT(smp_make_fc_noise_short, "Noise (short)") };
		for (int i = 0; i < 7; i++) {
			if (i && i != 4)
				ImGui::SameLine();
			if (ImGui::RadioButton(names[i], m_wm_fc == i)) {
				m_wm_fc = i;
				m_wm_stale = true;
			}
		}
		ImGui::TextWrapped("%s", UI_TEXT(smp_make_fc_note, "The Famicom / NES sound chip: four pulse duties, the 4-bit stepped triangle, and its shift-register noise (long, and the short metallic one). For the raw chip sound keep Highest harmonic at 64."));
	} else if (m_wm_mode == M_FM) {
		// FM（2 オペレーター）。比は整数（1 周期で閉じるので、そのままループになる）
		struct preset { const char *name; int c, m; float index, fb; };
		const preset presets[] = {
			{ UI_TEXT(smp_make_fm_epiano, "E. piano"), 1, 14, 0.7f, 0.0f },
			{ UI_TEXT(smp_make_fm_bell, "Bell"), 2, 7, 3.0f, 0.0f },
			{ UI_TEXT(smp_make_fm_brass, "Brass"), 1, 1, 2.5f, 0.3f },
			{ UI_TEXT(smp_make_fm_organ, "Organ"), 1, 2, 1.2f, 0.0f },
			{ UI_TEXT(smp_make_fm_bass, "Bass"), 1, 1, 4.0f, 0.6f },
			{ UI_TEXT(smp_make_fm_clav, "Clavi"), 1, 3, 2.2f, 0.2f },
		};
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", UI_TEXT(smp_make_presets, "Presets"));
		for (size_t i = 0; i < std::size(presets); i++) {
			ImGui::SameLine();
			if (ImGui::SmallButton(presets[i].name)) {
				m_wm_fm_c = presets[i].c;
				m_wm_fm_m = presets[i].m;
				m_wm_fm_index = presets[i].index;
				m_wm_fm_fb = presets[i].fb;
				m_wm_stale = true;
			}
		}
		slider_i(UI_TEXT(smp_make_fm_c, "Carrier ratio"), m_wm_fm_c, 1, 8);
		slider_i(UI_TEXT(smp_make_fm_m, "Modulator ratio"), m_wm_fm_m, 1, 16);
		slider_f(UI_TEXT(smp_make_fm_index, "Modulation depth"), m_wm_fm_index, 0.0f, 12.0f);
		slider_f(UI_TEXT(smp_make_fm_fb, "Feedback"), m_wm_fm_fb, 0.0f, 1.0f);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", UI_TEXT(smp_make_fm_tip, "Two-operator FM: the modulator bends the carrier's phase. Depth 0 is a plain sine; more depth adds harmonics spaced by the modulator ratio. Feedback lets the modulator modulate itself (towards a sawtooth). The wave is one fixed spectrum: shape its change over time with the voice's envelope, or layer two waves in different elements."));
	} else if (m_wm_mode == M_ORGAN) {
		// ドローバー 9 本。縦のつまみ（下げるほど大きい、の本物とは逆で、上げるほど大きい）
		struct preset { const char *name; int b[9]; };
		const preset presets[] = {
			{ UI_TEXT(smp_make_organ_full, "Full"), { 8, 8, 8, 8, 8, 8, 8, 8, 8 } },
			{ UI_TEXT(smp_make_organ_jazz, "Jazz"), { 8, 8, 8, 0, 0, 0, 0, 0, 0 } },
			{ UI_TEXT(smp_make_organ_gospel, "Gospel"), { 8, 8, 8, 8, 0, 0, 0, 0, 8 } },
			{ UI_TEXT(smp_make_organ_flute, "Flute"), { 0, 0, 8, 4, 0, 2, 0, 0, 0 } },
			{ UI_TEXT(smp_make_organ_reed, "Reed"), { 0, 0, 6, 8, 7, 6, 5, 4, 3 } },
		};
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", UI_TEXT(smp_make_presets, "Presets"));
		for (size_t i = 0; i < std::size(presets); i++) {
			ImGui::SameLine();
			if (ImGui::SmallButton(presets[i].name)) {
				std::copy(std::begin(presets[i].b), std::end(presets[i].b), m_wm_organ);
				m_wm_stale = true;
			}
		}
		static const char *feet[9] = { "16'", "5 1/3'", "8'", "4'", "2 2/3'", "2'", "1 3/5'", "1 1/3'", "1'" };
		for (int i = 0; i < 9; i++) {
			if (i)
				ImGui::SameLine();
			ImGui::PushID(i);
			ImGui::BeginGroup();
			if (ImGui::VSliderInt("##bar", ImVec2(fs * 2.2f, fs * 7), &m_wm_organ[i], 0, 8))
				m_wm_stale = true;
			ImGui::TextUnformatted(feet[i]);
			ImGui::EndGroup();
			ImGui::PopID();
		}
		ImGui::TextWrapped("%s", UI_TEXT(smp_make_organ_note, "Nine drawbars, 3 dB a step. 8' plays C3 at key 60; 16' sounds an octave below it and 5 1/3' a fifth above, so the loop is twice as long (0.2 s)."));
	} else if (m_wm_mode == M_UNISON) {
		shape_row();
		slider_i(UI_TEXT(smp_make_uni_voices, "Voices"), m_wm_uni_voices, 2, 9);
		char cents[32];
		std::snprintf(cents, sizeof(cents), "%.1f cent", wg::unison_cents(m_wm_uni_step));
		slider_i(UI_TEXT(smp_make_uni_detune, "Detune"), m_wm_uni_step, 1, 6, cents);
		ImGui::TextWrapped("%s", UI_TEXT(smp_make_uni_note, "Several copies of the shape, each tuned a little apart, in one sample (a supersaw with Sawtooth). The beating repeats every 0.76 s, the length of the loop; detune moves in steps of 8.6 cents so that every copy closes at the loop point."));
	} else if (m_wm_mode == M_VOWEL) {
		const char *vowels[5] = { "A", "I", "U", "E", "O" };
		for (int i = 0; i < 5; i++) {
			if (i)
				ImGui::SameLine();
			if (ImGui::SmallButton(vowels[i])) {
				m_wm_vowel = float(i);
				m_wm_stale = true;
			}
		}
		slider_f(UI_TEXT(smp_make_vowel_pos, "Vowel (A I U E O)"), m_wm_vowel, 0.0f, 4.0f);
		if (ImGui::Checkbox(UI_TEXT(smp_make_vowel_morph, "Morph to a second vowel and back"), &m_wm_vowel_morph))
			m_wm_stale = true;
		if (m_wm_vowel_morph)
			slider_f(UI_TEXT(smp_make_vowel_to, "Second vowel"), m_wm_vowel_to, 0.0f, 4.0f);
		ImGui::TextWrapped("%s", UI_TEXT(smp_make_vowel_note, "A sawtooth shaped by the three formants of a vowel. The formants are right at key 60 and move with the key, so it sounds most like a voice within an octave or so of C3."));
	} else if (m_wm_mode == M_SYNC) {
		slider_f(UI_TEXT(smp_make_sync_ratio, "Slave ratio"), m_wm_sync, 1.0f, 12.0f);
		ImGui::TextWrapped("%s", UI_TEXT(smp_make_sync_note, "Hard sync: a sawtooth running this many times faster is forced back to its start once a cycle. In-between ratios give the tearing sync-lead sound."));
	} else if (m_wm_mode == M_PWM) {
		slider_f(UI_TEXT(smp_make_pwm_center, "Pulse width"), m_wm_pwm_center, 0.1f, 0.9f);
		slider_f(UI_TEXT(smp_make_pwm_depth, "Sweep depth"), m_wm_pwm_depth, 0.0f, 0.45f);
		slider_i(UI_TEXT(smp_make_pwm_sweeps, "Sweeps per loop"), m_wm_pwm_sweeps, 1, 8);
		ImGui::TextWrapped("%s", UI_TEXT(smp_make_pwm_note, "A pulse whose width swings back and forth, baked into a 0.76 s loop (one sweep per loop is 1.3 Hz at key 60; higher keys sweep faster)."));
	} else if (m_wm_mode == M_PLUCK) {
		slider_f(UI_TEXT(smp_make_pluck_sustain, "Sustain"), m_wm_pluck_sustain, 0.0f, 1.0f);
		slider_f(UI_TEXT(smp_make_pluck_bright, "Brightness"), m_wm_pluck_bright, 0.0f, 1.0f);
		slider_f(UI_TEXT(smp_make_length, "Length (s)"), m_wm_pluck_len, 0.3f, 4.0f, "%.1f");
		if (ImGui::SmallButton(UI_TEXT(smp_make_pluck_again, "Pluck again"))) {
			m_wm_seed++;
			m_wm_stale = true;
		}
		ImGui::TextWrapped("%s", UI_TEXT(smp_make_pluck_note, "A plucked string (Karplus-Strong): a burst of noise goes round a delay one cycle long and is softened each time. It plays once and dies away (no loop). Each pluck is a little different."));
	} else if (m_wm_mode == M_DRUM) {
		const char *kinds[6] = { UI_TEXT(smp_make_drum_kick, "Kick"), UI_TEXT(smp_make_drum_snare, "Snare"),
		                         UI_TEXT(smp_make_drum_tom, "Tom"), UI_TEXT(smp_make_drum_hat, "Hi-hat"),
		                         UI_TEXT(smp_make_drum_clap, "Clap"), UI_TEXT(smp_make_drum_cowbell, "Cowbell") };
		for (int i = 0; i < 6; i++) {
			if (i)
				ImGui::SameLine();
			if (ImGui::RadioButton(kinds[i], m_wm_drum == i)) {
				m_wm_drum = i;
				m_wm_stale = true;
			}
		}
		slider_f(UI_TEXT(smp_make_drum_tune, "Tune"), m_wm_drum_tune, 0.0f, 1.0f);
		slider_f(UI_TEXT(smp_make_drum_decay, "Decay"), m_wm_drum_decay, 0.0f, 1.0f);
		slider_f(UI_TEXT(smp_make_drum_tone, "Tone"), m_wm_drum_tone, 0.0f, 1.0f);
		ImGui::TextWrapped("%s", UI_TEXT(smp_make_drum_note, "Drum sounds in the manner of analogue rhythm machines, played once (no loop). Tone is the click of the kick and tom, the snares of the snare, the noise of the hi-hat, the gap between the claps, the brightness of the cowbell. Give each its own key range in a voice to build a kit."));
	} else if (m_wm_mode == M_PD) {
		const char *kinds[3] = { UI_TEXT(smp_make_saw, "Sawtooth"), UI_TEXT(smp_make_square, "Square"),
		                         UI_TEXT(smp_make_pd_reso, "Resonance") };
		for (int i = 0; i < 3; i++) {
			if (i)
				ImGui::SameLine();
			if (ImGui::RadioButton(kinds[i], m_wm_pd == i)) {
				m_wm_pd = i;
				m_wm_stale = true;
			}
		}
		slider_f(UI_TEXT(smp_make_pd_amount, "Distortion"), m_wm_pd_amount, 0.0f, 1.0f);
		if (m_wm_pd == 2)
			slider_f(UI_TEXT(smp_make_pd_ratio, "Resonance pitch"), m_wm_pd_ratio, 1.0f, 16.0f, "%.1f");
		ImGui::TextWrapped("%s", UI_TEXT(smp_make_pd_note, "Phase distortion, the Casio CZ way: a cosine is read at an uneven speed, so more distortion bends it from a sine towards a sawtooth or a square. Resonance is a faster cosine under a window, like a filter ringing at that pitch."));
	} else if (m_wm_mode == M_PAINT) {
		// 絵で描く: 行がサイン 1 本（下が低い）、横がループの中の時間。左で描き、右（か Shift）で消す
		constexpr int R = wg::PAINT_ROWS, C = wg::PAINT_COLS;
		auto clear = [&]() { std::fill(std::begin(m_wm_paint), std::end(m_wm_paint), 0.0f); };
		auto sweep = [&]() {
			// 基音を弱く鳴らしっぱなしにして、その上を倍音が 1 つずつ上がっていく
			clear();
			for (int c = 0; c < C; c++) {
				m_wm_paint[c] = 0.5f;
				const int r = 1 + c * (R / 2 - 2) / (C - 1);
				m_wm_paint[r * C + c] = 1.0f;
				m_wm_paint[(r + 1) * C + c] = 0.5f;
			}
		};
		if (!m_wm_paint_init) {
			sweep();
			m_wm_paint_init = true;
		}
		if (ImGui::SmallButton(UI_TEXT(smp_make_paint_clear, "Clear"))) {
			clear();
			m_wm_stale = true;
		}
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", UI_TEXT(smp_make_presets, "Presets"));
		ImGui::SameLine();
		if (ImGui::SmallButton(UI_TEXT(smp_make_paint_sweep, "Rising sweep"))) {
			sweep();
			m_wm_stale = true;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(UI_TEXT(smp_make_paint_blink, "Blink"))) {
			// 和音のような倍音の組を、ループの中で 4 回つけたり消したりする
			clear();
			static const int rows[] = { 0, 1, 2, 4, 5, 7, 9 };
			for (int c = 0; c < C; c++)
				if ((c / (C / 8)) % 2 == 0)
					for (int r : rows)
						m_wm_paint[r * C + c] = 1.0f / float(1 + r / 3);
			m_wm_stale = true;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(UI_TEXT(smp_make_paint_dots, "Sparkle"))) {
			// 短い点をでたらめに散らす（押すたびに並びが変わる）
			clear();
			u32 x = (++m_wm_seed) * 2654435761u + 12345u;
			auto next = [&]() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; };
			for (int n = 0; n < 40; n++) {
				const int r = int(next() % u32(R * 2 / 3)), c = int(next() % u32(C));
				for (int k = 0; k < 3; k++)
					m_wm_paint[r * C + (c + k) % C] = k == 0 ? 1.0f : k == 1 ? 0.6f : 0.3f;
			}
			m_wm_stale = true;
		}
		ImGui::SetNextItemWidth(fs * 8);
		ImGui::SliderFloat(UI_TEXT(smp_make_paint_level, "Brush level"), &m_wm_paint_level, 0.1f, 1.0f, "%.2f");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(fs * 6);
		ImGui::SliderInt(UI_TEXT(smp_make_paint_size, "Brush size"), &m_wm_paint_size, 1, 4);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(fs * 8);
		if (ImGui::SliderFloat(UI_TEXT(smp_make_paint_spacing, "Row spacing"), &m_wm_paint_spacing, 0.25f, 2.0f, "%.2f"))
			m_wm_stale = true;
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", UI_TEXT(smp_make_paint_spacing_tip, "1.00 puts the rows on harmonics 1 to 48 of the note. Other values spread or squeeze them so they are no longer harmonics, which gives bell and metal tones."));

		const ImVec2 p = ImGui::GetCursorScreenPos(), sz(w, fs * 15);
		ImGui::InvisibleButton("##paint", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
		frame(p, sz);
		const float cw = sz.x / float(C), ch = sz.y / float(R);
		if (ImGui::IsItemActive()) {
			const ImVec2 m = ImGui::GetIO().MousePos;
			const int c = std::clamp(int((m.x - p.x) / cw), 0, C - 1);
			const int r = std::clamp(R - 1 - int((m.y - p.y) / ch), 0, R - 1);
			const bool erase = ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::GetIO().KeyShift;
			const float v = erase ? 0.0f : m_wm_paint_level;
			// 前の桝からここまでを埋める（速く動かしても線が切れない）
			const int r0 = m_wm_paint_last_r < 0 ? r : m_wm_paint_last_r, c0 = m_wm_paint_last_c < 0 ? c : m_wm_paint_last_c;
			const int steps = std::max({ std::abs(r - r0), std::abs(c - c0), 1 });
			for (int k = 0; k <= steps; k++) {
				const int rr = r0 + (r - r0) * k / steps, cc = c0 + (c - c0) * k / steps;
				for (int dr = 0; dr < m_wm_paint_size; dr++)
					for (int dc = 0; dc < m_wm_paint_size; dc++) {
						const int pr = rr + dr - (m_wm_paint_size - 1) / 2, pc = cc + dc - (m_wm_paint_size - 1) / 2;
						if (pr >= 0 && pr < R && pc >= 0 && pc < C)
							m_wm_paint[pr * C + pc] = v;
					}
			}
			m_wm_paint_last_r = r;
			m_wm_paint_last_c = c;
			m_wm_stale = true;
		} else {
			m_wm_paint_last_r = m_wm_paint_last_c = -1;
		}
		// 目盛り: 8 列ごと（ループの 1/8）と、倍音 2・4・8・16・32 の行
		for (int c = 8; c < C; c += 8)
			dl->AddLine(ImVec2(p.x + cw * float(c), p.y), ImVec2(p.x + cw * float(c), p.y + sz.y), IM_COL32(44, 52, 66, 255));
		for (int h = 2; h <= R; h *= 2)
			dl->AddLine(ImVec2(p.x, p.y + sz.y - ch * float(h - 1)), ImVec2(p.x + sz.x, p.y + sz.y - ch * float(h - 1)), IM_COL32(44, 52, 66, 255));
		for (int r = 0; r < R; r++)
			for (int c = 0; c < C; c++) {
				const float v = m_wm_paint[r * C + c];
				if (v <= 0.0f)
					continue;
				const ImVec2 a(p.x + cw * float(c), p.y + sz.y - ch * float(r + 1));
				dl->AddRectFilled(a, ImVec2(a.x + cw, a.y + ch),
				                  IM_COL32(int(30 + 80 * v), int(60 + 140 * v), int(90 + 165 * v), 255));
			}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", UI_TEXT(smp_make_paint_tip, "Drag with the left button to paint and with the right button (or Shift) to erase. Up is higher, across is one pass through the loop."));
		ImGui::TextWrapped("%s", UI_TEXT(smp_make_paint_note, "A painted loop, after the ANS synthesizer: each row is one sine, across is time, and brightness is level. The picture becomes a 0.76 s loop that closes without a click. Higher keys play it faster, as with any sample."));
	} else if (m_wm_mode == M_BELL) {
		struct preset { const char *name; float ratio, index, decay; };
		const preset presets[] = {
			{ UI_TEXT(smp_make_bell_tubular, "Tubular"), 3.5f, 5.0f, 1.2f },
			{ UI_TEXT(smp_make_bell_glass, "Glass"), 7.07f, 2.0f, 0.6f },
			{ UI_TEXT(smp_make_bell_gong, "Gong"), 1.41f, 8.0f, 1.8f },
			{ UI_TEXT(smp_make_bell_marimba, "Marimba"), 4.0f, 1.5f, 0.25f },
		};
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", UI_TEXT(smp_make_presets, "Presets"));
		for (size_t i = 0; i < std::size(presets); i++) {
			ImGui::SameLine();
			if (ImGui::SmallButton(presets[i].name)) {
				m_wm_bell_ratio = presets[i].ratio;
				m_wm_bell_index = presets[i].index;
				m_wm_bell_decay = presets[i].decay;
				m_wm_stale = true;
			}
		}
		slider_f(UI_TEXT(smp_make_bell_ratio, "Modulator ratio"), m_wm_bell_ratio, 0.5f, 12.0f);
		slider_f(UI_TEXT(smp_make_fm_index, "Modulation depth"), m_wm_bell_index, 0.0f, 12.0f);
		slider_f(UI_TEXT(smp_make_bell_decay, "Decay (s)"), m_wm_bell_decay, 0.05f, 2.5f);
		ImGui::TextWrapped("%s", UI_TEXT(smp_make_bell_note, "An FM bell that plays once: a ratio that is not a whole number gives the clang of metal, and the brightness fades faster than the level, as a struck bell does."));
	} else {
		slider_f(UI_TEXT(smp_make_fold_gain, "Fold amount"), m_wm_fold_gain, 0.1f, 12.0f);
		slider_f(UI_TEXT(smp_make_fold_bias, "Asymmetry"), m_wm_fold_bias, 0.0f, 1.57f);
		ImGui::TextWrapped("%s", UI_TEXT(smp_make_fold_note, "Wavefolding: a sine pushed past its limit folds back on itself. More amount adds harmonics; asymmetry brings in the even ones."));
	}

	ImGui::PopID();
	ImGui::PopID();

	// ---- 共通: 足す倍音の上限と大きさ（高さの無いノイズは、倍音にせずそのままサンプルにする）
	const bool unpitched = m_wm_mode == M_NOISE || (m_wm_mode == M_FC && m_wm_fc >= 5);
	const bool multi = m_wm_mode == M_ORGAN || m_wm_mode == M_UNISON || m_wm_mode == M_PWM || m_wm_mode == M_PAINT || (m_wm_mode == M_VOWEL && m_wm_vowel_morph);   // 倍音にならない成分を含む（長いループ）
	const bool oneshot = m_wm_mode == M_PLUCK || m_wm_mode == M_DRUM || m_wm_mode == M_BELL;       // 1 度だけ鳴って消える（ループを入れない）
	ImGui::Separator();
	if (!unpitched && !oneshot && m_wm_mode != M_ORGAN && m_wm_mode != M_PAINT) {
		slider_i(UI_TEXT(smp_make_max_h, "Highest harmonic"), m_wm_max_h, 1, wg::HARMONICS);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", UI_TEXT(smp_make_max_h_tip, "Harmonics above this are left out. Fewer gives a rounder sound and less aliasing on high keys"));
	}
	slider_i(UI_TEXT(smp_make_level, "Peak level (%)"), m_wm_level, 1, 100);
	// ローファイ: bit 数と、同じ値を続けるサンプル数
	ImGui::SetNextItemWidth(fs * 7.5f);
	if (ImGui::SliderInt("##lofibits", &m_wm_lofi_bits, 2, 16, "%d bit"))
		m_wm_stale = true;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 7.5f);
	if (ImGui::SliderInt(UI_TEXT(smp_make_lofi, "Lo-fi"), &m_wm_lofi_hold, 1, 16, "1/%d"))
		m_wm_stale = true;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(smp_make_lofi_tip, "Rounds the finished wave to fewer bits and holds each value for several samples (a lower sample rate), like an old sampler. 16 bit and 1/1 leave it alone."));

	// ---- 作り直す
	const bool rebuilt = m_wm_stale;
	if (m_wm_stale) {
		m_wm_stale = false;
		const double level = m_wm_level / 100.0;
		m_wm_spec = wg::spectrum{};
		if (oneshot) {
			m_wm_pcm = m_wm_mode == M_PLUCK ? wg::pluck(m_wm_pluck_len, m_wm_pluck_sustain, m_wm_pluck_bright, m_wm_seed + 1, level)
			         : m_wm_mode == M_BELL  ? wg::fm_bell(std::min(3.0, double(m_wm_bell_decay) * 4), m_wm_bell_ratio, m_wm_bell_index, m_wm_bell_decay, level)
			                                : wg::drum_hit(wg::drum(m_wm_drum), m_wm_drum_tune, m_wm_drum_decay, m_wm_drum_tone, level);
			// 見せるのは全体。600 の桝ごとに、いちばん大きく振れた値
			m_wm_cycle.assign(600, 0.0f);
			for (size_t i = 0; i < m_wm_pcm.size(); i++) {
				float &c = m_wm_cycle[i * 600 / m_wm_pcm.size()];
				const float v = float(m_wm_pcm[i]) / 32768.0f;
				if (std::fabs(v) > std::fabs(c))
					c = v;
			}
		} else if (unpitched) {
			m_wm_cycle.clear();
			m_wm_pcm = m_wm_mode == M_NOISE ? wg::noise(44100, level, 1, m_wm_noise_color)
			                                : wg::famicom_noise(m_wm_fc == 6, level, m_wm_fc == 6 ? 4 : 2);
		} else {
			switch (m_wm_mode) {
			case M_BASIC: case M_UNISON: m_wm_spec = wg::basic(wg::shape(m_wm_shape), m_wm_pulse); break;
			case M_BARS:  m_wm_spec = wg::from_bars(m_wm_bars, 32); break;
			case M_DRAW: {
				static const int STEPS[5] = { 0, 64, 32, 16, 8 }, BITS[6] = { 0, 8, 5, 4, 3, 2 };
				m_wm_spec = m_wm_steps || m_wm_bits ? wg::from_cycle_stepped(m_wm_draw, 256, STEPS[m_wm_steps], BITS[m_wm_bits])
				                                    : wg::from_cycle(m_wm_draw, 256);
				break;
			}
			case M_FC:    m_wm_spec = wg::famicom(wg::famicom_wave(m_wm_fc)); break;
			case M_FM:    m_wm_spec = wg::fm(m_wm_fm_c, m_wm_fm_m, m_wm_fm_index, m_wm_fm_fb); break;
			case M_VOWEL: m_wm_spec = wg::vowel(m_wm_vowel); break;
			case M_SYNC:  m_wm_spec = wg::sync(m_wm_sync); break;
			case M_FOLD:  m_wm_spec = wg::fold(m_wm_fold_gain, m_wm_fold_bias); break;
			case M_PD:    m_wm_spec = wg::phase_distortion(wg::pd_wave(m_wm_pd), m_wm_pd_amount, m_wm_pd_ratio); break;
			default: break;
			}
			if (m_wm_mode == M_ORGAN)
				m_wm_pcm = wg::render_partials(wg::organ(m_wm_organ), wg::ORGAN_MULT, level);
			else if (m_wm_mode == M_VOWEL && m_wm_vowel_morph)
				m_wm_pcm = wg::vowel_morph(m_wm_vowel, m_wm_vowel_to, m_wm_max_h, level);
			else if (m_wm_mode == M_PAINT)
				m_wm_pcm = wg::paint(m_wm_paint, m_wm_paint_spacing, level);
			else if (m_wm_mode == M_PWM)
				m_wm_pcm = wg::pwm(m_wm_pwm_center, m_wm_pwm_depth, m_wm_pwm_sweeps, m_wm_max_h, level);
			else if (m_wm_mode == M_UNISON)
				m_wm_pcm = wg::render_partials(wg::unison(m_wm_spec, m_wm_uni_voices, m_wm_uni_step, m_wm_max_h), wg::UNISON_MULT, level);
			else
				m_wm_pcm = wg::render(m_wm_spec, m_wm_max_h, level);
			if (m_wm_mode == M_PAINT) {
				// 絵は時間で変わるので、見せるのはループの全体（600 の桝ごとに、いちばん大きく振れた値）
				m_wm_cycle.assign(600, 0.0f);
				for (size_t i = 0; i < m_wm_pcm.size(); i++) {
					float &c = m_wm_cycle[i * 600 / m_wm_pcm.size()];
					const float v = float(m_wm_pcm[i]) / 32768.0f;
					if (std::fabs(v) > std::fabs(c))
						c = v;
				}
			} else if (multi) {
				// 見せるのはループの頭の 4 周期ぶん（8' の高さで）
				const size_t n = std::min<size_t>(m_wm_pcm.size(), size_t(wg::LOOP_FRAMES) * 4 / wg::CYCLES);
				m_wm_cycle.assign(n, 0.0f);
				for (size_t i = 0; i < n; i++)
					m_wm_cycle[i] = float(m_wm_pcm[i]) / 32768.0f;
			} else {
				m_wm_cycle = wg::cycle(m_wm_spec, 256, m_wm_max_h);
			}
		}
		wg::lofi(m_wm_pcm, m_wm_lofi_bits, m_wm_lofi_hold);
	}

	// ---- できた形（1 周期）と倍音の分布
	if (!unpitched) {
		const float half = multi || oneshot ? w : (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
		const ImVec2 p = ImGui::GetCursorScreenPos(), sz(half, fs * 6);
		ImGui::Dummy(sz);
		frame(p, sz);
		dl->AddLine(ImVec2(p.x, p.y + sz.y * 0.5f), ImVec2(p.x + sz.x, p.y + sz.y * 0.5f), IM_COL32(80, 90, 110, 255));
		float peak = 1e-6f;
		for (float v : m_wm_cycle)
			peak = std::max(peak, std::fabs(v));
		for (size_t i = 0; i + 1 < m_wm_cycle.size(); i++)
			dl->AddLine(ImVec2(p.x + sz.x * float(i) / float(m_wm_cycle.size() - 1), p.y + sz.y * 0.5f * (1.0f - 0.92f * m_wm_cycle[i] / peak)),
			            ImVec2(p.x + sz.x * float(i + 1) / float(m_wm_cycle.size() - 1), p.y + sz.y * 0.5f * (1.0f - 0.92f * m_wm_cycle[i + 1] / peak)),
			            IM_COL32(110, 200, 255, 255), 1.5f);
		if (oneshot) {
			ImGui::TextDisabled(UI_TEXT(smp_make_view_once_fmt, "The whole sound (%.2f s). It plays once, without a loop."), double(m_wm_pcm.size()) / sp::SAMPLE_RATE);
		} else if (m_wm_mode == M_PAINT) {
			ImGui::TextDisabled(UI_TEXT(smp_make_view_loop_fmt, "The whole loop (%.2f s)."), double(m_wm_pcm.size()) / sp::SAMPLE_RATE);
		} else if (multi) {
			ImGui::TextDisabled(UI_TEXT(smp_make_view_multi_fmt, "The first 4 cycles of the loop (%.2f s in all)."), double(m_wm_pcm.size()) / sp::SAMPLE_RATE);
		} else {
			ImGui::SameLine();
			const ImVec2 q = ImGui::GetCursorScreenPos(), sq(half, fs * 6);
			ImGui::Dummy(sq);
			frame(q, sq);
			double top = 1e-9;
			for (int h = 1; h <= wg::HARMONICS; h++)
				top = std::max(top, m_wm_spec.mag(h));
			const float bw = sq.x / float(wg::HARMONICS);
			for (int h = 1; h <= wg::HARMONICS; h++) {
				// 高さは dB（-60dB を下の端に）
				const double db = 20 * std::log10(std::max(m_wm_spec.mag(h) / top, 1e-6));
				const float t = float(std::clamp(1.0 + db / 60.0, 0.0, 1.0));
				if (t <= 0)
					continue;
				dl->AddRectFilled(ImVec2(q.x + bw * float(h - 1) + 0.5f, q.y + sq.y * (1.0f - t)),
				                  ImVec2(q.x + bw * float(h) - 0.5f, q.y + sq.y),
				                  h <= m_wm_max_h ? IM_COL32(110, 200, 255, 255) : IM_COL32(70, 80, 95, 255));
			}
			ImGui::TextDisabled("%s", UI_TEXT(smp_make_view_note, "Left: one cycle as it will play. Right: harmonics 1-64 (dB; grey ones are left out)."));
		}
	}

	// ---- 試聴と登録。鳴らしている間に値を変えたら、その場で新しい波形に差し替える（ループの位置はそのまま）
	const bool playing = m_view.preview_number == -1;
	if (rebuilt && playing && !m_wm_pcm.empty()) {
		std::vector<s16> pcm = m_wm_pcm;
		const bool keep = !oneshot;          // 1 度だけ鳴る音は頭から鳴らし直す
		br.post([pcm, keep](mu2000 &mu) mutable {
			mu.preview_pcm(std::move(pcm), 0, keep);
			return std::string();
		});
	}
	ImGui::BeginDisabled(m_wm_pcm.empty());
	if (!playing) {
		if (ImGui::Button(UI_TEXT(smp_play, "Play"))) {
			std::vector<s16> pcm = m_wm_pcm;
			br.post([pcm](mu2000 &mu) mutable {
				mu.preview_pcm(std::move(pcm), 0);   // 頭へ戻ってくり返す（止めるまで）
				return std::string();
			});
		}
	} else if (ImGui::Button(UI_TEXT(smp_play_stop, "Stop playing"))) {
		br.post([](mu2000 &mu) {
			mu.preview_stop();
			return std::string();
		});
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", UI_TEXT(smp_make_play_tip, "Plays the wave on the PC at the pitch of key 60 (C3), looping, without adding it to the MU2000. While it plays, every change you make is heard at once."));
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_name, "Name"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 8);
	ImGui::InputText("##wmname", m_wm_name, sizeof(m_wm_name));
	ImGui::SameLine();
	if (ImGui::Button(UI_TEXT(smp_make_add, "Add as a sample"))) {
		// ループで鳴らすものは、終わりに頭の 4 サンプルを足す（音源はその手前で折り返すので、ループの長さが波形ちょうどになる）
		std::vector<s16> pcm = oneshot ? m_wm_pcm : wg::with_loop_tail(m_wm_pcm);
		const std::string name = m_wm_name;
		const bool assign = m_wm_assign;
		const int slot = m_bank * 128 + (m_pgm - 1);
		const bool loop = !oneshot;                                          // プラックとドラムは 1 度だけ鳴らす
		const std::string done = loop ? UI_TEXT(smp_make_added_fmt, "Added sample %03d (%s), looped")
		                              : UI_TEXT(smp_make_added_once_fmt, "Added sample %03d (%s), no loop");
		br.post([pcm, name, assign, slot, done, loop](mu2000 &mu) {
			std::string err;
			const int n = mu.sampling_add(pcm.data(), pcm.size(), name, err);
			if (!n)
				return err;
			if (loop)
				mu.sampling_loop(n, true, 0);      // 全体をくり返す
			if (assign) {
				// 割り当ての欄の音色の要素 1 に入れる（ほかの値はそのまま）
				sp::voice v;
				mu.sampling_voice(slot, v);
				v.el[0].on = true;
				v.el[0].assigned = true;
				v.el[0].sample = n;
				v.el[0].rom_wave = -1;
				mu.sampling_set_voice(slot, v, err);
			}
			char buf[120];
			std::snprintf(buf, sizeof(buf), done.c_str(), n, name.c_str());
			return std::string(buf);
		});
		m_dirty = false;       // 割り当ての欄を、書き換わった音色で読み直す
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", UI_TEXT(smp_make_add_tip, "Writes the wave into the sampling RAM as a new sample with the loop on. At key 60 it plays C3, so it needs no pitch correction. It uses about 0.1 second of the sampling memory."));
	char abuf[96];
	std::snprintf(abuf, sizeof(abuf), UI_TEXT(smp_make_assign_fmt, "Also put it in element 1 of Bank# %d, program %d"), m_bank, m_pgm);
	ImGui::Checkbox(abuf, &m_wm_assign);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(smp_make_assign_tip, "The voice chosen on the Voice tab. Play it with bank MSB 16; level, pan, envelope and the other elements are set there."));
	user_board_pane(br, oneshot);
}

// ---- オリジナルのボード。上で作った波形を、ボードのプログラム番号に入れていく。
// 入れられるのはこのタブで計算した波形（m_wm_pcm）だけ。サンプリング RAM のサンプルや内蔵ウェーブを入れる道は作らない
void sampling_editor::user_board_pane(bridge &br, bool oneshot)
{
	namespace ub = user_boards;
	namespace vb = smu2000::vboard;
	const float fs = ImGui::GetFontSize();
	ImGui::SeparatorText(UI_TEXT(smp_ub_title, "Your own plug-in board"));
	const double now = ImGui::GetTime();
	if (m_ub_listed < 0 || now - m_ub_listed > 1.0) {
		m_ub_listed = now;
		m_ub_list = ub::list();
	}
	std::shared_ptr<const ub::board> cur = ub::current();
	{
		const std::string stem = ub::current_stem();
		ImGui::SetNextItemWidth(fs * 11);
		if (ImGui::BeginCombo("##ubfile", cur ? cur->name : UI_TEXT(me_board_user_none, "(no board)"))) {
			for (const std::string &s : m_ub_list)
				if (ImGui::Selectable(s.c_str(), s == stem) && s != stem) {
					std::string err;
					m_ub_note = ub::open(br, s, err) ? std::string() : err;
				}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		if (ImGui::Button(UI_TEXT(smp_ub_new, "New board"))) {
			ub::create(br);
			m_ub_listed = -1;
			m_ub_note.clear();
		}
		cur = ub::current();
	}
	if (!cur) {
		ImGui::TextWrapped("%s", UI_TEXT(smp_ub_intro, "Collect the waves you make here into a plug-in board of your own: one wave per program number, each with a name. Plug it in from the Master window (Imaginary plug-in board) and pick the waves with program change. Only waves computed on this tab can go in."));
		if (!m_ub_note.empty())
			ImGui::TextDisabled("%s", m_ub_note.c_str());
		return;
	}
	// 変えた中身を作る: いまのボードを写して（波形は共有）、f で書き換えて渡す
	const auto change = [&](const std::function<void(ub::board &)> &f) {
		auto nb = std::make_shared<ub::board>(*cur);
		f(*nb);
		ub::commit(br, nb);
		cur = nb;
	};
	// ---- 名前。変えるとファイルの名前も変わる
	const std::string path = ub::current_path();
	if (m_ub_name_for != path && !ImGui::IsAnyItemActive()) {
		m_ub_name_for = path;
		std::snprintf(m_ub_name, sizeof(m_ub_name), "%s", cur->name);
	}
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_ub_board_name, "Board name"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 10);
	ImGui::InputText("##ubname", m_ub_name, sizeof(m_ub_name));
	if (ImGui::IsItemDeactivatedAfterEdit()) {
		char clean[15];
		vb::detail::clean_name(clean, m_ub_name, 14, false);
		for (size_t n = std::strlen(clean); n && clean[n - 1] == ' '; n--)
			clean[n - 1] = 0;
		const std::string to = ub::file_for(clean);
		if (!clean[0]) {
			m_ub_note = UI_TEXT(smp_ub_name_empty, "A board needs a name");
		} else if (to != path && smu2000::is_file(to)) {
			m_ub_note = UI_TEXT(smp_ub_name_taken, "Another board already has that name");
		} else {
			m_ub_note.clear();
			change([&](ub::board &b) { std::snprintf(b.name, sizeof(b.name), "%s", clean); });
		}
		m_ub_name_for.clear();      // 欄を、決まった名前に合わせ直す
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", UI_TEXT(smp_ub_board_name_tip, "Up to 14 letters, digits and signs. The MU shows it under UTIL > PLG, and the file takes the same name."));

	// ---- 上で作った波形を入れる
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(smp_ub_program, "Program"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 6.5f);
	if (ImGui::InputInt("##ubpgm", &m_ub_pgm))
		m_ub_pgm = std::clamp(m_ub_pgm, 1, vb::user_board::PROGRAMS);
	ImGui::SameLine();
	const bool taken = cur->program[size_t(m_ub_pgm - 1)] != nullptr;
	ImGui::BeginDisabled(m_wm_pcm.empty() || m_wm_pcm.size() > vb::user_board::MAX_FRAMES);
	if (ImGui::Button(taken ? UI_TEXT(smp_ub_replace, "Replace with this wave") : UI_TEXT(smp_ub_put, "Put this wave on the board"))) {
		auto p = std::make_shared<ub::program>();
		vb::detail::clean_name(p->name, m_wm_name, 8, true);
		p->pcm = std::make_shared<std::vector<s16>>(m_wm_pcm);
		p->loop = !oneshot;
		if (oneshot)
			p->release = 0.3f;
		else if (const ub::program *old = cur->program[size_t(m_ub_pgm - 1)].get(); old && old->loop) {
			// 同じ番号のループの波形を差し替えるときは、決めてあった包絡線を残す
			p->attack = old->attack;
			p->decay = old->decay;
			p->sustain = old->sustain;
			p->release = old->release;
		}
		const int at = m_ub_pgm - 1;
		change([&](ub::board &b) { b.program[size_t(at)] = p; });
		// 次の空いている番号へ
		for (int i = at + 1; i < vb::user_board::PROGRAMS; i++)
			if (!cur->program[size_t(i)]) {
				m_ub_pgm = i + 1;
				break;
			}
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", UI_TEXT(smp_ub_put_tip, "Puts the wave made above on the board at this program number, under the name in the Name field (8 letters). It does not use the sampling memory. Looping waves sustain while the key is held; plucks, drums and bells play once."));

	// ---- 入っているプログラム
	if (cur->count() && ImGui::BeginTable("ubprogs", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
		ImGui::TableSetupColumn("#");
		ImGui::TableSetupColumn(UI_TEXT(smp_name, "Name"));
		ImGui::TableSetupColumn(UI_TEXT(smp_ub_col_wave, "Wave"));
		ImGui::TableSetupColumn(UI_TEXT(smp_ub_col_attack, "Attack"));
		ImGui::TableSetupColumn(UI_TEXT(smp_ub_col_decay, "Decay"));
		ImGui::TableSetupColumn(UI_TEXT(smp_ub_col_sustain, "Sustain"));
		ImGui::TableSetupColumn(UI_TEXT(smp_ub_col_release, "Release"));
		ImGui::TableSetupColumn("");
		ImGui::TableHeadersRow();
		for (int i = 0; i < vb::user_board::PROGRAMS; i++) {
			const std::shared_ptr<const ub::program> p = cur->program[size_t(i)];
			if (!p)
				continue;
			ImGui::PushID(i);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			ImGui::Text("%03d", i + 1);
			// 書き換えた写しを入れる（波形は共有のまま）
			const auto set = [&](const std::function<void(ub::program &)> &f) {
				auto np = std::make_shared<ub::program>(*p);
				f(*np);
				change([&](ub::board &b) { b.program[size_t(i)] = np; });
			};
			ImGui::TableNextColumn();
			char name[9];
			std::snprintf(name, sizeof(name), "%s", p->name);
			for (size_t n = std::strlen(name); n && name[n - 1] == ' '; n--)
				name[n - 1] = 0;
			ImGui::SetNextItemWidth(fs * 6);
			if (ImGui::InputText("##n", name, sizeof(name)))
				set([&](ub::program &q) { vb::detail::clean_name(q.name, name, 8, true); });
			ImGui::TableNextColumn();
			const size_t frames = p->pcm ? p->pcm->size() : 0;
			ImGui::Text(p->loop ? UI_TEXT(smp_ub_loop_fmt, "loop %.2f s") : UI_TEXT(smp_ub_once_fmt, "once %.2f s"), double(frames) / 44100.0);
			float a = p->attack, d = p->decay, s = p->sustain, r = p->release;
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(fs * 4.5f);
			if (ImGui::DragFloat("##a", &a, 0.005f, 0.0f, 3.0f, "%.3f s", ImGuiSliderFlags_AlwaysClamp))
				set([&](ub::program &q) { q.attack = a; });
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(fs * 4.5f);
			if (ImGui::DragFloat("##d", &d, 0.02f, 0.0f, 20.0f, d <= 0.0f ? "-" : "%.2f s", ImGuiSliderFlags_AlwaysClamp))
				set([&](ub::program &q) { q.decay = d; });
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(fs * 4.5f);
			if (ImGui::DragFloat("##s", &s, 0.005f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp))
				set([&](ub::program &q) { q.sustain = s; });
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(fs * 4.5f);
			if (ImGui::DragFloat("##r", &r, 0.01f, 0.0f, 10.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp))
				set([&](ub::program &q) { q.release = r; });
			ImGui::TableNextColumn();
			if (ImGui::SmallButton(UI_TEXT(smp_play, "Play")) && p->pcm) {
				std::vector<s16> pcm = *p->pcm;
				const bool loop = p->loop;
				br.post([pcm, loop](mu2000 &mu) mutable {
					mu.preview_pcm(std::move(pcm), loop ? 0 : ~0u);
					return std::string();
				});
			}
			ImGui::SameLine();
			if (ImGui::SmallButton(UI_TEXT(smp_ub_remove, "Remove")))
				change([&](ub::board &b) { b.program[size_t(i)].reset(); });
			ImGui::PopID();
		}
		ImGui::EndTable();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", UI_TEXT(smp_ub_table_tip, "The envelope of each program: Attack is the rise, Decay the fall to the Sustain level while the key is held (\"-\" keeps the level), Release the fade after the key goes up. Drag to change; the next note uses the new values."));
	}
	// つまみを離したら、ファイルへ書く
	if (ub::unsaved() && !ImGui::IsAnyItemActive()) {
		if (!ub::save(br))
			m_ub_note = UI_TEXT(smp_ub_save_fail, "Could not write the board file");
		m_ub_listed = -1;
	}
	if (!m_ub_note.empty())
		ImGui::TextDisabled("%s", m_ub_note.c_str());
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped(UI_TEXT(smp_ub_footer_fmt, "%d programs. Saved as you go: %s"), cur->count(), ub::current_path().c_str());
	ImGui::PopStyleColor();
	ImGui::TextWrapped("%s", UI_TEXT(smp_ub_how, "To play it: Master window > Imaginary plug-in board > Your own board. Program change picks the wave."));
}

} // namespace ui
