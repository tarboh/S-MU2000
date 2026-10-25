// license:BSD-3-Clause
// motif_play: MOTIF-RACK ES のサンプリング方式の芯（sampler_engine.h）を、画面で選んだ音色でその場で弾く（2026-10-25）。
//
//   build/motif_play.exe [パッチの置き場] [--midi 番号] [--nomidi] [--selftest 出力.wav [音色の番号]]
//
// パッチの置き場は、tools/motif/mr_patch.py が全部の音色を書いた所（既定は exe の置き場から ../roms/motif-rack-es/capture/patches_all）。
// 弾き方: 画面の鍵盤をマウスで、PC のキーボード（Z〜M・Q〜I の 2 段）で、MIDI 入力（ノート・CC1/2/4/16/17/22・CC64・CC7/11・
// アフタータッチ・ピッチベンド・プログラムチェンジ）で。MIDI の出力は開かない（実機へは何も送らない）。
// エフェクトは sampler_fx_rt.h で掛ける（オフラインの sampler_fx.h と同じ音。fxrt_test.cpp で比べた）: インサーションの 3 Band EQ・2 Band EQ・
// Cross Delay・コンプ（組とつなぎ方ごと）、リバーブ、コーラスの枠のコーラスの型・Cross Delay。ほかの型はまだ回せない（インサーションは素通し）。
// --shot 絵.bmp は、音色を読み終えてから画面を 1 枚 BMP に書いて終わる（試験用。--shot-keys 60,64,67 でその鍵を押した絵）。
// --selftest は、画面を出さずに音色を 1 つ読み、音の糸と同じ道（fill）で和音を 2 秒鳴らして WAV に書いて終わる（試験用）。

#include "sampler_engine.h"
#include "sampler_fx_rt.h"
#include "ui/audio_out.h"
#include "ui/midi_in.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fs = std::filesystem;

namespace {

// ---- 音色の一覧 ----

struct voice_entry {
	std::string file, name, bank;   // bank はファイル名の頭（00〜06 がプリセット、0A は GM など）
	int number = 0;
};

std::vector<voice_entry> scan_patches(const fs::path &dir)
{
	std::vector<voice_entry> v;
	std::error_code ec;
	for (const auto &de : fs::directory_iterator(dir, ec)) {
		if (de.path().extension() != ".patch")
			continue;
		voice_entry e;
		e.file = de.path().string();
		const std::string stem = de.path().stem().string();
		e.bank = stem.substr(0, stem.find('_'));
		e.number = stem.size() > 3 ? std::atoi(stem.c_str() + 3) : 0;
		FILE *f = std::fopen(e.file.c_str(), "r");
		char line[512] = {};
		if (f && std::fgets(line, sizeof line, f) && line[0] == '#') {
			e.name = line + 1;
			while (!e.name.empty() && (e.name.back() == '\n' || e.name.back() == '\r' || e.name.back() == ' '))
				e.name.pop_back();
			while (!e.name.empty() && e.name.front() == ' ')
				e.name.erase(e.name.begin());
		}
		if (f)
			std::fclose(f);
		if (e.name.empty() || stem.size() < 6)
			continue;
		v.push_back(e);
	}
	std::sort(v.begin(), v.end(), [](const voice_entry &a, const voice_entry &b) { return a.bank != b.bank ? a.bank < b.bank : a.number < b.number; });
	return v;
}

// コントロールセットのソースと行き先の名前（ファームウェアの名前の表の順。doc/motif-rack-es-sampler.md）
const char *source_name(int s)
{
	static const char *n[16] = {"PB", "MW (CC1)", "AT", "AT", "FC1?", "FS?", "RB (CC22)", "BC (CC2)", "AS1 (CC16)", "AS2 (CC17)",
	                            "FC (CC4)", "FC (CC4)", "FC (CC4)", "?", "?", "?"};
	return s >= 0 && s < 16 ? n[s] : "?";
}

std::string dest_name(int d)
{
	static const char *n[] = {"off", "Volume", "RevSend", "ChoSend"};
	static const char *m[] = {"CLFOSpd", "CLFO-D1", "CLFO-D2", "CLFO-D3", "PorTime", "ELM-Lvl", "ELM-Pan", "ELM-Dly", "ELFOSpd", "ELFO-PM", "ELFO-AM",
	                          "ELFO-FM", "PCH-Crs", "PCH-Fin", "PEG-Dpt", "PEG-Hld", "PEG-Atk", "PEG-D1", "PEG-D2", "PEG-Rel", "AEG-Atk", "AEG-D1",
	                          "AEG-D2", "AEG-Rel", "FEG-Dpt", "FEG-Hld", "FEG-Atk", "FEG-D1", "FEG-D2", "FEG-Rel", "FLT-Frq", "FLT-Rez", "HPF-Frq"};
	if (d >= 0 && d < 4)
		return n[d];
	if (d >= 4 && d < 20)
		return "INS1-P" + std::to_string(d - 3);
	if (d >= 20 && d < 36)
		return "INS2-P" + std::to_string(d - 19);
	if (d >= 36 && d < 69)
		return m[d - 36];
	return std::to_string(d);
}

// ---- 鳴らす側 ----

// 読み込んだ音色（エンジンはパッチを参照するので、一緒に持つ）
struct loaded {
	motif_sampler::patch pt;
	std::unique_ptr<motif_sampler::engine> eg;
	motif_fx_rt::insertion_rt ins;
	motif_fx_rt::reverb_rt rev;
	motif_fx_rt::chorus_rt cho;
	std::string label, ins_name, rev_name, cho_name;
};

struct player {
	std::mutex mtx;                          // 音の糸と画面の糸の間（engine・sustain・音量）
	std::unique_ptr<loaded> cur;
	ui::midi_in midi;
	std::atomic<int> key_count[128] = {};    // 押さえている数（画面の鍵盤の色）
	bool sustain = false;
	bool sustained[128] = {};
	int cc7 = 127, cc11 = 127;
	float master = 0.8f;
	bool use_ins = true, use_rev = true, use_cho = true;
	std::atomic<float> peak{0};
	std::atomic<int> voices{0};
	std::atomic<int> pending_program{-1};   // MIDI のプログラムチェンジ（画面の糸で読み込む）
	std::atomic<u64> midi_bytes{0};
	int compact_count = 0;
	// MIDI の解析
	u8 status = 0, d0 = 0;
	int nd = 0;
	bool in_sysex = false;

