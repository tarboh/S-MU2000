// license:BSD-3-Clause

#include "fx_icons.h"

#include "imgui_internal.h"
#include "xg/fx_types.h"

#include <algorithm>
#include <cmath>

namespace ui {
namespace xgui {

int fx_category_of(int msb)
{
	if (msb <= 0 || msb == 0x40)
		return -1;
	const auto &cats = xg::fx_categories();
	for (size_t c = 0; c < cats.size(); c++)
		for (u8 m : cats[c].msbs)
			if (m == msb)
				return int(c);
	return -1;
}

void fx_icon(ImDrawList *dl, ImVec2 min, float size, int msb, ImU32 color)
{
	const float PI = 3.14159265f;
	const float t = std::max(1.0f, size * 0.09f);            // 線の太さ
	// 正方形の中の 0..1 の位置
	auto at = [&](float u, float v) { return ImVec2(min.x + u * size, min.y + v * size); };
	auto faded = [&](float a) {
		const int alpha = int(float((color >> IM_COL32_A_SHIFT) & 0xff) * a);
		return (color & ~IM_COL32_A_MASK) | (ImU32(alpha) << IM_COL32_A_SHIFT);
	};
	// 0..1 の区間を u(x) → v(x) の関数でなぞる
	auto curve = [&](float u0, float u1, int n, auto &&v, ImU32 c) {
		for (int i = 0; i <= n; i++) {
			const float u = u0 + (u1 - u0) * float(i) / float(n);
			dl->PathLineTo(at(u, v(u)));
		}
		dl->PathStroke(c, t);
	};
	auto arrow_head = [&](ImVec2 tip, float dx, float dy) {  // 先端と向き（単位ベクトル）
		const float l = size * 0.18f;
		const ImVec2 back(tip.x - dx * l, tip.y - dy * l);
		dl->AddTriangleFilled(tip, ImVec2(back.x - dy * l * 0.6f, back.y + dx * l * 0.6f),
		                      ImVec2(back.x + dy * l * 0.6f, back.y - dx * l * 0.6f), color);
	};

	if (msb == 0x40) {                                         // THRU
		dl->AddLine(at(0.1f, 0.5f), at(0.72f, 0.5f), color, t);
		arrow_head(at(0.92f, 0.5f), 1, 0);
		return;
	}
	switch (fx_category_of(msb)) {
	case 0:                                                    // リバーブ
		dl->AddCircleFilled(at(0.24f, 0.5f), size * 0.08f, color);
		for (int i = 0; i < 3; i++) {
			const float r = size * (0.24f + 0.19f * float(i));
			dl->PathArcTo(at(0.24f, 0.5f), r, -PI * 0.28f, PI * 0.28f, 12);
			dl->PathStroke(faded(1.0f - 0.25f * float(i)), t);
		}
		break;
	case 1: {                                                  // 初期反射・ゲート
		static const float H[] = { 0.72f, 0.42f, 0.58f, 0.3f, 0.48f, 0.22f };
		for (int i = 0; i < 6; i++) {
			const float u = 0.12f + 0.15f * float(i);
			dl->AddLine(at(u, 0.86f), at(u, 0.86f - H[i]), color, t);
		}
		break;
	}
	case 2: {                                                  // ディレイ・エコー
		static const float U[] = { 0.2f, 0.47f, 0.69f, 0.87f };
		static const float R[] = { 0.15f, 0.11f, 0.075f, 0.05f };
		for (int i = 0; i < 4; i++)
			dl->AddCircleFilled(at(U[i], 0.5f), size * R[i], faded(1.0f - 0.18f * float(i)));
		break;
	}
	case 3:                                                    // カラオケ（マイク）
		dl->AddRectFilled(at(0.38f, 0.06f), at(0.62f, 0.54f), color, size * 0.12f);
		dl->PathArcTo(at(0.5f, 0.38f), size * 0.25f, 0.0f, PI, 12);        // 受け
		dl->PathStroke(color, t);
		dl->AddLine(at(0.5f, 0.63f), at(0.5f, 0.86f), color, t);
		dl->AddLine(at(0.3f, 0.9f), at(0.7f, 0.9f), color, t);
		break;
	case 4:                                                    // コーラス・セレステ
		curve(0.06f, 0.94f, 24, [](float u) { return 0.42f + 0.2f * std::sin(u * 2.0f * 3.14159265f * 1.3f); }, color);
		curve(0.06f, 0.94f, 24, [](float u) { return 0.58f + 0.2f * std::sin(u * 2.0f * 3.14159265f * 1.3f + 1.4f); }, faded(0.55f));
		break;
	case 5:                                                    // フランジャー・フェイザー（細かくなる波）
		curve(0.06f, 0.94f, 48, [](float u) { return 0.5f - 0.28f * std::sin(u * u * 17.0f); }, color);
		break;
	case 6:                                                    // 回転・トレモロ・パン
	{
		const float a1 = PI * 1.3f, r = 0.3f;
		dl->PathArcTo(at(0.5f, 0.5f), size * r, -PI * 0.25f, a1, 20);
		dl->PathStroke(color, t);
		// 弧の終わりで、回る向きに矢じり
		const float dx = -std::sin(a1), dy = std::cos(a1);
		arrow_head(at(0.5f + r * std::cos(a1) + dx * 0.14f, 0.5f + r * std::sin(a1) + dy * 0.14f), dx, dy);
		break;
	}
	case 7: {                                                  // 歪み・アンプ（頭のつぶれた波）
		const ImVec2 p[] = { at(0.06f, 0.5f), at(0.16f, 0.2f), at(0.38f, 0.2f), at(0.5f, 0.5f),
		                     at(0.62f, 0.8f), at(0.84f, 0.8f), at(0.94f, 0.5f) };
		dl->AddPolyline(p, 7, color, t);
		break;
	}
	case 8:                                                    // EQ・ワウ・フィルタ（山）
		dl->PathLineTo(at(0.06f, 0.7f));
		dl->PathLineTo(at(0.3f, 0.7f));
		dl->PathBezierCubicCurveTo(at(0.42f, 0.7f), at(0.42f, 0.16f), at(0.52f, 0.16f), 10);
		dl->PathBezierCubicCurveTo(at(0.62f, 0.16f), at(0.62f, 0.7f), at(0.74f, 0.7f), 10);
		dl->PathLineTo(at(0.94f, 0.7f));
		dl->PathStroke(color, t);
		dl->AddLine(at(0.06f, 0.88f), at(0.94f, 0.88f), faded(0.4f), std::max(1.0f, t * 0.6f));
		break;
	case 9:                                                    // コンプ・ゲート（押しつぶす）
		dl->AddLine(at(0.18f, 0.44f), at(0.82f, 0.44f), color, t);
		dl->AddLine(at(0.18f, 0.56f), at(0.82f, 0.56f), color, t);
		dl->AddLine(at(0.5f, 0.04f), at(0.5f, 0.2f), color, t);
		arrow_head(at(0.5f, 0.36f), 0, 1);
		dl->AddLine(at(0.5f, 0.96f), at(0.5f, 0.8f), color, t);
		arrow_head(at(0.5f, 0.64f), 0, -1);
		break;
	case 10:                                                   // 組み合わせ（つないだ箱）
		for (int i = 0; i < 3; i++) {
			const float u = 0.05f + 0.33f * float(i);
			dl->AddRect(at(u, 0.38f), at(u + 0.23f, 0.62f), color, size * 0.03f, 0, t);
			if (i < 2)
				dl->AddLine(at(u + 0.23f, 0.5f), at(u + 0.33f, 0.5f), color, t);
		}
		break;
	case 11: {                                                 // ローファイ・テクノ（階段）
		const int steps = 8;
		float prev = 0;
		for (int i = 0; i < steps; i++) {
			const float u0 = 0.06f + 0.88f * float(i) / float(steps);
			const float u1 = 0.06f + 0.88f * float(i + 1) / float(steps);
			const float v = 0.5f - 0.32f * std::round(std::sin((float(i) + 0.5f) / float(steps) * 2.0f * PI) * 2.0f) / 2.0f;
			if (i > 0)
				dl->PathLineTo(at(u0, prev));
			dl->PathLineTo(at(u0, v));
			dl->PathLineTo(at(u1, v));
			prev = v;
		}
		dl->PathStroke(color, t);
		break;
	}
	case 12:                                                   // ピッチ・その他（音符と矢印）
		dl->AddEllipseFilled(at(0.3f, 0.76f), ImVec2(size * 0.14f, size * 0.1f), color, -0.4f);
		dl->AddLine(at(0.42f, 0.73f), at(0.42f, 0.12f), color, t);
		dl->AddLine(at(0.78f, 0.9f), at(0.78f, 0.36f), color, t);
		arrow_head(at(0.78f, 0.12f), 0, -1);
		break;
	default:                                                   // NO EFFECT・表に無いもの
		dl->AddCircle(at(0.5f, 0.5f), size * 0.36f, faded(0.7f), 20, t);
		dl->AddLine(at(0.25f, 0.75f), at(0.75f, 0.25f), faded(0.7f), t);
		break;
	}
}

void fx_icon_inline(int msb, ImU32 color)
{
	const float h = ImGui::GetTextLineHeight();
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(h, h));
	fx_icon(ImGui::GetWindowDrawList(), pos, h, msb, color);
}

bool begin_fx_combo(const char *id, int type, ImGuiComboFlags flags)
{
	const bool open = ImGui::BeginCombo(id, "", flags);
	if (ImGui::BeginComboPreview()) {
		if (type >= 0) {
			fx_icon_inline(type >> 7, ImGui::GetColorU32(ImGuiCol_Text));
			ImGui::SameLine(0, ImGui::GetFontSize() * 0.3f);
		}
		ImGui::TextUnformatted(type >= 0 ? xg::fx_name(type).c_str() : "--");
		ImGui::EndComboPreview();
	}
	return open;
}

} // namespace xgui
} // namespace ui
