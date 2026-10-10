// license:BSD-3-Clause
//
// MU2000 一台ぶんの組み立て。配置は MAME の ymmu2000.cpp と同じ。

#include "compat/cli_text.h"
#include "mu2000.h"
#include "lcdfont.h"
#include "roms_dir.h"

#if defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

#include "xg/ram.h"
#include "xg/voices.h"
#include "xg/fx_params.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "compat/platform.h"
#include "compat/realtime.h"

namespace {

// 環境変数を読む（無ければ既定値）。調べもの用の窓で使う
const char *getenv_or2(const char *name, const char *def)
{
	const char *v = std::getenv(name);
	return v && *v ? v : def;
}

// MIDI は 31250bps。28MHz の CPU から見て 1 ビット = 896 サイクル
constexpr u64 MIDI_BIT_CYCLES = 28000000 / 31250;

// **USB で受ける速さは実機で 10,000 byte/s**（2026-09-23 に実機を録って測った。
// doc/native-engine.md の 6.218）。`doc/dump/usb.md` の 19,500 byte/s は
// **実機 → PC の向き**（334 バイトの SysEx を吸ったとき）の値で、こちらとは別の道。
// 1 バイトぶんのサイクル数
constexpr u64 USB_BYTE_CYCLES = 28000000 / 10000;

bool read_file(const std::string &path, std::vector<u8> &out, size_t expect)
{
	std::FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	std::fseek(f, 0, SEEK_END);
	const long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (expect && size_t(size) != expect) {
		std::fclose(f);
		return false;
	}
	out.resize(size_t(size));
	const size_t got = std::fread(out.data(), 1, out.size(), f);
	std::fclose(f);
	return got == out.size();
}

} // namespace


static std::atomic<int> g_live_instances{0};

mu2000::mu2000()
{
	g_live_instances++;
	// CPU。MAME は 7MHz の水晶を PLL で 4 倍していた
	m_cpu = &m_config.make<sh7043a_device>(m_cpu_finder, 7000000u * 4);

	// 内蔵周辺を作る。MAME の device_add_mconfig をそのまま呼ぶ
	m_cpu->device_add_mconfig(m_config);

	// PLG ボード用のシリアル。ボードは挿さないが、firmware はレジスタを触る
	m_sci4 = &m_config.make<sci4_device>(m_sci4_finder);

	// 時計とタイマの置き場を全デバイスに配る
	m_machine.set_clock_hz(7000000 * 4);
	for (auto &d : m_config.m_devices)
		d->set_machine(&m_machine);

	m_ram.assign(0x40000, 0);        // 256KB
	m_dram.assign(0x80000, 0);       // 512KB
	m_iram.assign(0x1000, 0);        // CPU 内蔵 4KB
	m_sampram.assign(0x400000, 0);   // SWP30 のサンプリング RAM
	m_swpm.set_sample_ram(m_sampram.data(), m_sampram.size());
	m_swps.set_sample_ram(m_sampram.data(), m_sampram.size());
	// パートの音を拾う口（見たいパートが無ければ、渡された所ですぐ帰る）
	for (auto &o : m_scope_owner)
		o.store(-1, std::memory_order_relaxed);
	m_swpm.m_voice_tap = &mu2000::scope_tap_fn;
	m_swpm.m_voice_tap_ctx = &m_scope_ctx[0];
	m_swps.m_voice_tap = &mu2000::scope_tap_fn;
	m_swps.m_voice_tap_ctx = &m_scope_ctx[1];
	m_swpm.m_meg_tap = &mu2000::scope_meg_fn;
	m_swpm.m_meg_tap_ctx = &m_scope_ctx[0];
	m_swps.m_meg_tap = &mu2000::scope_meg_fn;
	m_swps.m_meg_tap_ctx = &m_scope_ctx[1];

	build_bus();
}

// ---- パートの音（画面のスペクトラム用）

void mu2000::set_scope_part(int part)
{
	const int p = (part >= 0 && part < 64) ? part : -1;
	if (m_scope_part.exchange(p, std::memory_order_relaxed) != p && p >= 0)
		scope_refresh_owner();
}

void mu2000::set_part_scopes(bool on)
{
	if (m_pscope_on.exchange(on, std::memory_order_relaxed) != on && on)
		scope_refresh_owner();
}

void mu2000::scope_tap_fn(void *ctx, const s32 *samples)
{
	const scope_tap &t = *static_cast<const scope_tap *>(ctx);
	mu2000 &m = *t.self;
	const int part = m.m_scope_part.load(std::memory_order_relaxed);
	const int base = t.chip * 64;
	if (part >= 0) {
		float sum = 0.0f;
		for (int i = 0; i < 64; i++)
			if (samples[i] && m.m_scope_owner[size_t(base + i)].load(std::memory_order_relaxed) == part)
				sum += float(samples[i]);
		const u32 w = m.m_scope_w[size_t(t.chip)].load(std::memory_order_relaxed);
		m.m_scope_ring[size_t(t.chip)][w & (SCOPE_N - 1)] = sum;
		m.m_scope_w[size_t(t.chip)].store(w + 1, std::memory_order_release);
	}
	// 全パート（一覧）。1 サンプルに 64 パートぶんの行を 1 つ
	if (m.m_pscope_on.load(std::memory_order_relaxed)) {
		const u32 w = m.m_pscope_w[size_t(t.chip)].load(std::memory_order_relaxed);
		float *row = m.m_pscope.data() + (size_t(t.chip) * PSCOPE_N + (w & (PSCOPE_N - 1))) * 64;
		std::fill(row, row + 64, 0.0f);
		for (int i = 0; i < 64; i++) {
			if (!samples[i])
				continue;
			const int o = m.m_scope_owner[size_t(base + i)].load(std::memory_order_relaxed);
			if (o >= 0 && o < 64)
				row[o] += float(samples[i]);
		}
		m.m_pscope_w[size_t(t.chip)].store(w + 1, std::memory_order_release);
	}
}

void mu2000::part_scope_read(int part, float *out, size_t n) const
{
	n = std::min(n, PSCOPE_N);
	if (part == PSCOPE_OUT) {
		const u32 end = m_oscope_w.load(std::memory_order_acquire);
		for (size_t i = 0; i < n; i++) {
			const u32 k = end - u32(n) + u32(i);
			out[i] = (end >= n || k < end) ? m_oscope[k & (PSCOPE_N - 1)] : 0.0f;
		}
		return;
	}
	if (part < 0 || part >= 64) {
		std::fill(out, out + n, 0.0f);
		return;
	}
	const u32 end = std::min(m_pscope_w[0].load(std::memory_order_acquire), m_pscope_w[1].load(std::memory_order_acquire));
	for (size_t i = 0; i < n; i++) {
		const u32 k = end - u32(n) + u32(i);
		if (end < n && k >= end) {
			out[i] = 0.0f;
			continue;
		}
		const size_t at = size_t(k & (PSCOPE_N - 1));
		out[i] = m_pscope[at * 64 + size_t(part)] + m_pscope[(PSCOPE_N + at) * 64 + size_t(part)];
	}
}

namespace {
// エフェクト（mu2000::scope_fx の順）の入口・出口: チップと m20 からの組の番号（左右の 2 本で 1 組）。
// firmware が組む MEG の割り付け。エミュで送りと出口を比べて実測した（2026-09-22）:
//   マスタ  m20/21 乾いた音と戻りを混ぜたもの、m24/25 リバーブ、m26/27 コーラス、m28/29 インサーション 1、
//           m2c/2d バリエーション（システム接続でもインサーション接続でも）
//   スレーブ m28/29・m2a/2b・m2c/2d がインサーション 2-4
constexpr int SCOPE_FX_CHIP[8] = { 0, 1, 1, 1, 0, 0, 0, 0 };
constexpr int SCOPE_FX_PAIR[8] = { 4, 4, 5, 6, 6, 3, 2, 0 };
// MEG の目盛りは声の和と同じ（THRU のインサーションで入口・出口・声の和の rms が 0.00 dB でそろった）
}

void mu2000::scope_meg_fn(void *ctx, const s32 *in, const s32 *out)
{
	const scope_tap &t = *static_cast<const scope_tap *>(ctx);
	mu2000 &m = *t.self;
	if (m.m_scope_part.load(std::memory_order_relaxed) < 0)
		return;
	const u32 w = m.m_fx_w[size_t(t.chip)].load(std::memory_order_relaxed) & (SCOPE_N - 1);
	float *ring = m.m_fx_ring.data() + size_t(t.chip) * 16 * SCOPE_N;
	for (int p = 0; p < 8; p++) {
		ring[size_t(p * 2) * SCOPE_N + w]     = (float(in[p * 2]) + float(in[p * 2 + 1])) * 0.5f;
		ring[size_t(p * 2 + 1) * SCOPE_N + w] = (float(out[p * 2]) + float(out[p * 2 + 1])) * 0.5f;
	}
	m.m_fx_w[size_t(t.chip)].fetch_add(1, std::memory_order_release);
}

void mu2000::scope_read_fx(int fx, bool out, float *dst, size_t n) const
{
	n = std::min(n, SCOPE_N);
	if (fx < 0 || fx >= SCOPE_FX_N) {
		std::fill(dst, dst + n, 0.0f);
		return;
	}
	const int c = SCOPE_FX_CHIP[fx];
	const float *ring = m_fx_ring.data() + (size_t(c) * 16 + size_t(SCOPE_FX_PAIR[fx] * 2 + (out ? 1 : 0))) * SCOPE_N;
	const u32 end = m_fx_w[size_t(c)].load(std::memory_order_acquire);
	for (size_t i = 0; i < n; i++) {
		const u32 k = end - u32(n) + u32(i);
		dst[i] = (end >= n || k < end) ? ring[k & (SCOPE_N - 1)] : 0.0f;
	}
}

void mu2000::scope_refresh_owner()
{
	// 見ているパートに付いているインサーション（XG 03 0n 0C がパート番号。7F は無し）
	{
		const int part = m_scope_part.load(std::memory_order_relaxed);
		int ins = -1;
		for (int n = 0; n < 4 && part >= 0; n++) {
			u32 off = 0;
			if (xg::ram::locate(u32(0x03 << 14 | n << 7 | 0x0c), off) && off < m_ram.size() &&
			    (m_ram[off] & 0x7f) == part) {
				ins = n;
				break;
			}
		}
		m_scope_ins.store(ins, std::memory_order_relaxed);
	}
	// パートの塊の番地（下 16bit）→ パート
	static const std::array<u16, 64> PART_PTR = [] {
		std::array<u16, 64> a{};
		for (int p = 0; p < 64; p++)
			a[size_t(p)] = u16(0x400000 + xg::ram::part_base(p));
		return a;
	}();
	constexpr u32 VOICE_TABLE = 0x24386;     // 0x424386: 声ごとの記録（148 バイト）の +6 がパートの塊の番地
	constexpr u32 VOICE_STRIDE = 148;
	for (int v = 0; v < 128; v++) {
		int owner = -1;
		if (m_native_engine)
			owner = m_ndrv.slot_part(v);
		if (owner < 0) {
			const u32 off = VOICE_TABLE + u32(v) * VOICE_STRIDE;
			const u16 ptr = u16(m_ram[off] << 8 | m_ram[off + 1]);
			for (int p = 0; p < 64; p++)
				if (PART_PTR[size_t(p)] == ptr) {
					owner = p;
					break;
				}
		}
		m_scope_owner[size_t(v)].store(s8(owner), std::memory_order_relaxed);
	}
}

void mu2000::scope_read(float *out, size_t n) const
{
	n = std::min(n, SCOPE_N);
	const u32 w0 = m_scope_w[0].load(std::memory_order_acquire);
	const u32 w1 = m_scope_w[1].load(std::memory_order_acquire);
	const u32 end = std::min(w0, w1);
	for (size_t i = 0; i < n; i++) {
		const u32 k = end - u32(n) + u32(i);
		out[i] = (end >= n || k < end) ? m_scope_ring[0][k & (SCOPE_N - 1)] + m_scope_ring[1][k & (SCOPE_N - 1)] : 0.0f;
	}
}

int mu2000::scope_read_post(float *out, size_t n) const
{
	const int ins = m_scope_ins.load(std::memory_order_relaxed);
	if (ins < 0) {
		scope_read(out, n);
		return 0;
	}
	scope_read_fx(SCOPE_INS1 + ins, true, out, n);
	return ins + 1;
}

mu2000::~mu2000()
{
	set_threaded(false);
	g_live_instances--;
}

// スレーブを別スレッドで回すのは、動いている台数が少ないときだけ。
// 別スレッドは 1 台で 2 コアを回して使う（書き出しは 2 割ほど速い）が、1 つのプロセスで何台も
// 動かす（DAW に何枚も挿す）とコアの取り合いになる。16 論理コア（8 物理）で dense を並べて回すと、
// 4 台は別スレッドが速い（2.64 / 1 本 3.19 秒）が、8 台で逆転し（3.97 / 3.48）、16 台では 1 本が
// 2 倍速い（10.89 / 5.35）。だから動いている台数が論理コア数の 1/4 以下のときだけ別スレッドにする。
// 台数は途中で変わるので、run_sample がときどき見直す（1 本でも別スレッドでも出る音は同じ）。
// SMU2000_THREADED_MAX で台数の境を変えられる（0 なら全部 1 本）
static int threaded_max()
{
	static const int n = [] {
		if (const char *e = std::getenv("SMU2000_THREADED_MAX"))
			return std::max(0, std::atoi(e));
		return std::max(1, int(std::thread::hardware_concurrency() / 4));
	}();
	return n;
}

void mu2000::set_threaded(bool on)
{
	m_want_threaded = on;
	apply_threading();
}

// 頼まれていて、台数が境を超えていなければ別スレッドにする。そうでなければ 1 本に戻す
void mu2000::apply_threading()
{
	const bool on = m_want_threaded && g_live_instances.load(std::memory_order_relaxed) <= threaded_max();
	if (on == m_slave_thread.joinable())
		return;

	if (!on) {
		m_slave_quit = true;
		m_slave_go++;
		m_slave_go.notify_one();
		m_slave_thread.join();
		m_slave_quit = false;
		return;
	}
	// 合図の数は前に回した分だけ進んでいるので、今の数から待ち始める（0 からだと着いた途端に 1 サンプル余計に回す）
	const u64 seen = m_slave_go.load(std::memory_order_acquire);
	m_slave_done.store(seen, std::memory_order_release);
	m_slave_thread = std::thread([this, seen] { slave_loop(seen); });
}

// 空振りを何回続けたら眠るか。0 以下なら永久に回す（比較用）
#ifndef SLAVE_SPINS
#define SLAVE_SPINS 20000
#endif

void mu2000::slave_loop(u64 seen)
{
	// The platform's real-time audio workgroup, if it has one
	// (src/compat/realtime.h). Joins whatever the front end asked for and
	// leaves it on the way out; null keeps today's behavior.
	smu2000::realtime_join wg;
	for (;;) {
		if (wg.active())
			wg.reset(m_rt_wg_want.load(std::memory_order_acquire));
		// 合図を待つ。1 サンプルの中の待ちは 1 マイクロ秒に満たないので、
		// まず回して待つ。眠っていては 44100 回/秒には間に合わない。
		//
		// ただし DAW の中では、1 ブロック作り終えてから次に呼ばれるまでの
		// 数ミリ秒がまるごと空く。そこまで回し続けると 1 コアを常時
		// 焼くことになるので、しばらく空振りしたら本当に眠る
		int spins = 0;
		while (m_slave_go.load(std::memory_order_acquire) == seen) {
			if (m_slave_quit.load(std::memory_order_relaxed))
				return;
			if (SLAVE_SPINS <= 0 || ++spins < SLAVE_SPINS)
				smu2000::cpu_pause();
			else
				m_slave_go.wait(seen, std::memory_order_acquire);
		}
		seen = m_slave_go.load(std::memory_order_acquire);
		if (m_slave_quit.load(std::memory_order_relaxed))
			return;

		m_slave_l = m_slave_r = 0;
		m_swps.run_sample(m_slave_l, m_slave_r);
		m_slave_done.store(seen, std::memory_order_release);
	}
}


bool mu2000::load_program(const std::string &path)
{
	std::vector<u8> raw;
	if (!read_file(path, raw, 0x400000)) {
		m_error = CLI_T("Cannot read the program ROM (not 4MB, or not found): ", "プログラム ROM を読めない（4MB でないか、見つからない）: ") + path;
		return false;
	}
	return load_program_data(raw.data(), raw.size());
}

bool mu2000::load_program_data(const u8 *data, size_t size)
{
	if (!data || size != 0x400000) {
		m_error = CLI_T("The program ROM is not 4MB", "プログラム ROM の大きさが 4MB でない");
		return false;
	}
	set_program_rom(std::make_shared<std::vector<u8>>(data, data + size));
	return true;
}

// ROM は読むだけなので、何台の MU2000 で分け合っても構わない。
// VST3 を複数挿したときに 36MB を人数分持たずに済む
void mu2000::set_program_rom(u8rom p)
{
	m_prog = std::move(p);
	build_bus();
}

void mu2000::set_wave_rom(u8rom p)
{
	m_wave = std::move(p);
	if (!m_wave)
		return;
	m_swpm.set_wave_rom(m_wave->data(), m_wave->size());
	m_swps.set_wave_rom(m_wave->data(), m_wave->size());
}

void mu2000::set_sintab_rom(u16rom p)
{
	m_sintab = std::move(p);
	if (!m_sintab)
		return;
	m_swpm.set_sintab(m_sintab->data(), m_sintab->size());
	m_swps.set_sintab(m_sintab->data(), m_sintab->size());
}


const char *const mu2000::WAVE_ROM_NAMES[4] = {
	smu2000::kWaveRomNames[0], smu2000::kWaveRomNames[1],
	smu2000::kWaveRomNames[2], smu2000::kWaveRomNames[3]
};

bool mu2000::load_wave(const std::string &dir)
{
	std::vector<u8> parts[4];
	for (int i = 0; i < 4; i++) {
		const std::string path = dir + "/" + WAVE_ROM_NAMES[i];
		if (!read_file(path, parts[i], 0x800000)) {
			m_error = CLI_T("Cannot read the wave ROM (not 8MB, or not found): ", "波形 ROM を読めない（8MB でないか、見つからない）: ") + path;
			return false;
		}
	}
	const u8 *const data[4] = { parts[0].data(), parts[1].data(), parts[2].data(), parts[3].data() };
	const size_t size[4] = { parts[0].size(), parts[1].size(), parts[2].size(), parts[3].size() };
	return load_wave_data(data, size);
}

bool mu2000::load_wave_data(const u8 *const part[4], const size_t size[4])
{
	// MAME は 4 つの 8MB を 32bit 語に交互に置いている。
	//   ic49 -> 語の下位 16bit（0x0000000 から）
	//   ic50 -> 語の上位 16bit
	//   ic53 / ic54 -> 0x1000000 語目から同じ形で
	for (int i = 0; i < 4; i++)
		if (!part[i] || size[i] != 0x800000) {
			m_error = std::string(CLI_T("The wave ROM is not 8MB: ", "波形 ROM の大きさが 8MB でない: ")) + WAVE_ROM_NAMES[i];
			return false;
		}
	auto rom = std::make_shared<std::vector<u8>>(0x2000000, 0);   // 32MB
	for (int i = 0; i < 4; i++) {
		const size_t base = (i >= 2) ? 0x1000000 : 0;
		const size_t off  = (i & 1) ? 2 : 0;
		for (size_t j = 0; j < size[i]; j += 2) {
			const size_t dst = base + j * 2 + off;
			(*rom)[dst + 0] = part[i][j + 0];
			(*rom)[dst + 1] = part[i][j + 1];
		}
	}

	set_wave_rom(std::move(rom));
	return true;
}


bool mu2000::load_sintab(const std::string &path)
{
	std::vector<u8> raw;
	if (!read_file(path, raw, 0x10000)) {
		m_error = CLI_T("Cannot read the sine table (not 64KB, or not found): ", "sin 表を読めない（64KB でないか、見つからない）: ") + path;
		return false;
	}
	return load_sintab_data(raw.data(), raw.size());
}

bool mu2000::load_sintab_data(const u8 *data, size_t size)
{
	if (!data || size != 0x10000) {
		m_error = CLI_T("The sine table is not 64KB", "sin 表の大きさが 64KB でない");
		return false;
	}
	auto rom = std::make_shared<std::vector<u16>>(size / 2);
	for (size_t i = 0; i < rom->size(); i++)
		(*rom)[i] = u16(data[i * 2] | (data[i * 2 + 1] << 8));
	// 表は 1/4 周期を 0x8000（中心）から 0xffff（山）まで持つ形。MEG は後ろ半周期を ^0xffff で作るので、
	// 0 から始まる表だと山と谷の境目で値が 0 と 0xffff の間を跳び、深いコーラス（CELESTE・SYMPHONIC・CHORUS 3）に
	// 雑音が乗っていた。前の make_standins.py が作った 0 始まりの代替品は、ここで中心から始まる形に作り直す
	if (rom->size() == 0x8000 && (*rom)[0] < 0x4000) {
		for (size_t i = 0; i < rom->size(); i++)
			(*rom)[i] = u16(std::min(65535.0, std::round(0x8000 + std::sin((i + 0.5) / 0x8000 * 3.14159265358979323846 / 2) * 0x7fff)));
	}
	set_sintab_rom(std::move(rom));
	return true;
}


// ---- フロントパネル
//
// firmware は c80000 に「行」を書いてから同じ番地を読む。押されている桁が 0。
// 並びは MAME の mu500 の入力ポート（SWS0-SWS5）と同じ。

namespace {

struct button_slot { u8 row, bit; const char *name; };

// mu2000::button の並びと 1 対 1
const button_slot BUTTONS[] = {
	{ 0, 2, "Strings" },      { 0, 3, "Bass" },        { 0, 4, "Guitar" },
	{ 0, 5, "Organ" },        { 0, 6, "Chrom. Perc." }, { 0, 7, "Piano" },
	{ 1, 2, "Synth pad" },    { 1, 3, "Synth lead" },  { 1, 4, "Pipe" },
	{ 1, 5, "Reed" },         { 1, 6, "Brass" },       { 1, 7, "Ensemble" },
	{ 2, 2, "Drum" },         { 2, 3, "Model excl." }, { 2, 4, "SFX" },
	{ 2, 5, "Percussive" },   { 2, 6, "Ethnic" },      { 2, 7, "Synth effects" },
	{ 3, 1, "Part +" },       { 3, 2, "Part -" },      { 3, 3, "Mute/Solo" },
	{ 3, 4, "Effect" },       { 3, 5, "Util" },        { 3, 6, "Edit" },
	{ 3, 7, "Play" },
	{ 4, 1, "Value +" },      { 4, 2, "Value -" },     { 4, 3, "Exit" },
	{ 4, 4, "Select >" },     { 4, 5, "Select <" },    { 4, 6, "Enter" },
	{ 4, 7, "Seq" },
	{ 5, 5, "Audition" },     { 5, 6, "Select" },      { 5, 7, "Sampling/Mode" },
};

static_assert(sizeof(BUTTONS) / sizeof(BUTTONS[0]) == size_t(mu2000::button::count),
              "ボタンの表と enum がずれている");

} // namespace

const char *mu2000::button_name(button b)
{
	const int i = int(b);
	return (i >= 0 && i < int(button::count)) ? BUTTONS[i].name : "";
}

void mu2000::set_button(button b, bool pressed)
{
	const int i = int(b);
	if (i < 0 || i >= int(button::count))
		return;
	const button_slot &s = BUTTONS[i];
	const bool was = !BIT(m_sws[s.row], s.bit);
	if (pressed)
		m_sws[s.row] &= u8(~(1 << s.bit));
	else
		m_sws[s.row] |= u8(1 << s.bit);
	// **押し離しが変わったときだけ**（毎こま同じ値で呼ばれても効かないように）
	if (was != pressed)
		panel_touched();
}

bool mu2000::button_pressed(button b) const
{
	const int i = int(b);
	if (i < 0 || i >= int(button::count))
		return false;
	const button_slot &s = BUTTONS[i];
	return !BIT(m_sws[s.row], s.bit);
}

// 選ばれている行の押し具合を重ねて返す（MAME の mu500_state::ledsw_r と同じ）
u8 mu2000::ledsw_r() const
{
	u8 res = 0xff;
	for (u32 i = 0; i != 6; i++)
		if (BIT(m_ledsw1, i))
			res &= m_sws[i];
	return res;
}

// MAME の mulcd_device::set_leds に渡していた並びに直す
u16 mu2000::leds() const
{
	const u16 v = u16((u16(m_ledsw2) << 8) | m_ledsw1);
	// bitswap(v, 9,8,7,6,10,11,12,13,14,15) — 先頭が出来上がりの bit9
	static const int from[10] = { 9, 8, 7, 6, 10, 11, 12, 13, 14, 15 };
	u16 out = 0;
	for (int i = 0; i < 10; i++)
		out |= u16(BIT(v, from[i])) << (9 - i);
	// native の口では MU の灯（bit 6）の点滅をこちらで作る（led_blink）
	if (m_native_engine && m_ne_clock >= m_led_off_from && m_ne_clock < m_led_off_until)
		out &= u16(~(1u << 6));
	return out;
}

// **MU の灯を一瞬消す**。firmware は**ノートオン**（強さ 0 は除く）で MU の灯を
// 消し、受けてから約 38ms 後に消えて約 52ms で点き直す。続けて受けている間は消えたまま
// （最後に受けてから約 90ms で点く）。プログラムチェンジ・コントロールチェンジ・
// ノートオフでは消えない。firmware の道で 1ms 刻みに測った（DIN も USB も同じ）。
// リセットでも消えるが、その間は firmware を回し続けるので firmware 自身が消す
void mu2000::led_blink(u64 at)
{
	constexpr u64 DELAY = 44100 * 38 / 1000, OFF = 44100 * 52 / 1000;
	if (at >= m_led_off_until)
		m_led_off_from = at + DELAY;
	m_led_off_until = std::max(m_led_off_until, at + DELAY + OFF);
}


bool mu2000::load_lcd_font(const std::string &path)
{
	auto rom = std::make_shared<std::vector<u8>>();
	if (!read_file(path, *rom, 0x1000)) {
		m_error = CLI_T("Cannot read the LCD font (not 4KB, or not found): ", "LCD の字を読めない（4KB でないか、見つからない）: ") + path;
		return false;
	}
	set_lcd_font(std::move(rom));
	return true;
}