	// ここから下は mtx を取ってから呼ぶ
	void note_on(int key, int vel)
	{
		if (key < 0 || key > 127)
			return;
		if (vel <= 0) {
			note_off(key);
			return;
		}
		key_count[key]++;
		sustained[key] = false;
		if (cur)
			cur->eg->note_on(key, std::min(127, vel));
	}
	void note_off(int key)
	{
		if (key < 0 || key > 127 || key_count[key] <= 0)
			return;
		key_count[key]--;
		if (sustain) {
			sustained[key] = true;
			return;
		}
		if (cur)
			cur->eg->note_off(key);
	}
	void set_sustain(bool on)
	{
		sustain = on;
		if (on)
			return;
		for (int k = 0; k < 128; k++)
			if (sustained[k]) {
				sustained[k] = false;
				if (cur)
					cur->eg->note_off(k);
			}
	}
	void source(int s, int v)
	{
		if (cur)
			cur->eg->set_source(s, v);
	}
	void all_off()
	{
		for (auto &c : key_count)
			c = 0;
		for (bool &s : sustained)
			s = false;
		if (cur)
			cur->eg->all_sound_off();
	}
	void midi_message(u8 st, u8 a, u8 b)
	{
		switch (st & 0xF0) {
		case 0x80: note_off(a); break;
		case 0x90: note_on(a, b); break;
		case 0xB0:
			switch (a) {
			case 1: source(1, b); break;
			case 2: source(7, b); break;
			case 4: source(10, b); source(11, b); source(12, b); break;
			case 16: source(8, b); break;
			case 17: source(9, b); break;
			case 22: source(6, b); break;
			case 7: cc7 = b; break;
			case 11: cc11 = b; break;
			case 64: set_sustain(b >= 64); break;
			case 120: all_off(); break;
			case 121:
				for (int s = 0; s < 16; s++)
					source(s, motif_sampler::engine::bipolar(s) ? 64 : 0);
				cc11 = 127;
				set_sustain(false);
				break;
			case 123:
				for (int k = 0; k < 128; k++)
					while (key_count[k] > 0)
						note_off(k);
				break;
			}
			break;
		case 0xC0: pending_program = a; break;
		case 0xD0: source(2, a); source(3, a); break;
		case 0xE0: source(0, b); break;          // 上位 7 ビットだけ（芯のピッチベンドは 0〜127）
		}
	}
	void midi_byte(u8 v)
	{
		if (v >= 0xF8)
			return;                          // リアルタイム
		if (v == 0xF0) {
			in_sysex = true;
			return;
		}
		if (v == 0xF7) {
			in_sysex = false;
			return;
		}
		if (v & 0x80) {
			in_sysex = false;
			if (v >= 0xF0) {
				status = 0;
				return;
			}
			status = v;
			nd = 0;
			return;
		}
		if (in_sysex || !status)
			return;
		const int need = (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 ? 1 : 2;
		if (nd == 0) {
			d0 = v;
			nd = 1;
			if (need == 1) {
				midi_message(status, d0, 0);
				nd = 0;
			}
			return;
		}
		midi_message(status, d0, v);
		nd = 0;
	}

	// 音の糸から: 16bit 2ch で frames ぶん
	void fill(s16 *out, u32 frames)
	{
		std::lock_guard<std::mutex> lk(mtx);
		u8 v;
		while (midi.is_open() && midi.pop(v)) {
			midi_byte(v);
			midi_bytes++;
		}
		float pk = 0;
		const double part = std::pow(cc7 / 127.0, 2.0) * std::pow(cc11 / 127.0, 2.0) * master;
		u32 done = 0;
		while (done < frames) {
			const int n = int(std::min<u32>(256, frames - done));
			float l[256] = {}, r[256] = {}, voff[256] = {};
			float g0[256] = {}, g1[256] = {}, g2[256] = {}, h0[256] = {}, h1[256] = {}, h2[256] = {};
			float *gl[3] = {g0, g1, g2}, *gr[3] = {h0, h1, h2};
			if (cur)
				cur->eg->render(n, l, r, gl, gr, voff);
			for (int j = 0; j < n; j++) {
				// 共通の音量まで掛けたドライに、リバーブとコーラスの枠の wet を足し、パートの音量（CC7・CC11）と全体の音量を掛ける
				double dl = 0, dr = 0;
				if (cur) {
					// インサーションは組（エレメントのスイッチ）ごとの音に掛け、そのあとで共通の音量を掛ける
					const double g = cur->eg->volume_gain(voff[j]);
					if (use_ins) {
						const double xl[3] = {g0[j], g1[j], g2[j]}, xr[3] = {h0[j], h1[j], h2[j]};
						cur->ins.tick(xl, xr, dl, dr);
					} else {
						dl = l[j];
						dr = r[j];
					}
					dl *= g;
					dr *= g;
					double wl = 0, wr = 0, xl = 0, xr = 0;
					if (cur->rev.active()) {
						cur->rev.tick(dl, dr, xl, xr);
						if (use_rev)
							wl += xl, wr += xr;
					}
					if (cur->cho.active()) {
						cur->cho.tick(dl, dr, xl, xr);
						if (use_cho)
							wl += xl, wr += xr;
					}
					dl += wl;
					dr += wr;
				}
				// 実機の出口は ±1.0 で頭打ち
				const float a = float(std::fmax(-1.0, std::fmin(1.0, dl * part))), b = float(std::fmax(-1.0, std::fmin(1.0, dr * part)));
				pk = std::max(pk, std::max(std::fabs(a), std::fabs(b)));
				out[2 * (done + j)] = s16(std::lround(a * 32767.0f));
				out[2 * (done + j) + 1] = s16(std::lround(b * 32767.0f));
			}
			done += n;
		}
		if (cur) {
			if (++compact_count >= 16) {
				compact_count = 0;
				cur->eg->compact();
			}
			voices = int(cur->eg->active_voices());
		} else
			voices = 0;
		peak = std::max(pk, peak.load() * 0.9f);
	}
};

std::unique_ptr<loaded> load_voice(const voice_entry &v)
{
	auto L = std::make_unique<loaded>();
	if (!L->pt.load(v.file))
		return nullptr;
	L->eg = std::make_unique<motif_sampler::engine>(L->pt);
	L->eg->preload();
	L->label = v.bank + ":" + std::to_string(v.number) + " " + v.name;
	// リバーブとコーラスの枠（応答は capture の置き場の fx/・fxd/。パッチの置き場の 1 つ上）
	const std::string cap = L->pt.dir + "..";
	auto get = [&](const char *k) { auto it = L->pt.fxline.find(k); return it == L->pt.fxline.end() ? motif_fx::block() : motif_fx::parse_block(it->second); };
	const motif_fx::block rv = get("reverb"), ch = get("chorus");
	char t[96];
	const motif_fx::block ia = get("insa"), ib = get("insb");
	L->ins.init(ia, ib, motif_sampler::geti(L->pt.fxline, "connect"), motif_sampler::geti(L->pt.fxline, "tempo", 120));
	std::snprintf(t, sizeof t, "A %02X %02X・B %02X %02X・つなぎ %d", ia.msb, ia.lsb, ib.msb, ib.lsb, motif_sampler::geti(L->pt.fxline, "connect"));
	L->ins_name = L->ins.supported() ? t : std::string(t) + "（まだ回せない型は素通し）";
	L->rev.init(rv, cap + "/fx");
	std::snprintf(t, sizeof t, "%02X %02X・送り %d", rv.msb, rv.lsb, rv.send);
	L->rev_name = rv.send ? (L->rev.active() ? t : std::string(t) + "（応答が無い）") : "送り 0";
	L->cho.init(ch, motif_sampler::geti(L->pt.fxline, "tempo", 120));
	std::snprintf(t, sizeof t, "%02X %02X・送り %d", ch.msb, ch.lsb, ch.send);
	L->cho_name = ch.send ? (L->cho.active() ? t : std::string(t) + "（この型はまだ）") : "送り 0";
	return L;
}

void write_wav16(const std::string &path, const std::vector<s16> &pcm)
{
	FILE *f = std::fopen(path.c_str(), "wb");
	if (!f)
		return;
	const uint32_t data = uint32_t(pcm.size() * 2);
	auto u32w = [&](uint32_t x) { std::fwrite(&x, 4, 1, f); };
	auto u16w = [&](uint16_t x) { std::fwrite(&x, 2, 1, f); };
	std::fwrite("RIFF", 1, 4, f); u32w(36 + data); std::fwrite("WAVEfmt ", 1, 8, f);
	u32w(16); u16w(1); u16w(2); u32w(44100); u32w(44100 * 4); u16w(4); u16w(16);
	std::fwrite("data", 1, 4, f); u32w(data);
	std::fwrite(pcm.data(), 2, pcm.size(), f);
	std::fclose(f);
}

// ---- 画面（Win32 + Direct3D 11） ----

ID3D11Device *g_dev = nullptr;
ID3D11DeviceContext *g_ctx = nullptr;
IDXGISwapChain *g_swap = nullptr;
ID3D11RenderTargetView *g_rtv = nullptr;
UINT g_resize_w = 0, g_resize_h = 0;

void create_rtv()
{
	ID3D11Texture2D *back = nullptr;
	g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
	if (back) {
		g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
		back->Release();
	}
}

void release_rtv()
{
	if (g_rtv) {
		g_rtv->Release();
		g_rtv = nullptr;
	}
}

bool create_device(HWND hwnd)
{
	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferCount = 2;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hwnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
	D3D_FEATURE_LEVEL got;
	HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
	if (hr == DXGI_ERROR_UNSUPPORTED)
		hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
	if (hr != S_OK)
		return false;
	create_rtv();
	return true;
}

void cleanup_device()
{
	release_rtv();
	if (g_swap) g_swap->Release();
	if (g_ctx) g_ctx->Release();
	if (g_dev) g_dev->Release();
	g_swap = nullptr;
	g_ctx = nullptr;
	g_dev = nullptr;
}

LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
		return true;
	switch (msg) {
	case WM_SIZE:
		if (wp != SIZE_MINIMIZED) {
			g_resize_w = LOWORD(lp);
			g_resize_h = HIWORD(lp);
		}
		return 0;
	case WM_SYSCOMMAND:
		if ((wp & 0xfff0) == SC_KEYMENU)
			return 0;
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

// 画面の鍵盤（C から 5 オクターブ）。押された鍵を返す（無ければ −1）
int draw_keyboard(player &pl, int low, int octaves, int &mouse_key, int vel)
{
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const ImVec2 p0 = ImGui::GetCursorScreenPos();
	const float avail = ImGui::GetContentRegionAvail().x;
	const int nwhite = octaves * 7 + 1;
	const float ww = std::max(10.0f, std::floor(avail / nwhite)), wh = 120.0f, bw = ww * 0.62f, bh = wh * 0.62f;
	ImGui::InvisibleButton("##keyboard", ImVec2(ww * nwhite, wh));
	const bool active = ImGui::IsItemActive();
	const ImVec2 m = ImGui::GetIO().MousePos;
	static const int white_off[7] = {0, 2, 4, 5, 7, 9, 11};
	static const int black_after[5] = {0, 1, 3, 4, 5};      // 白鍵の番号のあと（C#・D#・F#・G#・A#）
	static const int black_off[5] = {1, 3, 6, 8, 10};
	int hover = -1;
	// 黒鍵を先に当たり判定
	for (int o = 0; o < octaves && hover < 0; o++)
		for (int b = 0; b < 5; b++) {
			const float x = p0.x + (o * 7 + black_after[b] + 1) * ww - bw / 2;
			if (m.x >= x && m.x < x + bw && m.y >= p0.y && m.y < p0.y + bh) {
				hover = low + o * 12 + black_off[b];
				break;
			}
		}
	if (hover < 0)
		for (int w = 0; w < nwhite; w++) {
			const float x = p0.x + w * ww;
			if (m.x >= x && m.x < x + ww && m.y >= p0.y && m.y < p0.y + wh) {
				hover = low + (w / 7) * 12 + white_off[w % 7];
				break;
			}
		}
	for (int w = 0; w < nwhite; w++) {
		const int k = low + (w / 7) * 12 + white_off[w % 7];
		const float x = p0.x + w * ww;
		const bool on = k < 128 && pl.key_count[k] > 0;
		dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + ww - 1, p0.y + wh), on ? IM_COL32(120, 180, 255, 255) : IM_COL32(240, 240, 236, 255), 3.0f);
		if (w % 7 == 0) {
			char t[8];
			std::snprintf(t, sizeof t, "C%d", (k / 12) - 1);
			dl->AddText(ImVec2(x + 3, p0.y + wh - 20), IM_COL32(90, 90, 90, 255), t);
		}
	}
	for (int o = 0; o < octaves; o++)
		for (int b = 0; b < 5; b++) {
			const int k = low + o * 12 + black_off[b];
			const float x = p0.x + (o * 7 + black_after[b] + 1) * ww - bw / 2;
			const bool on = k < 128 && pl.key_count[k] > 0;
			dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + bw, p0.y + bh), on ? IM_COL32(60, 120, 230, 255) : IM_COL32(30, 30, 34, 255), 2.0f);
		}
	// マウス: 押している間、乗っている鍵を鳴らす（滑らせれば次の鍵へ）
	const int want = active ? hover : -1;
	if (want != mouse_key) {
		std::lock_guard<std::mutex> lk(pl.mtx);
		if (mouse_key >= 0)
			pl.note_off(mouse_key);
		if (want >= 0 && want < 128)
			pl.note_on(want, vel);
		mouse_key = want;
	}
	return hover;
}

