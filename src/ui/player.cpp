// license:BSD-3-Clause

#include "player.h"
#include "bend_thinner.h"

#include <algorithm>
#include <chrono>
#include <random>

// timeBeginPeriod() only exists on Windows, as the way to ask the scheduler for
// a 1 ms timer resolution. macOS already sleeps finely enough, so the call is
// simply not made there
#if defined(_WIN32)
#include <windows.h>
// winmm's header is what declares them, and a lean windows.h leaves it out.
// CMake defines WIN32_LEAN_AND_MEAN tree-wide; the Makefile does not, which is
// why only the CMake build missed it. midi_in.cpp includes it either way.
#include <mmsystem.h>
#endif

namespace ui {

namespace {

// 鳴りっぱなしを消す。口 A と口 B の 16 チャンネルぶん
void all_off(bridge &br)
{
	for (int ch = 0; ch < 16; ch++) {
		const u8 msg[3] = { u8(0xb0 | ch), 0x7b, 0x00 };   // オールノートオフ
		const u8 sus[3] = { u8(0xb0 | ch), 0x40, 0x00 };   // ダンパも離す
		br.send(msg, 3);
		br.send(sus, 3);
		br.send_b(msg, 3);
		br.send_b(sus, 3);
	}
}

std::string file_name(const std::string &path)
{
	const size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

// 曲の終わりから次の曲までの間（音の尾を少し待つ）
constexpr double GAP = 1.0;

} // namespace


// ---- 一覧

std::vector<player::entry> player::list() const
{
	std::lock_guard<std::mutex> lock(m_lock);
	return m_list;
}

std::string player::name() const
{
	std::lock_guard<std::mutex> lock(m_lock);
	const int c = m_cur.load(std::memory_order_relaxed);
	if (c < 0 || c >= int(m_list.size()))
		return std::string();
	return m_list[size_t(c)].title.empty() ? m_list[size_t(c)].name : m_list[size_t(c)].title;
}

// 一覧に足す曲を作るのは 1 つだけ。長さと口の数もここで決まり、鳴らす側（run）が
// 状態行に出すのと同じ値が入る
static player::entry make_entry(const std::string &path, const std::string &name,
                                const std::string &title,
                                const std::vector<smf::event> &evs)
{
	player::entry e;
	e.path = path;
	e.name = name;
	e.title = title;
	e.length = evs.back().time;
	for (const smf::event &x : evs)
		e.ports = std::max(e.ports, int(x.port) + 1);
	return e;
}

bool player::add(const std::string &path, std::string &err, int *index)
{
	{
		std::lock_guard<std::mutex> lock(m_lock);
		for (size_t i = 0; i < m_list.size(); i++)
			if (m_list[i].path == path) {
				if (index)
					*index = int(i);
				return true;
			}
	}
	std::vector<smf::event> evs;
	smf::song_meta meta;
	if (!smf::load(path, evs, err, &meta))
		return false;
	if (evs.empty()) {
		err = "中身が空";
		return false;
	}
	entry e = make_entry(path, file_name(path), meta.title, evs);
	std::lock_guard<std::mutex> lock(m_lock);
	m_list.push_back(std::move(e));
	m_order.clear();
	if (index)
		*index = int(m_list.size()) - 1;
	return true;
}

bool player::add_from_memory(const u8 *data, size_t size, const std::string &name,
                             std::string &err, int *index)
{
	std::vector<smf::event> evs;
	smf::song_meta meta;
	if (!smf::load_from_memory(data, size, evs, err, &meta))
		return false;
	if (evs.empty()) {
		err = "中身が空";
		return false;
	}
	entry e = make_entry(std::string(), name, meta.title, evs);
	e.evs = std::move(evs);   // path が無いぶん、ここが中身の唯一の持ち物
	std::lock_guard<std::mutex> lock(m_lock);
	m_list.push_back(std::move(e));
	m_order.clear();
	if (index)
		*index = int(m_list.size()) - 1;
	return true;
}

void player::remove(int index)
{
	bool was_current = false;
	{
		std::lock_guard<std::mutex> lock(m_lock);
		if (index < 0 || index >= int(m_list.size()))
			return;
		const int c = m_cur.load(std::memory_order_relaxed);
		was_current = c == index;
		if (!was_current) {
			m_list.erase(m_list.begin() + index);
			m_order.clear();
			if (c > index)
				m_cur.store(c - 1, std::memory_order_relaxed);
			return;
		}
	}
	// 鳴らしている曲を消す: 先に止める（止めるのは鍵の外で）
	stop();
	std::lock_guard<std::mutex> lock(m_lock);
	if (index < int(m_list.size()))
		m_list.erase(m_list.begin() + index);
	m_order.clear();
	m_cur.store(m_list.empty() ? -1 : std::min(index, int(m_list.size()) - 1), std::memory_order_relaxed);
}

void player::move(int from, int to)
{
	std::lock_guard<std::mutex> lock(m_lock);
	const int n = int(m_list.size());
	if (from < 0 || from >= n || to < 0 || to >= n || from == to)
		return;
	entry e = std::move(m_list[size_t(from)]);
	m_list.erase(m_list.begin() + from);
	m_list.insert(m_list.begin() + to, std::move(e));
	m_order.clear();
	// 鳴らしている曲の番号を付け替える
	int c = m_cur.load(std::memory_order_relaxed);
	if (c == from)
		c = to;
	else if (from < c && c <= to)
		c--;
	else if (to <= c && c < from)
		c++;
	m_cur.store(c, std::memory_order_relaxed);
}

void player::clear()
{
	stop();
	std::lock_guard<std::mutex> lock(m_lock);
	m_list.clear();
	m_order.clear();
	m_cur.store(-1, std::memory_order_relaxed);
}

void player::reshuffle(int first)
{
	m_order.resize(m_list.size());
	for (size_t i = 0; i < m_order.size(); i++)
		m_order[i] = int(i);
	static std::mt19937 rng{ std::random_device{}() };
	std::shuffle(m_order.begin(), m_order.end(), rng);
	// いまの曲から始める（同じ曲が続けて鳴らないように）
	const auto it = std::find(m_order.begin(), m_order.end(), first);
	if (it != m_order.end())
		std::iter_swap(m_order.begin(), it);
}

int player::after(int index, int step)
{
	const int n = int(m_list.size());
	if (n == 0)
		return -1;
	const loop_mode mode = loop();
	if (mode == loop_mode::shuffle) {
		if (m_order.size() != m_list.size())
			reshuffle(index);
		const auto it = std::find(m_order.begin(), m_order.end(), index);
		int k = it == m_order.end() ? 0 : int(it - m_order.begin()) + step;
		if (k >= n) {                      // 一回りした: 混ぜ直す
			reshuffle(-1);
			k = 0;
		}
		if (k < 0)
			k = n - 1;
		return m_order[size_t(k)];
	}
	int k = index + step;
	if (k >= n)
		return mode == loop_mode::none ? -1 : 0;
	if (k < 0)
		return mode == loop_mode::none ? 0 : n - 1;
	return k;
}


// ---- 操作

bool player::start(const std::string &path, bridge &br, std::string &err)
{
	int index = -1;
	if (!add(path, err, &index))
		return false;
	play(index, br);
	return true;
}

bool player::start_from_memory(const u8 *data, size_t size, const std::string &name,
                               bridge &br, std::string &err)
{
	int index = -1;
	if (!add_from_memory(data, size, name, err, &index))
		return false;
	play(index, br);
	return true;
}

void player::launch(bridge &br)
{
	m_quit.store(false);
	m_playing.store(true, std::memory_order_release);
	m_thread = std::thread([this, &br] { run(br); });
}

void player::play(int index, bridge &br)
{
	{
		std::lock_guard<std::mutex> lock(m_lock);
		if (index < 0 || index >= int(m_list.size()))
			return;
		// 長さと口の数は、流す糸が曲を開く前でも聞かれる（「再生: …（N 秒）」の表示）。一覧に控えたものを先に入れておく
		m_len.store(m_list[size_t(index)].length, std::memory_order_relaxed);
		m_ports_used.store(m_list[size_t(index)].ports, std::memory_order_relaxed);
	}
	m_paused.store(false, std::memory_order_relaxed);
	if (m_playing.load(std::memory_order_acquire) && m_thread.joinable()) {
		m_jump.store(index, std::memory_order_release);     // 動いている糸に曲を替えさせる
		return;
	}
	if (m_thread.joinable())
		m_thread.join();
	m_cur.store(index, std::memory_order_relaxed);
	m_jump.store(-1);
	m_step.store(0);
	m_seek.store(-1.0);
	launch(br);
}

void player::stop()
{
	m_quit.store(true, std::memory_order_release);
	if (m_thread.joinable())
		m_thread.join();
	m_playing.store(false, std::memory_order_release);
	m_paused.store(false, std::memory_order_relaxed);
	m_state.store(int(state::stopped), std::memory_order_relaxed);
	m_pos.store(0);
}

void player::next() { if (playing()) m_step.store(1, std::memory_order_release); }

void player::prev()
{
	if (!playing())
		return;
	if (position() > 2.0)
		m_seek.store(0.0, std::memory_order_release);
	else
		m_step.store(-1, std::memory_order_release);
}

void player::pause(bool on) { if (playing()) m_paused.store(on, std::memory_order_release); }

void player::seek(double sec)
{
	if (playing())
		m_seek.store(std::max(0.0, sec), std::memory_order_release);
}

bool player::beat(int &bar, int &beat_, double &bpm) const
{
	if (!playing())
		return false;
	std::lock_guard<std::mutex> lock(m_lock);
	m_meta.bar_beat(position(), bar, beat_, bpm);
	return true;
}


// ---- 流す糸。曲を次々に開いて流す。止める・曲を替える・飛ぶは、旗を見て自分でやる

void player::run(bridge &br)
{
	// 1 ミリ秒で起きられるようにしておく。既定の 15.6 ミリ秒だと
	// 音符の頭がばらつく
#if defined(_WIN32)
	timeBeginPeriod(1);
#endif
	using clock = std::chrono::steady_clock;
	auto send = [&br](int to, const u8 *d, size_t n) {
		if (to == 0)      br.send(d, n);
		else if (to > 0)  br.send_port(to, d, n);
	};
	auto quit = [this] { return m_quit.load(std::memory_order_acquire); };
	// 旗を見ながら待つ（止める・曲を替えるが来たら早く戻る）
	auto wait = [&](double sec) {
		const auto until = clock::now() + std::chrono::duration<double>(sec);
		while (clock::now() < until && !quit() && m_jump.load() < 0 && m_step.load() == 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
	};

	while (!quit()) {
		// ---- 曲を開く
		std::string path;
		std::vector<smf::event> events;
		smf::song_meta meta;
		{
			std::lock_guard<std::mutex> lock(m_lock);
			const int c = m_cur.load(std::memory_order_relaxed);
			if (c < 0 || c >= int(m_list.size()))
				break;
			const entry &e = m_list[size_t(c)];
			path = e.path;
			events = e.evs;   // ファイルでない曲（iOS の選択、wasm）は最初から入っている
		}
		std::string err;
		const bool ok = (events.empty() ? smf::load(path, events, err, &meta)
		                                : true) && !events.empty();
		int ports = 1;
		for (const smf::event &e : events)
			ports = std::max(ports, int(e.port) + 1);
		{
			std::lock_guard<std::mutex> lock(m_lock);
			m_meta = meta;
		}
		m_ports_used.store(ports, std::memory_order_relaxed);
		m_len.store(ok ? events.back().time : 0.0, std::memory_order_relaxed);
		m_pos.store(0.0, std::memory_order_relaxed);
		m_seek.store(-1.0);
		m_state.store(int(state::playing), std::memory_order_relaxed);

		// ---- 流す
		bend_thinner thinner;   // set_thin_bends のとき
		size_t at = 0;
		double pos = 0.0;
		auto last = clock::now();
		bool was_paused = false;
		int step = 0, jump = -1;
		while (ok && !quit()) {
			jump = m_jump.exchange(-1);
			step = m_step.exchange(0);
			if (jump >= 0 || step)
				break;

			// 頭出し: 鳴っている音を止め、行き先までの設定を追いかけてから続きを流す
			const double to_sec = m_seek.exchange(-1.0);
			if (to_sec >= 0.0) {
				all_off(br);
				const double target = std::min(to_sec, events.back().time);
				const bool fold = m_fold.load(std::memory_order_relaxed), usb = m_usb.load(std::memory_order_relaxed);
				size_t bytes = 0;
				for (const smf::event &e : smf::chase(events, target)) {
					send(smf::mu_port(e.port, fold, usb, br.board_port()), e.bytes.data(), e.bytes.size());
					bytes += e.bytes.size();
				}
				at = size_t(std::lower_bound(events.begin(), events.end(), target,
				                             [](const smf::event &e, double t) { return e.time < t; }) - events.begin());
				pos = target;
				m_pos.store(pos, std::memory_order_relaxed);
				// 音源が読み終えるまで待つ（待たずに続きを流すと、続きの音符が設定の後ろに並んで遅れる）
				m_state.store(int(state::chasing), std::memory_order_relaxed);
				wait(double(bytes) / (usb ? 10000.0 : 3125.0) + 0.05);
				m_state.store(int(m_paused.load() ? state::paused : state::playing), std::memory_order_relaxed);
				thinner = bend_thinner();
				last = clock::now();
				continue;
			}

			// 一時停止: 時計を止める。入るときに鳴っている音を止める
			if (m_paused.load(std::memory_order_acquire)) {
				if (!was_paused) {
					all_off(br);
					was_paused = true;
					m_state.store(int(state::paused), std::memory_order_relaxed);
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				last = clock::now();
				continue;
			}
			if (was_paused) {
				was_paused = false;
				m_state.store(int(state::playing), std::memory_order_relaxed);
			}

			const auto now = clock::now();
			pos += std::chrono::duration<double>(now - last).count();
			last = now;
			m_pos.store(std::min(pos, events.back().time), std::memory_order_relaxed);

			// 来ている分をまとめて送る。トラックの出し先（SMF のポート指定かトラック名）が 0-3 なら口 A-D。
			// USB の口（gui の既定）なら 4 口ともそのまま。DIN の口だけ（--host-midi）なら、口 3・4 は
			// 選んだ扱いに従う（A・B に重ねるか、鳴らさない）
			const bool fold = m_fold.load(std::memory_order_relaxed);
			const bool usb = m_usb.load(std::memory_order_relaxed);
			const bool thin = m_thin.load(std::memory_order_relaxed);
			while (at < events.size() && events[at].time <= pos) {
				const smf::event &e = events[at];
				const int to = smf::mu_port(e.port, fold, usb, br.board_port());
				at++;
				if (thin)
					thinner.event(to, e.bytes.data(), e.bytes.size(), e.time, send);
				else
					send(to, e.bytes.data(), e.bytes.size());
			}
			// 持っているベンドは GAP たったら送る（途中で切りにしたとき・曲の終わりはすぐ）
			const bool done = at >= events.size();
			thinner.tick(pos, !thin || done, send);
			if (done)
				break;

			// 次まで待つ。長く待ちすぎないように刻む
			const double until = events[at].time - pos;
			std::this_thread::sleep_for(std::chrono::milliseconds(until > 0.010 ? 5 : 1));
		}
		all_off(br);
		if (quit())
			break;

		// ---- 次の曲を決める
		int next_index = -1;
		{
			std::lock_guard<std::mutex> lock(m_lock);
			const int c = m_cur.load(std::memory_order_relaxed);
			if (jump >= 0)
				next_index = jump < int(m_list.size()) ? jump : -1;
			else if (step)
				next_index = after(c, step);
			else if (loop() == loop_mode::one)
				next_index = c;
			else
				next_index = after(c, 1);
		}
		if (next_index < 0)
			break;
		// 曲が自分で終わったときは、音の尾を少し待ってから次へ（操作で替えたときはすぐ）
		if (jump < 0 && !step) {
			wait(GAP);
			if (quit())
				break;
			const int j = m_jump.exchange(-1);
			if (j >= 0)
				next_index = j;
		}
		m_cur.store(next_index, std::memory_order_relaxed);
	}

#if defined(_WIN32)
	timeEndPeriod(1);
#endif
	m_state.store(int(state::stopped), std::memory_order_relaxed);
	m_pos.store(0);
	m_playing.store(false, std::memory_order_release);
}

} // namespace ui