// 代用の字の絵に足りない分を起こす。
//
// MU2000 の firmware は、LCD 下段の左 9 マスにレベルメータを描く。
// 1 マスに 2 本のバーが入っていて、文字コードが
//
//     0x89 + 9 × 左の高さ + 右の高さ      （高さは 0-8）
//
// になっている。無音だと全マス 0x89（両方 0）、鳴らすと 0xcf（両方いっぱい）
// まで上がる。実機の CGROM にはその絵が入っているが、こちらが持っている
// 代用フォントは ASCII しか無くて空白になってしまうので、規則から起こす。
// 0x80-0x88 は幅いっぱいの 1 本バーとして使われている。
//
// **本物の CGROM（MAME の mulcd.zip の hd44780u_b04.bin）を置けば、
// そちらが優先される**。空いているところだけ埋める
void mu2000::fill_missing_glyphs(std::vector<u8> &rom)
{
	// 1 マスに 2 本。**バーの幅は 2 ドット**。左は 0-1 列、右は 3-4 列
	auto bar = [&](int code, int left, int right) {
		for (int y = 0; y < 8; y++) {
			u8 v = 0;
			if (y >= 8 - left)  v |= 0x18;
			if (y >= 8 - right) v |= 0x03;
			rom[code * 16 + y] = v;
		}
	};

	// レベルメータの字。この LCD の字の絵は手に入らないので、firmware が
	// 何を書くかを**測って**割り出した（doc/gui.md）。
	//
	//   コード = 0x7f + 9a + b   （a, b は 0-8）
	//   a = 左のバーの点の数、b = 右のバーの点の数
	//
	// 上の行と下の行で同じ表を使う。バーが上の行まで届かないときは
	// その側が 0、全部消えているマスには空白 (0x20) が入る。
	// 鳴っていないパートも 1 点だけ出る（a = b = 1、コード 0x89）
	//
	// **この範囲は ROM の中身より作り物を優先する**（doc/native-engine.md の
	// 6.148）。`mulcd.zip` の字形 ROM は MU2000 自身のものではないらしく、
	// この範囲で中身があるのは `87` `89` `C7` `CF` の 4 つだけ。うち
	// `87`（右が満タン）`C7`（左が満タン）`CF`（両方満タン）は作り物と
	// 1 ドット違わず同じだが、**`89` だけ違っていた**（ROM は `#.#.#`、
	// 実機は `##.##`。実機の画面を見てもらって分かった）。
	// `0x7f`（両方 0）は棒には使われず、普通の字として使われるので触らない
	for (int a = 0; a <= 8; a++)
		for (int b = 0; b <= 8; b++) {
			if (!a && !b)
				continue;              // 0x7f は普通の字
			bar(0x7f + a * 9 + b, a, b);
		}

	// **PAN の画面のパンの印**。演奏画面で SELECT を PAN に合わせると、メーターの
	// 欄が各パートのパンの位置に変わる。firmware の書く字を測ると
	//
	//     上の行: 上のレベルメータと同じ棒の字で、R 寄りの量（0-5）を下から伸ばす
	//             （どちらも 0 なら空白）
	//     下の行: コード = 0xd0 + 7(a + 1) + b
	//             a, b = L 寄りの量 0-5。-1 はその側を消す（点滅で選んだパートを
	//             消すときに使う。1 マスに 2 パート入るので、片側だけ消える）。
	//             測った字: C/C d7、L1/R1 de、L64/R63 fa、C/L2 d8、R2/L63 dc、
	//             消/R1 d0、L1/消 dd、L64/消 f9、C/消 d6。両方消すと 0xcf に
	//             なって上の棒の字と重なるので、それは作らない
	//
	// 下の行の字は**いちばん上の段が真ん中**で、そこから下へ a + 1 段点く。
	// 実機では C が真ん中の 1 段だけ、L に 1 でも寄ると下へ 2 段、R に 1 でも
	// 寄ると上の行の棒と合わせて上へ 2 段になる（利用者に実機で見てもらった）。
	// pan 0 は L64 で 5、pan 127 は R63 で 5、1 と 2 はどちらも 1。
	// 手元の字形 ROM ではこの範囲が全部空白で、C のときに何も出ていなかった
	for (int a = -1; a <= 5; a++)
		for (int b = -1; b <= 5; b++) {
			const int code = 0xd0 + (a + 1) * 7 + b;
			if (code < 0xd0 || code > 0xff)
				continue;
			for (int y = 0; y < 8; y++) {
				u8 v = 0;
				if (y <= a) v |= 0x18;
				if (y <= b) v |= 0x03;
				rom[code * 16 + y] = v;
			}
		}

	// **バンク No.・プログラム No. の前に出る右向きの三角**（利用者が実機を
	// 撮ってくれた写真で分かった）。演奏画面の下の行は
	//
	//     11 30 30 30  10 30 30 31      ＝  ▶000 ▷001
	//
	// で、`0x11` がバンクの前、`0x10` がプログラムの前。**黒塗りが「ダイヤル
	// で動く側」、白抜きが「固定されている側」**で、SELECT のボタンで入れ替わる。
	// この画面は `0x11` が白抜き（バンクは固定）、`0x10` が黒塗り
	// （プログラムが動く）＝ 利用者に GUI で見てもらって向きを確かめた。
	//
	// `mulcd.zip` の字形 ROM は MU2000 自身のものではないので、ここは
	// **`0x10` が黒塗りの右向き・`0x11` が黒塗りの左向き**になっていた。
	// 実機はどちらも右向きで、片方が白抜き
	// 字形は利用者が実機を見て書き起こしたもの（**左に 1 列空く**）:
	//
	//     黒塗り        白抜き
	//     .#...         .#...
	//     .##..         .##..
	//     .###.         .#.#.
	//     .####         .#..#
	//     .###.         .#.#.
	//     .##..         .##..
	//     .#...         .#...
	//
	// ここは**下敷き**で、`art/lcdfont.txt` があればそちらが上から被さる
	// （手描きの字はぜんぶあちらに集める。src/lcdfont.h）
	static const u8 TRI_FILLED[8] = { 0x08, 0x0c, 0x0e, 0x0f, 0x0e, 0x0c, 0x08, 0x00 };
	static const u8 TRI_HOLLOW[8] = { 0x08, 0x0c, 0x0a, 0x09, 0x0a, 0x0c, 0x08, 0x00 };
	for (int y = 0; y < 8; y++) {
		rom[0x10 * 16 + y] = TRI_FILLED[y];
		rom[0x11 * 16 + y] = TRI_HOLLOW[y];
	}

	// **手描きの字を上から被せる**（art/lcdfont.txt）。実機を見て描き起こした
	// ものだけを入れる場所で、ROM から起こした字は入れない（src/lcdfont.h）
	smu2000::lcdfont::overlay_default(rom);
}

void mu2000::set_lcd_font(u8rom p)
{
	if (p && p->size() >= 0x1000) {
		auto patched = std::make_shared<std::vector<u8>>(*p);
		fill_missing_glyphs(*patched);
		m_lcd_font = std::move(patched);
	} else {
		m_lcd_font = std::move(p);
	}
	if (m_lcd_font)
		m_lcd.set_cgrom(m_lcd_font->data(), m_lcd_font->size());
}


void mu2000::build_bus()
{
	m_bus = mem_bus();

	// 000000-3fffff: プログラム ROM
	// SMU2000_ROMTRACE=<pc16進> なら、その辺りの命令が読んだ ROM の番地を出す
	if (m_prog && !m_prog->empty() && std::getenv("SMU2000_ROMTRACE")) {
		const u32 want = u32(std::strtoul(std::getenv("SMU2000_ROMTRACE"), nullptr, 16));
		const u8 *base = m_prog->data();
		mem_bus::device d;
		d.start = 0x000000; d.end = 0x3fffff;
		// want は**読まれる側の番地**。表の引き方を見るための仕掛け
		auto note = [this, want](offs_t a, u32 v, int size) {
			if (a >= want && a < want + 0x100)
				std::fprintf(stderr, "romread pc=%06x 番地=%06x = %x (%d bit)\n",
				             m_cpu ? m_cpu->pc() : 0, u32(a), v, size * 8);
		};
		d.r8  = [base, note](offs_t a) { const u8 v = base[a]; note(a, v, 1); return v; };
		d.r16 = [base, note](offs_t a) {
			const u16 v = u16(base[a] << 8 | base[a + 1]); note(a, v, 2); return v;
		};
		d.r32 = [base, note](offs_t a) {
			const u32 v = u32(base[a]) << 24 | u32(base[a + 1]) << 16 |
			              u32(base[a + 2]) << 8 | base[a + 3];
			note(a, v, 4);
			return v;
		};
		m_bus.add_device(std::move(d));
	} else if (m_prog && !m_prog->empty()) {
		m_bus.add_region(0x000000, 0x3fffff, m_prog->data(), false);
	}
	// 400000-43ffff: ワーク RAM
	// SMU2000_RAMTRACE=<pc16進> が立っていれば、素通しの region ではなく
	// device として繋いで、**その番地の命令が読んだワーク RAM の番地**を出す。
	// 実機がどの表を引いているかを外から突き止めるための仕掛け（とても遅い）
	if (const char *tp = std::getenv("SMU2000_RAMTRACE")) {
		const u32 want = u32(std::strtoul(tp, nullptr, 16));
		mem_bus::device d;
		d.start = 0x400000; d.end = 0x43ffff;
		// SMU2000_RAMREADAT=<番地16進>[:<長さ16進>] で、**その範囲を読んだ命令の
		// 番地**を出す（RAMWRITE の読み版。どの関数がその表を引いているかを探す）
		u32 ra = 0xffffffffu, rlen = 1;
		if (const char *rp = std::getenv("SMU2000_RAMREADAT")) {
			char *end = nullptr;
			ra = u32(std::strtoul(rp, &end, 16));
			if (end && *end == ':')
				rlen = u32(std::strtoul(end + 1, nullptr, 16));
			if (!rlen)
				rlen = 1;
		}
		// SMU2000_TRACE_S0 / _S1 で、**この標本の間だけ**出す（窓を絞る）
		const u64 s0 = u64(std::strtoull(getenv_or2("SMU2000_TRACE_S0", "0"), nullptr, 10));
		const u64 s1 = u64(std::strtoull(getenv_or2("SMU2000_TRACE_S1", "18446744073709551615"),
		                                 nullptr, 10));
		auto note = [this, want, ra, rlen, s0, s1](offs_t a, u32 v, int size) {
			const u64 now = u64(trace_sample());
			if (now < s0 || now > s1)
				return;
			const u32 pc = m_cpu ? m_cpu->pc() : 0;
			if ((pc >= want && pc <= want + 0x100)
			    || (a < ra + rlen && ra < a + u32(size)))
				std::fprintf(stderr, "ramread s=%llu pc=%06x 番地=%06x = %x (%d bit)\n",
				             (unsigned long long)trace_sample(),
				             pc, u32(a), v, size * 8);
		};
		d.r8  = [this, note](offs_t a) {
			const u8 v = m_ram[a - 0x400000]; note(a, v, 1); return v;
		};
		d.r16 = [this, note](offs_t a) {
			const u16 v = u16(m_ram[a - 0x400000] << 8 | m_ram[a - 0x400000 + 1]);
			note(a, v, 2);
			return v;
		};
		d.r32 = [this, note](offs_t a) {
			const u8 *p = m_ram.data() + (a - 0x400000);
			const u32 v = u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3];
			note(a, v, 4);
			return v;
		};
		// SMU2000_RAMWRITE=<番地16進>[:<長さ16進>] で、その範囲に**書いた**命令の
		// 番地を出す。長さを付けると、どのバイトが動いたか分からないときに
		// 塊ごと見張れる（マスター移調を探すときに要った）
		const char *wp = std::getenv("SMU2000_RAMWRITE");
		u32 wa = 0xffffffffu, wlen = 1;
		if (wp) {
			char *end = nullptr;
			wa = u32(std::strtoul(wp, &end, 16));
			if (end && *end == ':')
				wlen = u32(std::strtoul(end + 1, nullptr, 16));
			if (!wlen)
				wlen = 1;
		}
		auto notew = [this, wa, wlen](offs_t a, u32 v, int size) {
			if (a < wa + wlen && wa < a + u32(size))
				std::fprintf(stderr, "ramwrite s=%llu pc=%06x pr=%06x 番地=%06x = %x (%d bit)\n",
				             (unsigned long long)trace_sample(),
				             m_cpu ? m_cpu->pc() : 0, m_cpu ? m_cpu->pr() : 0,
				             u32(a), v, size * 8);
		};
		d.w8  = [this, notew](offs_t a, u8 v)  { notew(a, v, 1); m_ram[a - 0x400000] = v; };
		d.w16 = [this, notew](offs_t a, u16 v) {
			notew(a, v, 2);
			m_ram[a - 0x400000] = u8(v >> 8); m_ram[a - 0x400000 + 1] = u8(v);
		};
		d.w32 = [this, notew](offs_t a, u32 v) {
			notew(a, v, 4);
			u8 *p = m_ram.data() + (a - 0x400000);
			p[0] = u8(v >> 24); p[1] = u8(v >> 16); p[2] = u8(v >> 8); p[3] = u8(v);
		};
		m_bus.add_device(std::move(d));
	} else {
		m_bus.add_region(0x400000, 0x43ffff, m_ram.data(), true);
	}
	// 1000000-107ffff: DRAM
	m_bus.add_region(0x1000000, 0x107ffff, m_dram.data(), true);
	// fffff000-ffffffff: CPU 内蔵 RAM
	m_bus.add_region(0xfffff000, 0xffffffff, m_iram.data(), true);

	// 800000-801fff: SWP30 マスタ / 802000-803fff: スレーブ。
	// レジスタは 16bit 単位なので、番地を 2 で割って渡す
	auto swp = [this](swp30_device &dev, u32 base) {
		// 実機のマスタの SWP30 へ書くと、CPU は 1 本あたり **440 サイクル**（15.7 マイクロ秒、
		// 1 サンプルの 0.69 ぶん）待たされる（BSC の WAIT）。書き込み百回ほどが一瞬で終わる形にすると、
		// 遅れて鳴る層の遅れが実機より約 64 サンプル短くなる。
		//
		// 440 という数は実機から直に測った。XG モードでパート 1 と 2 を同じ受信チャンネルにして
		// 1 つのノートオンで鳴らし、左右へ振ると、左右の立ち上がりの差がそのまま
		// 「firmware が 1 パートぶんのレジスタを書く時間」になる（キーオンの間の待つ書き込みは 68 本）。
		// 実機 61.4 サンプル（8 音、標準偏差 1.0）に対し、この値で 61.1（doc/upstream.md の 36）。
		//
		// スレーブは待たせない（2.4kHz の割り込みが毎回ミキサを 7 つ書くので、待たせると CPU の 4 割が
		// 止まる。待たせると遅れが実機より 10 サンプル余計に長くなり、SLICE の位相も遠ざかる）。
		// 制御の 2 つ（0x0e / 0x0f）は、中身を書くもの（MEG のプログラムの中身 = チャンネル 0x11・0x12、
		// リバーブ RAM へ直に書く中身 = 0x26）だけ待たせ、番地・合図・状態は待たせない。エフェクトの種類を
		// 替えたときの読み込みの時間が、これで実機と合う（SLICE は表を 2052 項目書くので実機で 62ms 長い）
		const bool waits = base == 0x800000;
		auto hold = [this, waits](offs_t reg) {
			const u32 slot = reg & 0x3f;
			const u32 chan = (reg >> 6) & 0x3f;
			const bool control = slot == 0x0e || slot == 0x0f;
			const bool data = chan == 0x11 || chan == 0x12 || chan == 0x26;
			if (waits && (!control || data)) {
				m_swp_wait += SWP_WRITE_CYCLES;
				m_cpu->abort_timeslice();
			}
		};
		mem_bus::device d;
		d.start = base;
		d.end   = base + 0x1fff;
		d.r16 = [this, &dev, base](offs_t a) {
			const u16 v = dev.read16((a - base) >> 1);
			if (m_swp_trace && m_swp_trace_reads)
				std::fprintf(m_swp_trace, "R %08x %04x %04x  pc=%08x  t=%.6f s=%llu\n", base, (a - base) >> 1, v, m_cpu->pc(), double(m_cpu->total_cycles()) / 28000000.0, (unsigned long long)trace_sample());
			return v;
		};
		// 幅の内訳を数える。MAME は 16bit ハンドラに mem_mask を渡せるが
		// こちらは渡せないので、byte 幅の書き込みがあると片側が壊れる
		d.w8 = [this, &dev, base, hold](offs_t a, u8 v) {
			m_swp_w8++;
			const offs_t reg = (a - base) >> 1;
			const u16 old = dev.read16(reg);
			dev.write16(reg, (a & 1) ? u16((old & 0xff00) | v)
			                         : u16((old & 0x00ff) | (u16(v) << 8)));
			hold(reg);
		};
		d.r8 = [this, &dev, base](offs_t a) {
			m_swp_r8++;
			return u8(dev.read16((a - base) >> 1) >> ((a & 1) ? 0 : 8));
		};
		d.w32 = [this, &dev, base, hold](offs_t a, u32 v) {
			m_swp_w32++;
			const offs_t reg = (a - base) >> 1;
			if (m_swp_trace) {
				std::fprintf(m_swp_trace, "%s%08x %04x %04x  pc=%08x  t=%.6f s=%llu\n",
				             m_swp_trace_reads ? "W " : "", base, reg, u16(v >> 16), m_cpu->pc(), double(m_cpu->total_cycles()) / 28000000.0, (unsigned long long)trace_sample());
				std::fprintf(m_swp_trace, "%s%08x %04x %04x  pc=%08x  t=%.6f s=%llu\n",
				             m_swp_trace_reads ? "W " : "", base, reg + 1, u16(v), m_cpu->pc(), double(m_cpu->total_cycles()) / 28000000.0, (unsigned long long)trace_sample());
			}
			if (m_swp_watch) {
				m_swp_watch(base == 0x800000, reg, u16(v >> 16));
				m_swp_watch(base == 0x800000, reg + 1, u16(v));
			}
			// **録りは写し取りと別の口**（1 つしか無いと、次の写し取りが
			// 始まったときに前の録りが切れる）
			if (m_traj_rec && base == 0x800000) {
				traj_watch(reg, u16(v >> 16));
				traj_watch(reg + 1, u16(v));
			}
			note_fw_swp(base == 0x800000, reg, u16(v >> 16));
			note_fw_swp(base == 0x800000, reg + 1, u16(v));
			dev.write16(reg, u16(v >> 16));
			dev.write16(reg + 1, u16(v));
			hold(reg);
		};
		d.w16 = [this, &dev, base, hold](offs_t a, u16 v) {
			m_swp_w16++;
			if (m_swp_trace)
				std::fprintf(m_swp_trace, "%s%08x %04x %04x  pc=%08x  t=%.6f s=%llu\n",
				             m_swp_trace_reads ? "W " : "", base, (a - base) >> 1, v, m_cpu->pc(), double(m_cpu->total_cycles()) / 28000000.0, (unsigned long long)trace_sample());
			if (m_swp_watch)
				m_swp_watch(base == 0x800000, (a - base) >> 1, v);
			if (m_traj_rec && base == 0x800000)
				traj_watch((a - base) >> 1, v);
			note_fw_swp(base == 0x800000, (a - base) >> 1, v);
			dev.write16((a - base) >> 1, v);
			hold((a - base) >> 1);
		};
		return d;
	};
	m_bus.add_device(swp(m_swpm, 0x800000));
	m_bus.add_device(swp(m_swps, 0x802000));

	// c80000: LED ラッチとスイッチ走査、e00000: LED ラッチその 2。
	// 音には関わらないが、firmware が起動時に触るので受けておく
	{
		mem_bus::device d;
		d.start = 0xc80000; d.end = 0xc80000;
		d.r8 = [this](offs_t) { return ledsw_r(); };
		d.w8 = [this](offs_t, u8 v) { m_ledsw1 = v; };
		m_bus.add_device(d);
	}
	{
		mem_bus::device d;
		d.start = 0xe00000; d.end = 0xe00000;
		d.w8 = [this](offs_t, u8 v) { m_ledsw2 = v; };
		m_bus.add_device(d);
	}
	{
		mem_bus::device d;
		d.start = 0xd80000; d.end = 0xd80000;
		d.r8 = [this](offs_t) { return m_d80; };
		// 下 3bit が LCD のコントラスト（UTIL > SYS の Contrast − 1）。
		// 上の bit は起動中に a1 / e1 などと動く別のもの（入力の levels か）
		d.w8 = [this](offs_t, u8 v) { m_d80 = v; };
		m_bus.add_device(d);
	}

	// c00000: SmartMedia のデータ、d00000: 制御の留め金（smartmedia.h）
	{
		mem_bus::device d;
		d.start = 0xc00000; d.end = 0xc7ffff;
		d.r8 = [this](offs_t) { return m_card.data_r(); };
		d.w8 = [this](offs_t, u8 v) { m_card.data_w(v); };
		m_bus.add_device(d);
	}
	{
		mem_bus::device d;
		d.start = 0xd00000; d.end = 0xd7ffff;
		d.r8 = [](offs_t) -> u8 { return 0xff; };
		d.w8 = [this](offs_t, u8 v) { m_card.control_w(v); };
		m_bus.add_device(d);
	}

	// f00000-f0003f: PLG ボード用の SCI4。ボードは挿さないが register は生きている
	{
		mem_bus::device d;
		d.start = 0xf00000; d.end = 0xf0003f;
		d.r8 = [this](offs_t a) { return m_sci4->read8(a - 0xf00000); };
		d.w8 = [this](offs_t a, u8 v) { m_sci4->write8(a - 0xf00000, v); };
		m_bus.add_device(d);
	}

	// f80000-f80001: USB の M37640 マイコン。SH-2 から見えるのはこの 2 番地だけ。
	// 読みは 0 が受信バイト、1 が状態。書きは 0 が MIDI、1 が M37640 への指示
	{
		mem_bus::device d;
		d.start = 0xf80000; d.end = 0xf80001;
		d.r8 = [this](offs_t a) { return usb_r(a - 0xf80000); };
		d.w8 = [this](offs_t a, u8 v) { usb_w(a - 0xf80000, v); };
		m_bus.add_device(d);
	}

	// ffff8000-ffff9fff: CPU の内蔵周辺（sh7042_map.hxx が振り分ける）
	{
		mem_bus::device d;
		d.start = 0xffff8000; d.end = 0xffff9fff;
		d.r8  = [this](offs_t a) { return m_cpu->internal_r8(a); };
		d.r16 = [this](offs_t a) { return m_cpu->internal_r16(a); };
		d.r32 = [this](offs_t a) { return m_cpu->internal_r32(a); };
		d.w8  = [this](offs_t a, u8 v)  { m_cpu->internal_w8(a, v); };
		d.w16 = [this](offs_t a, u16 v) { m_cpu->internal_w16(a, v); };
		d.w32 = [this](offs_t a, u32 v) { m_cpu->internal_w32(a, v); };
		m_bus.add_device(d);
	}

	m_cpu->set_program_bus(&m_bus);
}


// ポート E は LCD の 8bit バス。上位バイトがデータ、下位が制御線。
// MAME の mu500_state::pe_r / pe_w と同じ形にしてある
//   bit 4: E（立ち下がりで確定）  bit 2: RS（1 でデータ）  bit 0: R/W
u16 mu2000::lcd_port_r()
{
	m_lcd.set_now(m_cpu->total_cycles());
	if (BIT(m_pe, 4)) {
		if (BIT(m_pe, 0))
			return u16((BIT(m_pe, 2) ? m_lcd.data_r() : m_lcd.control_r()) << 8);
		return 0x0000;
	}
	return 0;
}

void mu2000::lcd_port_w(u16 data)
{
	m_lcd.set_now(m_cpu->total_cycles());
	if (BIT(m_pe, 4) && !BIT(data, 4)) {        // E の立ち下がり
		if (!BIT(data, 0)) {                    // R/W = 0、つまり書き込み
			if (BIT(data, 2))
				m_lcd.data_w(u8(data >> 8));
			else
				m_lcd.control_w(u8(data >> 8));
			// **押したあと画面が動いている間は延ばす**（6.119）。ボタンを
			// 押したあとの仕事が 0.5 秒で終わらないことがある（品書きの
			// 読み込みなど）。書き換えが止まれば、すぐ細い回しに戻る。
			// **触っていないときは延ばさない**。ここを「液晶が動いたら
			// いつでも」にすると、曲を鳴らしている最中の表示更新でも
			// firmware が全速になり、写し取りの中身まで変わってしまう
			// （port_b 100% -> 97%、porta 81% -> 53%）
			if (m_native_engine && m_panel_hold && m_panel_hold < LCD_RUN)
				m_panel_hold = LCD_RUN;
		}
	}
	m_pe = data;
}

void mu2000::update_sci_irq()
{
	m_cpu->execute_set_input(0, (m_sci_irq[0] || m_sci_irq[1]) ? ASSERT_LINE : CLEAR_LINE);
}

void mu2000::start_devices()
{
	// MAME はスケジューラが順に呼ぶ。こちらは生成順にそのまま呼ぶ
	for (auto &d : m_config.m_devices)
		d->device_start();
}


