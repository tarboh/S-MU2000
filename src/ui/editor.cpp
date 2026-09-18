// license:BSD-3-Clause
//
// エディタの面。SOL2 の XG エディタに倣って、パートを 1 つ選び、
// そのパートのつまみを並べる。
//
// **音源には手を入れない。MIDI を送るだけ**。つまみは XG のパラメータチェンジを
// 送る。実機に送るのと同じことなので、パネルから触った結果とも矛盾しない。
//
// 値は画面では覚えない。音源に問い合わせた返事をパラメータの層（xg::model）が
// 持っていて、描くときはそれを読む（doc/params.md）。パネルや曲で変えた値も出る。
// まだ読めていない値は「--」で出す（決め打ちの初期値は出さない）。

#include "panel.h"
#include "draw.h"
#include "xg/ram.h"
#include "xg_state.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {

namespace {

struct knob_place { const char *key; double x, y; const char *label; };

// つまみ 18 個。6 列 × 3 行。どれもマルチパートの塊（08 pp xx）に入っていて、
// 塊 1 つの読み返しで全部そろう。前は CC11・CC5・CC1 のつまみもあったが、
// あれらは firmware に問い合わせる口が無い（読み返せない）ので入れ替えた
const knob_place KNOBS[] = {
	{ "part.volume",         336,  74, "Volume"    },
	{ "part.pan",            440,  74, "Pan"       },
	{ "part.dry_level",      544,  74, "Dry"       },
	{ "part.reverb_send",    648,  74, "Reverb"    },
	{ "part.chorus_send",    752,  74, "Chorus"    },
	{ "part.variation_send", 856,  74, "Variation" },

	{ "part.cutoff",         336, 158, "Cutoff"    },
	{ "part.resonance",      440, 158, "Resonance" },
	{ "part.attack",         544, 158, "Attack"    },
	{ "part.decay",          648, 158, "Decay"     },
	{ "part.release",        752, 158, "Release"   },
	{ "part.vib_rate",       856, 158, "Vib Rate"  },

	{ "part.vib_depth",      336, 242, "Vib Depth" },
	{ "part.vib_delay",      440, 242, "Vib Delay" },
	{ "part.note_shift",     544, 242, "Note Shift" },
	{ "part.detune",         648, 242, "Detune"    },
	{ "part.bank_msb",       752, 242, "Bank"      },
	{ "part.program",        856, 242, "Program"   },
};
constexpr int KNOB_COUNT = int(sizeof(KNOBS) / sizeof(KNOBS[0]));

constexpr int PARTS = 32;


} // namespace


const xg::param *panel::knob_param(int ctl) const
{
	const int i = ctl - CTL_KNOB;
	if (i < 0 || i >= KNOB_COUNT)
		return nullptr;
	static std::vector<const xg::param *> cache;
	if (cache.empty())
		for (const knob_place &k : KNOBS)
			cache.push_back(xg::find(k.key));
	return cache[i];
}

bool panel::value_of(int ctl, int &v) const
{
	const xg::param *p = knob_param(ctl);
	return p && m_xg.get(*p, m_part, v);
}

void panel::set_value(int ctl, int v, bridge &br)
{
	const xg::param *p = knob_param(ctl);
	if (!p)
		return;
	v = std::clamp(v, p->min, p->max);
	br.send(m_xg.set(*p, m_part, v));
}


bool panel::tick(bridge &br)
{
	// A minimum-hold release whose time has come (see release())
	if (m_release_pending && m_held && m_held->kind == spot_kind::button &&
	    std::chrono::steady_clock::now() - m_press_at >= MIN_HOLD) {
		br.press(m_held->button, false);
		m_held = nullptr;
		m_release_pending = false;
	}
	// 値は MU2000 に問い合わせずに、音声の糸が 25ms ごとに写すワーク RAM から読む
	// （xg/ram.h）。問い合わせは MIDI IN に入るので、LCD の受信マークが点きっぱなしになる
	br.read_xg(m_ram);
	if (m_ram.serial == m_ram_serial)
		return false;
	m_ram_serial = m_ram.serial;
	// インサーションのパラメータ 1-10 の 16bit の数も、XG の 2 バイトの番地の形にして入れる（xg_state.h）
	load_model(m_xg, m_ram, br.audio_ms());
	return true;
}


