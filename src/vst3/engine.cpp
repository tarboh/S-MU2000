// license:BSD-3-Clause

#include "engine.h"

#include "mu2000.h"
#include "bootcache.h"
#include "nvram.h"
#include "smartmedia.h"
#include "ui/xg_ui.h"

#include "compat/paths.h"
#include "compat/platform.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace smu2000 {
namespace vst3 {

namespace {

// ---- プラグイン本体（DLL）の置かれている場所
//
// The OS answers now come from compat/paths.h: where this image lives, what
// the environment says, and where the per-user settings directory is. Only the
// search order below is this file's business.

// そのディレクトリが ROM 置き場かどうか
bool has_roms(const std::string &dir)
{
	return !dir.empty() && smu2000::is_file(smu2000::join(dir, "mu2000_flash.bin"));
}

// roms.txt に書かれた場所を読む（1 行目だけ）
std::string read_pointer_file(const std::string &path)
{
	std::FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return {};
	char line[1024] = {};
	if (!std::fgets(line, sizeof(line), f)) { std::fclose(f); return {}; }
	std::fclose(f);
	std::string s(line);
	// メモ帳などが付ける BOM を落とす。これがあると場所を見失う
	if (s.size() >= 3 && (unsigned char)s[0] == 0xef && (unsigned char)s[1] == 0xbb &&
	    (unsigned char)s[2] == 0xbf)
		s.erase(0, 3);
	while (!s.empty() && (s.back() == '\r' || s.back() == '\n' ||
	                      s.back() == ' '  || s.back() == '\t'))
		s.pop_back();
	return s;
}

// ---- 記録。画面が無いので、うまくいかなかったときはここを見てもらう

std::string log_path()
{
	// The same per-user directory the GUI keeps gui.ini and panel.txt in
	const std::string dir = smu2000::ensure_config_dir();
	return dir.empty() ? std::string() : smu2000::join(dir, "log.txt");
}

void logf(const char *fmt, ...)
{
	static const std::string path = log_path();
	if (path.empty())
		return;
	const bool fresh = !smu2000::is_file(path);
	std::FILE *f = std::fopen(path.c_str(), "ab");
	if (!f)
		return;
	if (fresh)
		std::fwrite("\xef\xbb\xbf", 1, 3, f);   // UTF-8 の印。無いと化けて読まれる
	std::fprintf(f, "%s  ", smu2000::local_time().c_str());
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(f, fmt, ap);
	va_end(ap);
	std::fputc('\n', f);
	std::fclose(f);
}

} // namespace

// Find the ROM directory; see engine.h. Published rather than file-local so the
// AUv3's carrier application can report the same search the plug-in performs
std::string find_rom_dir(std::string &tried)
{
	std::vector<std::string> cand;

	// 1. 環境変数。一番強い
	const std::string ev = smu2000::env("S_MU2000_ROMS");
	if (!ev.empty())
		cand.push_back(ev);

	// The address of a function in this image is what locates the image:
	// a module handle on Windows, the Mach-O header on macOS
	const std::string dir = smu2000::module_dir(reinterpret_cast<const void *>(&logf));
	if (!dir.empty()) {
		// 2. バンドルの Resources。
		//    <名前>.vst3/Contents/x86_64-win/ に DLL がいるので 1 つ上
		//    (macOS puts the binary in Contents/MacOS, also one level up)
		cand.push_back(smu2000::join(dir, "../Resources"));
		cand.push_back(smu2000::join(dir, "../Resources/roms"));
		// 3. DLL のすぐ横
		cand.push_back(smu2000::join(dir, "roms"));
		cand.push_back(dir);
		// 4. 場所を書いた紙
		const std::string notes[2] = { smu2000::join(dir, "../Resources/roms.txt"),
		                               smu2000::join(dir, "roms.txt") };
		for (const std::string &p : notes) {
			const std::string s = read_pointer_file(p);
			if (!s.empty())
				cand.push_back(s);
		}
	}

	// 5. The fixed places, following where macOS puts an application's own data
	//    (Application Support, Documents). Per-user comes first and machine-wide
	//    last, so a user's own copy wins
	const std::string local = smu2000::config_dir();
	if (!local.empty()) {
		// A note naming the directory. Someone using this from a DAW has nowhere
		// to set an environment variable, so one line here (roms.txt) does it
		const std::string note = read_pointer_file(smu2000::join(local, "roms.txt"));
		if (!note.empty())
			cand.push_back(note);
		cand.push_back(smu2000::join(local, "roms"));
		cand.push_back(local);
	}
	const std::string home = smu2000::home_dir();
	if (!home.empty())
		cand.push_back(smu2000::join(home, "Documents/S-MU2000/roms"));

	// 6. The machine-wide places. **Put the ROMs here once and every user of the
	//    machine, and every instance of either plug-in, finds them.** The AU is
	//    one bundle in Components, shared by all accounts, so this is its
	//    intended home (/Library/Application Support, %ProgramData% on Windows)
	//
	//    This program never writes here: creating it takes the rights to
	const std::string shared = smu2000::shared_config_dir();
	if (!shared.empty()) {
		const std::string note = read_pointer_file(smu2000::join(shared, "roms.txt"));
		if (!note.empty())
			cand.push_back(note);
		cand.push_back(smu2000::join(shared, "roms"));
		cand.push_back(shared);
	}

	for (const std::string &c : cand) {
		const std::string p = smu2000::full_path(c);
		if (has_roms(p))
			return p;
		tried += "  " + p + "\n";
	}
	return {};
}


// ---- 読み込んだ ROM の使い回し。
// 誰も使わなくなったら消えるよう、控えは weak_ptr で持つ

namespace {

struct rom_set {
	mu2000::u8rom  prog, wave;
	mu2000::u16rom sintab;
	mu2000::u8rom  font;
	std::string    warn;
};

std::mutex             g_rom_mutex;
std::string            g_rom_dir;
std::weak_ptr<rom_set> g_roms;

} // namespace


engine::engine()
{
	build_table();
	for (std::vector<uint8_t> &p : m_pending)
		p.reserve(4096);
}

engine::~engine()
{
	m_abort.store(true, std::memory_order_relaxed);
	if (m_thread.joinable())
		m_thread.join();
	// 音声スレッドはもう回っていない。SmartMedia に書いたものをその場で残す
	if (m_mu && !card_path().empty()) {
		std::vector<smartmedia::block> blocks;
		m_mu->card().take_dirty_blocks(blocks);
		std::string err;
		if (!smartmedia::write_blocks(card_path(), blocks, err))
			log_line(err.c_str());
	}
	delete m_mu;
}

std::string engine::message() const
{
	return state() == status::loading ? std::string("起動中") : m_message;
}

void engine::log_line(const char *text)
{
	logf("%s", text);
}

void engine::start()
{
	if (!m_thread.joinable())
		m_thread = std::thread([this] { boot(); });
}

bool engine::wait_ready(int ms)
{
	start();
	const auto limit = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (state() == status::loading) {
		if (std::chrono::steady_clock::now() >= limit)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return true;
}

void engine::boot()
{
	std::string tried;
	const std::string dir = find_rom_dir(tried);
	if (dir.empty()) {
		m_message = "ROM が見つからない。探した場所:\n" + tried +
		            "環境変数 S_MU2000_ROMS で場所を指定できる";
		logf("ROM が見つからない。探した場所:\n%s", tried.c_str());
		ui::driver::publish_message(m_bridge, "ROM が見つからない");
		m_state.store(status::failed, std::memory_order_release);
		return;
	}
	logf("ROM: %s", dir.c_str());
	ui::driver::publish_message(m_bridge, "ROM 読み込み中");

	mu2000 *mu = new mu2000;
	std::string warn;
	{
		// ROM は読むだけなので、この DLL の中で 1 組あればいい。
		// トラックごとに挿されると 36MB × 枚数になってしまう
		std::lock_guard<std::mutex> lock(g_rom_mutex);
		std::shared_ptr<rom_set> shared;
		if (g_rom_dir == dir)
			shared = g_roms.lock();

		if (shared) {
			mu->set_program_rom(shared->prog);
			mu->set_wave_rom(shared->wave);
			mu->set_sintab_rom(shared->sintab);
			mu->set_lcd_font(shared->font);
			warn = shared->warn;
			logf("ROM は読み込み済みのものを借りた");
		} else {
			if (!mu->load_program(smu2000::join(dir, "mu2000_flash.bin")) ||
			    !mu->load_wave(smu2000::join(dir, "dump"))) {
				m_message = mu->error();
				logf("%s", m_message.c_str());
				delete mu;
				m_state.store(status::failed, std::memory_order_release);
				return;
			}
			if (!mu->load_sintab(smu2000::join(dir, "standin/sin-table.bin"))) {
				warn = mu->error();
				logf("警告: %s", warn.c_str());
			}
			// LCD の字の絵。無くても音は出るが、画面に何も映らなくなる
			if (!mu->load_lcd_font(smu2000::join(dir, "hd44780u_b04.bin")) &&
			    !mu->load_lcd_font(smu2000::join(dir, "standin/hd44780u_b04.bin")))
				logf("警告: %s", mu->error().c_str());
			shared = std::make_shared<rom_set>();
			shared->prog   = mu->program_rom();
			shared->wave   = mu->wave_rom();
			shared->sintab = mu->sintab_rom();
			shared->font   = mu->lcd_font();
			shared->warn   = warn;
			g_rom_dir = dir;
			g_roms    = shared;
		}
		// 借り手が 1 人でも生きている限り、次の人も借りられる
		m_roms = shared;
	}

	// SWP30 のスレーブを別スレッドで回すか。既定は回す（挿した枚数が論理コアの 1/4 を超えたら自動で 1 本）。
	// DAW の中では、DAW が管理しない糸が 1 本増える。嫌う DAW や、自分でコアを割り振りたい人のために、
	// %LOCALAPPDATA%\S-MU2000\plugin.ini に threaded=0 と書けば 1 本で回す
	bool threaded = true;
	// MIDI IN の口 C・D（パート 33-64）は実機では USB だけの口で、firmware は
	// HOST SELECT が USB のときしか通さない。**既定は USB**（実機を PC に繋ぐときと
	// 同じ姿）。A・B も USB 側を通り、バイトの届き方が DIN の 31250bps から
	// 実機の USB の速さになる。plugin.ini に usb=0 と書けば DIN に戻る
	bool usb = true;
	if (const std::string local = smu2000::config_dir(); !local.empty())
		if (std::FILE *f = std::fopen(smu2000::join(local, "plugin.ini").c_str(), "rb")) {
			char line[256];
			while (std::fgets(line, sizeof(line), f)) {
				if (!std::strncmp(line, "threaded=", 9))
					threaded = line[9] != '0';
				if (!std::strncmp(line, "usb=", 4))
					usb = line[4] != '0';
			}
			std::fclose(f);
		}
	// 一覧やエディタで音色の名前と楽器の絵を利用者の ROM から読む（xg/voices.h）。
	// gui.exe と同じ
	ui::xgui::set_voice_rom(mu->program_rom());
	mu->set_usb_host(usb);
	logf(usb ? "MIDI は USB の口（A-D の 64 パート）" : "plugin.ini: usb=0（DIN の口 A・B だけ）");
	mu->set_threaded(threaded);
	if (!threaded)
		logf("plugin.ini: threaded=0（スレーブを別スレッドにしない）");
	// gui / live が残した設定で起動する。**読むだけで書かない。**VST3 の中で
	// 変えたものは DAW のプロジェクトに残るし、何枚も挿されたときに
	// 同じファイルを取り合わずに済む
	if (nvram::load(*mu))
		logf("設定: %s", nvram::path(*mu).c_str());

	// 鍵は起動に使うワーク RAM も混ぜるので、reset() の前に作る
	const u64 boot_key = bootcache::key(*mu);
	mu->reset();

	// 前に起動し切った姿を取ってあれば、そこから始める（bootcache.h）。
	// 回した結果と 1 ビットも違わないので音は同じで、DAW に何枚挿しても
	// そのたびに黙ることが無くなる。
	// **reset() のあとで読むこと**（タイマが揃っていないと形が合わない）
	std::string boot_from;
	if (bootcache::load(*mu, boot_key, &boot_from)) {
		// **Name the one that was actually read.** The copy baked into the
		// bundle (AUv3) and the one under the settings directory are different
		// files, and logging path() names whichever was not opened
		logf("起動: 前の写しから（%s）", boot_from.c_str());
		m_mu = mu;
		m_message = warn.empty() ? std::string("ROM: ") + dir
		                         : std::string("ROM: ") + dir + "\n警告: " + warn;
		m_state.store(status::ready, std::memory_order_release);
		ui::driver::publish_now(*m_mu, m_bridge, true, nullptr);
		return;
	}

	ui::driver::publish_message(m_bridge, "MU2000 起動中");

	// 起動を待つ。ここを待たずに MIDI を流すと音色指定が全部捨てられる
	const auto t0 = std::chrono::steady_clock::now();
	const int64_t limit = int64_t(30.0 * NATIVE_RATE);
	int64_t i = 0;
	for (; i < limit; i++) {
		if (!(i & 4095) && m_abort.load(std::memory_order_relaxed)) {
			delete mu;
			return;
		}
		if (mu->midi_ready())
			break;
		s32 l = 0, r = 0;
		mu->run_sample(l, r);
	}
	if (i >= limit) {
		m_message = "MU2000 が起動しなかった（ROM が壊れている可能性）";
		logf("%s", m_message.c_str());
		delete mu;
		m_state.store(status::failed, std::memory_order_release);
		return;
	}

	const double wall = std::chrono::duration<double>(
	    std::chrono::steady_clock::now() - t0).count();
	logf("起動: 音 %.2f 秒ぶん / 実時間 %.2f 秒", double(i) / NATIVE_RATE, wall);
	// 次からはここまでを飛ばせるように残す
	if (bootcache::save(*mu, boot_key))
		logf("起動の写しを残した: %s", bootcache::path(boot_key).c_str());

	m_mu = mu;
	m_message = warn.empty() ? std::string("ROM: ") + dir
	                         : std::string("ROM: ") + dir + "\n警告: " + warn;
	// A restore that arrived before the machine came up is kept in
	// m_deferred_state and applied by the first fill() (apply_deferred_state),
	// which is also where the m_machine lock is already held
	m_state.store(status::ready, std::memory_order_release);
	ui::driver::publish_now(*m_mu, m_bridge, true, nullptr);
}


// ---- 標本化周波数の変換

void engine::build_table()
{
	m_tab.resize(size_t(HALF) * STEPS + 2);
	for (size_t k = 0; k < m_tab.size(); k++) {
		const double d = double(k) / STEPS;             // 中心からの距離
		const double x = M_PI * d;
		const double sinc = (k == 0) ? 1.0 : std::sin(x) / x;
		// ブラックマン窓。TAPS 本で阻止域 -74dB くらい
		const double t = (d + HALF) / double(TAPS);
		const double w = 0.42 - 0.5 * std::cos(2.0 * M_PI * t)
		                      + 0.08 * std::cos(4.0 * M_PI * t);
		m_tab[k] = float(sinc * w);
	}
}

void engine::set_output_rate(double rate)
{
	// 変換器の入れ物を作り直すので、音声スレッドと重ならないようにする
	std::lock_guard<std::mutex> lock(m_machine);
	if (rate <= 0.0)
		rate = NATIVE_RATE;
	m_direct = std::fabs(rate - NATIVE_RATE) < 1e-6;
	m_step   = NATIVE_RATE / rate;
	// 上へ変換するときは入力のナイキストまで通す。
	// 下へ変換するときは出力のナイキストで切らないと折り返す
	m_cutoff = std::min(1.0, rate / NATIVE_RATE) * 0.955;
	// 音源側は「必要な先の音」をその場で作れるので、変換に先読みの遅れは無い
	m_latency = 0;
	m_in_rs.configure(rate, NATIVE_RATE);
	m_in_w = m_in_r = 0;
	flush_resampler();
}

void engine::flush_resampler()
{
	std::memset(m_ring_l, 0, sizeof(m_ring_l));
	std::memset(m_ring_r, 0, sizeof(m_ring_r));
	m_written = 0;
	m_pos     = 0.0;
}

void engine::one_sample(float &l, float &r)
{
	s32 li = 0, ri = 0;
	// A/D INPUT。溜めが空なら無音（入力の変換器の先読みの分だけ、頭が少し欠ける）
	if (m_in_r != m_in_w) {
		m_mu->set_audio_input(m_in_q[m_in_r * 2], m_in_q[m_in_r * 2 + 1]);
		m_in_r = (m_in_r + 1) & IN_MASK;
	} else {
		m_mu->set_audio_input(0, 0);
	}
	m_mu->run_sample(li, ri);
	const float k = 1.0f / float(mu2000::DAC_FULL_SCALE);
	l = std::clamp(float(li) * k, -1.0f, 1.0f);
	r = std::clamp(float(ri) * k, -1.0f, 1.0f);
}


void engine::midi(const uint8_t *bytes, size_t n, int port)
{
	if (port < 0 || port >= mu2000::MIDI_PORTS)
		port = 0;
	const status s = state();
	if (s == status::failed)
		return;
	if (s == status::ready) {
		std::unique_lock<std::mutex> lock(m_machine, std::try_to_lock);
		if (lock.owns_lock()) {
			// 溜まっていた分を先に流して、順番を保つ
			for (uint8_t b : m_pending[port]) {
				m_mu->midi_in(b, port);
				m_drv.watch(b, port);
			}
			m_pending[port].clear();
			for (size_t i = 0; i < n; i++) {
				m_mu->midi_in(bytes[i], port);
				m_drv.watch(bytes[i], port);
			}
			return;
		}
	}
	// 起動待ちか、機械を他が使っている。あふれるようなら捨てる
	std::vector<uint8_t> &pending = m_pending[port];
	if (pending.size() + n > 65536)
		return;
	pending.insert(pending.end(), bytes, bytes + n);
}

void engine::all_notes_off(const uint16_t *mask, int ports)
{
	for (int port = 0; port < ports && port < mu2000::MIDI_PORTS; port++)
		for (int ch = 0; ch < 16; ch++) {
			if (!((mask[port] >> ch) & 1))
				continue;
			const uint8_t msg[6] = { uint8_t(0xb0 | ch), 120, 0,
			                         uint8_t(0xb0 | ch), 123, 0 };
			midi(msg, sizeof(msg), port);
		}
}


// ホストの周波数の入力を 44100 に直して溜める。溢れる分は捨てる
void engine::push_input(const float *in_l, const float *in_r, int n)
{
	if (!in_l || n <= 0)
		return;
	if (!in_r)
		in_r = in_l;
	for (int at = 0; at < n;) {
		const int k = std::min(1024, n - at);
		m_in_stage.resize(size_t(k) * 2);
		for (int i = 0; i < k; i++) {
			m_in_stage[size_t(i) * 2]     = s16(std::lround(std::clamp(in_l[at + i], -1.0f, 1.0f) * 32767.0f));
			m_in_stage[size_t(i) * 2 + 1] = s16(std::lround(std::clamp(in_r[at + i], -1.0f, 1.0f) * 32767.0f));
		}
		m_in_rs.push(m_in_stage.data(), k);
		at += k;
		const int out = m_in_rs.output_available();
		if (out <= 0)
			continue;
		m_in_conv.resize(size_t(out) * 2);
		m_in_rs.pull(m_in_conv.data(), out);
		for (int i = 0; i < out; i++) {
			const int next = (m_in_w + 1) & IN_MASK;
			if (next == m_in_r)
				break;
			m_in_q[m_in_w * 2]     = s16(std::lround(std::clamp(m_in_conv[size_t(i) * 2], -1.0f, 1.0f) * 32767.0f));
			m_in_q[m_in_w * 2 + 1] = s16(std::lround(std::clamp(m_in_conv[size_t(i) * 2 + 1], -1.0f, 1.0f) * 32767.0f));
			m_in_w = next;
		}
	}
}

// ---- MIDI OUT: the machine's own OUT jack.
//
// What pump_out() passes by is collected here; on overflow the oldest goes.
// Written and read on the audio thread, so no lock
void engine::tx_push(uint8_t v)
{
	const int next = (m_tx_w + 1) & TX_MASK;
	if (next == m_tx_r)
		m_tx_r = (m_tx_r + 1) & TX_MASK;
	m_tx[m_tx_w] = v;
	m_tx_w = next;
}

size_t engine::midi_out(uint8_t *dst, size_t max)
{
	if (!dst || !max)
		return 0;
	size_t n = 0;
	while (n < max && m_tx_r != m_tx_w) {
		dst[n++] = m_tx[m_tx_r];
		m_tx_r = (m_tx_r + 1) & TX_MASK;
	}
	return n;
}

void engine::fill(float *left, float *right, int n, const float *in_l, const float *in_r)
{
	if (n <= 0)
		return;
	// 音声スレッドは待たない。保存などで機械が使われていれば、この区間は無音
	std::unique_lock<std::mutex> lock(m_machine, std::try_to_lock);
	if (!lock.owns_lock() || state() != status::ready) {
		std::memset(left,  0, size_t(n) * sizeof(float));
		std::memset(right, 0, size_t(n) * sizeof(float));
		if (lock.owns_lock())
			push_input(in_l, in_r, n);
		return;
	}
	push_input(in_l, in_r, n);
	apply_deferred_state();

	m_drv.apply_buttons(*m_mu, m_bridge);
	m_drv.pump_midi(*m_mu, m_bridge);
	m_drv.pump_wheel(*m_mu, m_bridge);

	for (int port = 0; port < 2; port++) {
		for (uint8_t b : m_pending[port]) {
			m_mu->midi_in(b, port);
			m_drv.watch(b, port);
		}
		m_pending[port].clear();
	}

	if (m_direct) {
		for (int i = 0; i < n; i++)
			one_sample(left[i], right[i]);
		m_drv.pump_out(*m_mu, m_bridge, [this](u8 v) { tx_push(v); });
		m_drv.publish(*m_mu, m_bridge, u32(n), u32(NATIVE_RATE), true, nullptr);
		return;
	}

	for (int i = 0; i < n; i++) {
		const int64_t centre = int64_t(std::floor(m_pos));
		// 畳み込みに要る一番先のサンプルまで作る
		while (m_written <= centre + HALF) {
			float l, r;
			one_sample(l, r);
			m_ring_l[m_written & RMASK] = l;
			m_ring_r[m_written & RMASK] = r;
			m_written++;
		}

		double al = 0.0, ar = 0.0, sum = 0.0;
		for (int k = -HALF + 1; k <= HALF; k++) {
			const int64_t idx = centre + k;
			const double d  = std::fabs((m_pos - double(idx)) * m_cutoff);
			const double fx = d * STEPS;
			const size_t j  = size_t(fx);
			if (j + 1 >= m_tab.size())
				continue;
			const double t = fx - double(j);
			const double h = m_tab[j] + (m_tab[j + 1] - m_tab[j]) * t;
			al  += h * m_ring_l[idx & RMASK];
			ar  += h * m_ring_r[idx & RMASK];
			sum += h;
		}
		if (sum > 1e-9) { al /= sum; ar /= sum; }
		left[i]  = std::clamp(float(al), -1.0f, 1.0f);
		right[i] = std::clamp(float(ar), -1.0f, 1.0f);
		m_pos += m_step;
	}

	// firmware が MIDI OUT から送り出したもの（画面の問い合わせの返事）
	m_drv.pump_out(*m_mu, m_bridge, [this](u8 v) { tx_push(v); });
	m_drv.publish(*m_mu, m_bridge, u32(n), u32(NATIVE_RATE), true, nullptr);

	// 桁が落ちる前に原点を戻す。RING の倍数だけずらせば環の並びは変わらない
	if (m_pos > double(1 << 28)) {
		const int64_t base = (int64_t(m_pos) - HALF) & ~int64_t(RMASK);
		m_pos     -= double(base);
		m_written -= base;
	}
}



// ---- 状態の保存と復元
//
// どれも m_machine を取ってその場でやる。音声スレッドは取れない区間を無音にして待たない

void engine::apply_deferred_state()
{
	if (m_deferred_state.empty() || state() != status::ready || !m_mu)
		return;
	std::string err;
	if (!m_mu->load_state(m_deferred_state.data(), m_deferred_state.size(), err))
		log_line(("状態を読み戻せない: " + err).c_str());
	m_deferred_state.clear();
	m_deferred_state.shrink_to_fit();
}

std::vector<uint8_t> engine::save_state()
{
	std::lock_guard<std::mutex> lock(m_machine);
	if (state() != status::ready || !m_mu) {
		// 起動が終わる前に保存されたら、戻す予定だった状態をそのまま返す
		return m_deferred_state;
	}
	apply_deferred_state();
	return m_mu->save_state();
}

bool engine::load_state(const uint8_t *p, size_t n)
{
	if (!p || !n)
		return false;
	std::lock_guard<std::mutex> lock(m_machine);
	if (state() == status::failed)
		return false;
	if (state() != status::ready || !m_mu) {
		m_deferred_state.assign(p, p + n);
		return true;
	}
	m_deferred_state.clear();
	std::string err;
	const bool ok = m_mu->load_state(p, n, err);
	if (!ok)
		log_line(("状態を読み戻せない: " + err).c_str());
	return ok;
}


// ---- 機械に触る仕事（SmartMedia の差し替えなど）

bool engine::on_machine(const std::function<void(mu2000 &)> &fn)
{
	std::lock_guard<std::mutex> lock(m_machine);
	if (state() != status::ready || !m_mu)
		return false;
	fn(*m_mu);
	return true;
}

std::string engine::card_path() const
{
	std::lock_guard<std::mutex> lock(m_card_mutex);
	return m_card_path;
}

void engine::card_flush()
{
	const std::string path = card_path();
	if (path.empty() || !m_mu)
		return;
	std::vector<smartmedia::block> blocks;
	const std::function<void(mu2000 &)> take = [&](mu2000 &m) { m.card().take_dirty_blocks(blocks); };
	if (state() == status::ready)
		on_machine(take);
	else
		take(*m_mu);
	std::string err;
	if (!smartmedia::write_blocks(path, blocks, err))
		log_line(err.c_str());
}

void engine::card_eject()
{
	card_flush();
	on_machine([](mu2000 &m) { m.card().eject(); });
	std::lock_guard<std::mutex> lock(m_card_mutex);
	m_card_path.clear();
}

bool engine::card_insert(const std::string &path, std::string &err)
{
	auto card = std::make_shared<smartmedia>();
	if (!card->load(path, err))
		return false;
	if (state() != status::ready) {
		err = "まだ起動していない";
		return false;
	}
	card_flush();
	if (!on_machine([card](mu2000 &m) { m.card() = std::move(*card); })) {
		err = "カードを差せなかった（音声スレッドが応じない）";
		return false;
	}
	std::lock_guard<std::mutex> lock(m_card_mutex);
	m_card_path = path;
	return true;
}

} // namespace vst3
} // namespace smu2000
