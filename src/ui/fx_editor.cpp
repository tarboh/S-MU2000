// license:BSD-3-Clause

#include "fx_editor.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "xg/fx_params.h"
#include "xg/fx_types.h"
#include "fx_help.h"
#include "fx_icons.h"
#include "eq_curve.h"
#include "ui/lang.h"
#include "ui/texts.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <cstdio>
#include <string>

namespace ui {

using namespace xgui;

namespace {

ImU32 col(ImGuiCol c, float a = 1.0f) { return ImGui::GetColorU32(c, a); }

// 種類の系統ごとの筐体の色。残響は青、ディレイは青緑、揺れ（コーラス・フランジャなど）は紫、
// 歪みは赤、EQ は灰、そのほか（ワウ・トレモロ・オートパンなど）は緑
ImU32 body_color(int msb)
{
	if (msb >= 0x01 && msb <= 0x04) return IM_COL32(40, 62, 112, 255);
	if (msb >= 0x05 && msb <= 0x08) return IM_COL32(26, 88, 96, 255);
	if (msb >= 0x09 && msb <= 0x0c) return IM_COL32(48, 58, 104, 255);
	if (msb >= 0x41 && msb <= 0x45) return IM_COL32(80, 50, 112, 255);
	if (msb == 0x48)                return IM_COL32(80, 50, 112, 255);
	if (msb >= 0x49 && msb <= 0x4b) return IM_COL32(130, 40, 34, 255);
	if (msb == 0x4c || msb == 0x4d) return IM_COL32(70, 72, 78, 255);
	return IM_COL32(42, 90, 54, 255);
}

std::string value_text(const xg::fx_param &p, int v)
{
	char buf[24];
	switch (p.fmt) {
	case xg::fx_fmt::table:
		if (p.texts && v >= p.lo && v <= p.hi)
			return p.texts[v - p.lo];
		break;
	case xg::fx_fmt::tenths:
		std::snprintf(buf, sizeof(buf), "%.1f", v / 10.0);
		return buf;
	default:
		break;
	}
	std::snprintf(buf, sizeof(buf), "%d", v);
	return buf;
}

} // namespace


// 実物のつまみ風。上下ドラッグ（Shift で細かく）、ホイール、ダブルクリックで数を打つ。
// 戻り値は「値が変わったか」
bool fx_editor::knob(const char *id, int &v, int lo, int hi, float size, const char *label, const char *text, bool tooltip,
                     bool dim)
{
	ImGuiIO &io = ImGui::GetIO();
	const float fs = ImGui::GetFontSize();
	const float w = size + fs * 1.6f, h = size + fs * 2.5f;
	ImGui::PushID(id);
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##k", ImVec2(w, h));
	const ImGuiID iid = ImGui::GetItemID();
	const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
	int nv = v;
	if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
		// 全域を 200px ほどで回す。広い範囲（ディレイの時間など）も同じ手ざわりにする
		// a Get*Ref reference goes stale when an insert grows the storage, so
		// take a value and write it back
		ImGuiStorage *st = ImGui::GetStateStorage();
		float acc = st->GetFloat(iid, 0.0f);
		acc -= io.MouseDelta.y * float(hi - lo) / (io.KeyShift ? 800.0f : 200.0f);
		const int step = int(acc);
		if (step) { nv = std::clamp(nv + step, lo, hi); acc -= float(step); }
		st->SetFloat(iid, acc);
	}
	if (ImGui::IsItemDeactivated())
		ImGui::GetStateStorage()->SetFloat(iid, 0.0f);
	if (hovered) {
		ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
		if (io.MouseWheel != 0.0f) {
			const int unit = hi - lo > 300 ? (hi - lo) / 100 : 1;
			nv = std::clamp(nv + (io.MouseWheel > 0 ? 1 : -1) * unit * (io.KeyCtrl ? 10 : 1), lo, hi);
		}
	}
	if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		ImGui::OpenPopup("##typein");
	if (ImGui::BeginPopup("##typein")) {
		ImGui::TextDisabled(UI_TEXT(cap_range1_fmt, "%s (%d-%d)"), label, lo, hi);
		const ImGuiID typed_id = ImGui::GetID("typed");
		ImGuiStorage *st = ImGui::GetStateStorage();
		int typed = st->GetInt(typed_id, v);
		if (ImGui::IsWindowAppearing()) { typed = v; ImGui::SetKeyboardFocusHere(); }
		ImGui::SetNextItemWidth(fs * 6);
		if (ImGui::InputInt("##n", &typed, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue)) {
			nv = std::clamp(typed, lo, hi);
			ImGui::CloseCurrentPopup();
		}
		st->SetInt(typed_id, typed);
		ImGui::EndPopup();
	}