void panel::draw_knob(HDC dc, const spot &sp) const
{
	// つまみの場所は「丸の中心 = 枠の上から 26、半径 18」と決めてある
	const double PI = 3.14159265358979;
	const int cx = (sp.r.left + sp.r.right) / 2;
	const int cy = sp.r.top + int(26 * m_scale);
	const int r  = int(18 * m_scale);
	const xg::param *p = knob_param(sp.ctl);
	int v = 0;
	const bool known = value_of(sp.ctl, v);
	const double frac = (known && p && p->max > p->min)
	                  ? std::clamp(double(v - p->min) / (p->max - p->min), 0.0, 1.0) : 0.0;

	// 12 時を 0 度として、-135 度から +135 度まで
	auto at = [&](double deg, double rad_scale, int &x, int &y) {
		const double a = deg * PI / 180.0;
		x = cx + int(std::sin(a) * r * rad_scale);
		y = cy - int(std::cos(a) * r * rad_scale);
	};

	const int lit = known ? int(std::lround(frac * 24)) : -1;
	for (int i = 0; i <= 24; i++) {
		const double deg = -135.0 + 270.0 * i / 24.0;
		int x1, y1, x2, y2;
		at(deg, 1.18, x1, y1);
		at(deg, 1.42, x2, y2);
		line(dc, x1, y1, x2, y2, (i <= lit) ? ACCENT : RGB(66, 70, 76),
		     std::max(1, int(2 * m_scale)));
	}

	disc(dc, cx, cy, r, RGB(52, 56, 62), RGB(88, 93, 100), std::max(1, int(m_scale)));

	// 読めていないうちは針を出さない
	if (known) {
		int px, py;
		at(-135.0 + 270.0 * frac, 0.80, px, py);
		line(dc, cx, cy, px, py, RGB(236, 240, 244), std::max(2, int(2.5 * m_scale)));
	}

	RECT lab{ sp.r.left, sp.r.top + int(44 * m_scale),
	          sp.r.right, sp.r.top + int(55 * m_scale) };
	text_in(dc, lab, sp.label, TEXT_DIM, m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);

	const std::string num = (known && p) ? xg::format(*p, v) : "--";
	RECT val{ sp.r.left, sp.r.top + int(54 * m_scale),
	          sp.r.right, sp.r.top + int(66 * m_scale) };
	text_in(dc, val, num.c_str(), known ? TEXT : TEXT_DIM, m_font_small,
	        DT_CENTER | DT_TOP | DT_SINGLELINE);
}