struct pc_key { ImGuiKey key; int semi; };
const pc_key PC_KEYS[] = {
	{ImGuiKey_Z, 0}, {ImGuiKey_S, 1}, {ImGuiKey_X, 2}, {ImGuiKey_D, 3}, {ImGuiKey_C, 4}, {ImGuiKey_V, 5}, {ImGuiKey_G, 6}, {ImGuiKey_B, 7},
	{ImGuiKey_H, 8}, {ImGuiKey_N, 9}, {ImGuiKey_J, 10}, {ImGuiKey_M, 11}, {ImGuiKey_Comma, 12}, {ImGuiKey_L, 13}, {ImGuiKey_Period, 14},
	{ImGuiKey_Q, 12}, {ImGuiKey_2, 13}, {ImGuiKey_W, 14}, {ImGuiKey_3, 15}, {ImGuiKey_E, 16}, {ImGuiKey_R, 17}, {ImGuiKey_5, 18},
	{ImGuiKey_T, 19}, {ImGuiKey_6, 20}, {ImGuiKey_Y, 21}, {ImGuiKey_7, 22}, {ImGuiKey_U, 23}, {ImGuiKey_I, 24}, {ImGuiKey_9, 25},
	{ImGuiKey_O, 26}, {ImGuiKey_0, 27}, {ImGuiKey_P, 28},
};