	ImDrawList *dl = ImGui::GetWindowDrawList();
	const ImVec2 c(pos.x + w * 0.5f, pos.y + size * 0.5f);
	const float r = size * 0.36f;
	const float a0 = IM_PI * 0.75f, a1 = IM_PI * 2.25f;
	const float frac = hi > lo ? float(nv - lo) / float(hi - lo) : 0.0f;
	const float av = a0 + (a1 - a0) * frac;
	// 目盛り（11 本）
	for (int i = 0; i <= 10; i++) {
		const float a = a0 + (a1 - a0) * float(i) / 10.0f;
		const ImVec2 d(std::cos(a), std::sin(a));
		dl->AddLine(ImVec2(c.x + d.x * r * 1.18f, c.y + d.y * r * 1.18f), ImVec2(c.x + d.x * r * 1.34f, c.y + d.y * r * 1.34f),
		            dim ? IM_COL32(150, 150, 155, 70) : IM_COL32(235, 235, 225, 150), 1.5f);
	}
	// 値のところまでの弧（光る帯）
	dl->PathArcTo(c, r * 1.26f, a0, av, 32);
	dl->PathStroke(dim ? IM_COL32(140, 140, 145, 110) : IM_COL32(255, 200, 90, 200), 0, std::max(2.0f, size * 0.05f));
	// 本体。影、縁、面
	dl->AddCircleFilled(ImVec2(c.x + 1.5f, c.y + 2.5f), r, IM_COL32(0, 0, 0, 100), 40);
	dl->AddCircleFilled(c, r, dim ? IM_COL32(48, 49, 54, 255) : IM_COL32(62, 62, 66, 255), 40);
	dl->AddCircleFilled(c, r * 0.86f, hovered || active ? IM_COL32(44, 44, 48, 255) : IM_COL32(30, 30, 33, 255), 40);
	dl->AddCircle(c, r, dim ? IM_COL32(85, 86, 92, 255) : IM_COL32(125, 125, 130, 255), 40, 1.5f);
	// 指し
	const ImVec2 d(std::cos(av), std::sin(av));
	dl->AddLine(ImVec2(c.x + d.x * r * 0.2f, c.y + d.y * r * 0.2f), ImVec2(c.x + d.x * r * 0.84f, c.y + d.y * r * 0.84f),
	            dim ? IM_COL32(135, 135, 140, 255) : IM_COL32(250, 250, 245, 255), std::max(2.0f, size * 0.05f));
	// 名前と値
	const ImVec2 ls = ImGui::CalcTextSize(label);
	dl->AddText(ImVec2(pos.x + (w - ls.x) * 0.5f, pos.y + size + fs * 0.1f), dim ? IM_COL32(135, 137, 145, 255) : IM_COL32(245, 245, 235, 255), label);
	const ImVec2 ts = ImGui::CalcTextSize(text);
	const ImVec2 t0(pos.x + (w - ts.x) * 0.5f - fs * 0.3f, pos.y + size + fs * 1.2f);
	dl->AddRectFilled(t0, ImVec2(t0.x + ts.x + fs * 0.6f, t0.y + fs * 1.1f), IM_COL32(12, 14, 10, 200), 3.0f);
	dl->AddText(ImVec2(t0.x + fs * 0.3f, t0.y + fs * 0.05f), dim ? IM_COL32(120, 140, 110, 255) : IM_COL32(150, 230, 90, 255), text);
	if (tooltip && hovered && !active)
		ImGui::SetItemTooltip(UI_TEXT(fxe_slider_tip_fmt, "%s  %s\nDrag up/down (Shift for fine), wheel, or double-click to type a value"), label, text);
	ImGui::PopID();
	const bool changed = nv != v;
	v = nv;
	return changed;
}