void mu2000::reset()
{
	std::memset(m_cc_last, 0xff, sizeof(m_cc_last));
	// ボードとのやり取りは起動のたびにやり直す
	for (auto &q : m_plg_rx)
		q.clear();
	for (vb_slot &vs : m_vbs) {
		vs.msg.clear();
		vs.known = false;
	}
	// 実機の M37640 は、PC に繋がっていると「ホストが居る」を知らせてくる
	// （状態の bit6 を立てて F4 03 01 01 01。0x43810 が受け、0x43DAD1 を 1 にする）。
	// これが来ないと、HOST SELECT が USB のとき firmware は起動の途中（0x1167CE）で
	// 液晶に「HOST Is Offline!」を出す。エミュでは PC が常に繋がっているので、起動時に 1 回送る
	m_usb.cmd.clear();
	m_usb.cur_cmd = false;
	// ケーブルメッセージで回した口は、電源を入れ直せば元に戻る
	for (int p = 0; p < MIDI_PORTS; p++) {
		m_cable[p] = p;
		m_cable_wait[p] = false;
	}
	if (m_usb_host)
		for (u8 b : { 0xf4, 0x03, 0x01, 0x01, 0x01 })
			m_usb.cmd.push_back(b);

	// ポート A。MAME の mu500_state::pa_r は 0xffff を返すだけだったが、
	// そこに付いていた覚え書きに配線が書いてある。
	//   21 出力（前面と背面の MIDI A を切り替える）
	//   20 smvprt / 19 smvins / 18 smbusy（スマートカード）
	//   17 rea / 16 reb        ← **前面の大きなダイヤル**
	//
	// firmware は 2.5ms ごと（400Hz）にここを読む。読んだときに
	// bit17 が立っていれば 1 目盛りぶん動いたとみなし、bit16 で向きを決める。
	// 位相を細かく作るのではなく、走査 1 回につき 1 目盛りを渡せばよい。
	// **0xffff には bit16/17 が入っていない**（MAME が返していた値は
	// 「ダイヤルが止まっている」に当たる）ので、立てる側で書く。
	//
	// この決まりは実測で出した。bit17 を上げっぱなしにすると音色番号が
	// 最後（128 Gunshot）まで走り、bit16 も一緒に上げると逆に動く
	m_cpu->read_porta().set([this]() {
		u32 v = 0xffff;
		// SmartMedia の線（firmware は 0xFFFF8380 の下の 8bit で見る）:
		//   PA18 (0x04) 忙しい（0 で準備ができている。firmware は 0 になるのを待つ）/ PA19 (0x08) 差し込まれている /
		//   PA20 (0x10) 書き込みを禁じていない
		// 読み書きはその場で済むので、忙しい印は立てない
		if (m_card.inserted() && m_sample_count >= m_card_back_at) {
			v |= 1u << 19;
			if (!m_card.write_protected)
				v |= 1u << 20;
		}
		if (m_enc_pending) {
			if (m_enc_pending < 0) v |= 1u << 16;   // B 相は向きのあいだ立てておく
			if (m_enc_high) {
				v |= 1u << 17;                      // A 相の立ち上がりで 1 目盛り
				m_enc_high = false;
			} else {
				m_enc_high = true;
				m_enc_pending += (m_enc_pending > 0) ? -1 : 1;
			}
		}
		return v;
	});

	// A/D 変換。MAME の配線と同じ。
	// **電池の残量を返さないと起動画面が「Battery Low!」のままになる**
	// AN0 と AN2 は A/D INPUT の大きさ（AD1 と AD2）。サンプリングの REC の画面のレベルメーターとトリガに使う。
	// firmware は起動から AN0-AN3 を回し続け（ADCSR0 = 0xb3）、ADDR の上 8bit を 0xff から引いて使う（2.01 の 0x116196、0x13b6e6）。
	// つまり静かなほど値が大きい。引いた値が 0x18 以下でメーター 0、0x85 以上で振り切れる（0x13b78c）。
	// 実機の検波の回路は分からないので、ピーク（すぐ上がり、0.1 秒で 1/e に下がる）を 0x18 から 0x85 に割り当てる
	m_cpu->read_adc<0>().set([this]() { return ad_level_adc(0); });
	m_cpu->read_adc<1>().set_constant(0);
	m_cpu->read_adc<2>().set([this]() { return ad_level_adc(1); });
	m_cpu->read_adc<3>().set_constant(0);
	// ホストスイッチ。firmware は 8 ビットに落として境で分ける（0x1098）。
	// 0x20 未満が MIDI、0xBA-0xE0 が USB
	m_cpu->read_adc<4>().set([this]() -> u16 { return m_usb_host ? 0x330 : 0; });
	m_cpu->read_adc<5>().set_constant(0);
	m_cpu->read_adc<6>().set_constant(0x3ff);    // 電池は満タン
	m_cpu->read_adc<7>().set_constant(0);
	m_cpu->read_porte().set([this]() { return lcd_port_r(); });
	m_cpu->write_porte().set([this](u16 v) { lcd_port_w(v); });

	m_lcd.reset();

	// SCI4 の割り込み。MAME は 0 と 1 を input_merger で束ねて CPU の IRQ0 に、
	// 3 を IRQ1 に入れていた
	m_sci4->write_irq<0>().set([this](int s) { m_sci_irq[0] = s; update_sci_irq(); });
	m_sci4->write_irq<1>().set([this](int s) { m_sci_irq[1] = s; update_sci_irq(); });
	m_sci4->write_irq<3>().set([this](int s) { m_cpu->execute_set_input(1, s); });
	m_sci4->set_tx_tap([this](int chan, u8 targets, u8 byte) { plg_tx_byte(chan, targets, byte); });

	// 2 個のチップで乱数の数列を分ける。同じ種だと雑音まで揃ってしまう
	m_swpm.set_rand_seed(0x9d14abd7);
	m_swps.set_rand_seed(0x6c1f35e9);
	m_swpm.reset();
	m_swps.reset();

	// MIDI IN の線は何も来ていないとき High
	m_cpu->sci_rx_w<0>(1);
	m_cpu->sci_rx_w<1>(1);

	// MIDI OUT。SCI ch0 の送信線（MAME も ch0 を mdout へ繋いでいる）
	m_tx_r = m_tx_w = 0;
	m_tx_bit = -1;
	m_cpu->write_sci_tx<0>().set([this](int s) { tx_line(s); });

	start_devices();

	for (auto &d : m_config.m_devices)
		d->device_reset();
}


void mu2000::run_cycles(u64 n)
{
	// 前回はみ出した分を先に返す
	if (m_overrun >= n) { m_overrun -= n; return; }
	n -= m_overrun;
	m_overrun = 0;

	// MAME ではスケジューラがやっていたこと。周辺の予定を跨がないように区切る。
	// MAME は予定の時刻ちょうどで CPU を止めてタイマを鳴らし、そのあと再開する。
	// 周辺がレジスタ書き込みに反応して新しい予定を入れた場合は、CPU が
	// abort_timeslice() でその場で戻ってくるので、ここで組み直す
	int idle = 0;
	while (n) {
		m_loops++;
		const u64 now = m_cpu->total_cycles();
		m_machine.set_cycles(now);

		// MAME のスケジューラが持っていたタイマ（SCI4 の送受信など）
		const u64 tmr = m_machine.next_timer_cycles();
		if (tmr <= now) {
			m_timer_fires++;
			m_machine.run_timers(now);
			m_machine.set_cycles(now);
			continue;
		}

		const u64 ev  = m_cpu->event_cycles();

		if (ev && now >= ev) {
			m_event_fires++;
			m_cpu->event_tick();
			if (m_cpu->event_cycles() == ev && ++idle > 2)
				break;          // 予定が動かない。放っておくと止まる
			continue;
		}
		idle = 0;

		// MIDI のビット送出も跨がないように
		midi_step(now);
		usb_step(now);

		u64 chunk = n;
		if (ev && ev - now < chunk)
			chunk = ev - now;
		if (tmr != ~u64(0) && tmr - now < chunk)
			chunk = tmr - now;
		if (!m_fast_midi)
			for (const midi_line &m : m_midi)
				if (m.bit >= 0 || !m.queue.empty()) {
					const u64 left = m.next > now ? m.next - now : 1;
					if (left < chunk)
						chunk = left;
				}
		// SWP30 に書いた後は、その待ちぶんだけ命令を進めずに時間を送る（上の swp の説明）。
		// 周辺のタイマや MIDI の送出は、区切りごとにここまでで進めている
		if (m_swp_wait) {
			const u64 skip = std::min<u64>(m_swp_wait, chunk);
			m_cpu->skip_cycles(skip);
			m_swp_wait -= skip;
			n = skip >= n ? 0 : n - skip;
			continue;
		}

		const int done = m_cpu->run_cycles(int(chunk));
		if (done <= 0) {
			if (m_cpu->event_cycles() == ev)
				break;
			continue;
		}
		// 命令の途中では止まれないので、頼まれた数より少し多く走ることがある。
		// 出た分は捨てずに次の呼び出しから引く（捨てると CPU が音より速くなる）
		if (u64(done) >= n) {
			m_overrun += u64(done) - n;
			n = 0;
		} else
			n -= u64(done);
	}
}

// ダイヤルを 1 位相ぶん進める。
//
// 実機のエンコーダは A 相と B 相が 1/4 周期ずれて開閉する。firmware は
// その順番で向きを読むので、位相をまとめて飛ばしてはいけない。
void mu2000::tx_line(int state)
{
	if (m_tx_bit < 0) {
		if (!state) {            // スタートビット
			m_tx_bit = 0;
			m_tx_cur = 0;
		}
		return;
	}
	if (m_tx_bit < 8) {
		m_tx_cur |= u8((state ? 1 : 0) << m_tx_bit);
		m_tx_bit++;
		return;
	}
	// ストップビット。0 なら枠がずれているので、その 1 バイトは捨てる
	m_tx_bit = -1;
	if (!state)
		return;
	const size_t next = (m_tx_w + 1) & TX_MASK;
	if (next == m_tx_r)
		return;                  // 溢れ。誰も読んでいない
	m_tx_buf[m_tx_w] = m_tx_cur;
	m_tx_w = next;
}

// ---- USB（M37640）の代役
//
// 溜めに積むときに口が変わっていれば `F5 <口>` を先に挟む。firmware 側は
// 0x042932 で 0xF5 を見て次のバイトを「今の口」として覚え、以後のバイトを
// その口として 0x04437C へ渡す。口は 1 始まり（1=A 2=B 3=C 4=D）