// 今の描いた絵（バックバッファ）を 24 ビットの BMP に書く
bool save_backbuffer(const std::string &path)
{
	ID3D11Texture2D *back = nullptr;
	g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
	if (!back)
		return false;
	D3D11_TEXTURE2D_DESC d;
	back->GetDesc(&d);
	d.Usage = D3D11_USAGE_STAGING;
	d.BindFlags = 0;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	d.MiscFlags = 0;
	ID3D11Texture2D *st = nullptr;
	g_dev->CreateTexture2D(&d, nullptr, &st);
	bool ok = false;
	if (st) {
		g_ctx->CopyResource(st, back);
		D3D11_MAPPED_SUBRESOURCE m;
		if (g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m) == S_OK) {
			const int w = int(d.Width), h = int(d.Height), row = (w * 3 + 3) & ~3;
			FILE *f = std::fopen(path.c_str(), "wb");
			if (f) {
				const uint32_t size = 54 + row * h;
				unsigned char hd[54] = {'B', 'M'};
				auto put32 = [&](int o, uint32_t v) { std::memcpy(hd + o, &v, 4); };
				put32(2, size); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
				hd[26] = 1; hd[28] = 24; put32(34, row * h);
				std::fwrite(hd, 1, 54, f);
				std::vector<unsigned char> line(row);
				for (int y = h - 1; y >= 0; y--) {
					const unsigned char *src = (const unsigned char *)m.pData + y * m.RowPitch;
					for (int x = 0; x < w; x++) {
						line[3 * x] = src[4 * x + 2];
						line[3 * x + 1] = src[4 * x + 1];
						line[3 * x + 2] = src[4 * x];
					}
					std::fwrite(line.data(), 1, row, f);
				}
				std::fclose(f);
				ok = true;
			}
			g_ctx->Unmap(st, 0);
		}
		st->Release();
	}
	back->Release();
	return ok;
}