// パラメータの番地。インサーションは表のまま、システムエフェクトは xg/sysfx.h で読み替える
bool fx_editor::where(const xg::fx_param &fp, u32 &addr, int &size) const
{
	if (m_slot <= 4) {
		addr = xg::pack(0x03, u8(m_slot - 1), fp.addr);
		size = fp.size;
		return true;
	}
	const xg::sysfx which = m_slot == 5 ? xg::sysfx::reverb : m_slot == 6 ? xg::sysfx::chorus : xg::sysfx::variation;
	const int lo = xg::sysfx_addr(which, fp, size);
	if (lo < 0)
		return false;
	addr = xg::pack(0x02, 0x01, u8(lo));
	return true;
}


// 値を送る。リンクしていれば、相手のインサーションの同じ番地（中の桁だけ違う）にも同じ値を
void fx_editor::send_raw(xg::model &m, bridge &br, u32 addr, int size, int v)
{
	drag_send(br, m.set_raw(addr, size, v));
	if (m_link_ad && m_partner && m_slot <= 4)
		drag_send(br, m.set_raw(xg::pack(0x03, u8(m_partner - 1), u8(addr & 0x7f)), size, v));
}

// 種類を先に送ってから（種類を書くと firmware がパラメータを既定に戻すので）、表にあるつまみを 1 つずつ
void fx_editor::copy_to_partner(xg::model &m, bridge &br, int type)
{
	if (!m_partner || m_slot > 4)
		return;
	char key[24];
	std::snprintf(key, sizeof(key), "insertion%d.type", m_partner);
	br.send(m.set(P(key), 0, type));
	const xg::fx_def *def = xg::fx_find(type);
	if (!def)
		return;
	for (int i = 0; i < def->count; i++) {
		const xg::fx_param &fp = def->params[i];
		u32 addr = 0;
		int size = 0, v = 0;
		if (where(fp, addr, size) && m.get_raw(addr, size, v))
			br.send(m.set_raw(xg::pack(0x03, u8(m_partner - 1), u8(addr & 0x7f)), size, v));
	}
}