void mu2000::usb_midi_in(u8 byte, int port)
{
	usb_line &u = m_usb;
	if (u.rx.size() >= MIDI_QUEUE_LIMIT) {
		m_midi_dropped.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	if (port != u.in_port) {
		u.rx.push_back(0xf5);
		u.rx.push_back(u8(port + 1));
		u.in_port = port;
	}
	u.rx.push_back(byte);
}

void mu2000::usb_step(u64 now)
{
	usb_line &u = m_usb;

	// USB を使っていないときは何もしない。割り込みを上げると firmware の
	// USB ドライバが動き出してしまう
	if (!m_usb_host && u.rx.empty() && !u.have)
		return;

	// 受信。1 バイト渡すごとに IRQ3（ベクタ 67）を上げる。
	// 間隔は実機で測った USB の受けの速さ 10,000 byte/s に合わせる
	// （2026-09-23・doc/native-engine.md の 6.218）。DIN の 3,125 byte/s より
	// 3 倍速い。荷物の大きさを振って実機と並べると、ずれは平均 3ms に収まる。
	// 4 つの口が 1 本の流れを分け合うので、遅くすると互いに待たせてしまう
	if (!u.have && now >= u.next && (!u.cmd.empty() || !u.rx.empty())) {
		// コマンドを先に渡す
		std::deque<u8> &q = u.cmd.empty() ? u.rx : u.cmd;
		u.cur_cmd = !u.cmd.empty();
		u.cur  = q.front();
		u.have = true;
		q.pop_front();
		u.next = now + (m_fast_midi ? 0 : USB_BYTE_CYCLES);
	}
	// 送信の線を一度下ろす。下で上げ直すので、山は 1 標本ぶんになる
	m_cpu->execute_set_input(2, 0);
	// **読まれるまで上げておく**。実機の M37640 は「受信あり」を線で示しているので、
	// firmware が受け取りを止めている間に来たバイトも、止めるのをやめた時点で必ず拾われる。
	// 渡した瞬間に 1 回だけ上げる形にしていたため、firmware が受信を詰まらせて
	// IRQ3 の優先度を 0 に落としている隙に渡すと、優先度を戻しても二度と上がらず、
	// 以後 MIDI を 1 バイトも受け取らなくなっていた（USB の口へ 1 秒に 2 万バイト近い
	// 設定データを流すと起きる。X で報告された testxg.mid）
	if (u.have)
		m_cpu->execute_set_input(3, 1);

	// 送信。firmware は IRQ2（ベクタ 66）が来るたびに 1 バイト出す。
	// 上げないとリングが埋まり、0x437A0 の空き待ちで固まる（実機でやらかした）
	if (now >= u.tx_next) {
		u.tx_next = now + USB_BYTE_CYCLES;
		m_cpu->execute_set_input(2, 1);
	}
}

u8 mu2000::usb_r(offs_t a)
{
	usb_line &u = m_usb;
	if (a & 1)
		return u.have ? (u.cur_cmd ? 0x41 : 0x01) : 0x00;   // bit0 = 受信あり、bit6 = コマンド
	// 受け取られたのでその場で線を下ろす。次の標本まで待つと、その隙に
	// 割り込みがもう一度入って同じバイトを二度読まれてしまう
	u.have = false;
	m_cpu->execute_set_input(3, 0);
	return u.cur;
}

void mu2000::usb_w(offs_t a, u8 v)
{
	if (a & 1)
		return;                        // コマンド口。M37640 への指示なので捨てる
	usb_line &u = m_usb;
	if (u.tx.size() < TX_SIZE)
		u.tx.push_back(v);
}

bool mu2000::usb_out_take(u8 &v, int &port)
{
	usb_line &u = m_usb;
	while (!u.tx.empty()) {
		const u8 b = u.tx.front();
		u.tx.pop_front();
		if (b == 0xf5) {
			if (u.tx.empty()) {        // 口の番号がまだ来ていない。戻しておく
				u.tx.push_front(b);
				return false;
			}
			u.out_port = int(u.tx.front()) - 1;
			u.tx.pop_front();
			continue;
		}
		v = b;
		port = u.out_port;
		return true;
	}
	return false;
}

void mu2000::midi_step(u64 now)
{
	// A と B は別々の SCI に繋がっている。互いに待たせない
	for (int port = 0; port < MIDI_DIN_PORTS; port++) {
		midi_line &m = m_midi[port];
		sh_sci_device *sci = m_cpu->sci(port);
		if (m_fast_midi) {
			if (!m.queue.empty() && sci->rx_can_accept()) {
				const u8 byte = m.queue.front();
				m.queue.pop_front();
				logerror("midi in %c %02x @ %llu (fast)\n", 'A' + port, byte,
				         (unsigned long long)now);
				sci->receive_byte(byte);
			}
			continue;
		}

		if (m.bit < 0) {
			// 直前のバイトのストップビットぶんは空けてから次を出す
			if (m.queue.empty() || now < m.next)
				continue;
			m.cur = m.queue.front();
			m.queue.pop_front();
			m.bit  = 0;
			m.next = now + MIDI_BIT_CYCLES;
			logerror("midi in %c %02x @ %llu\n", 'A' + port, m.cur,
			         (unsigned long long)now);
			sci->do_rx_w(0);            // スタートビット
			continue;
		}

		if (now < m.next)
			continue;

		m.bit++;
		m.next = now + MIDI_BIT_CYCLES;
		if (m.bit <= 8)
			sci->do_rx_w((m.cur >> (m.bit - 1)) & 1);   // 下位ビットから
		else {
			sci->do_rx_w(1);            // ストップビット
			m.bit = -1;
		}
	}
}



// ---- native の口（doc/native-engine.md の段 2）

// **firmware が、こちらが鳴らしているスロットに書いたか**を数える。
// ここは CPU のバス経由の書き込みだけを通る（native の poke は直に
// write16 を呼ぶので通らない）ので、firmware の書き込みだけが見える。
//
// native の口では firmware を 2% ほどしか回さない。firmware が自分の
// 仕事の途中で止められ、ずっと後に再開して**古い前提のまま**スロットに
// 書くと、そのスロットを native が別の音で使っていれば音色が壊れる。
// 利用者から「LCD が途中で止まり、そのとき音色が壊れて見える」という
// 報告があり、LCD を描いているのも firmware なので筋が合う
void mu2000::note_fw_swp(bool master, u32 reg, u16 value)
{
	// **包絡線の格子の位相を拾う**。native の口が始まる前は firmware が
	// 普通に走っているので、そのときの 0x00 の書き込みが格子の目にあたる
	if (master && !m_native_engine && reg < 0x1000 && (reg % 64) == 0)
		m_ndrv.set_eg_phase(u32(trace_sample()));
	if (!m_native_engine)
		return;
	// リセットが終わったかを測るのに使う（issue #51。hold_after_reset）
	if (master)
		m_fw_swp_at = m_ne_clock;
	// スレーブの声はスロット 64-127（native_driver の SLOTS）
	const int chip = master ? 0 : 1;
	const u32 base = master ? 0 : 64;
	u64 &keymask = m_fw_keymask[chip];
	// **firmware が鍵を押した瞬間のマスク**を拾う。これが firmware の
	// 「このスロットを使う」という宣言なので、以後そこは避ける。
	// あらゆる書き込みで印を付けると、ほとんどのスロットが firmware の
	// ものになってしまい、かえってぶつかりが増えた
	switch (reg) {
	case 0x18e: keymask = (keymask & ~(u64(0xffff) << 48)) | (u64(value) << 48); return;
	case 0x18f: keymask = (keymask & ~(u64(0xffff) << 32)) | (u64(value) << 32); return;
	case 0x1ce: keymask = (keymask & ~(u64(0xffff) << 16)) | (u64(value) << 16); return;
	case 0x1cf: keymask = (keymask & ~u64(0xffff)) | value; return;
	case 0x20e: m_ndrv.mark_fw_slots(keymask, int(base)); return;
	default: break;
	}
	if (reg >= 0x1000)
		return;
	const u32 rr = reg % 64;
	// MEG の戻りのミキサは毎サンプル書き替わるので数えない
	if (rr == 0x0e || rr == 0x0f || (rr >= 0x38 && rr <= 0x3f))
		return;
	const u32 slot = base + reg / 64;
	if (m_ndrv.slot_mask().test(int(slot))) {
		m_ne_fw_stomp++;
		static const bool dbg = std::getenv("SMU2000_STOMP_DEBUG") != nullptr;
		if (dbg)
			std::fprintf(stderr, "stomp slot=%u reg=%02x value=%04x\n", slot, rr, value);
		// そこはもう firmware の音が走っている。二重に書かず、譲って避ける
		m_ndrv.yield_slot(slot);
		return;
	}
	// **書いたスロットは firmware のものとして避け続ける**（6.220）。
	// 鍵を押した瞬間の印（上の 0x20e）だけだと、firmware の音が 2 秒より
	// 長く伸びるときに印が切れてしまい、こちらが取ったあとも firmware が
	// 自分の音の続きを書いてきて、鳴っている音が途中で化ける。
	// **いま鳴らしているスロットには印を付けない**（上で返している）。
	// そこはもう取り合いになっていて、避けても今の音は直らないうえ、
	// 使える枠だけが減って下のほう（firmware が使う側）へ押し出される
	m_ndrv.mark_fw_slot(slot);
}

// firmware が表示を変えた書き込みから、点滅しているマスを覚える
void mu2000::blink_learn()
{
	const hd44780_device::change *ch = nullptr;
	const int n = m_lcd.changes(ch);
	for (int i = 0; i < n; i++) {
		const int at = ch[i].cg ? 0x80 + (ch[i].addr & 0x3f) : (ch[i].addr & 0x7f);
		blink_cell &c = m_blink[at];
		const u8 was = ch[i].before, now = ch[i].after;
		const u64 t = m_fw_clock;
		const bool pair = c.count && ((was == c.v[0] && now == c.v[1]) ||
		                              (was == c.v[1] && now == c.v[0]));
		if (!pair) {
			// 別の値が来た。ここから数え直す
			c = blink_cell();
			c.v[0] = was;
			c.v[1] = now;
			c.count = 1;
			c.last_fw = t;
			continue;
		}
		// was の値が続いた長さ。前に覚えた長さと 25% 以上違えば数え直す
		const int wi = (was == c.v[0]) ? 0 : 1;
		const u64 gap = t - c.last_fw;
		c.last_fw = t;
		if (c.dur[wi] && (gap * 4 < c.dur[wi] * 3 || gap * 4 > c.dur[wi] * 5)) {
			c.dur[wi] = gap;
			c.count = 1;
			c.on = false;
			continue;
		}
		c.dur[wi] = c.dur[wi] ? (c.dur[wi] * 3 + gap) / 4 : gap;
		if (c.count < 250)
			c.count++;
		const bool sane = c.dur[0] >= 44100 / 50 && c.dur[1] >= 44100 / 50 &&
		                  c.dur[0] <= 44100 * 2 && c.dur[1] <= 44100 * 2;
		// 両方の長さが 2 回ずつ揃い、20ms-2 秒に入っていれば点滅とみなす。
		// 全速の間は firmware の切り替えに位相を合わせ直す
		if (sane && c.count >= 5 && (!c.on || !m_throttled)) {
			c.on = true;
			c.anchor = m_ne_clock;
			c.anchor_i = u8(1 - wi);
		}
	}
	m_lcd.clear_changes();
}

const u8 *mu2000::lcd_render()
{
	if (!m_native_engine || !m_throttled)
		return m_lcd.render();
	// 細く回している間は、覚えた点滅をこちらの時計で切り替えて描き、
	// 描いたら元に戻す（firmware の思っている画面は変えない）
	u8 keep[0xC0];
	bool touched[0xC0] = {};
	u8 *dd = const_cast<u8 *>(m_lcd.ddram());
	u8 *cg = const_cast<u8 *>(m_lcd.cgram());
	for (int at = 0; at < 0xC0; at++) {
		blink_cell &c = m_blink[at];
		if (!c.on)
			continue;
		const u64 cycle = c.dur[0] + c.dur[1];
		// firmware の時計で 1 周期半書き換わらなければ、点滅は終わった
		if (!cycle || m_fw_clock - c.last_fw > cycle * 3 / 2) {
			c.on = false;
			continue;
		}
		const bool is_cg = at >= 0x80;
		const int addr = is_cg ? at - 0x80 : at;
		if (is_cg ? m_lcd.cg_owned(u32(addr)) : m_lcd.owned(u32(addr)))
			continue;
		u8 &cell = is_cg ? cg[addr] : dd[addr];
		const u64 pos = (m_ne_clock - c.anchor) % cycle;
		const int first = c.anchor_i;
		keep[at] = cell;
		touched[at] = true;
		cell = c.v[pos < c.dur[first] ? first : 1 - first];
	}
	const u8 *img = m_lcd.render();
	for (int at = 0; at < 0xC0; at++)
		if (touched[at])
			(at >= 0x80 ? cg[at - 0x80] : dd[at]) = keep[at];
	return img;
}

void mu2000::set_native_engine(int mode)
{
	// 切るときは、こちらで鳴らしている音を先に離す。切ったあとは firmware が
	// そのスロットを知らないので、離さないと鳴りっぱなしになる
	if (!mode && m_native_engine)
		m_ndrv.silence();
	// **液晶のマスを firmware に返す**（6.188・6.190）
	m_lcd.clear_owned();
	m_lcd.clear_cg_owned();
	for (blink_cell &c : m_blink)
		c = blink_cell();
	m_lcd.clear_changes();
	m_throttled = false;
	m_native_engine = mode;
	m_fw_hold = 0;
	m_learning = false;
	m_learn_left = 0;
	for (u8 &c : m_fw_notes)
		c = 0;
	for (u8 &v : m_fw_meter)
		v = 0;
	for (u64 &v : m_fw_meter_at)
		v = 0;
	for (part_prog &p : m_prog_sel)
		p = part_prog();
	for (s8 &m : m_part_mode)
		m = -1;
	for (auto &q : m_prog_seen)
		q[0] = q[1] = q[2] = 0xff;
	m_fw_note_total = 0;
	m_fw_note_until = 0;
	m_nq.clear();
	m_sx_pos = -1;
	m_traj_rec = false;
	for (traj_rec &t : m_trajs)
		t = traj_rec();
	m_ne_clock = 0;
	// **液晶のメーターも初期化**（6.148）。つぎに描く時刻は m_ne_clock で
	// 測っているので、戻さないと切り替えたあと動かなくなる
	m_meter_next = 0;
	for (u8 &v : m_meter_lv)
		v = 0;
	for (u8 &v : m_meter_smooth)
		v = 0;
	for (u8 &v : m_meter_cell)
		v = 0;
	for (u64 &t : m_rx_at)
		t = 0;
	m_rx_at_usb = 0;
	m_rx_usb_port = -1;
	std::memset(m_nown, 0, sizeof(m_nown));
	for (nmidi &n : m_nmidi)
		n = nmidi();
	m_ne_samples.store(0, std::memory_order_relaxed);
	m_ne_fw_samples.store(0, std::memory_order_relaxed);
	m_ne_by_note.store(0, std::memory_order_relaxed);
	m_ne_by_sysex.store(0, std::memory_order_relaxed);
	m_ne_by_other.store(0, std::memory_order_relaxed);
	m_ne_by_learn.store(0, std::memory_order_relaxed);
	m_ne_by_midi.store(0, std::memory_order_relaxed);
	m_ne_by_keep.store(0, std::memory_order_relaxed);
	m_ne_by_panel.store(0, std::memory_order_relaxed);
	m_fw_why = 0;
	m_ne_stats = native_stats();
	if (!mode) {
		set_swp_watch(nullptr);
		return;
	}
	m_ndrv.reset();
	m_ndrv.set_rom(m_prog ? m_prog->data() : nullptr);
	m_ndrv.set_ram(m_ram.data());
	m_ndrv.set_poke([this](u32 reg, u16 value) {
		// **--trace-swp に native の書き込みも残す**。firmware の書き込みは
		// バスの所で記録されるが、こちらは write16 を直に呼ぶので通らない。
		// 両方を同じ形で残せば、firmware と native の書き込みを 1 つずつ
		// 突き合わせられる（"N " が native）
		// **0x1000 から上はスレーブ**（スロット 64-127。native_driver の SLOTS）
		const bool slave = reg >= 0x1000;
		if (slave) {
			reg -= 0x1000;
			// スレーブの声の出口（0x35-0x37）はマスタと値が違う（native_driver::slave_mixer）
			const u32 r = reg % 64;
			if (reg < 0x1000 && r >= 0x35 && r <= 0x37)
				value = xg::native_driver::slave_mixer(value);
		}
		if (m_swp_trace)
			std::fprintf(m_swp_trace, "N %s %04x %04x  pc=00000000  t=%.6f s=%llu\n",
			             slave ? "00802000" : "00800000", reg, value, double(trace_sample()) / 44100.0,
			             (unsigned long long)trace_sample());
		(slave ? m_swps : m_swpm).write16(reg, value);
	});
	// **チップの「音程の包絡線が着いた」印**を native の口にも見せる。
	// 実機の firmware も内部レジスタ 4 の bit14 で同じものを見ている（0x12B81C）
	// スロット 64-127 はスレーブの声 0-63
	m_ndrv.set_peg_peek([this](int chan) { return (chan < 64 ? m_swpm : m_swps).peg_reached(chan & 63); });
	m_ndrv.set_slot_peek([this](int chan) { return (chan < 64 ? m_swpm : m_swps).slot_active(chan & 63); });
	m_ndrv.set_slot_held([this](int chan) { return !(chan < 64 ? m_swpm : m_swps).slot_freed(chan & 63); });
}

// 音色の 1 音目を firmware に鳴らさせて、スロットに書かれた値を写し取る
void mu2000::native_learn_start(u32 rec)
{
	m_learning = true;
	m_learn_rec = rec;
	// その鍵・強さで鳴るはずの要素の数
	m_learn_want = 1;
	if (rec && m_prog) {
		const u8 *rom0 = m_prog->data();
		int n = 0;
		const int nel = xg::nv::element_count(rom0, rec);
		for (int k = 0; k < nel; k++)
			if (xg::nv::element_active(xg::nv::element(rom0, rec, k), learn_note_shifted(),
			                           learn_vel_sensed()))
				n++;
		if (n > 0)
			m_learn_want = n;
	}
	m_learn_first.clear();
	m_learn_last.clear();
	m_learn_traj.clear();
	m_learn_key_clock = 0;
	m_learn_mask = m_learn_keyed = 0;
	for (s8 &c : m_learn_chan)
		c = -1;
	// レジスタは鍵を押した所でまとめて書かれるので、短くてよい。
	// 長くすると、その間の音が全部 firmware に回ってしまう。
	// ただし短すぎると 0x01（鳴らしてから上がっていく）が落ち着く前に切れる
	m_learn_left = 44100 / 50;          // 20ms ぶん見る
	set_swp_watch([this](bool master, u32 reg, u16 value) {
		if (!master)
			return;
		m_learn_last[reg] = value;
		// 鍵を押したあとのフィルタ・LFO の動きを、時刻つきで控えておく
		if (m_learn_key_clock) {
			const int r2 = int(reg % 64);
			// **10ms タイマの位相をここで学ぶ**（6.118）。実機が 0x00 を
			// 書いた時刻そのものが、firmware の 10ms 割り込みの目
			if (r2 == 0x00 && reg < 0x1000)
				m_ndrv.set_eg_phase(u32(m_ne_clock));
			if ((r2 == 0x00 || r2 == 0x01 || r2 == 0x04 || r2 == 0x05 || r2 == 0x0a) &&
			    reg < 0x1000 && m_learn_traj.size() < 512)
				m_learn_traj.push_back({ int(reg / 64),
				    xg::nv::fstep{ u32(m_ne_clock - m_learn_key_clock), u8(r2), value } });
		}
		switch (reg) {
		case 0x18e: m_learn_mask = (m_learn_mask & ~(u64(0xffff) << 48)) | (u64(value) << 48); break;
		case 0x18f: m_learn_mask = (m_learn_mask & ~(u64(0xffff) << 32)) | (u64(value) << 32); break;
		case 0x1ce: m_learn_mask = (m_learn_mask & ~(u64(0xffff) << 16)) | (u64(value) << 16); break;
		case 0x1cf: m_learn_mask = (m_learn_mask & ~u64(0xffff)) | value; break;
		case 0x20e:
			// **要素のぶんだけ**。速い曲では、写し取りの窓の中に次の音の
			// 引き金が入ってしまい、余計なスロットまで拾っていた
			// **写し取りの窓の中で、別の音が同じスロットに鳴り始めたか**。
			// 写し取りは「窓の中で最後に見た値」を取るので、ここで重なると
			// その音色の包絡線が別の音の値で焼き付いてしまう
			if (m_learn_keyed && (m_learn_mask & m_learn_keyed)) {
				m_ne_learn_dirty++;
				if (std::getenv("SMU2000_NATIVE_DEBUG"))
					std::fprintf(stderr, "写し取りが汚れた: すでに %d 個、新しい鍵 %016llx 重なり %016llx\n",
					             std::popcount(m_learn_keyed),
					             (unsigned long long)m_learn_mask,
					             (unsigned long long)(m_learn_mask & m_learn_keyed));
			}
			if (std::popcount(m_learn_keyed) < m_learn_want) {
				// **firmware がこちらの鳴っているスロットを取ったか**を見る。
				// firmware は native の使用中を知らないので、声が増えると
				// 奪い合いになり、写し取りに 2 つの音の値が混ざる
				if (const u64 clash = m_learn_mask & m_ndrv.slot_mask().w[0]) {
					m_ne_slot_clash++;
					if (std::getenv("SMU2000_NATIVE_DEBUG"))
						std::fprintf(stderr, "スロットの奪い合い: firmware=%016llx native=%016llx 重なり=%016llx\n",
						             (unsigned long long)m_learn_mask,
						             (unsigned long long)m_ndrv.slot_mask().w[0],
						             (unsigned long long)clash);
				}
				m_learn_keyed |= m_learn_mask;
			}
			if (m_learn_first.empty())
				m_learn_first = m_learn_last;
			if (!m_learn_key_clock)
				m_learn_key_clock = m_ne_clock;
			// 鳴り始めたら、あと少しだけ見て終える（0x01 が落ち着くぶん）。
			// ただし**要素がそろうまでは待つ**。MusicBox のように 2 つ目の要素を
			// 37ms 遅れて鳴らす音色があり、打ち切ると片方しか写し取れない。
			// 長く占有すると、その間ほかの音色が写し取りを始められないので、
			// そろったら 5ms で切り上げる
			m_learn_left = std::popcount(m_learn_keyed) >= m_learn_want
			             ? 44100 / 200 : 44100 / 16;
			break;
		default: break;
		}
	});
}

void mu2000::native_learn_finish()
{
	set_swp_watch(nullptr);
	m_learning = false;
	if (!m_learn_keyed || !m_prog)
		return;
	// ドラムは、音色の記録が引けないので中身を写すだけ（音ごとに覚える）
	if (m_learn_drum) {
		std::vector<xg::nv::voice_cal> cals;
		for (int ch = 0; ch < 64; ch++) {
			if (!(m_learn_keyed & (u64(1) << ch)))
				continue;
			xg::nv::voice_cal cal;
			for (int i = 0; i < 0x40; i++) {
				const bool at_key = (i == 0x05 || i == 0x0a || i == 0x11);
				const std::map<u32, u16> &src = at_key ? m_learn_first : m_learn_last;
				const auto it = src.find(u32(ch) * 64 + u32(i));
				if (it != src.end())
					cal.set(i, it->second);
			}
			if (!cal.has(0x16) || !cal.has(0x17))
				continue;
			if (int(cals.size()) >= m_learn_want)
				break;
			cal.cal_vel  = learn_vel_sensed();
			cal.cal_note = learn_note_shifted();
			cal.cal_vol  = m_ndrv.part_vol(m_learn_part);
			cal.cal_expr = m_ndrv.part_expr(m_learn_part);
			cal.cal_pan  = m_ndrv.part_pan(m_learn_part);
			cal.cal_mod  = m_ndrv.part_mod(m_learn_part);
			cal.cal_rev  = m_ndrv.part_rev(m_learn_part);
			cal.cal_cho  = m_ndrv.part_cho(m_learn_part);
			cal.cal_bri  = m_ndrv.part_bri(m_learn_part);
			cal.cal_res  = m_ndrv.part_res(m_learn_part);
			cal.cal_ctx  = m_ndrv.part_ctx(m_learn_part);
			cal.have = true;
			m_learn_chan[ch] = s8(cals.size());
			cals.push_back(cal);
		}
		const int ndcal = int(cals.size());
		const u64 dkey = m_learn_drum;
		m_ndrv.learn_drum(m_learn_drum, std::move(cals));
		traj_start(0, dkey, ndcal, 0);
		m_learn_drum = 0;
		return;
	}
	// 波形の番地まで取れていなければ、写し取りとして使えない（次の音でやり直す）
	{
		bool ok = false;
		for (int ch = 0; ch < 64 && !ok; ch++)
			if ((m_learn_keyed & (u64(1) << ch)) &&
			    m_learn_last.count(u32(ch) * 64 + 0x16) && m_learn_last.count(u32(ch) * 64 + 0x17))
				ok = true;
		if (!ok)
			return;
	}
	const u8 *rom = m_prog->data();
	const int nel = xg::nv::element_count(rom, m_learn_rec);
	unsigned used_elem = 0;
	std::vector<xg::nv::voice_cal> cals;
	for (int ch = 0; ch < 64; ch++) {
		if (!(m_learn_keyed & (u64(1) << ch)))
			continue;
		if (int(cals.size()) >= nel)     // 要素より多く拾わない
			break;
		xg::nv::voice_cal cal;
		for (int i = 0; i < 0x40; i++) {
			// 0x05・0x0a・0x11 は LFO が動かし続けるので引き金の瞬間、
			// ほかは落ち着いた値（doc/native-engine.md の 6.10）
			const bool at_key = (i == 0x05 || i == 0x0a || i == 0x11);
			const std::map<u32, u16> &src = at_key ? m_learn_first : m_learn_last;
			const auto it = src.find(u32(ch) * 64 + u32(i));
			if (it != src.end())
				cal.set(i, it->second);
		}
		// どの要素かは、そのスロットが鳴らしている波形の番地で見分ける。
		// 同じ波形の要素が 2 つあるときは、まだ使っていないほうを取る
		int idx = -1;
		if (cal.has(0x16) && cal.has(0x17)) {
			const u32 want = u32(cal.reg[0x16]) << 16 | cal.reg[0x17];
			for (int k = 0; k < nel; k++) {
				if (used_elem & (1u << k))
					continue;
				const u8 *e2 = xg::nv::element(rom, m_learn_rec, k);
				const u8 *w2 = xg::nv::wave_entry(rom, xg::nv::wave_set(e2),
				                                  xg::nv::wave_note(rom, e2, learn_note_shifted()));
				if (w2 && xg::nv::read_wave(w2).format_addr == want) {
					idx = k;
					used_elem |= 1u << k;
					break;
				}
			}
		}
		if (idx < 0) {
			// **波形の番地が取れているのに、どの要素とも合わない**＝この
			// スロットはこの音色のものではない。同時に音が鳴ると firmware の
			// 鳴らす順で関係ないスロットを掴むことがあり、そのまま覚えると
			// **その音の包絡線がこの音色に焼き付く**（アタックが極端に遅い、
			// リリースが無い、など）。捨てて次の音でやり直す
			if (cal.has(0x16) && cal.has(0x17)) {
				m_ne_learn_wrong++;
				continue;
			}
			idx = int(cals.size()) < nel ? int(cals.size()) : 0;
		}
		// **音量の目盛りは実機の塊から直に取る**（6.101）。減衰の表は同じ値が
		// 3-4 段つづくので、減衰から目盛りを逆に引くと幅でしか分からない。
		// 掛ける前の目盛りは実機がボイスの塊 +118 に持っているので、それを
		// そのまま使えば当て推量が要らない。取れなければ逆引きに落とす
		{
			const u8 *el0 = xg::nv::element(rom, m_learn_rec, idx);
			const int att_ref = cal.has(9) ? (cal.reg[9] & 0xff) : 64;
			const int gain = m_ndrv.vol_gain_of(m_learn_part,
			                                    m_ndrv.part_vol(m_learn_part),
			                                    m_ndrv.part_expr(m_learn_part));
			const int rest = xg::nv::volume_rest(rom, el0, learn_note_shifted(),
			                                     learn_vel_sensed());
			const int fwl = xg::nv::fw_voice_level(m_ram.data(), ch);
			// **検算**: 読んだ目盛りから組み直した減衰が、実機が書いた 0x09 と
			// 合うか。合わなければ塊が別の声のものなので、逆引きに落とす
			// **目盛りは ROM から出す**（6.113）。ここで覚えるのは、実機の
			// ボイスの塊 +118 とのずれだけ（普通は 0）。**頭打ち（0 か 128）に
			// なっている鍵では差が取れない**ので、そのときは 0 のままにする
			const int mine = xg::nv::volume_level(rom, m_learn_rec, el0,
			                                      learn_note_shifted(), 0);
			cal.base_level = 0;
			if (fwl >= 1 && fwl <= 127 && mine >= 1 && mine <= 127
			    && xg::nv::volume_att_from(rom, fwl, rest, gain) == att_ref) {
				cal.base_level = fwl - mine;
				if (cal.base_level)
					m_ne_lvl_miss++;
			}
		}
		// **減衰の目盛りのずれを覚える**。実機が書いた 0x07・0x08 の上位から
		// 目盛りを引き直し、こちらの式で出した目盛りとの差を取る。
		// 同じ値が並ぶ表なので、こちらの目盛りにいちばん近いものを選ぶ
		{
			const u8 *el2 = xg::nv::element(rom, m_learn_rec, idx);
			const int corr2 = xg::nv::rate_key_corr(el2, learn_note_shifted());
			const int raw[2] = { int(el2[74]), int(el2[75]) };
			for (int k = 0; k < 2; k++) {
				if (!cal.has(0x07 + k))
					continue;
				const int mine = xg::nv::rate_scale(raw[k], corr2);
				const u8 want = u8(cal.reg[0x07 + k] >> 8);
				int best = -1, bestd = 1 << 30;
				// **奇数の目盛りも見る**（実機は 2 倍の単位に乗らない値も使う）
				for (int i = 0; i <= 127; i++)
					if (rom[xg::nv::DECAY_TAB + i] == want && std::abs(i - mine) < bestd) {
						bestd = std::abs(i - mine);
						best = i;
					}
				if (best >= 0)
					cal.dec_adj[k] = best - mine;
			}
		}
		cal.cal_vel  = m_learn_vel;
		cal.cal_note = learn_note_shifted();
		cal.cal_vol  = m_ndrv.part_vol(m_learn_part);
		cal.cal_expr = m_ndrv.part_expr(m_learn_part);
		cal.cal_pan  = m_ndrv.part_pan(m_learn_part);
		cal.cal_mod  = m_ndrv.part_mod(m_learn_part);
		cal.cal_rev  = m_ndrv.part_rev(m_learn_part);
		cal.cal_cho  = m_ndrv.part_cho(m_learn_part);
		cal.cal_bri  = m_ndrv.part_bri(m_learn_part);
		cal.cal_res  = m_ndrv.part_res(m_learn_part);
		cal.cal_ctx  = m_ndrv.part_ctx(m_learn_part);
		cal.have = true;
		m_learn_chan[ch] = s8(cals.size());
		cals.push_back(cal);
	}
	const int ncal = int(cals.size());
	if (std::getenv("SMU2000_NATIVE_DEBUG")) {
		std::fprintf(stderr, "learn rec=%06x 要素 %d 写し %d 鍵いた %d part=%d ctx=%08x\n",
		             m_learn_rec, nel, ncal, std::popcount(m_learn_keyed),
		             m_learn_part, m_ndrv.part_ctx(m_learn_part));
		for (int k = 0; k < ncal; k++) {
			const xg::nv::voice_cal &c = cals[size_t(k)];
			const u8 *e2 = xg::nv::element(rom, m_learn_rec, k);
			const u8 *w2 = xg::nv::wave_entry(rom, xg::nv::wave_set(e2),
			                                  xg::nv::wave_note(rom, e2, m_learn_note));
						std::fprintf(stderr, "  写し%d 0x11=%04x 0x32=%04x 0x09=%04x 波形=%08x"
			                     " / 式 0x11=%04x 要素b18=%d b0=%d b1=%d\n",
			             k, c.reg[0x11], c.reg[0x32], c.reg[0x09], c.wave_addr(),
			             w2 ? xg::nv::pitch_reg(xg::nv::read_wave(w2), m_learn_note,
			                                    xg::nv::key_follow(rom, e2), 0,
			                                    xg::nv::key_pivot(e2)) : 0,
			             e2[18], e2[0], e2[1]);
			if (w2)
				std::fprintf(stderr, "        こちらの波形=%08x 基準鍵=%d 微調=%d 上限鍵=%d 追従=%d 組=%d%s",
				             xg::nv::read_wave(w2).format_addr, xg::nv::read_wave(w2).base_key,
				             xg::nv::read_wave(w2).fine_cents, xg::nv::read_wave(w2).key_max,
				             xg::nv::key_follow(rom, e2), xg::nv::wave_set(e2), "\n");
		}
	}
	const u32 learn_ctx = cals.empty() ? 0 : cals[0].cal_ctx;
	m_ndrv.learn(m_learn_rec, std::move(cals));
	traj_start(m_learn_rec, 0, ncal, learn_ctx);
}


// ---- 写し取りをファイルに残す・戻す（voicecache.h）
//
// 形は「頭 → 記録ごと」。記録 1 つぶんは
//   種類(1) 鍵(8) 写しの数(2) ／ 写しごとに
//   覚えたレジスタの印(8)・素の音量(2)・写したときの強さ/音量/表現/パン(2 ずつ)
//   ・印の立っているレジスタの値(2 ずつ)・包絡線の段数(2)・段ごとに 時刻(4) 番地(1) 値(2)

namespace {

constexpr u32 CAL_MAGIC = 0x43563253u;   // "S2VC"
constexpr u32 CAL_VERSION = 9;

void put8(std::vector<u8> &v, u8 x) { v.push_back(x); }
void put16v(std::vector<u8> &v, u16 x) { v.push_back(u8(x)); v.push_back(u8(x >> 8)); }
void put32v(std::vector<u8> &v, u32 x) { for (int i = 0; i < 4; i++) v.push_back(u8(x >> (i * 8))); }
void put64v(std::vector<u8> &v, u64 x) { for (int i = 0; i < 8; i++) v.push_back(u8(x >> (i * 8))); }

struct rd {
	const u8 *p, *e;
	bool ok = true;
	u8 g8() { if (p + 1 > e) { ok = false; return 0; } return *p++; }
	u16 g16() { const u8 a = g8(), b = g8(); return u16(a | (b << 8)); }
	u32 g32() { u32 x = 0; for (int i = 0; i < 4; i++) x |= u32(g8()) << (i * 8); return x; }
	u64 g64() { u64 x = 0; for (int i = 0; i < 8; i++) x |= u64(g8()) << (i * 8); return x; }
};

void write_cals(std::vector<u8> &out, u8 kind, u64 key, const std::vector<xg::nv::voice_cal> &cals)
{
	put8(out, kind);
	put64v(out, key);
	put16v(out, u16(cals.size()));
	for (const xg::nv::voice_cal &c : cals) {
		put64v(out, c.mask);
		put16v(out, u16(c.base_level));
		put16v(out, u16(c.cal_vel));
		put16v(out, u16(c.cal_note));
		put16v(out, u16(c.cal_vol));
		put16v(out, u16(c.cal_expr));
		put16v(out, u16(c.cal_pan));
		put16v(out, u16(c.cal_mod));
		put16v(out, u16(c.cal_rev));
		put16v(out, u16(c.cal_cho));
		put16v(out, u16(c.cal_bri));
		put16v(out, u16(c.cal_res));
		put32v(out, c.cal_ctx);
		put16v(out, u16(s16(c.dec_adj[0])));
		put16v(out, u16(s16(c.dec_adj[1])));
		for (int i = 0; i < 0x40; i++)
			if (c.mask & (u64(1) << i))
				put16v(out, c.reg[i]);
		const u16 n = u16(std::min<size_t>(c.filter_env.size(), 4096));
		put16v(out, n);
		for (int i = 0; i < n; i++) {
			put32v(out, c.filter_env[i].at);
			put8(out, c.filter_env[i].reg);
			put16v(out, c.filter_env[i].v);
		}
	}
}

} // namespace

std::vector<u8> mu2000::native_cal_save() const
{
	const auto &cal = m_ndrv.cal_map();
	const auto &drum = m_ndrv.drum_map();
	if (cal.empty() && drum.empty())
		return {};
	std::vector<u8> out;
	put32v(out, CAL_MAGIC);
	put32v(out, CAL_VERSION);
	put32v(out, u32(cal.size() + drum.size()));
	for (const auto &kv : cal)
		write_cals(out, 0, kv.first, kv.second);
	for (const auto &kv : drum)
		write_cals(out, 1, kv.first, kv.second);
	return out;
}

bool mu2000::native_cal_load(const u8 *data, size_t n)
{
	rd r{ data, data + n };
	if (r.g32() != CAL_MAGIC || r.g32() != CAL_VERSION || !r.ok)
		return false;
	const u32 count = r.g32();
	if (count > 100000)
		return false;
	for (u32 k = 0; k < count && r.ok; k++) {
		const u8 kind = r.g8();
		const u64 key = r.g64();
		const u16 ncal = r.g16();
		if (ncal > 8 || !r.ok)
			return false;
		std::vector<xg::nv::voice_cal> cals;
		for (u16 c2 = 0; c2 < ncal && r.ok; c2++) {
			xg::nv::voice_cal c;
			c.mask = r.g64();
			c.base_level = s16(r.g16());
			c.cal_vel = s16(r.g16());
			c.cal_note = s16(r.g16());
			c.cal_vol = s16(r.g16());
			c.cal_expr = s16(r.g16());
			c.cal_pan = s16(r.g16());
			c.cal_mod = s16(r.g16());
			c.cal_rev = s16(r.g16());
			c.cal_cho = s16(r.g16());
			c.cal_bri = s16(r.g16());
			c.cal_res = s16(r.g16());
			c.cal_ctx = r.g32();
			c.dec_adj[0] = s16(r.g16());
			c.dec_adj[1] = s16(r.g16());
			for (int i = 0; i < 0x40; i++)
				if (c.mask & (u64(1) << i))
					c.reg[i] = r.g16();
			const u16 steps = r.g16();
			if (steps > 4096 || !r.ok)
				return false;
			c.filter_env.reserve(steps);
			for (u16 i = 0; i < steps && r.ok; i++) {
				xg::nv::fstep s;
				s.at = r.g32();
				s.reg = r.g8();
				s.v = r.g16();
				c.filter_env.push_back(s);
			}
			c.have = true;
			cals.push_back(std::move(c));
		}
		if (!r.ok)
			return false;
		if (kind == 0)
			m_ndrv.learn(u32(key), std::move(cals));
		else
			m_ndrv.learn_drum(key, std::move(cals));
	}
	return true;
}

// 写し取った音が鳴っている間、firmware がフィルタ（0x00・0x01・0x04）を
// どう動かすかを録る。あとの音でも同じように動かせば、音色の動きまで揃う。
// **同時に何本も走らせる**（空きが無ければ録らない）
void mu2000::traj_start(u32 rec, u64 drum_key, int ncal, u32 ctx)
{
	if (!ncal)
		return;
	std::vector<xg::nv::voice_cal> *cals =
	    drum_key ? m_ndrv.drum_cals_of(drum_key) : m_ndrv.cals_of_ctx(rec, ctx);
	if (!cals)
		return;
	int slot = -1;
	for (int k = 0; k < TRAJ_MAX && slot < 0; k++)
		if (!m_trajs[k].left)
			slot = k;
	if (slot < 0)
		return;                      // 空きが無い。この音色は次の音でやり直す
	traj_rec &t = m_trajs[slot];
	t = traj_rec();
	t.cals = cals;
	t.rec_key = rec;
	t.ctx = ctx;
	t.drum_key = drum_key;
	// 写し取ったチャンネルの順が、そのまま写し取りの並び
	for (int ch = 0; ch < 64; ch++)
		t.chan[ch] = m_learn_chan[ch];
	// 鍵を押した瞬間からの控えを、まず入れる
	for (const auto &e : m_learn_traj)
		if (e.first < 64 && t.chan[e.first] >= 0 &&
		    size_t(t.chan[e.first]) < cals->size())
			(*cals)[t.chan[e.first]].filter_env.push_back(e.second);
	m_learn_traj.clear();
	t.start = m_learn_key_clock ? m_learn_key_clock : m_ne_clock;
	t.left = 44100 * 3;              // 3 秒ぶん見る（押している間の動きを取り切る）
	m_traj_rec = true;
	m_ndrv.set_recording(true);
}

// SWP30 への書き込みを、録っている全部の本に配る
void mu2000::traj_watch(u32 reg, u16 value)
{
	const int ch = int(reg / 64), r = int(reg % 64);
	if (ch >= 64)
		return;
	for (traj_rec &t : m_trajs) {
		if (!t.left || t.chan[ch] < 0)
			continue;
		// **離しに入っても止めない**。実機はフィルタを離しのあいだも動かす。
		// 写し取りの元にした音が短いと、録れる段のほとんどが離しのあとになる。
		// ここから先の段には印を付けて、鳴らすときは離した時刻から流す。
		// ただし鳴らし始めてすぐは見ない。前の音の離しが同じスロットに来る
		if (r == 0x09 && (value & 0x8000)) {
			if (m_ne_clock - t.start > 44100 / 10 && !t.rel_at[ch])
				t.rel_at[ch] = m_ne_clock;
			continue;
		}
		// フィルタ（0x00・0x01・0x04）と LFO（0x05・0x0a）。
		// LFO は「かけ始めるまでの間」や深さの増やし方を firmware がソフトでやっている
		if (r != 0x00 && r != 0x01 && r != 0x04 && r != 0x05 && r != 0x0a)
			continue;
		if (r == 0x00)
			m_ndrv.set_eg_phase(u32(m_ne_clock));
		if (t.n >= 4096 || size_t(t.chan[ch]) >= t.cals->size())
			continue;
		// **その場で**写し取りに足す。いま鳴っている native の音も、
		// 次の tick でこの段を拾う（xg/native_driver.h の tick）
		const u64 rel = t.rel_at[ch];
		(*t.cals)[t.chan[ch]].filter_env.push_back(
		    xg::nv::fstep{ u32(m_ne_clock - (rel ? rel : t.start)),
		                   u8(r), value, u8(rel ? 1 : 0) });
		t.n++;
	}
}

// 1 サンプルぶん進めて、終わった本を片付ける
void mu2000::traj_step()
{
	for (int k = 0; k < TRAJ_MAX; k++)
		if (m_trajs[k].left && --m_trajs[k].left == 0)
			traj_finish_one(k);
	m_traj_rec = traj_any();
	if (!m_traj_rec)
		m_ndrv.set_recording(false);
}

void mu2000::traj_finish_one(int i)
{
	traj_rec &t = m_trajs[i];
	// 録れた段が少なければ、写し取りごと捨ててつぎの音でやり直す。
	// 回数を切っておかないと、短い音しか鳴らさない音色がいつまでも
	// firmware 送りのままになる
	bool again = false;
	if (!t.drum_key && t.rec_key && t.n < TRAJ_ENOUGH) {
		const u64 k = u64(t.rec_key) | (u64(t.ctx) << 32);
		int &n = m_traj_tries[k];
		if (n < TRAJ_TRIES) {
			n++;
			again = true;
			m_ndrv.drop_cal(t.rec_key, t.ctx);
		}
	}
	if (std::getenv("SMU2000_NATIVE_DEBUG")) {
		u32 nrel = 0, ntot = 0;
		if (!again && t.cals && !t.cals->empty()) {
			ntot = u32((*t.cals)[0].filter_env.size());
			for (const auto &e : (*t.cals)[0].filter_env)
				nrel += e.rel ? 1 : 0;
		}
		std::fprintf(stderr, "traj rec=%06x drum=%llx 段 %u（写し1 は %u 段、うち離し %u）%s%c",
		             t.rec_key, (unsigned long long)t.drum_key, t.n,
		             ntot, nrel, again ? "（短いので取り直す）" : "", 10);
	}
	t = traj_rec();
}

// バンクとプログラムから音色の記録を引いて、native の口に渡す。
// firmware がワーク RAM に入れるのを待たなくて済む（引き方は
// xg::voice_rom::lookup。旋律系のバンク 640 音色で firmware と食い違い 0）

// **液晶のメーターを自分で描く**（doc/native-engine.md の 6.148）。
//
// 実機は「演奏画面を描く係」（ROM 0x0D1340）でメーターを描いているが、
// これは **firmware が自分で音を持っている間しか呼ばれない**（実測。
// firmware を全速で回しても、native が鳴らしているだけでは走らない）。
// だから native の口では、棒の字を液晶へ直に置く。
//
// 並びは実機を見て割り出した:
//   * 下の行の 1-8 桁目が 8 マス。1 マスに **2 パート**（左＝偶数、右＝奇数）
//   * 上の行の同じ桁は、棒が 8 点を越えたぶん。両方 0 なら空白
//   * 字のコードは 0x7f + 9a + b（a・b は 0-8 点。mu2000::fill_missing_glyphs）
//   * 点の数は **目盛り / 8 + 1**（鳴っていないパートも 1 点出る）
// **いま選んでいるパート**（ワーク RAM 0x42158F。0 から数える。6.190）。
// 演奏画面の係（0x0D136C）がここを読んで口と番号を作る
static constexpr u32 SEL_PART = 0x2158f;

void mu2000::draw_meter()
{
	// **firmware が思っている画面を見る**（6.188）。ここの 16 マスは
	// native の持ち物にしてあるので、表示そのものを見ても
	// こちらが前に置いた字しか無く、画面が替わったのに気づけない
	const u8 *cur = m_lcd.fw_ddram();
	// **触ってよいのは次の 2 つだけ**:
	//   * こちらが前に書いた値がそのまま残っているマス
	//   * firmware が置いた「鳴っていない」形（下 0x89 / 上 空白）
	// どれか 1 つでも当てはまらなければ、**1 マスも触らない**。
	// 別の画面では同じ桁に文字が出ていて、消すと表示が壊れる
	if (cur[0x40] != 0x89) {
		m_lcd.clear_owned();
		return;
	}
	// **演奏画面だけ**。VOL・EXP・REV・CHO・VAR の画面でも firmware は同じ
	// 欄へ同じ棒の字で各パートの値を描くので、棒の字かどうかでは見分けられず、
	// 上から音量メーターを描いて値を消していた。演奏画面は下の行 9 桁目が
	// バンク番号の前の三角（0x10 / 0x11）、値の画面はそこが '='
	if (cur[0x49] != 0x10 && cur[0x49] != 0x11) {
		m_lcd.clear_owned();
		return;
	}
	// **上と下は別々に見る**。演奏画面には「上の行が棒の続きではなく数字」の
	// 形もあって（パネルで `play` を押したあとの画面）、まとめて見ると
	// 下の棒まで描けなくなる
	bool lo_ok = true, hi_ok = true;
	for (int c = 1; c <= 8; c++) {
		const u8 lo = cur[0x40 + u32(c)], hi = cur[u32(c)];
		// 下の行は**棒の字ならこちらのものとして引き取る**。実機モードから
		// 戻ったとき、firmware が最後に描いた棒がそのまま残っていることが
		// あって、`0x89`（鳴っていない形）だけを待っていると二度と描けない
		if (lo != m_meter_cell[c - 1] && (lo < 0x7f || lo > 0xd0))
			lo_ok = false;
		// 上の行は**下が満杯のときだけ棒の続き**（6.188）。
		// 写し取りのあいだは firmware も同じマスへ自分の
		// 続きを書くので、「空白かこちらの字」だけを待って
		// いると手放してしまう。といって棒の字なら何でも、と
		// すると**MUTE の画面**を壊す：あそこは上の行 16 マスを
		// 別の意味で使っていて、上が `89`（左右 1 点）なのに
		// 下も `89`（1 点）だった。
		//
		// 棒としては**下が 8 点になって初めて上が 1 点以上**に
		// なるので、その辻つまが合わなければ別の画面だと分かる
		bool hi_mine = hi == m_meter_cell[8 + c - 1] || hi == 0x20;
		if (!hi_mine && hi >= 0x7f && hi <= 0xd0
		    && lo >= 0x7f && lo <= 0xd0) {
			const int la = (lo - 0x7f) / 9, lb = (lo - 0x7f) % 9;
			const int ta = (hi - 0x7f) / 9, tb = (hi - 0x7f) % 9;
			hi_mine = (ta == 0 || la == 8) && (tb == 0 || lb == 8);
		}
		if (!hi_mine)
			hi_ok = false;
	}
	if (!lo_ok) {
		m_lcd.clear_owned();
		return;
	}
	for (int c = 1; c <= 8; c++) {
		const int l = int(m_meter_smooth[(c - 1) * 2]) / 8 + 1;
		const int r = int(m_meter_smooth[(c - 1) * 2 + 1]) / 8 + 1;
		const int lb = l > 8 ? 8 : l, rb = r > 8 ? 8 : r;
		const int lt = l > 8 ? (l - 8 > 8 ? 8 : l - 8) : 0;
		const int rt = r > 8 ? (r - 8 > 8 ? 8 : r - 8) : 0;
		const u8 lo = u8(0x7f + lb * 9 + rb);
		const u8 hi = (lt || rt) ? u8(0x7f + lt * 9 + rt) : u8(0x20);
		// **そのマスをこちらの持ち物にする**（6.188）。
		// 写し取りのあいだなど firmware も音を持っているときは、
		// 向こうも演奏画面の係を回して同じ 16 マスへ自分の棒を
		// 書く。二人で交互に書くので、画面が 25ms ごとにちらついていた
		m_lcd.set_owned(u32(0x40 + c), true);
		m_lcd.poke_ddram(u32(0x40 + c), lo);
		m_meter_cell[c - 1] = lo;
		if (hi_ok) {
			m_lcd.set_owned(u32(c), true);
			m_lcd.poke_ddram(u32(c), hi);
			m_meter_cell[8 + c - 1] = hi;
		} else
			m_lcd.set_owned(u32(c), false);
	}
}

// **演奏画面の音色まわりを native が描く**（doc/native-engine.md の 6.190）。
//
// native の口では firmware を 100ms につき 5ms しか回さないので、
// 音色を替えてから画面が追いつくまで**最大 100ms 遅れる**。
// ここで描くのは、記録から直に出せる 3 つだけ:
//
//   行 0 の 9-16   音色名の頭 8 文字
//   行 1 の 14-16  プログラム番号 + 1 の 3 桁
//   外字 0-2・4-6  楽器の絵（16 行 × 16 ビットを 5 ビットずつ）
//
// バンクの 3 桁とパート番号はまだ firmware に任せる（MSB が 0 でない
// ときの出方がまだ測れていない。6.190 の「まだ埋まっていないもの」）。
//
// **演奏画面だと分かるときだけ**書く。見分けは firmware が思っている
// 画面（6.188 の `fw_ddram`）の、こちらが持っていないマスでする。
void mu2000::release_voice_fields()
{
	if (!m_vf_owned)
		return;
	m_vf_owned = false;
	m_vf_part = -1;
	for (u32 c = 9; c <= 16; c++)
		m_lcd.set_owned(c, false);
	for (u32 c = 10; c <= 16; c++)
		m_lcd.set_owned(0x40 + c, false);
	m_lcd.clear_cg_owned();
}

void mu2000::draw_voice_fields()
{
	const u8 *fw = m_lcd.fw_ddram();

	// **演奏画面の印**。帯の字と口の字がそろっていること。
	// 1 つでも違えば別の画面なので、手を出さない
	if (fw[19] != 0xc6 || fw[0x40 + 9] != 0x11
	    || (fw[0x40 + 13] != 0x10 && fw[0x40 + 13] != 0x15)
	    || fw[0x40 + 17] < 'A' || fw[0x40 + 17] > 'D') {
		release_voice_fields();
		return;
	}
	if (m_ram.size() <= SEL_PART) {
		release_voice_fields();
		return;
	}
	const int part = int(m_ram[SEL_PART]);
	if (part < 0 || part >= 64) {
		release_voice_fields();
		return;
	}
	const xg::voice_rom vr(m_prog);
	if (!vr.ok()) {
		release_voice_fields();
		return;
	}
	const part_prog &p = m_prog_sel[part];
	const int mode = m_ram.size() > xg::ram::VOICE_MODE ? m_ram[xg::ram::VOICE_MODE] : 1;
	const int set  = m_ram.size() > xg::ram::VOICE_SET  ? m_ram[xg::ram::VOICE_SET]  : 1;
	const bool drum = (p.msb == 127 || p.msb == 126);
	const u32 rec = drum ? 0 : vr.lookup(mode, set, p.msb, p.lsb, p.prog);
	const std::string nm = vr.screen_name(rec, p.msb, p.prog);
	u16 ico[16];
	if (nm.size() != 8 || !vr.icon_of(rec, p.msb, p.prog, ico)) {
		release_voice_fields();
		return;
	}
	// 番号は 1 から数える 3 桁
	const int pn = (int(p.prog) & 0x7f) + 1;
	const u8 dg[3] = { u8('0' + pn / 100), u8('0' + (pn / 10) % 10),
	                   u8('0' + pn % 10) };

	// **バンクの 3 桁**（6.202）。MSB 64 は「SFX」、ドラム（126・127）は
	// MSB、それ以外は **LSB** の 3 桁（MSB 1-32 でも LSB のまま）
	u8 bk[3];
	if (p.msb == 64) {
		bk[0] = 'S'; bk[1] = 'F'; bk[2] = 'X';
	} else {
		const int bn = p.msb >= 126 ? int(p.msb) : int(p.lsb);
		bk[0] = u8('0' + bn / 100);
		bk[1] = u8('0' + (bn / 10) % 10);
		bk[2] = u8('0' + bn % 10);
	}

	if (!m_vf_owned || m_vf_part != part) {
		m_vf_owned = true;
		m_vf_part = part;
		for (int i = 0; i < 8; i++)
			m_vf_name[i] = 0;
		for (int i = 0; i < 3; i++)
			m_vf_prog[i] = 0;
		for (int i = 0; i < 3; i++)
			m_vf_bank[i] = 0;
		for (int y = 0; y < 16; y++)
			m_vf_icon[y] = 0xffff;
	}
	// **調べ用**（`SMU2000_VF_DBG=1`）。音色が替わった時刻を出す。
	// firmware に任せていたときの遅れと見比べるため
	if (std::getenv("SMU2000_VF_DBG")) {
		static int last = -1;
		if (last != int(p.prog)) {
			last = int(p.prog);
			std::fprintf(stderr, "VF %.3f part=%d prog=%d %.8s\n",
			             double(m_ne_clock) / 44100.0, part,
			             int(p.prog), nm.c_str());
		}
	}
	for (int i = 0; i < 8; i++) {
		const u8 v = u8(nm[size_t(i)]);
		m_lcd.set_owned(u32(9 + i), true);
		if (m_vf_name[i] != v) {
			m_vf_name[i] = v;
			m_lcd.poke_ddram(u32(9 + i), v);
		}
	}
	for (int i = 0; i < 3; i++) {
		m_lcd.set_owned(u32(0x40 + 14 + i), true);
		if (m_vf_prog[i] != dg[i]) {
			m_vf_prog[i] = dg[i];
			m_lcd.poke_ddram(u32(0x40 + 14 + i), dg[i]);
		}
	}
	for (int i = 0; i < 3; i++) {
		m_lcd.set_owned(u32(0x40 + 10 + i), true);
		if (m_vf_bank[i] != bk[i]) {
			m_vf_bank[i] = bk[i];
			m_lcd.poke_ddram(u32(0x40 + 10 + i), bk[i]);
		}
	}
	// **絵は 16 行 × 16 ビットを左から 5 ビットずつ**（6.190）。
	// 外字 0・1・2 が上半分（行 0-7）、4・5・6 が下半分（行 8-15）。
	// いちばん右の 1 列（bit0）は出さない
	for (int y = 0; y < 16; y++) {
		if (m_vf_icon[y] == ico[y]) {
			for (int c = 0; c < 3; c++)
				m_lcd.set_cg_owned(u32((y < 8 ? c : 4 + c) * 8 + (y & 7)), true);
			continue;
		}
		m_vf_icon[y] = ico[y];
		for (int c = 0; c < 3; c++) {
			const u32 at = u32((y < 8 ? c : 4 + c) * 8 + (y & 7));
			m_lcd.set_cg_owned(at, true);
			m_lcd.poke_cgram(at, u8((ico[y] >> (11 - c * 5)) & 0x1f));
		}
	}
}

// **パネルで替えられた音色を拾う**（6.146）。ジョグダイヤルや PART+/- の
// 音色替えは MIDI を通らないので、こちらが持っている `m_prog_sel` が古い
// ままになり、**画面は変わるのに音が変わらない**（実機モードへ行って戻ると
// 直るのは、そこで選びが作り直されるため）。
//
// ワーク RAM の値が**前に見たときから動いていたら**拾う。こちらが MIDI で
// 動かしたぶんは firmware が同じ値を書くので、二重には効かない
void mu2000::sync_prog()
{
	if (m_ram.size() < xg::ram::PARTS)
		return;
	for (int p = 0; p < 64; p++) {
		const u32 b = xg::ram::part_base(p);
		if (b + 4 > m_ram.size())
			continue;
		const u8 msb = m_ram[b + 1], lsb = m_ram[b + 2], prog = m_ram[b + 3];
		u8 *seen = m_prog_seen[p];
		if (seen[0] == msb && seen[1] == lsb && seen[2] == prog)
			continue;
		seen[0] = msb; seen[1] = lsb; seen[2] = prog;
		part_prog &sel = m_prog_sel[p];
		if (sel.msb == msb && sel.lsb == lsb && sel.prog == prog)
			continue;                    // MIDI で先に効かせてあった
		sel.msb = msb; sel.lsb = lsb; sel.prog = prog;
		native_select_voice(p);
	}
}

bool mu2000::voice_lsb_ok(int msb, int lsb) const
{
	if (!m_prog)
		return true;
	const xg::voice_rom vr(m_prog);
	const int mode = m_ram.size() > xg::ram::VOICE_MODE ? m_ram[xg::ram::VOICE_MODE] : 1;
	const int set  = m_ram.size() > xg::ram::VOICE_SET  ? m_ram[xg::ram::VOICE_SET]  : 1;
	return vr.lsb_ok(mode, set, msb, lsb);
}

void mu2000::native_select_voice(int part)
{
	if (part < 0 || part >= 64 || !m_prog)
		return;
	const part_prog &p = m_prog_sel[part];
	// バンク 127/126 だけでなく、**パートの種類**（08 pp 07）でもドラムになる
	const bool drum = (p.msb == 127 || p.msb == 126) || part_is_drum(part);
	if (drum) {
		m_ndrv.set_record(part, 0, 1);
		return;
	}
	const xg::voice_rom vr(m_prog);
	const int mode = m_ram.size() > xg::ram::VOICE_MODE ? m_ram[xg::ram::VOICE_MODE] : 1;
	const int set  = m_ram.size() > xg::ram::VOICE_SET  ? m_ram[xg::ram::VOICE_SET]  : 1;
	const u32 rec = vr.lookup(mode, set, p.msb, p.lsb, p.prog);
	m_ndrv.set_record(part, rec, rec ? 0 : -1);
}

// **受け取り終えた XG の SysEx を native の側にも効かせる**。
// 43 1n 4C hh mm ll dd… のうち、いま見るのは 08 pp ll（パートの設定）だけ。
// ここを入れるまでは、パートの設定を SysEx で送る曲（CC ではなく SysEx で
// 送りや音量を決める打ち込みは珍しくない）で、firmware がその SysEx を
// 処理し終えるまで native が古い値のまま鳴らしていた。native の口では
// firmware を 100ms につき 5ms しか回さないので、その遅れは 1 秒を超える
// issue #51 の調べ用（`SMU2000_RESET_DEBUG=1`）。**環境は 1 回だけ読む**
// （毎回 getenv を呼ぶと per-event の道が重くなる。6.225 の教訓）
static bool reset_debug()
{
	static const bool on = std::getenv("SMU2000_RESET_DEBUG") != nullptr;
	return on;
}


// リセット（GM・GS・XG）を受けたら、firmware がそれを処理し終えるまで
// こちらの発音を待たせる（issue #51）。ここまでに並んでいた分（リセット自身を含む）は
// そのまま流してよい。解くのは native_pump の呼び出し元（run_sample）
void mu2000::hold_after_reset(u64 fire)
{
	if (reset_debug())
		std::fprintf(stderr, "[reset] 待たせ始め fire=%llu ne_clock=%llu 並び=%zu\n",
		             (unsigned long long)fire, (unsigned long long)m_ne_clock, m_nq.size());
	m_ne_reset_hold = true;
	m_ne_reset_free = m_nq.size();
	const u64 now = fire > m_ne_clock ? fire : m_ne_clock;
	const u64 deadline = now + RESET_HOLD_MAX;
	if (deadline > m_ne_reset_deadline)
		m_ne_reset_deadline = deadline;
	m_fw_swp_at = now;                       // まだ静かになっていない、から始める
}


void mu2000::native_sysex(u64 fire)
{
	// **リセットは native にも効かせる**（6.136）。GM システムオン
	// （7E 7F 09 xx）・GS リセット（41 1n 42 12 40 00 7F）・XG システムオン
	// （43 1n 4C 00 00 7E/7F）。パートの状態も音色の選びも既定に戻るので、
	// こちらも戻さないと古い音色・古いつまみで鳴り続ける。
	// **ヤマハの判定より前に見る**（GM と GS は 43 で始まらない）
	if (m_sx_pos >= 3 && m_sx[0] == 0x7e && m_sx[2] == 0x09) {
		m_nq.push_back({ fire, 6, 0, 0, 0 });
		hold_after_reset(fire);
		return;
	}
	if (m_sx_pos >= 7 && m_sx[0] == 0x41 && m_sx[2] == 0x42 &&
	    m_sx[4] == 0x40 && m_sx[6] == 0x7f) {
		m_nq.push_back({ fire, 6, 0, 0, 0 });
		hold_after_reset(fire);
		return;
	}
	if (m_sx_pos < 7)
		return;
	if (!(m_sx[0] == 0x43 && (m_sx[1] & 0xf0) == 0x10 && m_sx[2] == 0x4c))
		return;
	const u8 hh = m_sx[3], mm = m_sx[4], ll = m_sx[5];
	if (hh == 0x00 && mm == 0x00 && (ll == 0x7e || ll == 0x7f)) {
		m_nq.push_back({ fire, 6, 0, 0, 0 });
		hold_after_reset(fire);
		return;
	}
	// ドラムのセットアップのリセット（00 00 7D nn）。その組だけ既定に戻る
	if (hh == 0x00 && mm == 0x00 && ll == 0x7d) {
		m_nq.push_back({ fire, 8, u8(m_sx[6] & 0x7f), 0, 0 });
		return;
	}
	// **ドラムのセットアップ（3n rr pp）も渡す**。実機は SysEx で書いても
	// 切る高さ・共振・EG・EQ・HPF を次の打から変える（2026-09-28 に firmware で
	// 確かめた。6.180 の「SysEx では計算し直さない」は番号の取り違えから出た誤り）。
	// 組と並びの番号を 1 バイトに詰める（組 2bit、番号 5bit）
	if (hh >= 0x30 && hh <= 0x33) {
		const int n = m_sx_pos - 6;
		for (int i = 0; i < n && i + 6 < int(sizeof(m_sx)); i++) {
			const int idx = xg::ram::drum_setup_index(ll + i);
			if (idx >= 0)
				m_nq.push_back({ fire, 7, u8((hh - 0x30) | (idx << 2)), mm, u8(m_sx[6 + i] & 0x7f) });
		}
		return;
	}
	if (hh != 0x08 || mm >= 32)
		return;
	// **1 回の SysEx で続けて何バイトも書ける**（ll から順に並ぶ）
	const int n = m_sx_pos - 6;
	for (int i = 0; i < n && i + 6 < int(sizeof(m_sx)); i++) {
		const u8 addr = u8(ll + i), dd = m_sx[6 + i] & 0x7f;
		if (addr == 0x01 || addr == 0x02 || addr == 0x03) {
			// バンクと音色。音色の指定と同じ行列に乗せる
			m_nq.push_back({ fire, 4, u8(mm),
			                 u8(addr == 0x01 ? 0 : addr == 0x02 ? 1 : 2), dd });
		// **つまみの割り当て（0x4D-0x66）も渡す**（6.195）。
		// これまでは 0x28 までしか渡していなくて、native はワーク RAM から
		// 読むしか無かったので、**割り当てを戻しても 100ms 気づかなかった**
		} else if (addr <= 0x28 || (addr >= 0x4d && addr <= 0x66)) {
			m_nq.push_back({ fire, 5, u8(mm), addr, dd });
		}
	}
}

// 待っている native の出来事を、時が来たものから実行する
void mu2000::native_pump()
{
	while (!m_nq.empty() && m_nq.front().at <= m_ne_clock) {
		// リセットが効き終わるまでは、そのあとに並んだものを止めておく
		if (m_ne_reset_hold) {
			if (m_ne_reset_free == 0)
				break;
			m_ne_reset_free--;
		}
		const nev e = m_nq.front();
		m_nq.pop_front();
		switch (e.kind) {
		case 0: m_ndrv.note_off(e.part, e.d0); break;
		case 1:
			if (reset_debug())
				std::fprintf(stderr, "[reset] 打鍵 part=%d note=%d ne_clock=%llu\n",
				             e.part, e.d0, (unsigned long long)m_ne_clock);
			if (m_ndrv.note_on(e.part, e.d0, e.d1))
				m_ne_stats.note_native++;
			break;
		case 2: m_ndrv.control(e.part, e.d0, e.d1); break;
		case 3: m_ndrv.bend(e.part, int(e.d1) << 7 | e.d0); break;
		// **音色の指定も行列に乗せる**。CC は線の遅れを模して行列に入れて
		// いるのに、音色の指定だけその場で効かせていたので、順番が入れ替わって
		// いた。曲が「CC91 → 音色の指定」の順で送っていても、こちらでは
		// 音色の指定が先に効き、そのあと CC91 が上書きしてしまう。
		// 実機では音色の指定がパートのつまみを音色の既定値に戻すので、
		// 送りの値が 7 音ぶん違っていた（doc/native-engine.md の 6.53）
		case 4:
			if (e.part >= 0 && e.part < 64) {
				if (e.d0 == 0) {
					m_prog_sel[e.part].msb = e.d1;
				} else if (e.d0 == 1) {
					// **受け付けない LSB は無視**（6.202）。実機は前の
					// バンクのまま鳴らすのに、こちらは LSB 0 の音色へ
					// 落ちていた（`banklsb` の 7 打目）
					if (voice_lsb_ok(m_prog_sel[e.part].msb, e.d1))
						m_prog_sel[e.part].lsb = e.d1;
				} else {
					m_prog_sel[e.part].prog = e.d1;
					// ドラムのパートなら、その組のセットアップが既定に戻る
					m_ndrv.drum_program(e.part);
				}
				native_select_voice(e.part);
			}
			break;
		// XG のパートの設定（08 pp ll）。ワーク RAM の並びと同じなので、
		// 番地をそのまま渡す
		case 5:
			m_ndrv.set_part_param(e.part, e.d0, e.d1);
			// **パートの種類が変わったら音色を引き直す**（6.137）
			if (e.d0 == 0x07 && e.part < 64) {
				m_part_mode[e.part] = s8(e.d1);
				native_select_voice(e.part);
			}
			break;
		// リセット（6.136）。音色の選びもパートの状態も既定に戻す
		case 6:
			for (int p = 0; p < 64; p++) {
				m_prog_sel[p] = part_prog();
				m_part_mode[p] = -1;
				m_prog_seen[p][0] = m_prog_seen[p][1] = m_prog_seen[p][2] = 0xff;
			}
			std::memset(m_nown, 0, sizeof(m_nown));
			std::memset(m_cc_last, 0xff, sizeof(m_cc_last));   // CC の控えも忘れる
			m_ndrv.reset_parts();
			// **音色の記録も引き直す**。m_prog_sel を戻すだけでは、
			// 口が持っている記録（`set_record`）が古いままになる
			for (int p = 0; p < 64; p++)
				native_select_voice(p);
			break;
		// ドラムのセットアップを書いた（3n rr pp）。part に組と並びの番号が詰めてある
		case 7:
			m_ndrv.mark_drum_setup_index(e.part & 3, e.d0, e.part >> 2, e.d1);
			break;
		// ドラムのセットアップのリセット（00 00 7D nn）
		case 8:
			if (e.part < xg::ram::DRUM_SETUP_SETS)
				m_ndrv.clear_drum_setup(e.part);
			break;
		default: break;
		}
	}
}

// MIDI を 1 バイト受けて、native でさばけたら true。
// さばけなかったもの（音色の指定・コントローラ・SysEx）は firmware へ回す
bool mu2000::native_midi(u8 byte, int port)
{
	if (byte >= 0xf8)
		return false;                    // リアルタイムはそのまま
	// 実機は 1 バイトずつ線で受ける。和音のように何音も一度に来ると、
	// あとの音ほど遅れて鳴る。そのぶんをここで数える
	const u64 fire = rx_advance(port);
	nmidi &n = m_nmidi[port];
	if (byte & 0x80) {
		if (byte >= 0xf0) {              // SysEx など。以後は firmware に任せる
			n.status = 0;
			if (byte == 0xf0) {
				// 頭を見て決めるので、まずは短く。0x0b 番目までに分かる
				m_sx_pos = 0;
				m_fw_hold = std::max(m_fw_hold, u32(44100 / 30));
			} else {
				// **F7 で XG のパートの設定を自分にも効かせる**。native の口では
				// firmware を 100ms につき 5ms しか回さないので、firmware が
				// この SysEx を処理し終えるのは 1 秒以上あと。それまで待つと、
				// 曲の頭の何音かが古いつまみの値で鳴る（利用者の曲で、送りを
				// SysEx で 33 にしているのに CC91 の 40 のまま鳴っていた）
				if (byte == 0xf7)
					native_sysex(fire);
				m_sx_pos = -1;
				m_fw_hold = std::max(m_fw_hold, u32(44100 / 30));
			}
			if (m_fw_why != 1)
				m_fw_why = 2;
			return false;
		}
		n.status = byte;
		n.have = 0;
		// アフタータッチ。**割り当て（CAT / PAT）が既定なら音に何も起きない**ので、
		// そのときは firmware に任せなくてよい（doc/native-engine.md の 6.43）
		if ((byte & 0xf0) == 0xd0 || (byte & 0xf0) == 0xa0)
			m_ndrv.aftertouch((byte & 0x0f) + port * 16, (byte & 0xf0) == 0xa0);
		// 鍵の上げ下げ・CC・ベンドはこちらで見る。残り（音色の指定など）は firmware へ
		const u8 kind = byte & 0xf0;
		if (kind == 0xc0)
			return true;                     // 音色の指定は下でバイトを見る
		if (kind != 0x80 && kind != 0x90 && kind != 0xb0 && kind != 0xe0) {
			m_ne_stats.other++;
			// 音色の指定。ワーク RAM に入るのは 30 サンプル（0.68ms）で済むが
			// （nativeplay --ccwatch）、firmware の中の下ごしらえはもっとかかる。
			// 10ms だと piano の残差が -58dB から -54dB に落ちたので 20ms 見る
			m_fw_hold = std::max(m_fw_hold, u32(44100 / 50));
			if (m_fw_why != 1)
				m_fw_why = 2;
			return false;
		}
		return true;                     // 状態のバイトは飲み込む
	}
	// SysEx の中身。エフェクトの種類を変えるものだけ長く回す（MEG のプログラムを
	// 1 万件以上書き直すので、途中で止めると音が出なくなる）。
	// パートの設定（08 pp xx）やドラムの設定は短くてよい
	if (m_sx_pos >= 0) {
		// 長い SysEx（MEG のプログラムなど）の間は待ちを切らさない
		m_fw_hold = std::max(m_fw_hold, u32(44100 / 200));
		if (m_sx_pos < int(sizeof(m_sx)))
			m_sx[m_sx_pos] = byte;
		m_sx_pos++;
		// XG のパラメータチェンジ（43 1n 4C hh mm ll …）かどうかは 3 バイトで分かる。
		// そうならもう 1 バイト（ll）まで待って細かく分ける。そうでないもの
		// （GM システムオンなど）は 5 バイトで決める＝前と同じ
		const bool xg_param = m_sx[0] == 0x43 && (m_sx[1] & 0xf0) == 0x10 && m_sx[2] == 0x4c;
		if (m_sx_pos == (xg_param ? 6 : 5)) {
			// **重いのは「MEG のプログラムを書き直すもの」だけ**。
			// `nativeplay --sxsettle` で SWP30 を触り終わるまでを測った:
			//   00 00 7E XG システムオン       212ms
			//   02 01 00 リバーブの種類        176ms
			//   02 01 20 コーラスの種類        177ms
			//   02 01 40 バリエーションの種類  182ms
			//   03 0n 00 インサーションの種類  182ms
			// 一方、**値を変えるだけ**のものは 0〜4ms で終わる:
			//   00 00 04 マスターボリューム 0ms / 02 01 02 リバーブのパラメータ 3.9ms
			//   03 0n 02 インサーションのパラメータ 3.1ms / 08 pp xx パートの設定 0ms
			// 前はエフェクトとシステムなら何でも 300ms 待っていたので、
			// エフェクトのパラメータを流す曲で SH-2 を無駄に回していた
			const u8 hh = m_sx[3], mm = m_sx[4], ll = m_sx[5];
			bool heavy = !xg_param;
			if (xg_param) {
				if (hh == 0x00 && mm == 0x00 && (ll == 0x7e || ll == 0x7f))
					heavy = true;               // システムオン・全パラメータリセット
				else if (hh == 0x02 && mm == 0x01 &&
				         (ll <= 0x01 || ll == 0x20 || ll == 0x21 || ll == 0x40 || ll == 0x41))
					heavy = true;               // リバーブ・コーラス・バリエーションの種類
				else if (hh == 0x03 && ll <= 0x01)
					heavy = true;               // インサーションの種類
			}
			if (heavy) {
				// 実測の 212ms に余裕を見て 300ms（前は 500ms だった）。
				// **ここを弄ると全体の時間がずれる**（6.211）。
				// 400ms にすると fxchange と drums は良くなるが、
				// bend・pegcc・meter が悪くなる。当たり外れなので動かさない。
				// `SMU2000_FX_HOLD`（ミリ秒）で振れる
				m_fw_hold = std::max(m_fw_hold, fx_hold());
				m_fw_why = 1;
			}
			// ここでは**止めない**。F7 まで受け取って、パートの設定なら
			// 値まで読む（native_sysex）
		}
		return false;
	}

	const u8 kind = n.status & 0xf0;
	// 音色の指定（1 バイト）。自分で記録を引いて、firmware にも渡す
	if (kind == 0xc0) {
		const int part2 = m_ndrv.rcv_part(port, n.status & 0x0f);
		if (part2 < 0) {          // 聞いているパートが無い／2 つ以上（6.150）
			m_ne_stats.other++;
			m_fw_hold = std::max(m_fw_hold, u32(44100 / 50));
			const int save3 = m_native_engine;
			m_native_engine = 0;
			midi_in(n.status, port);
			midi_in(byte, port);
			m_native_engine = save3;
			return true;
		}
		m_nq.push_back({ fire, 4, u8(part2), 2, u8(byte & 0x7f) });
		m_ne_stats.other++;
		// 記録はこちらで引けたが、firmware も自分の下ごしらえに時間が要る
		// （5ms に詰めると piano の残差が -58dB から -53dB に落ちる）
		m_fw_hold = std::max(m_fw_hold, u32(44100 / 50));
		if (m_fw_why != 1)
			m_fw_why = 2;
		const int save2 = m_native_engine;
		m_native_engine = 0;
		midi_in(n.status, port);
		midi_in(byte, port);
		m_native_engine = save2;
		return true;
	}
	// **アフタータッチの値もこちらで覚える**（6.191）。
	// フィルタ側 LFO の深さに乗るので、**鳴っている音に効かせる**
	// 必要がある。バイトは今までどおり firmware へも流す
	if (kind == 0xa0 || kind == 0xd0) {
		const int part4 = m_ndrv.rcv_part(port, n.status & 0x0f);
		if (kind == 0xd0) {
			if (part4 >= 0)
				m_ndrv.chan_press(part4, byte & 0x7f);
		} else if (n.have == 0) {
			n.d0 = byte;
			n.have = 1;
		} else {
			n.have = 0;
			if (part4 >= 0)
				m_ndrv.poly_at(part4, n.d0 & 0x7f, byte & 0x7f);
		}
		return false;
	}
	if (kind != 0x80 && kind != 0x90 && kind != 0xb0 && kind != 0xe0)
		return false;
	if (n.have == 0) {
		n.d0 = byte;
		n.have = 1;
		return true;
	}
	n.have = 0;
	if (kind == 0x90 && (byte & 0x7f))
		led_blink(fire);                  // MU の灯（leds）
	// **受信チャンネル**（08 pp 04。6.150）。既定はパート = チャンネル + 口 x 16
	// だが、曲が付け替えることがある。聞いているパートが無いときは実機も
	// 黙るので何もせず、2 つ以上のときは重ねて鳴るので firmware に任せる
	const int part = m_ndrv.rcv_part(port, n.status & 0x0f);
	if (part < 0) {
		m_ne_stats.other++;
		if (part == -2)
			m_fw_hold = std::max(m_fw_hold, u32(44100 / 50));
		replay_note(n.status, n.d0, byte, port);
		return true;
	}

	// ピッチベンドは、音程のレジスタを自分で作れるので firmware には渡さない。
	// ただし、そのパートで firmware が鳴らしている音がある間は渡す
	if (kind == 0xe0) {
		m_nq.push_back({ fire, 3, u8(part), u8(n.d0 & 0x7f), u8(byte & 0x7f) });
		if (m_fw_notes[part]) {
			m_fw_hold = std::max(m_fw_hold, u32(44100 / 200));
			replay_note(n.status, n.d0, byte, port);
		}
		return true;
	}
	// コントローラ。音量・表現・パン・ダンパーは自分でさばく。
	// それでも firmware には渡す（写し取りのとき同じ位置で鳴らしてほしい）が、
	// 回す時間は短くてよい
	if (kind == 0xb0) {
		m_ne_stats.other++;
		const int cc = n.d0 & 0x7f;
		// **値の変わらない CC は firmware を起こし直さない**。
		// 受けるたびに意味が変わるもの（データ入力・RPN/NRPN の指定・増減・
		// チャンネルモード）は対象にしない。バイトは下でいつもどおり流すので、
		// 線の時間も firmware の状態も変わらない
		const bool cc_stateful = cc == 6 || cc == 38 || (cc >= 96 && cc <= 101) || cc >= 120;
		bool cc_same = false;
		if (!cc_stateful) {
			cc_same = m_cc_last[part][cc] == (byte & 0x7f);
			m_cc_last[part][cc] = u8(byte & 0x7f);
		}
		if (cc == 0x00) m_nq.push_back({ fire, 4, u8(part), 0, u8(byte & 0x7f) });
		if (cc == 0x20) m_nq.push_back({ fire, 4, u8(part), 1, u8(byte & 0x7f) });
		const bool mine = m_ndrv.handles_cc(n.d0 & 0x7f);
		if (mine)
			m_nq.push_back({ fire, 2, u8(part), u8(n.d0 & 0x7f), u8(byte & 0x7f) });
		else
			m_ndrv.control(part, n.d0 & 0x7f, byte & 0x7f);   // 音を全部切るなどは待たない
		// こちらでさばける CC（音量・パン・ダンパー）は firmware に渡すだけなので短く。
		// 知らない CC は firmware がすべてやるので、処理が終わるまで見る
		// （5ms に詰めたら bend の残差が -35.8dB から -14dB に落ちた）
		// こちらでさばける CC は渡すだけなので短く。ただし **そのパートを
		// firmware が鳴らしている間**は、firmware に効かせてもらうので長く見る
		// （渡したバイトは列に並ぶので、写し取りで回すときに順に処理される）
		// まだ写し取っていないパートは、1 音目を firmware が鳴らすので、
		// CC も firmware に効かせてもらう
		const bool quick = mine && !m_fw_notes[part] && m_ndrv.part_learned(part);
		if (!cc_same)
			m_fw_hold = std::max(m_fw_hold, u32(quick ? 44100 / 500 : 44100 / 50));
		replay_note(n.status, n.d0, byte, port);
		return true;
	}
	const int note = n.d0 & 0x7f, vel = byte & 0x7f;
	if (kind == 0x80 || vel == 0) {
		// **離しは押しと処理時間が違う**（SMU2000_OFF_PROC）。実機は
		// ドラムの離しを最後のバイトの次のサンプルで書いていた
		const u64 fire_off = fire - native_proc64() / 64 + off_proc64() / 64;
		if (nown(part, note)) {
			nown_set(part, note, false);
			m_nq.push_back({ fire_off, 0, u8(part), u8(note), u8(vel) });
			return true;
		}
		// native で鳴っていない音は firmware に任せる
		if (m_fw_notes[part]) {
			m_fw_notes[part]--;
			if (m_fw_note_total)
				m_fw_note_total--;
		}
		m_fw_hold = std::max(m_fw_hold, u32(44100 / 50));    // 離しの下ごしらえまで
		replay_note(n.status, u8(note), u8(vel), port);
		return true;
	}
	// **鍵の範囲の外は鳴らさない**（08 pp 0F/10）。実機も鳴らさないので、
	// firmware には渡すだけにして、こちらでは 1 音も出さない。
	// **押しを受けないドラム**（3n rr 0A = 0）も同じ（6.151）
	if (!m_ndrv.note_in_range(part, note)
	    || (m_ndrv.is_drum(part) && !m_ndrv.drum_rcv_note_on(part, note))) {
		m_ne_stats.other++;
		replay_note(n.status, u8(note), u8(vel), port);
		return true;
	}
	if (m_ndrv.can_play(part, note)) {
		nown_set(part, note, true);
		// **ドラムは実機のほうが 3 サンプル早い**（6.139）。旋律は +1 で
		// 合っているのに、打楽器だけ +3 になる。1 打を引く道が短いためと
		// 見ている（`SMU2000_DRUM_LEAD` で振れる）。打楽器は立ち上がりが
		// 鋭いので、2 サンプルでも波形の相関がはっきり変わる
		const u64 at = (part_is_drum(part) && fire > drum_lead())
		             ? fire - drum_lead() : fire;
		m_nq.push_back({ at, 1, u8(part), u8(note), u8(vel) });
		return true;
	}
	m_ne_stats.note_fw++;
	// firmware が鳴らす音でも、最後に押した鍵は覚えておく
	// （つぎの音のポルタメントの出発点になる）
	m_ndrv.note_fw(part, note);
	// まだ写し取っていない音（ドラムは音ごと）。firmware に鳴らさせて覚える
	const u32 rec = m_ndrv.record_of(part);
	const bool drum = m_ndrv.is_drum(part);
	// 知らない CC で firmware に任せているパートは、写し取っても使わない。
	// **前の音色のフィルタの動きを録っている間は始めない**（m_traj_rec）。
	// 写し取りも録りも SWP30 の覗き口（set_swp_watch）を 1 つしか持てないので、
	// 新しい写し取りを始めると前の録りがそこで切れる。実測では、曲の頭で
	// 3 パートがほぼ同時に鳴り出すと、最後の 1 つ以外は **4 段（139ms）**で
	// 切れていた（実機は 110 段・1.1 秒かけてフィルタを閉じる）。
	// 録り終わるまで待つぶん、その音色が native になるのは遅れるが、
	// その間は firmware が鳴らすので音は正しい
	// **パートモード「DRUM」（番号なし）の打は写し取らない**。ドラムセットアップの
	// 編集が効かないキットの既定値で鳴るので、それを覚えると、同じ鍵を DRUMS1-4 の
	// パートで鳴らしたときに編集が効かなくなる（覚えた値は鍵で引くため）
	const bool plain_drum = part >= 0 && part < 64 &&
	                        size_t(xg::ram::part_base(part) + 0x07) < m_ram.size() &&
	                        m_ram[xg::ram::part_base(part) + 0x07] == 1;
	if ((rec || drum) && !m_learning && !m_ndrv.delegated(part) && !plain_drum) {
		m_learn_note = note;
		m_learn_vel = vel;
		m_learn_drum = drum ? m_ndrv.drum_key(part, note) : 0;
		m_learn_part = part;
		m_ne_stats.learn++;
		native_learn_start(rec);
		if (m_fw_why != 1)
			m_fw_why = 3;
	}
	m_fw_hold = std::max(m_fw_hold, u32(44100 / 20));
	if (m_fw_notes[part] < 255) {
		m_fw_notes[part]++;
		m_fw_note_total++;
	}
	m_fw_note_until = m_ne_clock + FW_NOTE_RUN;
	// **液晶のメーター用の目盛り**（6.188）
	if (part < 16) {
		m_fw_meter[part] = u8(m_ndrv.part_meter(part, vel));
		m_fw_meter_at[part] = m_ne_clock;
	}
	replay_note(n.status, u8(note), u8(vel), port);
	return true;
}

// 飲み込んだバイトを firmware へ流し直す。
// **回す時間も作る**。渡しただけでは、CPU を止めたままなので誰も読まない
void mu2000::replay_note(u8 status, u8 d0, u8 d1, int port)
{
	const int save = m_native_engine;
	m_native_engine = 0;                 // 二重に読まない
	midi_in(status, port);
	midi_in(d0, port);
	midi_in(d1, port);
	m_native_engine = save;
	// **待ちはここでは置かない。** 以前はここで一律 20ms 回していて、
	// CC を 1 つ渡すたびに 20ms 走らせることになっていた（曲の出だしの重さの正体）。
	// 要るぶんは呼ぶ側が置く
}

// S-MU2000: 軽量モードの入り切り（doc/native-dsp.md）
void mu2000::set_native_fx(int mode)
{
	m_nfx_on = mode;
	// 遅延の線は作り直さない（音声の糸が読んでいる最中に切り替えても危なくないように）。
	// 大きさは 44100Hz ぶんで固定なので、1 度用意すれば足りる。
	// **台ごとに持つ**。前は関数の static で、DAW に 2 枚目を挿すと
	// 2 台目の遅延の線が空のままになって落ちていた
	// 非正規化数（0 に近すぎる値）を 0 に丸める設定は、**ここでは触らない**。
	// 糸ごとの設定なので、音声の糸が入れ替わると消えてしまうし、
	// 音源として挿されている側が host の糸の設定を変えたままにするのも行儀が悪い。
	// 1 ブロックごとに smu2000::denormals_off を置く（compat/platform.h）
	if (!m_nfx_ready) {
		m_nfx.set_rate(44100.0f);
		m_nfx_ready = true;
	}
	m_nfx.reset();
	int mask = 15;
	if (const char *e = std::getenv("SMU2000_NATIVE_SLOTS"))
		mask = std::atoi(e);
	m_swpm.set_native_fx(mode ? &m_nfx : nullptr, mode >= 2, mask);
	if (mode)
		native_fx_update();
}

namespace {

// XG の番地から、ワーク RAM の値を読む（7bit ずつ。無ければ -1）
int xg_read(const std::vector<u8> &ram, int hi, int mid, int lo, int size)
{
	int v = 0;
	for (int i = 0; i < size; i++) {
		u32 off = 0;
		if (!xg::ram::locate(u32(hi << 14 | mid << 7 | (lo + i)), off) || off >= ram.size())
			return -1;
		v = (v << 7) | (ram[off] & 0x7f);
	}
	return v;
}

// インサーション n のパラメータ 1-10 が 2 バイトの種類のときの値（16bit がそのまま並ぶ）
int ins_wide(const std::vector<u8> &ram, int n, int addr)
{
	// 2 バイトのパラメータは 0x30, 0x32, ... と 2 番地ずつ使い、RAM にも 2 バイトずつ並ぶ。
	// つまり RAM での位置は「番地の差」そのもの（前は 2 倍していて 1 つおきに読んでいた）
	const u32 off = xg::ram::INS_BLOCK[n] + xg::ram::INS_WIDE + u32(addr - 0x30);
	if (off + 1 >= ram.size())
		return -1;
	return ram[off] << 8 | ram[off + 1];
}

// バリエーションのパラメータ 1-10（02 01 42-55）。塊の +0x02 から 16bit の数が 10 個並ぶ
// （xg::ram::VAR_WIDE）。7bit ずつの番地の表（locate）には無いので、xg_read では読めない
int var_wide(const std::vector<u8> &ram, int index)
{
	const u32 off = xg::ram::VAR_BLOCK + xg::ram::VAR_WIDE + u32(index) * 2;
	if (off + 1 >= ram.size())
		return -1;
	return ram[off] << 8 | ram[off + 1];
}

} // namespace

// RAM に入っている XG の設定を読んで、C++ のエフェクトに渡す。
// 音を作る糸から 512 サンプルごとに呼ぶ（設定はそんなに速く変わらない）
void mu2000::native_fx_update()
{
	using nfx = smu2000::dsp::native_fx;
	const std::vector<u8> &ram = m_ram;
	if (ram.size() < 0x30000)
		return;

	struct slot_def { nfx::slot_id id; int hi, mid, base, ret_lo, ins; };
	static const slot_def SLOTS[] = {
		{ nfx::REVERB,    0x02, 0x01, 0x00, 0x0c, -1 },
		{ nfx::CHORUS,    0x02, 0x01, 0x20, 0x2c, -1 },
		{ nfx::VARIATION, 0x02, 0x01, 0x40, 0x56, -1 },
		{ nfx::INS1,      0x03, 0x00, 0x00, -1,    0 },
	};

	// パラメータの並びは、置き場ごとに違う（表の addr はインサーションの番地）。
	//   リバーブ・コーラス … 1-10 は base+02〜0B の 1 バイト、11-16 は base+10〜15
	//   バリエーション     … 1-10 は 02 01 42 から 2 バイトずつ、11-16 は 02 01 70〜75
	//   インサーション     … 表の番地そのまま（2 バイトのものは +0x18 に 16bit で並ぶ）
	auto read_param = [&](const slot_def &s, const xg::fx_param &p, int index) {
		if (s.ins >= 0)
			return p.addr >= 0x30 ? ins_wide(ram, s.ins, p.addr)
			                      : xg_read(ram, s.hi, s.mid, s.base + p.addr, p.size);
		if (s.id == nfx::VARIATION) {
			// **1-10 は RAM に 16bit の数で並ぶ**（issue #3）。xg_read で 02 01 42 を引いていたが、
			// その番地は RAM の表に無いので -1 になり、どのパラメータも下限（ディレイ 0.1ms など）で
			// 鳴っていた。Children.mid のピアノのディレイが native fx で消えていた
			if (p.addr >= 0x30 || index < 10)
				return var_wide(ram, index);
			return xg_read(ram, s.hi, s.mid, 0x70 + (p.addr - 0x20), 1);
		}
		if (p.addr >= 0x20)
			return xg_read(ram, s.hi, s.mid, s.base + 0x10 + (p.addr - 0x20), 1);
		return xg_read(ram, s.hi, s.mid, s.base + p.addr, p.size);
	};

	for (const slot_def &s : SLOTS) {
		const int type = xg_read(ram, s.hi, s.mid, s.base, 2);
		if (type < 0)
			continue;
		const xg::fx_def *def = xg::fx_find(type);
		int raw[16] = {};
		const int n = def ? std::min(def->count, 16) : 0;
		for (int i = 0; i < n; i++) {
			const xg::fx_param &p = def->params[i];
			const int v = read_param(s, p, i);
			raw[i] = v < 0 ? int(p.lo) : v;
		}
		m_nfx.set(s.id, type, raw, n);
		// 調べもの用: SMU2000_NATIVE_FX_DEBUG=1 で、読んだ値を出す
		static const bool dbg = std::getenv("SMU2000_NATIVE_FX_DEBUG") != nullptr;
		if (dbg) {
			std::printf("nfx slot %d type %02x %02x kind %d:", int(s.id), type >> 7, type & 0x7f,
			            int(m_nfx.slot(s.id).current()));
			for (int i = 0; i < n; i++)
				std::printf(" %s=%d", def->params[i].label, raw[i]);
			std::putchar(10);
		}

		// 戻り量。XG の 64 を基準にする（送りに対する量で、実機の中身とは別物）
		if (s.ret_lo >= 0) {
			const int ret = xg_read(ram, s.hi, s.mid, s.ret_lo, 1);
			// 戻り量の基準。実機の混ざり具合に合わせた実測の値（SMU2000_NATIVE_RETURN で変えられる）
			static const float base = [] {
				const char *e = std::getenv("SMU2000_NATIVE_RETURN");
				return e ? float(std::atof(e)) : 0.8f;
			}();
			float g = ret < 0 ? base : base * float(ret) / 64.0f;
			// バリエーションを INSERTION でパートに掛けているときは、送りの目盛りが
			// インサーションと同じになる（戻り量は使われない）
			if (s.id == nfx::VARIATION && xg_read(ram, 0x02, 0x01, 0x5a, 1) == 0)
				g = 0.31f;
			m_nfx.set_return(s.id, g);
		}
	}

	// マスター EQ（02 40 00-14）
	{
		int gain[5], freq[5], q[5];
		static const int G[5] = { 0x01, 0x05, 0x09, 0x0d, 0x11 };
		bool ok = true;
		for (int i = 0; i < 5; i++) {
			gain[i] = xg_read(ram, 0x02, 0x40, G[i], 1);
			freq[i] = xg_read(ram, 0x02, 0x40, G[i] + 1, 1);
			q[i]    = xg_read(ram, 0x02, 0x40, G[i] + 2, 1);
			if (gain[i] < 0 || freq[i] < 0 || q[i] < 0)
				ok = false;
		}
		const int shape1 = xg_read(ram, 0x02, 0x40, 0x04, 1);
		const int shape5 = xg_read(ram, 0x02, 0x40, 0x14, 1);
		if (ok)
			m_nfx.meq().set_raw(gain, freq, q, shape1 < 0 ? 0 : shape1, shape5 < 0 ? 0 : shape5);
	}
}

void mu2000::set_external_audio(ext_bus bus, float left, float right)
{
	// どの SWP30 の、MEG のどの入口か（左。右はその次）
	struct where { bool slave; int slot; };
	static constexpr where WHERE[int(ext_bus::count)] = {
		{ false, 0x0 }, { false, 0x4 }, { false, 0x6 }, { false, 0xc }, { false, 0x8 },
		{ true, 0x8 }, { true, 0xa }, { true, 0xc },
	};
	if (int(bus) < 0 || bus >= ext_bus::count)
		return;
	const where w = WHERE[int(bus)];
	swp30_device &d = w.slave ? m_swps : m_swpm;
	// 外の音は 1.0 を超えることがある（エフェクトの手前なので、少しの余裕は持たせる）
	auto conv = [](float v) {
		return std::isfinite(v) ? s32(std::lround(std::clamp(v, -8.0f, 8.0f) * float(EXT_BUS_SCALE))) : 0;
	};
	d.m_ext_bus[size_t(w.slot)] = conv(left);
	d.m_ext_bus[size_t(w.slot) + 1] = conv(right);
	d.m_ext_on = true;
}

void mu2000::clear_external_audio()
{
	for (swp30_device *d : { &m_swpm, &m_swps }) {
		d->m_ext_bus.fill(0);
		d->m_ext_on = false;
	}
}

// ---- PLG ボードの側をこちらで演じる

void mu2000::set_plg_tx(plg_tx_fn fn)
{
	m_plg_user = std::move(fn);
}

// firmware が SCI4 へ書いた 1 バイト。ボードへ行くのは chan 3（4 本に分かれる線。行き先の印の下 3 ビットが PLG1-3）
void mu2000::plg_tx_byte(int chan, u8 targets, u8 byte)
{
	if (m_plg_user)
		m_plg_user(chan == 3 ? targets & 7 : 0x10 << chan, byte);      // 別々の 3 本の線（chan 0-2）は 0x10・0x20・0x40
	if (!m_vb_any)
		return;
	// chan 0 は、本体が自分で作った演奏をボードへ聞かせる線（[AUDITION] の音符など。外から来た MIDI はここには
	// 出てこない。実機ではコネクターの手前で MIDI IN A と合わさってボードに届くと思われる）。口 A の MIDI として聞く
	if (chan == 0) {
		for (vb_slot &vs : m_vbs)
			if (vs.single())
				vb_tap(vs, byte, 0);
		return;
	}
	// 架空のボードは PLG1〜3 に挿さっている。宛て先の印（下 3 ビット）が立っている差込口ごとに、
	// SysEx を 1 つずつ組み立てて読む。増設の差込口（PLG-4〜）に firmware は話しかけてこないが、3 枚全部に宛てたもの
	// （パネルで音色を変えたときの 4C 08 pp 01〜03、XG System On）は同じ線に乗っているものとして聞く。答えは返さない
	if (chan != 3 || byte >= 0xf8)
		return;
	for (vb_slot &vs : m_vbs) {
		if (!vs.kind || !(vs.extra() ? (targets & 7) == 7 : (targets >> vs.index) & 1))
			continue;
		if (byte == 0xf0)
			vs.msg.clear();
		else if (vs.msg.empty())
			continue;
		vs.msg.push_back(byte);
		if (byte == 0xf7) {
			vb_from_firmware(vs, vs.msg);
			vs.msg.clear();
		} else if (vs.msg.size() > 64) {
			vs.msg.clear();
		}
	}
}

void mu2000::plg_reply(int slot, const std::vector<u8> &bytes)
{
	if (slot < 0 || slot > 2)
		return;
	m_plg_rx[slot].insert(m_plg_rx[slot].end(), bytes.begin(), bytes.end());
}

// 積んであるバイトを 1 つ届ける。31250bps の 1 バイト（320 マイクロ秒）= 14 サンプルごとに、本体が聞いているスロットから
void mu2000::plg_pump()
{
	if (m_plg_rx[0].empty() && m_plg_rx[1].empty() && m_plg_rx[2].empty())
		return;
	if (++m_plg_tick < 14 || !m_sci4->rx_ready(3))
		return;
	const u8 listen = m_sci4->targets() >> 4;
	for (int s = 0; s < 3; s++)
		if (((listen >> s) & 1) && !m_plg_rx[s].empty()) {
			m_sci4->rx_inject(3, m_plg_rx[s].front());
			m_plg_rx[s].pop_front();
			m_plg_tick = 0;
			return;
		}
}

// ---- 架空のプラグインボード（src/vboard.h）

void mu2000::set_virtual_board(int kind, int part, int slot)
{
	if (slot < 0 || slot >= PLG_SLOTS)
		return;
	vb_slot &s = m_vbs[size_t(slot)];
	kind = kind >= VBOARD_FC && kind < VBOARD_KINDS ? kind : VBOARD_NONE;
	// 増設の差込口（PLG-4〜）には 1 パートのボードだけ（マルチパートのボードは firmware に口 E を割り当ててもらう）
	if (board_slot_extra(slot) && board_is_multi(kind))
		kind = VBOARD_NONE;
	part = std::clamp(part, 0, 63);
	if (kind == s.kind && part == s.part && s.on)
		return;
	// マルチパートのボードは 1 枚だけ（口 E は 1 つ）。ほかの差込口に挿さっていたら外す
	if (board_is_multi(kind))
		for (vb_slot &o : m_vbs)
			if (&o != &s && board_is_multi(o.kind))
				o.kind = VBOARD_NONE;
	const bool multi_changed = board_is_multi(kind) || board_is_multi(s.kind);
	s.reset_voices();
	if (multi_changed)
		vb16_reset(true);
	for (vb_parse &p : s.parse)
		p = vb_parse();
	const bool moved = part != s.part || !s.on;
	s.kind = kind;
	s.part = part;
	s.on = true;
	s.tick = 0;
	vb_bank_from_ram(s);
	m_vb_any = false;
	for (const vb_slot &o : m_vbs)
		m_vb_any |= o.kind != VBOARD_NONE;
	if (m_vb_live) {
		clear_external_audio();
		m_vb_live = false;
	}
	// firmware がボードを知っているなら、メニューの PartAssign も同じ値にする。
	// XG の「プラグインボードのパートの割り当て」（4C 70 xx 00。xx は差込口ごとの番地）を MIDI で送ると firmware は
	// 控えを書き換える。演奏の途中のメッセージに割り込みにくいよう、口 B から入れる（C・D は HOST SELECT が USB の
	// ときしか受けない）
	if (s.single() && s.known && moved && part < 16) {
		const u8 msg[] = { 0xf0, 0x43, 0x10, 0x4c, 0x70, s.assign_mid(), 0x00, u8(part), 0xf7 };
		for (u8 x : msg)
			midi_in(x, 1);
	}
}

// PartAssign の値（0-15、0x7f = off）。firmware のメニューか、MIDI の XG メッセージから来る
void mu2000::vb_assign(vb_slot &s, u8 value)
{
	const bool on = value < 16;
	if (on == s.on && (!on || value == s.part))
		return;
	s.reset_voices();
	s.on = on;
	if (on)
		s.part = value;
	s.tick = 0;
	vb_bank_from_ram(s);
}

// そのパートでいま選ばれているバンク（XG 08 pp 01・02）。パートを決め直したときに読む
void mu2000::vb_bank_from_ram(vb_slot &s)
{
	const u32 pb = xg::ram::part_base(s.part);
	s.bank[0] = s.bank_next[0] = m_ram[pb + 0x01];
	s.bank[1] = s.bank_next[1] = m_ram[pb + 0x02];
	s.ram_seen[0] = m_ram[pb + 0x01];
	s.ram_seen[1] = m_ram[pb + 0x02];
	s.ram_seen[2] = m_ram[pb + 0x03];
}

// firmware が知らないボード（増設の差込口、起動のあとで挿したボード）は、パネルや SysEx で音色を変えても何も知らせてもらえない。
// ワーク RAM のバンクとプログラムが**変わったとき**だけ、それに合わせる（MIDI のバンクセレクトは vb_tap が先に追っているので、
// RAM が後から同じ値になっても何も起きない。RAM が古い間にボードを戻してしまうこともない）
void mu2000::vb_follow_ram(vb_slot &s)
{
	const u32 pb = xg::ram::part_base(s.part);
	const u8 now[3] = { m_ram[pb + 0x01], m_ram[pb + 0x02], m_ram[pb + 0x03] };
	if (now[0] != s.ram_seen[0] || now[1] != s.ram_seen[1])
		vb_set_bank(s, now[0], now[1]);
	if (now[2] != s.ram_seen[2] && now[2] < 128)
		s.program(now[2]);
	std::copy(now, now + 3, s.ram_seen);
}

// バンクが変わった。ボードのバンクから外れたら、鳴っている音を止める
void mu2000::vb_set_bank(vb_slot &s, u8 msb, u8 lsb)
{
	const bool was = s.active();
	s.bank[0] = s.bank_next[0] = msb;
	s.bank[1] = s.bank_next[1] = lsb;
	if (was && !s.active())
		s.reset_voices();
}

// firmware から PLG1 のボードへ来た SysEx に答える。形は doc/plg-protocol.md
void mu2000::vb_from_firmware(vb_slot &s, const std::vector<u8> &m)
{
	if (m.size() < 8 || m[1] != 0x43)
		return;
	const u8 part = s.on && s.part < 16 ? u8(s.part) : 0x7f;
	const auto reply = [this, &s](std::vector<u8> r) {
		r.push_back(0xf7);
		plg_reply(s.index, r);
	};
	// 機種 4E: ボードの素性
	if ((m[2] & 0xf0) == 0x30 && m[3] == 0x4e && m.size() == 8) {
		const u32 addr = u32(m[4]) << 16 | u32(m[5]) << 8 | m[6];
		std::vector<u8> r = { 0xf0, 0x43, u8(0x10 | (m[2] & 15)), 0x4e, m[4], m[5], m[6] };
		switch (addr) {
		case 0x011000:                     // 種類（0 = 1 パートのボード、1 = マルチパートのボード）・番号・?
			r.insert(r.end(), { u8(board_is_multi(s.kind) ? 0x01 : 0x00), 0x01, 0x00 });
			s.known = true;
			break;
		case 0x010000: {                   // 名前 14 文字
			char name[15] = "FC BOARD      ";
			if (s.kind == VBOARD_USER || s.kind == VBOARD_USER16)
				std::snprintf(name, sizeof(name), "%-14.14s", m_vb_user_board ? m_vb_user_board->name : "MY BOARD");
			else if (s.kind != VBOARD_FC)
				std::snprintf(name, sizeof(name), "%-14.14s", s.kind == VBOARD_DLS ? "DLS BOARD" : s.kind == VBOARD_FM16 ? "FM BOARD" : "FC16 BOARD");
			r.insert(r.end(), name, name + 14);
			break;
		}
		case 0x010010:                     // 48 バイト（意味は分かっていない。0 で通る）
			r.insert(r.end(), 48, 0x00);
			break;
		case 0x011010:                     // SYS のパラメーターの数
		case 0x011011:                     // PART のパラメーターの数（0 だと firmware が起動の途中で止まる）
		case 0x011013:                     // バンクの数
			r.push_back(0x01);
			break;
		default:                           // 残りは 1 バイトずつ
			r.push_back(0x00);
			break;
		}
		reply(std::move(r));
		return;
	}
	// 機種 4F: バンクの表（MSB・LSB と、「どのプログラムに音色があるか」の 128 ビットを 4 ビットずつ。
	// プログラム 0 が 1 バイト目の最上位ビット）。ビットが 0 のプログラムを選ぶと、本体は名前を聞かずに Silence と出す。
	// FC ボードはどのプログラムでも鳴る（16 個のくり返し）ので、全部 1。オリジナルのボードも全部 1 にしておく
	// （この表は起動のときにしか聞かれないので、後から波形を足した番号が Silence のままにならないように。
	// 空いている番号の名前は "--------"）
	if ((m[2] & 0xf0) == 0x30 && m[3] == 0x4f && m.size() == 9 && m[4] == 0x7f && m[5] == 0x10 && m[6] == 0x01 && m[7] == 0x00) {
		std::vector<u8> r = { 0xf0, 0x43, u8(0x10 | (m[2] & 15)), 0x4f, 0x7f, 0x10, 0x01, s.bank_msb(), VBOARD_BANK_LSB };
		r.insert(r.end(), 32, 0x0f);
		reply(std::move(r));
		return;
	}
	// 機種 4F: パネルでボードの音色を選ぶと、名前を聞く前に 1 度来る（7F 10 02 <MSB>）。中身は分かっていない。0 で通る
	if ((m[2] & 0xf0) == 0x30 && m[3] == 0x4f && m.size() == 9 && m[4] == 0x7f && m[5] == 0x10 && m[6] == 0x02) {
		std::vector<u8> r = { 0xf0, 0x43, u8(0x10 | (m[2] & 15)), 0x4f, 0x7f, 0x10, 0x02 };
		r.insert(r.end(), 48, 0x00);
		reply(std::move(r));
		return;
	}
	// 機種 4F: 音色の名前（7F 10 00 <MSB> <LSB> <プログラム> 08）。答えは文字数と 8 文字。液晶の音色名の所に出る
	if ((m[2] & 0xf0) == 0x30 && m[3] == 0x4f && m.size() == 12 && m[4] == 0x7f && m[5] == 0x10 && m[6] == 0x00) {
		std::vector<u8> r = { 0xf0, 0x43, u8(0x10 | (m[2] & 15)), 0x4f, 0x7f, 0x10, 0x00, 0x08 };
		const char *name = s.kind == VBOARD_USER ? s.user.program_name(m[9]) : s.fc.name(m[9]);
		r.insert(r.end(), name, name + 8);
		reply(std::move(r));
		return;
	}
	// 機種 4F: パラメーターの表。番地 4 バイトと大きさ
	if ((m[2] & 0xf0) == 0x30 && m[3] == 0x4f && m.size() == 9 && m[4] == 0x7f && m[5] == 0x00 && m[7] == 0x00) {
		std::vector<u8> r = { 0xf0, 0x43, u8(0x10 | (m[2] & 15)), 0x4f, 0x7f, 0x00, m[6] };
		if (m[6] == 0x00)
			r.insert(r.end(), { 0x4c, 0x70, s.assign_mid(), 0x00, 0x01 });      // SYS: PartAssign（番地は差込口ごと）
		else if (m[6] == 0x01)
			r.insert(r.end(), { 0x4c, 0x08, 0x00, 0x07, 0x01 });      // PART: 1 つは要る。パートのモードを挙げておく
		else
			return;
		reply(std::move(r));
		return;
	}
	// いまの値を聞かれた: F0 43 40 03 <番地 4> 40 <大きさ> 00 F7
	if (m[2] == 0x40 && m[3] == 0x03 && m.size() == 12) {
		const bool assign = m[4] == 0x4c && m[5] == 0x70;
		reply({ 0xf0, 0x43, 0x40, 0x40, m[4], m[5], m[6], m[7], 0x01, assign ? part : u8(0x00) });
		return;
	}
	if ((m[2] & 0xf0) != 0x10 || m[3] != 0x4c || m.size() != 9)
		return;
	// メニューで PartAssign が変わった: F0 43 1n 4C 70 00 00 <パート> F7
	if (m[4] == 0x70 && m[5] == s.assign_mid() && m[6] == 0x00)
		vb_assign(s, m[7]);
	// パネルでそのパートの音色が変わった: 4C 08 pp 01（MSB）・02（LSB）・03（プログラム）が続けて来る
	else if (m[4] == 0x08 && m[5] == s.part && s.part < 16 && m[6] == 0x01)
		vb_set_bank(s, m[7], s.bank[1]);
	else if (m[4] == 0x08 && m[5] == s.part && s.part < 16 && m[6] == 0x02)
		vb_set_bank(s, s.bank[0], m[7]);
	else if (m[4] == 0x08 && m[5] == s.part && s.part < 16 && m[6] == 0x03)
		s.program(m[7]);
	// XG System On。firmware は起動の終わりにこれをボードへ送るが、パートの音色は電源を切る前のものを
	// 持ち越していて、それをボードへは知らせてこない（起動し直した直後、液晶はボードの音色なのに
	// ボードが鳴らなかった）。少し待ってから、firmware が持っているバンクに合わせる
	else if (m[4] == 0x00 && m[5] == 0x00 && m[6] == 0x7e) {
		vb_set_bank(s, 0, 0);
		s.resync = 22050;
		if (board_is_multi(s.kind))
			vb16_reset(false);
	}
}

// 入ってきた MIDI を 1 バイトずつ。チャンネルメッセージが揃ったら、ボードのパートのものだけを渡す
void mu2000::vb_tap(vb_slot &s, u8 byte, int port)
{
	if (byte >= 0xf8 || port < 0 || port >= MIDI_PORTS)
		return;
	vb_parse &p = s.parse[port];
	// SysEx は XG のパートの割り当て（F0 43 1n 4C 70 00 00 pp F7）と、バンクを 0 に戻すリセット（XG System On・GM System On）
	// だけ読む。firmware は MIDI で来たこれらをボードへは回してこない
	std::vector<u8> &sx = s.sx[port];
	if (byte == 0xf0) {
		sx.assign(1, byte);
	} else if (!sx.empty()) {
		if (byte == 0xf7) {
			const bool xg = sx.size() == 8 && sx[1] == 0x43 && (sx[2] & 0xf0) == 0x10 && sx[3] == 0x4c;
			if (xg && sx[4] == 0x70 && sx[5] == s.assign_mid() && sx[6] == 0x00)
				vb_assign(s, sx[7]);
			else if ((xg && sx[4] == 0x00 && sx[5] == 0x00 && sx[6] == 0x7e) ||
			         (sx.size() == 5 && sx[1] == 0x7e && sx[3] == 0x09 && sx[4] == 0x01)) {
				vb_set_bank(s, 0, 0);
				s.fc.clear_edits();
			}
			sx.clear();
		} else if ((byte & 0x80) || sx.size() >= 8) {
			sx.clear();
		} else {
			sx.push_back(byte);
		}
	}
	if (byte & 0x80) {
		p.status = byte < 0xf0 ? byte : 0;         // SysEx などの間は聞かない
		p.n = 0;
		return;
	}
	if (!p.status)
		return;
	p.d[p.n++] = byte;
	const int need = (p.status & 0xe0) == 0xc0 ? 1 : 2;       // プログラムチェンジとチャンネルプレッシャーは 1 バイト
	if (p.n < need)
		return;
	p.n = 0;
	if (!s.on || port != s.part / 16)
		return;
	// そのパートの受信チャンネル（XG 08 pp 04。0-15、0x7f は受けない）
	const u8 rcv = m_ram[xg::ram::part_base(s.part) + 0x04];
	if ((p.status & 15) != rcv)
		return;
	// バンクはボードが自分で追う（firmware がワーク RAM に書くのを待つと、同じ時刻に来た音符に間に合わない）
	const u8 kind = p.status & 0xf0;
	if (kind == 0xb0 && p.d[0] == 0) {
		s.bank_next[0] = p.d[1];
		return;
	}
	if (kind == 0xb0 && p.d[0] == 32) {
		s.bank_next[1] = p.d[1];
		return;
	}
	if (kind == 0xc0)
		vb_set_bank(s, s.bank_next[0], s.bank_next[1]);
	// ボードのバンクでないときは鳴らさない（内蔵の音が鳴る）。ほかのメッセージは聞いておく
	if (kind == 0x90 && p.d[1] && !s.active())
		return;
	s.midi(p.status, p.d[0], need == 2 ? p.d[1] : 0);
}

// ---- 16 パートのボード（VBOARD_FC16）。口 E の 16 チャンネルを自分で受け持つ

// ミキサーの値を XG の初期値に（voices なら鳴っている音も止め、プログラムも 0 に戻す）
void mu2000::vb16_reset(bool voices)
{
	for (vb_chan &c : m_vb16) {
		if (voices)
			c.fc.reset();
		c.vol = 100;
		c.exp = 127;
		c.pan = 64;
		c.rev = 40;
		c.cho = 0;
		c.var = 0;
		c.mod = c.hold = 0;
		c.bend = 0;
		if (voices) {
			c.insert = 0;
			c.notes[0] = c.notes[1] = 0;
		}
		vb16_gain(c);
	}
	if (voices) {
		m_vb_dls.reset();
		m_vb_user16.reset();
		m_vb_fm.reset();
		m_vb16_parse = vb_parse();
		m_vb16_sx.clear();
	}
}

void mu2000::vb16_gain(vb_chan &c)
{
	const float vol = smu2000::vboard::level_of(c.vol) * smu2000::vboard::level_of(c.exp);
	float pl = 1, pr = 1;
	smu2000::vboard::pan_of(c.pan, pl, pr);
	const float rev = smu2000::vboard::level_of(c.rev), cho = smu2000::vboard::level_of(c.cho);
	c.gain[0] = vol * pl;
	c.gain[1] = vol * pr;
	c.gain[2] = vol * pl * rev;
	c.gain[3] = vol * pr * rev;
	c.gain[4] = vol * pl * cho;
	c.gain[5] = vol * pr * cho;
	const float var = smu2000::vboard::level_of(c.var);
	c.gain[6] = vol * pl * var;
	c.gain[7] = vol * pr * var;
}

void mu2000::set_board_insert(int channel, int slot)
{
	if (channel >= 0 && channel < 16)
		m_vb16[size_t(channel)].insert = u8(std::clamp(slot, 0, 5));
}

// 口 E の MIDI を 1 バイト。チャンネルメッセージが揃ったら、そのチャンネルの音源へ
void mu2000::board_midi_in(u8 byte)
{
	if (!(multi_kind() != 0) || byte >= 0xf8)
		return;
	const bool dls = multi_kind() == VBOARD_DLS, user = multi_kind() == VBOARD_USER16, fm = multi_kind() == VBOARD_FM16;
	// SysEx は、リセット（XG System On F0 43 1n 4C 00 00 7E 00 F7、GM System On F0 7E 7F 09 01 F7、
	// GS リセット F0 41 dd 42 12 40 00 7F 00 41 F7）と、DLS のボードではドラムのパートの指定を読む:
	//   GS「リズムパートに使う」 F0 41 dd 42 12 40 1x 15 vv 和 F7（x = 0 はチャンネル 10、1-9 は 1-9、A-F は 11-16。vv = 0 でメロディ）
	//   XG パートのモード        F0 43 1n 4C 08 pp 07 vv F7（vv = 0 でメロディ。口 E ではパート = チャンネル）
	if (byte == 0xf0) {
		m_vb16_sx.assign(1, byte);
	} else if (!m_vb16_sx.empty()) {
		if (byte == 0xf7) {
			const std::vector<u8> &sx = m_vb16_sx;
			const bool xg = sx.size() == 8 && sx[1] == 0x43 && (sx[2] & 0xf0) == 0x10 && sx[3] == 0x4c;
			const bool gs = sx.size() == 10 && sx[1] == 0x41 && sx[3] == 0x42 && sx[4] == 0x12 && sx[5] == 0x40;
			if ((xg && sx[4] == 0x00 && sx[5] == 0x00 && sx[6] == 0x7e) ||
			    (sx.size() == 5 && sx[1] == 0x7e && sx[3] == 0x09 && sx[4] == 0x01) ||
			    (gs && sx[6] == 0x00 && sx[7] == 0x7f && sx[8] == 0x00)) {
				vb16_reset(true);
			} else if (gs && (sx[6] & 0xf0) == 0x10 && sx[7] == 0x15) {
				const int x = sx[6] & 15;
				m_vb_dls.set_drum(x == 0 ? 9 : x <= 9 ? x - 1 : x, sx[8] != 0);
				m_vb_fm.set_drum(x == 0 ? 9 : x <= 9 ? x - 1 : x, sx[8] != 0);
			} else if (xg && sx[4] == 0x08 && sx[5] < 16 && sx[6] == 0x07) {
				m_vb_dls.set_drum(sx[5], sx[7] != 0);
				m_vb_fm.set_drum(sx[5], sx[7] != 0);
			}
			m_vb16_sx.clear();
		} else if ((byte & 0x80) || m_vb16_sx.size() >= 12) {
			m_vb16_sx.clear();
		} else {
			m_vb16_sx.push_back(byte);
		}
	}
	vb_parse &p = m_vb16_parse;
	if (byte & 0x80) {
		p.status = byte < 0xf0 ? byte : 0;
		p.n = 0;
		return;
	}
	if (!p.status)
		return;
	p.d[p.n++] = byte;
	const int need = (p.status & 0xe0) == 0xc0 ? 1 : 2;
	if (p.n < need)
		return;
	p.n = 0;
	vb_chan &c = m_vb16[p.status & 15];
	// 画面に見せる演奏の様子
	switch (p.status & 0xf0) {
	case 0x90:
		if (p.d[1]) {
			c.notes[p.d[0] >> 6] |= u64(1) << (p.d[0] & 63);
			c.velocity = p.d[1];
			c.note_ons++;
			break;
		}
		[[fallthrough]];
	case 0x80:
		c.notes[p.d[0] >> 6] &= ~(u64(1) << (p.d[0] & 63));
		break;
	case 0xe0:
		c.bend = s16(((p.d[1] << 7) | p.d[0]) - 8192);
		break;
	case 0xb0:
		if (p.d[0] == 1)
			c.mod = p.d[1];
		else if (p.d[0] == 64)
			c.hold = p.d[1];
		else if (p.d[0] == 120 || p.d[0] == 123)
			c.notes[0] = c.notes[1] = 0;
		else if (p.d[0] == 121) {
			c.mod = c.hold = 0;
			c.bend = 0;
		}
		break;
	default:
		break;
	}
	if ((p.status & 0xf0) == 0xb0) {
		switch (p.d[0]) {
		case 7:   c.vol = p.d[1]; vb16_gain(c); return;
		case 10:  c.pan = p.d[1]; vb16_gain(c); return;
		case 11:  c.exp = p.d[1]; vb16_gain(c); return;
		case 91:  c.rev = p.d[1]; vb16_gain(c); return;
		case 93:  c.cho = p.d[1]; vb16_gain(c); return;
		case 94:  c.var = p.d[1]; vb16_gain(c); return;
		case 121:                         // Reset All Controllers: エクスプレッション・ベンド・モジュレーションを戻す
			c.exp = 127;
			vb16_gain(c);
			if (dls) {
				m_vb_dls.midi(p.status, 121, 0);
			} else if (user) {
				m_vb_user16.midi(p.status, 121, 0);
			} else if (fm) {
				m_vb_fm.midi(p.status, 121, 0);
			} else {
				c.fc.midi(0xe0, 0x00, 0x40);
				c.fc.midi(0xb0, 1, 0);
			}
			return;
		default:  break;
		}
	}
	if (dls)
		m_vb_dls.midi(p.status, p.d[0], need == 2 ? p.d[1] : 0);
	else if (user)
		m_vb_user16.midi(p.status, p.d[0], need == 2 ? p.d[1] : 0);
	else if (fm)
		m_vb_fm.midi(p.status, p.d[0], need == 2 ? p.d[1] : 0);
	else
		c.fc.midi(p.status, p.d[0], need == 2 ? p.d[1] : 0);
}

std::vector<mu2000::board_voice> mu2000::board_voices() const
{
	std::vector<board_voice> out;
	const auto add = [&out](bool drum, u8 msb, u8 lsb, u8 program, const char *name) {
		board_voice v;
		v.drum = drum;
		v.msb = msb;
		v.lsb = lsb;
		v.program = program;
		std::snprintf(v.name, sizeof(v.name), "%s", name);
		out.push_back(v);
	};
	const int kind = multi_kind();
	if (kind == VBOARD_DLS) {
		if (const smu2000::vboard::dls_bank *bank = m_vb_dls.bank())
			for (const smu2000::vboard::dls_instrument &i : bank->instruments)
				add(i.drum, i.msb, i.lsb, i.program, i.name.c_str());
	} else if (kind == VBOARD_FC16) {
		// 初期の音色は 16 個（17 以降はそのくり返し）。音色の組を開いていれば 128 個とも自分の音色
		for (int i = 0; i < (m_vb_fc_bank[FC_BANK_MULTI] ? 128 : 16); i++)
			add(false, 0, 0, u8(i), m_vb16[0].fc.name(i));
	} else if (kind == VBOARD_FM16) {
		// プログラム番号は GM の並び（分類ごとに 2 つの音色を 4 つずつ）。ドラムは 1 つ
		for (int i = 0; i < 128; i++)
			add(false, 0, 0, u8(i), m_vb_fm.voice_of(i).name);
		add(true, 0, 0, 0, "FM Kit");
	} else if (kind == VBOARD_USER16 && m_vb_user_board) {
		for (int i = 0; i < smu2000::vboard::user_board::PROGRAMS; i++)
			if (const auto &p = m_vb_user_board->program[size_t(i)])
				add(false, 0, 0, u8(i), p->name);
	}
	return out;
}

void mu2000::board_parts(board_part out[16])
{
	for (int i = 0; i < 16; i++) {
		vb_chan &c = m_vb16[size_t(i)];
		board_part &o = out[i];
		o = board_part();
		o.vol = c.vol;
		o.exp = c.exp;
		o.pan = c.pan;
		o.rev = c.rev;
		o.cho = c.cho;
		o.var = c.var;
		o.insert = c.insert;
		o.notes[0] = c.notes[0];
		o.notes[1] = c.notes[1];
		o.velocity = c.velocity;
		o.mod = c.mod;
		o.hold = c.hold;
		o.bend = c.bend;
		o.note_ons = c.note_ons;
		o.level = c.peak;
		c.peak *= 0.6f;                    // 読むたびに下げる（画面は 1 秒に 30 回ほど読む）
		const char *name = "";
		if (multi_kind() == VBOARD_DLS) {
			const smu2000::vboard::dls_instrument *ins = nullptr;
			m_vb_dls.channel_voice(i, o.msb, o.lsb, o.program, o.drum, ins);
			if (ins)
				name = ins->name.c_str();
		} else if (multi_kind() == VBOARD_FM16) {
			o.program = m_vb_fm.program(i);
			o.drum = m_vb_fm.drum(i);
			if (o.drum) {
				name = "FM Kit";
			} else {
				std::snprintf(o.name, sizeof(o.name), "%s", m_vb_fm.voice_of(o.program).name);
				continue;                  // 名前はもう入れた（写しなので、下の name では渡せない）
			}
		} else if (multi_kind() == VBOARD_USER16) {
			o.program = m_vb_user16.program(i);
			name = m_vb_user16.program_name(o.program);
		} else if (multi_kind() == VBOARD_FC16) {
			o.program = c.fc.program();
			name = c.fc.name(o.program);
		}
		std::snprintf(o.name, sizeof(o.name), "%s", name);
	}
}

void mu2000::set_user_board(std::shared_ptr<const smu2000::vboard::user_board> board, const std::string &path)
{
	m_vb_user_board = board;
	m_vb_user_path = path;
	for (vb_slot &vs : m_vbs)
		vs.user.set_board(board);
	m_vb_user16.set_board(std::move(board));
}

bool mu2000::load_board_dls(const std::string &path, std::string &err)
{
	std::shared_ptr<smu2000::vboard::dls_bank> bank = smu2000::vboard::dls_load(path, err);
	if (!bank)
		return false;
	m_vb_dls.set_bank(std::move(bank));
	m_vb_dls_path = path;
	return true;
}

// 1 サンプルぶん。鳴っているチャンネルを足して、MU のエフェクトの入口へ入れる。
// そのまま出す音は dry へ、インサーションへ通すチャンネルはその入口へ（どちらか片方）。送りは別に足す
bool mu2000::vb16_mix(float bus[][2])
{
	bool any = false;
	const auto mix = [bus](vb_chan &c, float l, float r) {
		// insert: 0 = dry、1-4 = insertion1-4、5 = variation（インサーションとして）
		const int main = c.insert == 0 ? int(ext_bus::dry) : c.insert == 5 ? int(ext_bus::variation) : int(ext_bus::insertion1) + c.insert - 1;
		bus[main][0] += l * c.gain[0];
		bus[main][1] += r * c.gain[1];
		bus[int(ext_bus::reverb)][0] += l * c.gain[2];
		bus[int(ext_bus::reverb)][1] += r * c.gain[3];
		bus[int(ext_bus::chorus)][0] += l * c.gain[4];
		bus[int(ext_bus::chorus)][1] += r * c.gain[5];
		if (c.insert != 5) {
			bus[int(ext_bus::variation)][0] += l * c.gain[6];
			bus[int(ext_bus::variation)][1] += r * c.gain[7];
		}
		c.peak = std::max(c.peak, std::max(std::fabs(l * c.gain[0]), std::fabs(r * c.gain[1])));
	};
	if (multi_kind() == VBOARD_DLS) {
		// DLS の音源はチャンネルごとの左右を返す。チャンネルの音量・パン・送りはここで掛ける
		if (m_vb_dls.sounding()) {
			any = true;
			float ch[16][2] = {};
			m_vb_dls.render(ch);
			for (int i = 0; i < 16; i++)
				if (ch[i][0] != 0.0f || ch[i][1] != 0.0f)
					mix(m_vb16[size_t(i)], ch[i][0], ch[i][1]);
		}
	} else if (multi_kind() == VBOARD_FM16) {
		if (m_vb_fm.sounding()) {
			any = true;
			float ch[16] = {};
			m_vb_fm.render(ch);
			for (int i = 0; i < 16; i++)
				if (ch[i] != 0.0f)
					mix(m_vb16[size_t(i)], ch[i], ch[i]);
		}
	} else if (multi_kind() == VBOARD_USER16) {
		if (m_vb_user16.sounding()) {
			any = true;
			float ch[16] = {};
			m_vb_user16.render(ch);
			for (int i = 0; i < 16; i++)
				if (ch[i] != 0.0f)
					mix(m_vb16[size_t(i)], ch[i], ch[i]);
		}
	} else {
		for (vb_chan &c : m_vb16) {
			if (!c.fc.sounding())
				continue;
			any = true;
			const float v = c.fc.render();
			mix(c, v, v);
		}
	}
	return any;
}

// 1 パートのボード 1 枚ぶんの 1 サンプルを、入れ物に足す。鳴っていなければ false
bool mu2000::vb1_mix(vb_slot &s, float bus[][2])
{
	if (s.resync && !--s.resync)
		vb_bank_from_ram(s);
	// firmware が知らないボードは、ワーク RAM を 256 サンプル（6ms）ごとに見て音色の変更を追う
	if (!s.known && s.on && !(++s.poll & 255))
		vb_follow_ram(s);
	const bool user = s.kind == VBOARD_USER;
	if (!(user ? s.user.sounding() : s.fc.sounding()))
		return false;
	// パートの設定は 64 サンプル（1.5ms）ごとに読み直す
	if (!(s.tick++ & 63)) {
		const u32 pb = xg::ram::part_base(s.part);
		const float vol = smu2000::vboard::level_of(m_ram[pb + 0x0b]) * smu2000::vboard::level_of(m_ram[pb + xg::ram::PART_EXP]);
		float pl = 1, pr = 1;
		smu2000::vboard::pan_of(m_ram[pb + 0x0e], pl, pr);
		const float cho = smu2000::vboard::level_of(m_ram[pb + 0x12]), rev = smu2000::vboard::level_of(m_ram[pb + 0x13]);
		s.gain[0] = vol * pl;
		s.gain[1] = vol * pr;
		s.gain[2] = vol * pl * rev;
		s.gain[3] = vol * pr * rev;
		s.gain[4] = vol * pl * cho;
		s.gain[5] = vol * pr * cho;
	}
	float v;
	if (user) {
		float ch[16] = {};
		s.user.render(ch);
		v = ch[0];
	} else {
		v = s.fc.render();
	}
	bus[int(ext_bus::dry)][0] += v * s.gain[0];
	bus[int(ext_bus::dry)][1] += v * s.gain[1];
	bus[int(ext_bus::reverb)][0] += v * s.gain[2];
	bus[int(ext_bus::reverb)][1] += v * s.gain[3];
	bus[int(ext_bus::chorus)][0] += v * s.gain[4];
	bus[int(ext_bus::chorus)][1] += v * s.gain[5];
	return true;
}

// 1 サンプルぶん。挿さっているボードの音を全部足して、MU のエフェクトの入口へ入れる
void mu2000::vb_render_all()
{
	float bus[int(ext_bus::count)][2] = {};
	bool any = false;
	for (vb_slot &vs : m_vbs) {
		if (!vs.kind)
			continue;
		if (board_is_multi(vs.kind))
			any |= vb16_mix(bus);
		else
			any |= vb1_mix(vs, bus);
	}
	if (!any) {
		if (m_vb_live) {
			clear_external_audio();
			m_vb_live = false;
		}
		return;
	}
	for (int b = 0; b < int(ext_bus::count); b++)
		set_external_audio(ext_bus(b), bus[b][0], bus[b][1]);
	m_vb_live = true;
}

void mu2000::run_sample(s32 &left, s32 &right)
{
	if (m_vb_any)
		vb_render_all();
	plg_pump();
	// S-MU2000: 軽量モードでは、XG の設定をときどき読み直す
	if (m_nfx_on && !(++m_nfx_tick & 0x1ff))
		native_fx_update();
	// 画面がパートの音を見ているときは、声 → パートを 256 サンプル（6ms）ごとに読み直す
	if ((m_scope_part.load(std::memory_order_relaxed) >= 0 || m_pscope_on.load(std::memory_order_relaxed)) &&
	    !(++m_scope_tick & 0xff))
		scope_refresh_owner();

	// パートのミュート。消すパートの声を SWP30 に伝える（外したときは 1 度だけ空にする）
	// 架空のボードを挿したパートは、内蔵の音を消す（ボードが代わりに鳴る）
	u64 board_mute = 0;
	if (m_vb_any)
		for (const vb_slot &vs : m_vbs)
			if (vs.active())
				board_mute |= u64(1) << vs.part;
	if (const u64 pm = m_part_mute.load(std::memory_order_relaxed) | board_mute; pm || m_mute_live) {
		if (!pm || !(++m_mute_tick & 0x1f)) {
			u64 vm[2] = { 0, 0 };
			if (pm) {
				scope_refresh_owner();
				for (int v = 0; v < 128; v++) {
					const int o = m_scope_owner[size_t(v)].load(std::memory_order_relaxed);
					if (o >= 0 && ((pm >> o) & 1))
						vm[v / 64] |= u64(1) << (v % 64);
				}
			}
			m_swpm.m_voice_mute.store(vm[0], std::memory_order_relaxed);
			m_swps.m_voice_mute.store(vm[1], std::memory_order_relaxed);
			m_mute_live = pm != 0;
		}
	}

	// 台数が変わっていたら別スレッドの使い方を見直す（8192 サンプルごと）
	if (m_want_threaded && !(++m_thread_check & 0x1fff))
		apply_threading();

	m_sample_count++;

	// SWP30 は 44100Hz で 1 サンプル。CPU はその間に 28MHz/44100 ≒ 634.9 サイクル
	m_cycle_debt += 28000000;
	const u64 cycles = m_cycle_debt / 44100;
	m_cycle_debt -= cycles * 44100;

	// 内訳を測る（set_profile(true) のときだけ）
	// (smu2000::perf_ticks() is QueryPerformanceCounter on Windows, so the
	//  measurement is the same one on both platforms -- see compat/platform.h)
	u64 pt0 = 0, pt1 = 0, pt2 = 0;
	if (m_profile)
		pt0 = smu2000::perf_ticks();

	// native の口が動いているときは、firmware を回すのは
	//   * 渡した MIDI がまだ溜まっている間（受け取って処理させる）
	//   * そのあと少しの間（処理が終わるまで）
	// だけ。ふだんは止めておく
	bool run_cpu = m_cpu_enabled;
	// 内訳をもう一段割る（m_t_ndrv / m_t_nemisc / m_t_sh2）。測るときだけ読む
	u64 pn0 = 0, pn1 = 0;
	if (m_native_engine) {
		if (m_profile)
			pn0 = smu2000::perf_ticks();
		m_ne_samples.fetch_add(1, std::memory_order_relaxed);
		// firmware が鳴らしている音がある間は止めない。LFO・包絡線・ベンドの
		// 追従をやっているのは firmware なので、止めるとその音だけ変わってしまう。
		// MIDI の溜まり具合は、止まっているときだけ見る（毎サンプル数えると重い）
		if (m_fw_note_total && m_ne_clock < m_fw_note_until)
			m_fw_hold = std::max(m_fw_hold, u32(2));
		// **フィルタの動きを録っている間は firmware を全速で回す**。
		// 包絡線を動かしているのは firmware のソフトで、10ms ごとに
		// 0x00・0x01・0x04 を書き直す。細く回している（100ms につき 5ms）
		// ままだと firmware の時間が 20 分の 1 しか進まず、1 秒の窓で
		// 実機の 5% ぶんしか録れない。音色 1 つにつき 1 秒だけの負担
		if (m_traj_rec)
			m_fw_hold = std::max(m_fw_hold, u32(2));
		// **firmware を細く回し続ける**。ここを入れるまでは、全部 native で
		// 鳴る曲だと MIDI が来たときしか CPU を回さず、firmware が丸ごと
		// 止まっていた。その結果:
		//   * 液晶が固まる／前面のボタンが一切効かない（どちらも firmware の仕事）
		//   * **firmware が自分の鳴らした音の後始末をできない**。声の管理表が
		//     「使用中」のまま埋まっていき、窓を閉じるとその状態が NVRAM に
		//     保存されて、次に開いたときは曲の頭から壊れる
		// 100ms ごとに 5ms だけ回す。止まりっぱなしにしないのが目的なので、
		// これで十分（パネルの反応は 100ms 以内、CPU は数 % 増えるだけ）
		if (m_ne_clock % KEEPALIVE_EVERY == 0) {
			// 直前の 100ms に firmware が半分も回っていなければ、細く回している
			m_throttled = (m_fw_clock - m_thr_fw0) < KEEPALIVE_EVERY / 2;
			m_thr_fw0 = m_fw_clock;
			m_fw_hold = std::max(m_fw_hold, KEEPALIVE_RUN);
			if (!m_fw_why)
				m_fw_why = 5;
			// **つまみを 100ms ごとに必ず拾い直す**（6.125）。下の
			// 「hold が 0 になったら」だけでは、firmware が音を鳴らしている
			// 間（m_fw_note_total）や写し取りの録画中は hold が 0 に
			// ならないので、**一度も拾えない**ことがあった。RPN でベンド幅を
			// 広げても native は既定の 2 半音のまま鳴らしていた
			m_ndrv.sync_cc();
			sync_prog();
		}
		// **パネルを触っている間は全速**（6.119）。ボタン・ダイヤル・液晶は
		// ぜんぶ firmware の仕事なので、細く回したままだと手触りが 20 分の 1 に
		// なる。ダイヤルの目盛りが残っている間も回し続ける（実機は 2.5ms ごとに
		// 1 目盛りしか読まないので、止めると入力が溜まったままになる）
		if (m_panel_hold || m_enc_pending) {
			if (m_panel_hold)
				m_panel_hold--;
			m_fw_hold = std::max(m_fw_hold, u32(2));
			if (!m_fw_why)
				m_fw_why = 6;
		}
		// 「溜まっている間は回す」はやめた。渡した MIDI は 1 バイト 14 サンプルかけて
		// 線を流れるので、それを待つだけで実時間の 2 割を SH-2 に持っていかれていた。
		// メッセージごとに置く待ち（下の native_midi）で足りる
		// **液晶のメーターは 25ms ごと**（6.148）。実機の 0x0D158A と同じ刻み・
		// 同じ式（半分ずつ寄せる）。1 音鳴らしたときの
		// 39→58→68→73→75→76→77 がこれで出る
		if (m_ne_clock >= m_meter_next) {
			m_meter_next = m_ne_clock + 44100 / 40;
			m_ndrv.fill_meter(m_meter_lv, 16);
			// **firmware が鳴らしている音も混ぜる**（6.188）。写し取りの
			// 1 音目は firmware が持つので native のスロットには無く、
			// 混ぜないと**音が鳴っているのにメーターだけ落ちる**。
			//
			// 実機の演奏画面が読む 0x402DD8 は使えない。native の口では
			// その係が回らないため、**読み出して消す人がいなくて値が張り付く**
			//（実測: 音が終わっても 90 b2 8b ce のままだった）。
			// 消すのはこわい（ここへ書くと音そのものが壊れる。6.148）ので、
			// **渡した打鍵からこちらで作る**
			for (int p = 0; p < 16; p++)
				if (m_fw_notes[p] && m_fw_meter[p] > m_meter_lv[p]
				    && m_ne_clock - m_fw_meter_at[p] < FW_METER_HOLD)
					m_meter_lv[p] = m_fw_meter[p];
			for (int p = 0; p < 16; p++) {
				const int now = int(m_meter_smooth[p]);
				const int tgt = int(m_meter_lv[p]);
				m_meter_smooth[p] = u8(now + (tgt - now) / 2);
			}
			// **調べ用**（`SMU2000_METER_DBG=1`）。draw_meter の前の
			// 液晶の中身と、目盛り（生 / なまし）を出す
			if (m_meter_dbg) {
				const u8 *dd = m_lcd.ddram();
				std::fprintf(stderr, "MTR %.3f",
				             double(m_ne_clock) / 44100.0);
				for (int c = 0; c <= 8; c++)
					std::fprintf(stderr, " %02x", dd[0x40 + c]);
				std::fprintf(stderr, " |");
				for (int c = 0; c <= 8; c++)
					std::fprintf(stderr, " %02x", dd[c]);
				std::fprintf(stderr, " |");
				for (int p = 0; p < 16; p++)
					std::fprintf(stderr, " %d/%d", int(m_meter_lv[p]),
					             int(m_meter_smooth[p]));
				std::fprintf(stderr, "\n");
			}
			draw_meter();
			draw_voice_fields();
		}
		if (m_fw_hold) {
			if (--m_fw_hold == 0) {
				// つまみの位置を RAM から取り直す。こちらが動かした値は
				// 上書きしない（native_driver::sync_cc）。ベンド幅のように
				// こちらが持たない値は、ここで拾う
				m_ndrv.sync_cc();
				m_fw_why = 0;
			}
		// `SMU2000_FW_ALWAYS=1` で **native の口でも SH-2 を止めない**。
		// 止めると firmware の打鍵が 1 サンプル後ろへずれるのを見つけた
		// ときの道具（doc/native-engine.md の 6.153）。ふだんは使わない
		} else if (!m_fw_always) {
			run_cpu = false;
		}
		if (run_cpu) {
			m_ne_fw_samples.fetch_add(1, std::memory_order_relaxed);
			if (m_fw_note_total)
				m_ne_by_note.fetch_add(1, std::memory_order_relaxed);
			else if (m_fw_why == 1)
				m_ne_by_sysex.fetch_add(1, std::memory_order_relaxed);
			else if (m_fw_why == 3)
				m_ne_by_learn.fetch_add(1, std::memory_order_relaxed);
			else if (m_fw_why == 4)
				m_ne_by_midi.fetch_add(1, std::memory_order_relaxed);
			else if (m_fw_why == 5)
				m_ne_by_keep.fetch_add(1, std::memory_order_relaxed);
			else if (m_fw_why == 6)
				m_ne_by_panel.fetch_add(1, std::memory_order_relaxed);
			else
				m_ne_by_other.fetch_add(1, std::memory_order_relaxed);
		}
		if (m_learning && m_learn_left && --m_learn_left == 0)
			native_learn_finish();
		// **10ms 割り込みの印を見て格子の位相を学ぶ**（6.145）。
		// 写し取りの `0x00` からしか学べなかったので、写し取り済み
		// （2 回目以降）だと一度も学べず、滑りが前の道に落ちていた。
		// firmware は音を鳴らしていなくてもここを裏返すので、いつでも学べる
		if (m_ram.size() > xg::ram::TICK_MARK) {
			const u8 tk = m_ram[xg::ram::TICK_MARK];
			if (tk != m_tick_seen) {
				m_tick_seen = tk;
				m_ndrv.set_eg_phase(u32(m_ne_clock));
			}
		}
		m_ne_clock++;
		// **リセットが効き終わったら解く**（issue #51）。firmware が SWP30 を
		// 20ms 触らなくなったら終わったとみなす。取り逃しても 400ms で必ず解く
		if (m_ne_reset_hold &&
		    (m_ne_clock >= m_ne_reset_deadline ||
		     (m_ne_clock > m_fw_swp_at && m_ne_clock - m_fw_swp_at >= RESET_QUIET))) {
			m_ne_reset_hold = false;
			// **間隔を保ったままずらす**。溜めた分を一度に鳴らすと、線の上で
			// 1 ミリ秒ずつずれていた和音が完全に揃ってしまい、音が大きくなる
			// （firmware の道より +1.7dB になった）
			if (!m_nq.empty() && m_nq.front().at < m_ne_clock) {
				const u64 delta = m_ne_clock - m_nq.front().at;
				for (nev &e : m_nq)
					e.at += delta;
			}
			if (reset_debug())
				std::fprintf(stderr, "[reset] 解いた ne_clock=%llu（期限=%llu 最後の SWP=%llu）残り=%zu\n",
				             (unsigned long long)m_ne_clock, (unsigned long long)m_ne_reset_deadline,
				             (unsigned long long)m_fw_swp_at, m_nq.size());
		}
		if (!m_nq.empty())
			native_pump();
		if (m_profile)
			pn1 = smu2000::perf_ticks();
		m_ndrv.tick(m_ne_clock);
		if (m_traj_rec)
			traj_step();
		if (m_profile) {
			const u64 pn2 = smu2000::perf_ticks();
			m_t_ndrv += pn2 - pn1;
			m_t_nemisc += pn1 - pn0;
		}
	}
	if (run_cpu) {
		m_fw_clock++;
		if (m_profile) {
			const u64 pc0 = smu2000::perf_ticks();
			run_cycles(cycles);
			m_t_sh2 += smu2000::perf_ticks() - pc0;
			m_n_sh2++;
		} else
			run_cycles(cycles);
		// firmware が液晶を書き換えていれば、点滅かどうかを覚える
		if (m_native_engine)
			blink_learn();
		else
			m_lcd.clear_changes();
	}

	if (m_profile) {
		pt1 = smu2000::perf_ticks();
		m_t_cpu += pt1 - pt0;
	}

	// マスタとスレーブを 1 サンプルずつ進める。
	// 別スレッドが空いていればスレーブをそちらに投げ、同時に走らせる
	s32 lm = 0, rm = 0, ls = 0, rs = 0;
	if (m_slave_thread.joinable()) {
		const u64 tag = m_slave_go.load(std::memory_order_relaxed) + 1;
		m_slave_go.store(tag, std::memory_order_release);
		m_slave_go.notify_one();   // 眠っていたら起こす。起きていれば素通り
		m_swpm.run_sample(lm, rm);
		while (m_slave_done.load(std::memory_order_acquire) != tag)
			smu2000::cpu_pause();
		ls = m_slave_l;
		rs = m_slave_r;
	} else {
		m_swpm.run_sample(lm, rm);
		m_swps.run_sample(ls, rs);
	}

	if (m_profile) {
		pt2 = smu2000::perf_ticks();
		m_t_swpm += pt2 - pt1;
		m_t_n++;
	}

	// 2 個の SWP30 は MELO/MELI のシリアルで相互に結ばれている。
	// スレーブの声は自分の DAC には出ず、この線でマスタのミキサに入る。
	// 結線は MAME の mu1000_state::mu1000() と同じ:
	//   スレーブ 出力 4..17 -> マスタ  入力 0..13
	//   マスタ   出力 4..9, 12..13 -> スレーブ 入力 0..5, 8..9
	// **マスタからスレーブの 6 と 7 の線は無い**。実機にも MAME にも無いので繋いではいけない。
	// 繋ぐと、マスタのミキサ出力 3 番（melo 6/7）がスレーブへ回り込み、
	// スレーブ→マスタの線と合わせて輪になってしまう（スレーブの 6 と 7 には A/D INPUT が入る。下を参照）
	// 相互に繋がっているので 1 サンプル遅れで渡す（MAME も同じ）
	for (int i = 0; i < 14; i++)
		m_swpm.set_meli(i, m_swps.melo(i));
	static const int TO_SLAVE[] = { 0, 1, 2, 3, 4, 5, 8, 9 };
	for (int i : TO_SLAVE)
		m_swps.set_meli(i, m_swpm.melo(i));
	// A/D INPUT はスレーブの入力 6（AD1）と 7（AD2）に入る。上のマスタからの線が飛ばしている 2 本で、
	// A/D パートの音量を上げると firmware がここをミキサに通す（エミュで線を 1 本ずつ試して決めた）。
	// サンプリングの録音も、この 2 本をミキサの出力 8 に集めて録る（swp30.cpp の sample_step）。
	// 目盛りは 16bit を 8bit 上げた 24bit にしている（実機の入力の大きさとはまだ突き合わせていない）
	m_swps.set_meli(6, m_ad_in[0] * 256);
	m_swps.set_meli(7, m_ad_in[1] * 256);
	// レベルメーター用の検波（AN0 / AN2）
	for (int i = 0; i < 2; i++) {
		const s32 a = std::min(std::abs(m_ad_in[i]), 32767);
		m_ad_peak[i] = a >= m_ad_peak[i] ? a : m_ad_peak[i] - ((m_ad_peak[i] >> 12) + 1);
	}
	// 録音（パネルを通さない道。src/sampling.cpp の rec_start）
	if (m_rec_state) {
		using smu2000::sampling::source;
		const s32 v = m_rec_src == source::ad1 ? m_ad_in[0]
		            : m_rec_src == source::ad2 ? m_ad_in[1]
		            : std::clamp(m_ad_in[0] + m_ad_in[1], -32768, 32767);
		if (m_rec_state == 1 && std::abs(v) >= m_rec_trigger)
			m_rec_state = 2;
		if (m_rec_state == 2) {
			if (m_rec_buf.size() < m_rec_max)
				m_rec_buf.push_back(s16(std::clamp(v, -32768, 32767)));
			else
				m_rec_state = 0;
		}
	}

	// スピーカーに出るのはマスタの DAC だけ。
	// スレーブの DAC はどこにも繋がっていない
	left  = lm;
	right = rm;
	// サンプリングの窓の試聴。サンプリング RAM の 16bit をそのまま DAC の目盛りで足す（src/sampling.cpp）
	if (m_prev_on) {
		const size_t at = size_t(m_prev_base + m_prev_pos) * 2;
		const bool ext = !m_prev_ext.empty();
		if (m_prev_pos < m_prev_end && (ext ? m_prev_pos < m_prev_ext.size() : at + 1 < m_sampram.size())) {
			const s16 v = ext ? m_prev_ext[m_prev_pos] : s16(m_sampram[at] | m_sampram[at + 1] << 8);
			const s32 o = s32(s64(v) * DAC_FULL_SCALE / 32768);
			left += o;
			right += o;
			if (++m_prev_pos >= m_prev_end && m_prev_loop != ~0u)
				m_prev_pos = m_prev_loop;
		} else {
			m_prev_on = false;
		}
	}
	// 一覧のマスターのスペクトラム（最終の出力、左右の平均）
	if (m_pscope_on.load(std::memory_order_relaxed)) {
		const u32 w = m_oscope_w.load(std::memory_order_relaxed);
		m_oscope[w & (PSCOPE_N - 1)] = (float(lm) + float(rm)) * 0.5f;
		m_oscope_w.store(w + 1, std::memory_order_release);
	}
}

// ---- 状態の保存と復元
//
// ROM（プログラム・波形・sin 表・字の絵）は入れない。戻すときは同じものを
// 積んでおくこと。調べもの用の数え上げも入れない。

namespace {

// 保存の形。中身の並びを変えたら上げる
constexpr u32 STATE_MAGIC   = 0x554d3253;   // "S2MU"
constexpr u32 STATE_VERSION = 15;  // 15: MEG の書き換わった命令（6.238） / 14: MEG の静まった区画（6.237） / 13: d80000（LCD のコントラスト） / 2: MIDI の入口が A/B の 2 口になった / 3: SWP30 のピッチ EG / 4: サンプリングの録音の位置 / 5: SmartMedia の命令の途中 / 6: MEG の印と 2 つ目の idx / 7: USB の口（C・D）の受け取り途中 / 8: 2 つ目の A/D 変換器（AN4 = HOST SELECT） / 9: SWP30 の書き込みの待ち / 10: USB のコマンド（M37640 からの知らせ） / 11: 液晶の「native の持ち物」（6.188） / 12: 外字の「native の持ち物」（6.190）
constexpr u32 STATE_VERSION_OLDEST = 2;

} // namespace

void mu2000::state(state_io &s)
{
	s.tag("mu2000");
	m_machine.state_sync(s);

	// 主記憶。番地の割り振りは build_bus() と同じ
	s.mem(m_ram.data(),     m_ram.size());
	s.mem(m_dram.data(),    m_dram.size());
	s.mem(m_iram.data(),    m_iram.size());
	s.mem(m_sampram.data(), m_sampram.size());
	// 版 5 から: SmartMedia の命令の途中の状態（カードの中身は入れない）
	if (s.version() >= 5)
		m_card.state(s);

	if (m_cpu)  m_cpu->state(s);
	m_swpm.state(s);
	m_swps.state(s);
	m_lcd.state(s);
	if (m_sci4) m_sci4->state(s);

	s.tag("panel");
	s.v(m_ledsw1); s.v(m_ledsw2); s.arr(m_sws);
	s.v(m_enc_pending); s.v(m_enc_high); s.v(m_pe);
	s.arr(m_sci_irq);
	s.v(m_cycle_debt);
	// **前のサンプルからのはみ出し**。これが無いと、戻した直後の 1 サンプルで
	// CPU の回す量が数サイクルずれる
	s.v(m_overrun);
	// 版 9 から: SWP30 へ書いた待ちの残り（サンプルを跨ぐことがある）
	if (s.version() >= 9)
		s.v(m_swp_wait);

	// 受け取り途中の MIDI。A と B の 2 口ぶん
	s.tag("midi");
	for (midi_line &m : m_midi) {
		u32 n = u32(m.queue.size());
		s.v(n);
		if (s.writing()) {
			for (u8 b : m.queue)
				s.v(b);
		} else {
			m.queue.clear();
			for (u32 i = 0; i < n && s.ok(); i++) {
				u8 b = 0;
				s.v(b);
				m.queue.push_back(b);
			}
		}
		s.v(m.bit); s.v(m.cur); s.v(m.next);
	}

	// 版 7 から: USB の口（C・D）の受け取り途中。firmware へ渡す前のバイト列
	if (s.version() >= 7) {
		s.tag("usb");
		u32 n = u32(m_usb.rx.size());
		s.v(n);
		if (s.writing()) {
			for (u8 b : m_usb.rx)
				s.v(b);
		} else {
			m_usb.rx.clear();
			for (u32 i = 0; i < n && s.ok(); i++) {
				u8 b = 0;
				s.v(b);
				m_usb.rx.push_back(b);
			}
		}
		s.v(m_usb.in_port); s.v(m_usb.next); s.v(m_usb.have); s.v(m_usb.cur); s.v(m_usb.tx_next);
		if (s.version() >= 10) {
			u32 c = u32(m_usb.cmd.size());
			s.v(c);
			if (s.writing()) {
				for (u8 b : m_usb.cmd)
					s.v(b);
			} else {
				m_usb.cmd.clear();
				for (u32 i = 0; i < c && s.ok(); i++) {
					u8 b = 0;
					s.v(b);
					m_usb.cmd.push_back(b);
				}
			}
			s.v(m_usb.cur_cmd);
		}
	}

	// 版 13 から: d80000 の値（LCD のコントラスト）
	if (s.version() >= 13) {
		s.tag("d80");
		s.v(m_d80);
	}
}

u32 mu2000::state_version()
{
	return STATE_VERSION;
}

std::vector<u8> mu2000::save_state() const
{
	std::vector<u8> out;
	state_io s(out);
	u32 magic = STATE_MAGIC, ver = STATE_VERSION;
	s.v(magic);
	s.v(ver);
	s.set_version(ver);
	const_cast<mu2000 *>(this)->state(s);
	return out;
}

bool mu2000::load_state(const u8 *p, size_t n, std::string &err)
{
	// 読み戻したら CC の控えは忘れる（線で見た値と、機械の中身が合わなくなるので）
	std::memset(m_cc_last, 0xff, sizeof(m_cc_last));
	state_io s(p, n);
	u32 magic = 0, ver = 0;
	s.v(magic);
	s.v(ver);
	if (!s.ok() || magic != STATE_MAGIC) {
		err = CLI_T("This is not an S-MU2000 state", "これは S-MU2000 の状態ではない");
		return false;
	}
	if (ver < STATE_VERSION_OLDEST || ver > STATE_VERSION) {
		err = CLI_T("The state has a different layout (this version cannot read it)", "状態の形が違う（この版では読めない）");
		return false;
	}
	s.set_version(ver);
	state(s);
	if (!s.ok()) {
		err = s.error();
		return false;
	}
	// MIDI OUT の途中の枠と溜めは保存していない。空から始める
	m_tx_r = m_tx_w = 0;
	m_tx_bit = -1;

	// 軽量モード（C++ のエフェクト）の中身は状態に**入れない**。ディレイと残響の
	// 遅延線だけで 5MB 近くあって、DAW の企画ファイルが膨らむわりに、得られるのは
	// 「尾が切れない」だけだから（MEG の側の尾は SWP30 のリバーブ RAM に入っている）。
	// ただし前の曲の尾が残ったままだと、戻した曲に混ざる。ここで消す
	if (m_nfx_on)
		m_nfx.reset();
	return true;
}