fs::path default_patch_dir()
{
	wchar_t buf[MAX_PATH];
	GetModuleFileNameW(nullptr, buf, MAX_PATH);
	const fs::path exe = fs::path(buf).parent_path();
	for (const fs::path &p : {exe / ".." / "roms" / "motif-rack-es" / "capture" / "patches_all", fs::current_path() / "roms" / "motif-rack-es" / "capture" / "patches_all",
	                          exe / "patches_all"})
		if (fs::exists(p))
			return fs::weakly_canonical(p);
	return exe / ".." / "roms" / "motif-rack-es" / "capture" / "patches_all";
}

} // namespace

int main(int argc, char **argv)
{
	fs::path dir;
	int midi_dev = -1;
	bool nomidi = false;
	std::string selftest, shot;
	std::vector<int> shot_keys;
	int selftest_voice = 0;
	for (int i = 1; i < argc; i++) {
		const std::string a = argv[i];
		if (a == "--midi" && i + 1 < argc)
			midi_dev = std::atoi(argv[++i]);
		else if (a == "--nomidi")
			nomidi = true;
		else if (a == "--shot" && i + 1 < argc)
			shot = argv[++i];
		else if (a == "--shot-keys" && i + 1 < argc) {
			for (const char *p = argv[++i]; *p;) {
				shot_keys.push_back(std::atoi(p));
				const char *c = std::strchr(p, ',');
				if (!c)
					break;
				p = c + 1;
			}
		}
		else if (a == "--selftest" && i + 1 < argc) {
			selftest = argv[++i];
			if (i + 1 < argc && argv[i + 1][0] != '-')
				selftest_voice = std::atoi(argv[++i]);
		} else
			dir = a;
	}
	if (dir.empty())
		dir = default_patch_dir();
	std::vector<voice_entry> list = scan_patches(dir);

	auto pl = std::make_unique<player>();
	if (!shot.empty())
		pl->master = 0.0f;       // 撮るだけのときは音を出さない
	if (!selftest.empty()) {
		// 画面を出さずに、音の糸と同じ道で和音を鳴らして書く
		if (list.empty()) {
			std::fprintf(stderr, "パッチが見つからない: %s\n", dir.string().c_str());
			return 1;
		}
		pl->cur = load_voice(list[std::min<size_t>(selftest_voice, list.size() - 1)]);
		if (!pl->cur)
			return 1;
		std::vector<s16> pcm(44100 * 3 * 2);
		const u32 block = 441;
		for (u32 i = 0; i < 44100 * 3; i += block) {
			{
				std::lock_guard<std::mutex> lk(pl->mtx);
				if (i == 0)
					for (int k : {60, 64, 67})
						pl->note_on(k, 100);
				if (i == 44100 * 2)
					for (int k : {60, 64, 67})
						pl->note_off(k);
			}
			pl->fill(pcm.data() + 2 * i, block);
		}
		write_wav16(selftest, pcm);
		std::printf("作ったエレメント %zu\n", pl->cur->eg->elements_started());
		std::printf("%s を鳴らした（ピーク %.3f）\n", pl->cur->label.c_str(), pl->peak.load());
		return 0;
	}

	ImGui_ImplWin32_EnableDpiAwareness();
	WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, wnd_proc, 0L, 0L, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"motif_play", nullptr};
	RegisterClassExW(&wc);
	HWND hwnd = CreateWindowW(wc.lpszClassName, L"MOTIF-RACK ES の芯を弾く", WS_OVERLAPPEDWINDOW, 100, 100, 1280, 820, nullptr, nullptr, wc.hInstance, nullptr);
	if (!create_device(hwnd)) {
		cleanup_device();
		UnregisterClassW(wc.lpszClassName, wc.hInstance);
		return 1;
	}
	ShowWindow(hwnd, SW_SHOWDEFAULT);
	UpdateWindow(hwnd);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr;
	ImGui::StyleColorsDark();
	for (const char *f : {"C:\\Windows\\Fonts\\meiryo.ttc", "C:\\Windows\\Fonts\\YuGothM.ttc", "C:\\Windows\\Fonts\\msgothic.ttc"})
		if (fs::exists(f) && io.Fonts->AddFontFromFileTTF(f, 18.0f))
			break;
	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(g_dev, g_ctx);

	// 音の出口
	ui::audio_out audio;
	std::string audio_err;
	const bool audio_ok = audio.start(40, [&](s16 *out, u32 frames) { pl->fill(out, frames); }, audio_err);
	std::vector<std::string> midi_names = nomidi ? std::vector<std::string>() : ui::midi_in::list();
	std::string midi_err;
	if (!nomidi && midi_dev >= 0) {
		std::lock_guard<std::mutex> lk(pl->mtx);
		pl->midi.open(midi_dev, midi_err);
	}

	// 音色を読む（読む糸で。終わったら入れ替える）
	std::thread loader;
	std::atomic<bool> loading{false};
	std::unique_ptr<loaded> ready;
	std::mutex ready_mtx;
	int selected = -1, want = list.empty() ? -1 : 0;
	std::string status;

	char filter[64] = "";
	int octave = 3, velocity = 100, mouse_key = -1;
	int mw = 0, at = 0, as1 = 64, as2 = 64, rb = 64, bc = 0, fc = 0, pb = 64;
	bool pc_held[sizeof PC_KEYS / sizeof PC_KEYS[0]] = {};
	int pc_note[sizeof PC_KEYS / sizeof PC_KEYS[0]] = {};
	bool done = false;
	int shot_frames = 0;
	while (!done) {
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
			if (msg.message == WM_QUIT)
				done = true;
		}
		if (done)
			break;
		if (g_resize_w != 0 && g_resize_h != 0) {
			release_rtv();
			g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
			g_resize_w = g_resize_h = 0;
			create_rtv();
		}

		// MIDI のプログラムチェンジ: 今のバンクの中の番号
		const int pc = pl->pending_program.exchange(-1);
		if (pc >= 0 && selected >= 0)
			for (size_t i = 0; i < list.size(); i++)
				if (list[i].bank == list[selected].bank && list[i].number == pc)
					want = int(i);
		// 読み終わったものを入れ替える（古いものは鳴らす糸の外で捨てる）
		{
			std::unique_ptr<loaded> nw;
			{
				std::lock_guard<std::mutex> lk(ready_mtx);
				nw = std::move(ready);
			}
			if (nw) {
				std::unique_ptr<loaded> old;
				{
					std::lock_guard<std::mutex> lk(pl->mtx);
					old = std::move(pl->cur);
					pl->cur = std::move(nw);
					for (int s = 0; s < 16; s++) {
						const int v = s == 1 ? mw : s == 2 || s == 3 ? at : s == 6 ? rb : s == 7 ? bc : s == 8 ? as1 : s == 9 ? as2 : s >= 10 && s <= 12 ? fc : s == 0 ? pb : -1;
						if (v >= 0)
							pl->cur->eg->set_source(s, v);
					}
					for (auto &c : pl->key_count)
						c = 0;
				}
				status = pl->cur->label;
				old.reset();
			}
		}
		if (want >= 0 && want != selected && !loading) {
			if (loader.joinable())
				loader.join();
			selected = want;
			loading = true;
			status = "読み込み中: " + list[want].name;
			loader = std::thread([&, v = list[want]]() {
				auto L = load_voice(v);
				std::lock_guard<std::mutex> lk(ready_mtx);
				ready = std::move(L);
				loading = false;
			});
		}

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		// PC のキーボード（文字を打っている間は使わない）
		if (!io.WantTextInput)
			for (size_t i = 0; i < sizeof PC_KEYS / sizeof PC_KEYS[0]; i++) {
				const bool down = ImGui::IsKeyDown(PC_KEYS[i].key);
				if (down != pc_held[i]) {
					std::lock_guard<std::mutex> lk(pl->mtx);
					if (down) {
						pc_note[i] = 12 * (octave + 1) + PC_KEYS[i].semi;
						pl->note_on(pc_note[i], velocity);
					} else
						pl->note_off(pc_note[i]);
					pc_held[i] = down;
				}
			}

		const ImGuiViewport *vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(vp->WorkPos);
		ImGui::SetNextWindowSize(vp->WorkSize);
		ImGui::Begin("main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

		// 左: 音色の一覧
		ImGui::BeginChild("voices", ImVec2(330, -150), ImGuiChildFlags_Borders);
		ImGui::SetNextItemWidth(-1);
		ImGui::InputTextWithHint("##filter", "音色の名前で絞る", filter, sizeof filter);
		ImGui::BeginChild("voicelist");
		std::string last_bank;
		std::string f = filter;
		std::transform(f.begin(), f.end(), f.begin(), ::tolower);
		for (size_t i = 0; i < list.size(); i++) {
			std::string nm = list[i].name;
			std::string low = nm;
			std::transform(low.begin(), low.end(), low.begin(), ::tolower);
			if (!f.empty() && low.find(f) == std::string::npos)
				continue;
			if (list[i].bank != last_bank) {
				last_bank = list[i].bank;
				ImGui::SeparatorText(("バンク " + last_bank).c_str());
			}
			char lab[96];
			std::snprintf(lab, sizeof lab, "%03d  %s##%zu", list[i].number, nm.c_str(), i);
			if (ImGui::Selectable(lab, int(i) == selected))
				want = int(i);
		}
		ImGui::EndChild();
		ImGui::EndChild();
		ImGui::SameLine();

		// 右: 今の音色・つまみ・状態
		ImGui::BeginChild("right", ImVec2(0, -150));
		ImGui::Text("音色: %s%s", status.c_str(), loading ? "" : "");
		if (pl->cur) {
			const motif_sampler::patch &pt = pl->cur->pt;
			using motif_sampler::geti;
			ImGui::TextDisabled("エレメント %zu・%s・同じ鍵 %s・ポルタメント %s（時間 %d）・音量 %d", pt.els.size(),
			                    geti(pt.voiceline, "mono", 1) == 0 ? "モノ" : "ポリ", geti(pt.voiceline, "same", 1) == 0 ? "1 音" : "重ねる",
			                    geti(pt.voiceline, "porta", 0) ? (geti(pt.voiceline, "pmode", 0) ? "フルタイム" : "フィンガード") : "切",
			                    geti(pt.voiceline, "ptime", 0), geti(pt.voiceline, "volume", 127));
			// コントロールセット（どのつまみが何に効くか）
			std::string sets;
			auto it = pt.ctrlline.find("sets");
			if (it != pt.ctrlline.end()) {
				int s = 0, d = 0, dp = 64;
				const char *p = it->second.c_str();
				int k = 0;
				while (*p) {
					if (std::sscanf(p, "%d:%d:%d", &s, &d, &dp) == 3 && d && dp != 64) {
						char t[96];
						std::snprintf(t, sizeof t, "%s%s → %s %+d", sets.empty() ? "" : "、", source_name(s), dest_name(d).c_str(), dp - 64);
						sets += t;
					}
					const char *c = std::strchr(p, ',');
					if (!c)
						break;
					p = c + 1;
					k++;
				}
			}
			ImGui::TextWrapped("コントロールセット: %s", sets.empty() ? "なし" : sets.c_str());
		}
		ImGui::Separator();
		ImGui::SliderInt("ベロシティ", &velocity, 1, 127);
		ImGui::SliderInt("オクターブ（PC キーボードの Z の段）", &octave, 0, 7);
		ImGui::SliderFloat("全体の音量", &pl->master, 0.0f, 1.0f);
		ImGui::Separator();
		auto knob = [&](const char *name, int &v, std::initializer_list<int> srcs) {
			if (ImGui::SliderInt(name, &v, 0, 127)) {
				std::lock_guard<std::mutex> lk(pl->mtx);
				for (int s : srcs)
					pl->source(s, v);
			}
		};
		ImGui::PushItemWidth(260);
		knob("MW（CC1）", mw, {1});
		ImGui::SameLine(0, 30);
		knob("AT（アフタータッチ）", at, {2, 3});
		knob("AS1（CC16、中心 64）", as1, {8});
		ImGui::SameLine(0, 30);
		knob("AS2（CC17、中心 64）", as2, {9});
		knob("RB（CC22、中心 64）", rb, {6});
		ImGui::SameLine(0, 30);
		knob("BC（CC2）", bc, {7});
		knob("FC（CC4）", fc, {10, 11, 12});
		ImGui::SameLine(0, 30);
		knob("ピッチベンド", pb, {0});
		if (!ImGui::IsItemActive() && pb != 64) {
			// 離したら中心へ戻す
			pb = 64;
			std::lock_guard<std::mutex> lk(pl->mtx);
			pl->source(0, 64);
		}
		ImGui::PopItemWidth();
		{
			bool sus = pl->sustain;
			if (ImGui::Checkbox("サステイン（CC64）", &sus)) {
				std::lock_guard<std::mutex> lk(pl->mtx);
				pl->set_sustain(sus);
			}
		}
		ImGui::SameLine(0, 30);
		if (ImGui::Button("全部止める")) {
			std::lock_guard<std::mutex> lk(pl->mtx);
			pl->all_off();
		}
		ImGui::SameLine(0, 30);
		if (ImGui::Button("つまみを戻す")) {
			mw = at = bc = fc = 0;
			as1 = as2 = rb = pb = 64;
			std::lock_guard<std::mutex> lk(pl->mtx);
			for (int s = 0; s < 16; s++)
				pl->source(s, motif_sampler::engine::bipolar(s) ? 64 : 0);
		}
		ImGui::Separator();
		// MIDI 入力
		{
			const std::string cur_name = pl->midi.is_open() ? pl->midi.device_name() : std::string("なし");
			ImGui::SetNextItemWidth(360);
			if (ImGui::BeginCombo("MIDI 入力", cur_name.c_str())) {
				if (ImGui::Selectable("なし", !pl->midi.is_open())) {
					std::lock_guard<std::mutex> lk(pl->mtx);
					pl->midi.close();
				}
				for (size_t i = 0; i < midi_names.size(); i++)
					if (ImGui::Selectable((midi_names[i] + "##m" + std::to_string(i)).c_str(), pl->midi.is_open() && pl->midi.device_name() == midi_names[i])) {
						std::lock_guard<std::mutex> lk(pl->mtx);
						pl->midi.close();
						pl->midi.open(int(i), midi_err);
					}
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			if (ImGui::Button("一覧を更新") && !nomidi)
				midi_names = ui::midi_in::list();
			if (!midi_err.empty())
				ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "MIDI: %s", midi_err.c_str());
		}
		if (audio_ok)
			ImGui::TextDisabled("音の出口: %s（%s）・鳴っているエレメント %d・CPU %.0f%%・MIDI %llu バイト", audio.device_name().c_str(),
			                    audio.exclusive() ? "排他" : "共有", pl->voices.load(), audio.cpu_percent(), (unsigned long long)pl->midi_bytes.load());
		else
			ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "音の出口を開けない: %s", audio_err.c_str());
		{
			const float pk = pl->peak.load();
			ImGui::ProgressBar(std::min(1.0f, pk), ImVec2(360, 0), pk >= 1.0f ? "頭打ち" : "");
			ImGui::SameLine();
			ImGui::TextDisabled("ピーク");
		}
		if (pl->cur) {
			ImGui::Checkbox(("インサーション（" + pl->cur->ins_name + "）").c_str(), &pl->use_ins);
			ImGui::Checkbox(("リバーブ（" + pl->cur->rev_name + "）").c_str(), &pl->use_rev);
			ImGui::SameLine(0, 30);
			ImGui::Checkbox(("コーラスの枠（" + pl->cur->cho_name + "）").c_str(), &pl->use_cho);
		}
		ImGui::TextDisabled("PC キーボード: Z〜M が下の段、Q〜I が上の段。マウスで鍵盤を押したまま滑らせると次の鍵");
		ImGui::EndChild();

		// 下: 鍵盤
		ImGui::Separator();
		draw_keyboard(*pl, 12 * (octave + 1) - 12, 5, mouse_key, velocity);
		ImGui::End();

		ImGui::Render();
		const float clear[4] = {0.08f, 0.08f, 0.09f, 1.0f};
		g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
		g_ctx->ClearRenderTargetView(g_rtv, clear);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		if (!shot.empty() && pl->cur && !loading) {
			// 読み終えてから少し描いて、鍵を押した絵を 1 枚
			if (shot_frames == 5) {
				std::lock_guard<std::mutex> lk(pl->mtx);
				for (int k : shot_keys)
					pl->note_on(k, velocity);
			}
			if (++shot_frames == 40) {
				save_backbuffer(shot);
				PostMessageW(hwnd, WM_CLOSE, 0, 0);
			}
		}
		g_swap->Present(1, 0);
	}

	// 片付け: 音を止めてから出口を閉じる
	{
		std::lock_guard<std::mutex> lk(pl->mtx);
		pl->all_off();
	}
	audio.stop();
	{
		std::lock_guard<std::mutex> lk(pl->mtx);
		pl->midi.close();
	}
	if (loader.joinable())
		loader.join();
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	cleanup_device();
	DestroyWindow(hwnd);
	UnregisterClassW(wc.lpszClassName, wc.hInstance);
	return 0;
}