// EQ の特性のグラフ。横が周波数（20Hz-20kHz の対数）、縦がゲイン（±15dB）。
// 帯の点をつまんで、横で周波数、縦でゲイン。真ん中の帯は点の近くでホイールを回すと幅。
// パラメータは LCD の名前で見分ける（EQ LowFreq / Low Freq など）
void fx_editor::eq_graph(const xg::fx_def &def, xg::model &m, bridge &br, ImVec2 p0, ImVec2 p1)
{
	struct band { eq::shape shape; int freq = -1, gain = -1, width = -1; int vf = 0, vg = 64, vw = 10; };
	auto find = [&](std::initializer_list<const char *> names) {
		for (int i = 0; i < def.count; i++)
			for (const char *n : names)
				if (!std::strcmp(def.params[i].label, n))
					return i;
		return -1;
	};
	band bands[3];
	bands[0].shape = eq::shape::low_shelf;
	bands[0].freq = find({ "EQ LowFreq", "Low Freq" });
	bands[0].gain = find({ "EQ LowGain", "Low Gain" });
	bands[1].shape = eq::shape::peak;
	bands[1].freq = find({ "EQ MidFreq", "Mid Freq", "EQ Freq" });
	bands[1].gain = find({ "EQ MidGain", "Mid Gain", "EQ Gain" });
	bands[1].width = find({ "EQ MidWidt", "Mid Width", "EQ Width" });
	bands[2].shape = eq::shape::high_shelf;
	bands[2].freq = find({ "EQHighFreq", "High Freq" });
	bands[2].gain = find({ "EQHighGain", "High Gain" });

	auto value = [&](int index, int &v) {
		const xg::fx_param &fp = def.params[index];
		u32 a = 0;
		int size = 0;
		if (!where(fp, a, size) || !m.get_raw(a, size, v))
			return false;
		v = std::clamp(v, int(fp.lo), int(fp.hi));
		return true;
	};
	std::vector<band *> used;
	for (band &b : bands) {
		if (b.freq < 0 || b.gain < 0)
			continue;
		if (!value(b.freq, b.vf) || !value(b.gain, b.vg))
			continue;
		if (b.width >= 0 && !value(b.width, b.vw))
			b.width = -1;
		used.push_back(&b);
	}
	if (used.empty())
		return;

	ImGuiIO &io = ImGui::GetIO();
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImGui::SetCursorScreenPos(p0);
	ImGui::InvisibleButton("##eqgraph", ImVec2(p1.x - p0.x, p1.y - p0.y));
	const ImGuiID id = ImGui::GetItemID();
	const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();

	const float pad = fs * 0.6f;
	const float x0 = p0.x + pad * 2.5f, x1 = p1.x - pad, top = p0.y + pad, bottom = p1.y - pad * 1.6f;
	const float DB = 15.0f;
	auto x_of = [&](float hz) { return x0 + (x1 - x0) * eq::t_of_hz(hz); };
	auto y_of = [&](float db) { return (top + bottom) * 0.5f - (bottom - top) * 0.5f * std::clamp(db, -DB, DB) / DB; };
	auto handle = [&](const band &b) { return ImVec2(x_of(float(eq::HZ[std::clamp(b.vf, 0, 60)])), y_of(float(b.vg - 64))); };
	auto nearest = [&]() {
		int best = -1; float bd = 1e9f;
		for (size_t k = 0; k < used.size(); k++) {
			const ImVec2 h = handle(*used[k]);
			const float d = (h.x - io.MousePos.x) * (h.x - io.MousePos.x) + (h.y - io.MousePos.y) * (h.y - io.MousePos.y);
			if (d < bd) { bd = d; best = int(k); }
		}
		return best;
	};

	ImGuiStorage *st = ImGui::GetStateStorage();
	int grab = st->GetInt(id, -1);
	float gx = st->GetFloat(id + 1, 0.0f), gy = st->GetFloat(id + 2, 0.0f);
	if (ImGui::IsItemActivated()) {
		grab = nearest();
		if (grab >= 0) {
			const ImVec2 h = handle(*used[grab]);
			gx = h.x - io.MousePos.x;
			gy = h.y - io.MousePos.y;
		}
	}
	if (!active)
		grab = -1;
	st->SetInt(id, grab);
	st->SetFloat(id + 1, gx);
	st->SetFloat(id + 2, gy);
	if (active && grab >= 0 && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
		const band &b = *used[grab];
		const xg::fx_param &pf = def.params[b.freq], &pg = def.params[b.gain];
		const int nf = eq::index_near((io.MousePos.x + gx - x0) / (x1 - x0), pf.lo, pf.hi);
		const float db = -((io.MousePos.y + gy) - (top + bottom) * 0.5f) / ((bottom - top) * 0.5f) * DB;
		const int ng = std::clamp(int(std::lround(64 + db)), int(pg.lo), int(pg.hi));
		u32 a = 0;
		int size = 0;
		if (nf != b.vf && where(pf, a, size)) send_raw(m, br, a, size, nf);
		if (ng != b.vg && where(pg, a, size)) send_raw(m, br, a, size, ng);
		m_focus = b.gain;
	}
	const int hot = active ? grab : hovered ? nearest() : -1;
	if (hovered && hot >= 0) {
		const band &b = *used[hot];
		m_focus = b.gain;
		if (b.width >= 0) {
			ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
			if (io.MouseWheel != 0.0f) {
				const xg::fx_param &pw = def.params[b.width];
				const int nw = std::clamp(b.vw + (io.MouseWheel > 0 ? 1 : -1) * (io.KeyCtrl ? 10 : 2), int(pw.lo), int(pw.hi));
				u32 a = 0;
				int size = 0;
				if (nw != b.vw && where(pw, a, size)) send_raw(m, br, a, size, nw);
				m_focus = b.width;
			}
		}
	}

	// 描く
	dl->AddRectFilled(p0, p1, IM_COL32(10, 12, 10, 150), 8.0f);
	const ImU32 grid = IM_COL32(255, 255, 255, 40), label = IM_COL32(255, 255, 255, 130);
	for (float db : { -12.0f, -6.0f, 0.0f, 6.0f, 12.0f }) {
		dl->AddLine(ImVec2(x0, y_of(db)), ImVec2(x1, y_of(db)), db == 0.0f ? IM_COL32(255, 255, 255, 90) : grid);
		char t[8];
		std::snprintf(t, sizeof(t), "%+.0f", db);
		dl->AddText(ImVec2(p0.x + pad * 0.4f, y_of(db) - fs * 0.5f), label, db == 0.0f ? "0" : t);
	}
	for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f }) {
		dl->AddLine(ImVec2(x_of(hz), top), ImVec2(x_of(hz), bottom), grid);
		char t[8];
		if (hz >= 1000) std::snprintf(t, sizeof(t), "%.0fk", hz / 1000);
		else            std::snprintf(t, sizeof(t), "%.0f", hz);
		dl->AddText(ImVec2(x_of(hz) - ImGui::CalcTextSize(t).x * 0.5f, bottom + pad * 0.3f), label, t);
	}
	const int np = std::max(16, int(x1 - x0) / 2);
	std::vector<ImVec2> pts;
	pts.reserve(np + 1);
	for (int i = 0; i <= np; i++) {
		const float t = float(i) / float(np);
		const float f = eq::hz_of_t(t);
		float db = 0;
		for (const band *b : used)
			db += eq::band_db(b->shape, float(b->vg - 64), float(eq::HZ[std::clamp(b->vf, 0, 60)]),
			                  b->width >= 0 ? b->vw / 10.0f : 0.7f, f);
		pts.push_back(ImVec2(x0 + (x1 - x0) * t, y_of(db)));
	}
	dl->PathClear();
	dl->PathLineTo(ImVec2(x0, y_of(0)));
	for (const ImVec2 &p : pts) dl->PathLineTo(p);
	dl->PathLineTo(ImVec2(x1, y_of(0)));
	dl->PathFillConcave(IM_COL32(150, 230, 90, 50));
	dl->AddPolyline(pts.data(), int(pts.size()), IM_COL32(150, 230, 90, 255), 0, std::max(2.0f, fs * 0.12f));
	const char *const names[] = { "L", "M", "H" };
	for (size_t k = 0; k < used.size(); k++) {
		const ImVec2 h = handle(*used[k]);
		const bool on = int(k) == hot;
		dl->AddCircleFilled(h, on ? fs * 0.55f : fs * 0.42f, on ? IM_COL32(255, 255, 240, 255) : IM_COL32(150, 230, 90, 255), 20);
		const char *n = names[used[k] - bands];
		dl->AddText(ImVec2(h.x - ImGui::CalcTextSize(n).x * 0.5f, h.y - fs * 0.5f), IM_COL32(10, 12, 10, 255), n);
	}
	if (hovered && !active && hot >= 0)
		ImGui::SetItemTooltip("%s", UI_TEXT(fxe_eq_tip, "Drag a point: sideways for frequency, up/down for gain. Wheel on M for width."));
}