void panel::paint_editor(HDC dc, const char *status) const
{
	RECT all{ 0, 0, m_w, m_h };
	fill(dc, all, BODY);
	RECT top{ 0, 0, m_w, m_oy + int(24 * m_scale) };
	fill(dc, top, BODY_TOP);

	// パート
	RECT lab = scale(26, 26, 120, 16);
	text_in(dc, lab, "PART", TEXT_DIM, m_font_small, DT_LEFT | DT_TOP | DT_SINGLELINE);

	for (const spot &sp : m_spots) {
		switch (sp.kind) {
		case spot_kind::part: {
			const bool on = (sp.ctl - CTL_PART) == m_part;
			round_box(dc, sp.r, on ? ACCENT : BTN_FACE, BTN_EDGE, int(4 * m_scale));
			char n[8];
			std::snprintf(n, sizeof(n), "%d", sp.ctl - CTL_PART + 1);
			text_in(dc, sp.r, n, on ? RGB(18, 26, 12) : TEXT, m_font_small,
			        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			break;
		}
		case spot_kind::knob:
			draw_knob(dc, sp);
			break;
		case spot_kind::action:
			round_box(dc, sp.r, BTN_FACE, BTN_EDGE, int(5 * m_scale));
			text_in(dc, sp.r, sp.label, TEXT, m_font_small,
			        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			break;
		default:
			break;
		}
	}

	// 選んでいるパートの中身を字でも出す
	auto show = [&](const char *key) -> std::string {
		const xg::param *p = xg::find(key);
		int v = 0;
		return (p && m_xg.get(*p, m_part, v)) ? xg::format(*p, v) : "--";
	};
	char line1[160];
	std::snprintf(line1, sizeof(line1), "Part %d (%s)   Bank %s/%s   Voice %s   Rcv Ch %s",
	              m_part + 1, m_part < 16 ? "A" : "B",
	              show("part.bank_msb").c_str(), show("part.bank_lsb").c_str(),
	              show("part.program").c_str(), show("part.rcv_channel").c_str());
	RECT info = scale(26, 200, 290, 36);
	text_in(dc, info, line1, TEXT, m_font_small, DT_LEFT | DT_TOP | DT_WORDBREAK);

	RECT hint = scale(26, 288, 290, 60);
	text_in(dc, hint,
	        "つまみは上下にドラッグ、またはホイール。\n"
	        "送っているのは XG のパラメータチェンジ。\n"
	        "値は MU2000 に問い合わせて読み返している。",
	        RGB(104, 109, 116), m_font_small, DT_LEFT | DT_TOP | DT_WORDBREAK);

	if (status && status[0])
		text_in(dc, m_status, status, TEXT_DIM, m_font_small,
		        DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	draw_tabs(dc);
}


// ---- 入力

bool panel::press(int x, int y, bridge &br)
{
	const spot *sp = hit(x, y);
	if (!sp)
		return false;
	flush_release(br);

	switch (sp->kind) {
	case spot_kind::tab:
		m_page = (sp->ctl == CTL_TAB_EDIT) ? page::editor
		       : (sp->ctl == CTL_TAB_FX)   ? page::effects : page::front;
		build_spots();
		return true;

	case spot_kind::button:
		m_held = sp;
		m_press_at = std::chrono::steady_clock::now();
		m_release_pending = false;
		br.press(sp->button, true);
		// VALUE −/+ はダイヤルとまったく同じ働き（実測で 1 目盛り = 1 回 = ±1）。
		// 同じものだと見て分かるよう、絵のダイヤルも一緒に回す
		if (sp->button == mu2000::button::value_plus)
			m_wheel_angle = (m_wheel_angle + 15) % 360;
		else if (sp->button == mu2000::button::value_minus)
			m_wheel_angle = (m_wheel_angle + 345) % 360;
		return true;

	case spot_kind::wheel:
		// 掴んで上下に動かす（drag）。掴んだだけでは回さない
		m_held = sp;
		m_drag_y = y;
		m_dial_rest = 0.0;
		return true;

	case spot_kind::volume:
		m_held = sp;
		m_drag_x = x;
		m_drag_y = y;
		m_drag_from = int(m_volume_now * 127);   // 掴んだところからの相対で動かす
		return true;

	case spot_kind::part:
		m_part = sp->ctl - CTL_PART;
		return true;

	case spot_kind::knob: {
		const xg::param *p = knob_param(sp->ctl);
		int v = 0;
		if (!p || !value_of(sp->ctl, v))
			return false;                        // 読めていないうちは動かさない
		m_held = sp;
		m_drag_y = y;
		m_drag_from = v;
		return true;
	}

	case spot_kind::list: {
		// 左右の端を押すと 1 つずつ。真ん中はホイールで回す
		const int edge = (sp->r.right - sp->r.left) / 6;
		if (x < sp->r.left + edge)        step_fx(sp->ctl, -1, br);
		else if (x >= sp->r.right - edge) step_fx(sp->ctl, 1, br);
		return true;
	}

	case spot_kind::action:
		if (sp->ctl == CTL_XG_RESET) {
			// XG システムオン。実機の電源投入直後と同じ状態に戻す。
			// 値は次に RAM を写したときに入れ替わる
			const u8 xg[9] = { 0xf0, 0x43, 0x10, 0x4c, 0x00, 0x00, 0x7e, 0x00, 0xf7 };
			br.send(xg, 9);
		} else if (sp->ctl == CTL_ALL_OFF) {
			for (int p = 0; p < 16; p++) {
				const u8 msg[6] = { u8(0xb0 | p), 120, 0, u8(0xb0 | p), 123, 0 };
				br.send(msg, 6);
			}
		}
		return true;

	default:
		return false;
	}
}

bool panel::drag(int x, int y, bridge &br)
{
	if (!m_held)
		return false;

	if (m_held->kind == spot_kind::wheel)
		return dial_follow(y, br);

	if (m_held->kind == spot_kind::volume) {
		// 横でも縦でも動かせるように、動いた量の大きいほうを取る。
		// 丸いつまみは縦で動かしたくなるので
		const int dx = x - m_drag_x, dy = m_drag_y - y;
		const int span = std::max(1L, m_held->r.right - m_held->r.left);
		const int move = (std::abs(dy) > std::abs(dx)) ? dy : dx;
		m_volume_now = std::clamp(m_drag_from / 127.0 + double(move) / span, 0.0, 1.0);
		br.set_gain(float(m_volume_now));
		return true;
	}
	if (m_held->kind == spot_kind::knob) {
		// 100 画素で端から端まで。細かく合わせたいときはホイールを使う
		const xg::param *p = knob_param(m_held->ctl);
		if (!p)
			return false;
		const int v = m_drag_from + int((m_drag_y - y) * double(p->max - p->min) / (100.0 * m_scale));
		int now = 0;
		if (!value_of(m_held->ctl, now) || std::clamp(v, p->min, p->max) != now) {
			set_value(m_held->ctl, v, br);
			return true;
		}
	}
	return false;
}

bool panel::release(bridge &br)
{
	if (!m_held)
		return false;
	if (m_held->kind == spot_kind::button) {
		// A tap shorter than an audio block would never reach the firmware:
		// hold it until the minimum, tick() completes the release
		if (std::chrono::steady_clock::now() - m_press_at < MIN_HOLD) {
			m_release_pending = true;
			return true;
		}
		br.press(m_held->button, false);
	}
	m_held = nullptr;
	return true;
}

void panel::flush_release(bridge &br)
{
	if (!m_release_pending || !m_held || m_held->kind != spot_kind::button)
		return;
	br.press(m_held->button, false);
	m_release_pending = false;
}

// ダイヤルを掴んで上下に動かす。上へ動かすと +、下へ動かすと −（ホイールと同じ向き）。
// 動かした距離に比例して目盛りを送り（1 目盛りは VALUE −/+ を 1 回押したのと同じ）、
// 絵のダイヤルもホイールと同じく 1 目盛りで 15° 回す。1 目盛りは DIAL_PIXELS 画素（窓の大きさに合わせて伸び縮みする）
bool panel::dial_follow(int y, bridge &br)
{
	static constexpr double DIAL_PIXELS = 4.0;
	m_dial_rest += double(m_drag_y - y);
	m_drag_y = y;
	const double per = DIAL_PIXELS * m_scale;
	const int steps = int(m_dial_rest / per);      // 0 の側へ切り捨て。余りは次へ持ち越す
	if (!steps)
		return false;
	m_dial_rest -= steps * per;
	br.turn(steps);
	m_wheel_angle = ((m_wheel_angle + steps * 15) % 360 + 360) % 360;
	return true;
}

bool panel::wheel_at(int x, int y, int delta, bridge &br)
{
	const spot *sp = hit(x, y);
	// 音量つまみの上ではホイールで音量。1 目盛りで 2%
	if (sp && sp->kind == spot_kind::volume) {
		m_volume_now = std::clamp(m_volume_now + delta * 0.02, 0.0, 1.0);
		br.set_gain(float(m_volume_now));
		return true;
	}
	if (sp && sp->kind == spot_kind::knob) {
		int v = 0;
		if (value_of(sp->ctl, v))
			set_value(sp->ctl, v + delta, br);
		return true;
	}
	if (sp && sp->kind == spot_kind::list) {
		step_fx(sp->ctl, delta, br);
		return true;
	}
	// どの面でも、つまみの上でなければダイヤルとして効く。
	// 実機ではダイヤルと VALUE −/+ は同じ働きなので、
	// ホイールをどこで回しても VALUE を 1 回叩いたのと同じになる
	br.turn(delta);
	m_wheel_angle = (m_wheel_angle + delta * 15) % 360;
	return true;
}


// ---- 配置

void panel::build_editor_spots()
{
	// 32 パート。8 列 × 4 行。上 2 行が口 A（1-16）、下 2 行が口 B（17-32）
	for (int i = 0; i < PARTS; i++)
		m_spots.push_back({ spot_kind::part, mu2000::button::count, CTL_PART + i,
		                    scale(26 + (i % 8) * 36, 44 + (i / 8) * 36, 30, 30), "", "" });

	// 枠は 80 × 66。丸の中心は上から 26、名前と値はその下
	for (int i = 0; i < KNOB_COUNT; i++) {
		const knob_place &k = KNOBS[i];
		m_spots.push_back({ spot_kind::knob, mu2000::button::count, CTL_KNOB + i,
		                    scale(k.x - 40, k.y - 26, 80, 66), k.label, "" });
	}

	m_spots.push_back({ spot_kind::action, mu2000::button::count, CTL_XG_RESET,
	                    scale(26, 250, 130, 24), "XG リセット", "" });
	m_spots.push_back({ spot_kind::action, mu2000::button::count, CTL_ALL_OFF,
	                    scale(162, 250, 150, 24), "オールノートオフ", "" });
}

} // namespace ui