void fx_editor::draw(xg::model &m, const xg_snapshot &, bridge &br)
{
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
	                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::Begin("fx", nullptr, wf);
	ImGui::PopStyleVar();
	const float fs = ImGui::GetFontSize();

	// ---- どのエフェクトか。1-4 がインサーション、5-7 がリバーブ・コーラス・バリエーション
	int slot = fx_window_slot();
	static const char *const SLOT_NAMES[] = { "INS 1", "INS 2", "INS 3", "INS 4", "REV", "CHO", "VAR" };
	for (int i = 1; i <= 7; i++) {
		if (i > 1) ImGui::SameLine(0, i == 5 ? fs * 0.8f : -1.0f);
		const bool sel = i == slot;
		if (sel) ImGui::PushStyleColor(ImGuiCol_Button, col(ImGuiCol_ButtonActive));
		if (ImGui::Button(SLOT_NAMES[i - 1])) { slot = i; set_fx_window_slot(i); }
		if (sel) ImGui::PopStyleColor();
	}
	m_slot = slot;
	const bool sys = slot >= 5;
	static const char *const SYS_TITLES[] = { "REVERB", "CHORUS", "VARIATION" };
	char type_key[24], part_key[24];
	if (sys) {
		std::snprintf(type_key, sizeof(type_key), "%s.type", slot == 5 ? "reverb" : slot == 6 ? "chorus" : "variation");
		std::snprintf(part_key, sizeof(part_key), "variation.part");
	} else {
		std::snprintf(type_key, sizeof(type_key), "insertion%d.type", slot);
		std::snprintf(part_key, sizeof(part_key), "insertion%d.part", slot);
	}
	const xg::param &ptype = P(type_key);
	const xg::param &ppart = P(part_key);
	int type = 0, part = 127;
	const bool has_type = m.get(ptype, 0, type);
	m.get(ppart, 0, part);
	// AD1 / AD2 の相手（もう片方の AD に掛かっているインサーション）
	m_partner = 0;
	if (!sys && (part == XG_PARTS || part == XG_PARTS + 1)) {
		const int other = part == XG_PARTS ? XG_PARTS + 1 : XG_PARTS;
		for (int j = 1; j <= 4 && !m_partner; j++) {
			char key[24];
			std::snprintf(key, sizeof(key), "insertion%d.part", j);
			int pj = 127;
			if (j != slot && m.get(P(key), 0, pj) && pj == other)
				m_partner = j;
		}
	}
	const std::vector<xg::fx_type> &types = slot == 5 ? xg::rev_types() : slot == 6 ? xg::cho_types() : xg::ins_types();

	// ---- 種類と掛けるパート
	ImGui::SameLine(0, fs * 1.5f);
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(UI_TEXT(fx_kind, "Type"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fs * 11);
	if (begin_fx_combo("##type", has_type ? type : -1, ImGuiComboFlags_HeightLarge)) {
		// 品書きの形（分類 → 系統 → LSB 違い）で選ぶ
		int chosen = 0;
		if (fx_type_menu(types, has_type ? type : -1, chosen)) {
			br.send(m.set(ptype, 0, chosen));
			if (m_link_ad && m_partner) {
				char key[24];
				std::snprintf(key, sizeof(key), "insertion%d.type", m_partner);
				br.send(m.set(P(key), 0, chosen));
			}
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndCombo();
	}
	if (!sys) {
		ImGui::SameLine(0, fs);
		ImGui::TextUnformatted(UI_TEXT(fx_part, "Part"));
		ImGui::SameLine();
		ImGui::SetNextItemWidth(fs * 6);
		if (ImGui::BeginCombo("##part", part < XG_PARTS + 2 ? part_name(part).c_str() : "OFF", ImGuiComboFlags_HeightLarge)) {
			if (ImGui::Selectable("OFF", part >= XG_PARTS + 2))
				br.send(m.set(ppart, 0, 127));
			for (int i = 0; i < XG_PARTS + 2; i++)   // 64 パートの後ろに AD1・AD2
				if (ImGui::Selectable(part_name(i).c_str(), i == part))
					br.send(m.set(ppart, 0, i));
			ImGui::EndCombo();
		}
	}
	if (m_partner) {
		ImGui::SameLine(0, fs);
		if (ImGui::Checkbox(UI_TEXT(fxe_link_ad, "Link AD1/AD2"), &m_link_ad) && m_link_ad && has_type)
			copy_to_partner(m, br, type);
		if (ImGui::IsItemHovered()) {
			char tip[512];
			std::snprintf(tip, sizeof(tip), UI_TEXT(fxe_link_ad_tip_fmt, "Edits here also go to INS %d, which is on the other A/D input: the same type and the same knob values on both. Turning it on copies this effect to INS %d once. Pan each AD part to taste in the part list (for a stereo source, hard left and hard right)."), m_partner, m_partner);
			ImGui::SetTooltip("%s", tip);
		}
	}
	ImGui::SameLine(0, fs * 1.5f);
	help_checkbox();

	// ---- 筐体
	ImGui::Spacing();
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	const ImVec2 end(pos.x + avail.x, pos.y + avail.y);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const int msb = type >> 7;
	dl->AddRectFilled(ImVec2(pos.x + 2, pos.y + 4), ImVec2(end.x + 2, end.y + 4), IM_COL32(0, 0, 0, 90), 16.0f);
	dl->AddRectFilled(pos, end, body_color(msb), 16.0f);
	dl->AddRectFilledMultiColor(pos, ImVec2(end.x, pos.y + fs * 4), IM_COL32(255, 255, 255, 30), IM_COL32(255, 255, 255, 30),
	                            IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
	dl->AddRect(pos, end, IM_COL32(255, 255, 255, 50), 16.0f, 0, 2.0f);
	// ねじ
	for (const ImVec2 &s : { ImVec2(pos.x + fs * 0.8f, pos.y + fs * 0.8f), ImVec2(end.x - fs * 0.8f, pos.y + fs * 0.8f),
	                         ImVec2(pos.x + fs * 0.8f, end.y - fs * 0.8f), ImVec2(end.x - fs * 0.8f, end.y - fs * 0.8f) }) {
		dl->AddCircleFilled(s, fs * 0.32f, IM_COL32(170, 170, 175, 255), 12);
		dl->AddLine(ImVec2(s.x - fs * 0.2f, s.y - fs * 0.2f), ImVec2(s.x + fs * 0.2f, s.y + fs * 0.2f), IM_COL32(80, 80, 85, 255), 1.5f);
	}
	// 名前のプレート
	const std::string title = has_type ? xg::fx_name(type) : "--";
	ImFont *font = ImGui::GetFont();
	if (has_type)
		fx_icon(dl, ImVec2(pos.x + fs * 1.8f, pos.y + fs * 0.85f), fs * 1.9f, msb, IM_COL32(250, 250, 240, 230));
	dl->AddText(font, fs * 1.9f, ImVec2(pos.x + fs * (has_type ? 4.2f : 1.8f), pos.y + fs * 0.8f), IM_COL32(250, 250, 240, 255), title.c_str());
	char sub[64];
	if (sys)
		std::snprintf(sub, sizeof(sub), "%s", SYS_TITLES[slot - 5]);
	else
		std::snprintf(sub, sizeof(sub), "INSERTION %d  →  %s", slot, part < XG_PARTS + 2 ? part_name(part).c_str() : "OFF");
	dl->AddText(ImVec2(pos.x + fs * 1.9f, pos.y + fs * 3.0f), IM_COL32(250, 250, 240, 150), sub);
	// 動作ランプ（インサーションはパートに掛かっていれば、システムエフェクトは種類があれば点く）
	const bool lit = sys ? has_type && msb != 0 : part < XG_PARTS + 2;
	const ImVec2 lamp(end.x - fs * 2.2f, pos.y + fs * 1.8f);
	if (lit)
		dl->AddCircleFilled(lamp, fs * 0.9f, IM_COL32(255, 60, 40, 60), 24);
	dl->AddCircleFilled(lamp, fs * 0.45f, lit ? IM_COL32(255, 80, 60, 255) : IM_COL32(70, 22, 18, 255), 20);

	const float left = pos.x + fs * 1.8f, right = end.x - fs * 1.8f;
	// 種類の説明
	float y = pos.y + fs * 4.6f;
	if (has_type) {
		if (const char *h = fx_type_help(type >> 7, type & 0x7f)) {
			dl->AddText(font, fs, ImVec2(left, y), IM_COL32(250, 250, 240, 220), h, nullptr, right - left);
			y += ImGui::CalcTextSize(h, nullptr, false, right - left).y;
		}
	}
	y += fs * 0.8f;

	// ---- 下の説明の欄（カーソルが載った・最後に触ったつまみ）
	const float note_h = fs * 3.4f;
	const ImVec2 note0(left - fs * 0.4f, end.y - fs * 0.9f - note_h), note1(right + fs * 0.4f, end.y - fs * 0.9f);

	// ---- つまみ
	const xg::fx_def *def = has_type ? xg::fx_find(type) : nullptr;
	if (m_focus_type != (slot << 16 | type)) {
		m_focus_type = slot << 16 | type;
		m_focus = -1;
	}
	const float ksize = fs * 3.8f;
	const float cell_w = ksize + fs * 1.6f, cell_h = ksize + fs * 2.5f;
	if (!def || def->count == 0) {
		ImGui::SetCursorScreenPos(ImVec2(left, y));
		ImGui::TextColored(ImVec4(1, 1, 1, 0.75f), "%s",
		                   msb == 0 ? UI_TEXT(fxe_noeffect, "NO EFFECT (pick a type to list its knobs here)")
		                   : msb == 0x40 ? UI_TEXT(fxe_thru, "THRU (no parameters)")
		                                 : UI_TEXT(fxe_no_table, "No parameter table for this type yet"));
	} else {
		const int per_row = std::max(1, int((right - left) / (cell_w + fs * 0.6f)));
		int shown = 0;                          // 並べた数（その塊に無いパラメータは飛ばす）
		for (int i = 0; i < def->count; i++) {
			const xg::fx_param &fp = def->params[i];
			u32 addr = 0;
			int size = 0;
			if (!where(fp, addr, size))
				continue;
			int v = 0;
			const bool known = m.get_raw(addr, size, v);
			v = std::clamp(v, int(fp.lo), int(fp.hi));
			const std::string text = known ? value_text(fp, v) : "--";
			ImGui::SetCursorScreenPos(ImVec2(left + float(shown % per_row) * (cell_w + fs * 0.6f),
			                                 y + float(shown / per_row) * (cell_h + fs * 0.6f)));
			shown++;
			char id[8];
			std::snprintf(id, sizeof(id), "p%d", i);
			if (knob(id, v, fp.lo, fp.hi, ksize, fp.label, text.c_str()) && known)
				send_raw(m, br, addr, size, v);
			if (ImGui::IsItemHovered() || ImGui::IsItemActive())
				m_focus = i;
		}
		// EQ のパラメータを持つ種類は、つまみの下に特性のグラフ
		const int rows = (shown + per_row - 1) / per_row;
		const float gy0 = y + float(rows) * (cell_h + fs * 0.6f);
		const float gy1 = note0.y - fs * 0.6f;
		if (gy1 - gy0 > fs * 4.0f)
			eq_graph(*def, m, br, ImVec2(left - fs * 0.4f, gy0), ImVec2(right + fs * 0.4f, gy1));
	}
	dl->AddRectFilled(note0, note1, IM_COL32(0, 0, 0, 70), 8.0f);
	u32 focus_addr = 0;
	int focus_size = 0;
	if (def && m_focus >= 0 && m_focus < def->count && where(def->params[m_focus], focus_addr, focus_size)) {
		const xg::fx_param &fp = def->params[m_focus];
		int v = 0;
		const bool known = m.get_raw(focus_addr, focus_size, v);
		char head[64];
		std::snprintf(head, sizeof(head), "%s   %s", fp.label, known ? value_text(fp, std::clamp(v, int(fp.lo), int(fp.hi))).c_str() : "--");
		dl->AddText(font, fs * 1.1f, ImVec2(note0.x + fs * 0.6f, note0.y + fs * 0.4f), IM_COL32(150, 230, 90, 255), head);
		const char *h = fx_param_help(fp.label);
		dl->AddText(font, fs, ImVec2(note0.x + fs * 0.6f, note0.y + fs * 1.8f), IM_COL32(250, 250, 240, 230),
		            h ? h : UI_TEXT(fxe_no_desc, "(no description yet)"), nullptr, note1.x - note0.x - fs * 1.2f);
	} else if (def && def->count) {
		dl->AddText(font, fs, ImVec2(note0.x + fs * 0.6f, note0.y + fs * 0.5f), IM_COL32(250, 250, 240, 150),
		            UI_TEXT(fxe_hover_knob, "Hover over a knob to see what it does."));
	}
	ImGui::SetCursorScreenPos(ImVec2(pos.x, end.y));
	ImGui::Dummy(ImVec2(0, 0));
	ImGui::End();
}

} // namespace ui
