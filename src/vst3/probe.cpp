// license:BSD-3-Clause
//
// VST3 プラグインを DAW 無しで動かしてみる小さなホスト。
//
//   vst3probe <S-MU2000.vst3 の DLL>                         名乗りだけ見る
//   vst3probe <DLL> <MIDI ファイル> <出力 wav> [--rate 48000] [--block 512] [--adc-sine]
//
// --adc-sine は A/D INPUT（補助の入力バス）に 440Hz の正弦を流す（入力の道が落ちないかを見る）
// --automation は XG の値のパラメータ（src/vst3/automation.h）を試す: 一覧、値を送って音源に入ったか、
//   状態の保存と復元、機械まるごとの状態を抜いて XG の値の控えだけで戻るか
// --view のとき、画面で値を触ると beginEdit / performEdit / endEdit が出る
// --data-midi は VSTHost 1.58 のまねで、コントロールチェンジやプログラムチェンジも
//   パラメータではなく DataEvent（システムエクスクルーシブ扱い）で、しかも 3 byte に
//   詰めて渡す。付けない時と同じ音が出れば、そういうホストでも正しく鳴る
//
// DAW に入れる前にここで確かめる。工場が名乗るか、インターフェースが揃うか、
// MIDI を受けて音が出るか、標本化周波数の変換が効いているか。

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstunits.h"

#include "probe_host.h"
#include "smf.h"

#include "compat/console.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// A VST3 module is opened differently on each platform: a Windows DLL by
// LoadLibrary, a .vst3 directory by CFBundle. Both ends the same way, with a
// pointer to GetPluginFactory
#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

// The host window stand-in (probe_host.h). Named here so the code below reads
// the same on both platforms
using smu2000::vst3::probe_host;
using smu2000::vst3::probe_host_create;

namespace {

// A monotonic millisecond clock.
//
// This used to be GetTickCount, which only exists on Windows and only counts to
// 32 bits. steady_clock is QueryPerformanceCounter underneath there and
// mach_absolute_time here, so one clock serves both and it does not wrap
long long now_ms()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

void sleep_ms(int ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void print16(const char16 *s)
{
	for (int i = 0; i < 64 && s[i]; i++)
		std::putchar(s[i] < 128 ? char(s[i]) : '?');
}

// ---- ホスト側の入れ物。プラグインに渡すためだけの最小限

class param_queue : public IParamValueQueue
{
public:
	param_queue(ParamID id) : m_id(id) {}
	tresult PLUGIN_API queryInterface(const TUID, void **obj) override
	{ *obj = this; return kResultOk; }
	uint32 PLUGIN_API addRef() override  { return 1; }
	uint32 PLUGIN_API release() override { return 1; }

	ParamID PLUGIN_API getParameterId() override { return m_id; }
	int32 PLUGIN_API getPointCount() override { return int32(m_pts.size()); }
	tresult PLUGIN_API getPoint(int32 i, int32 &off, ParamValue &v) override
	{
		if (i < 0 || i >= int32(m_pts.size())) return kResultFalse;
		off = m_pts[i].first; v = m_pts[i].second; return kResultOk;
	}
	tresult PLUGIN_API addPoint(int32 off, ParamValue v, int32 &idx) override
	{ idx = int32(m_pts.size()); m_pts.push_back({ off, v }); return kResultOk; }

	void add(int32 off, ParamValue v) { m_pts.push_back({ off, v }); }
	void clear() { m_pts.clear(); }
	bool empty() const { return m_pts.empty(); }

private:
	ParamID m_id;
	std::vector<std::pair<int32, ParamValue>> m_pts;
};

class param_changes : public IParameterChanges
{
public:
	tresult PLUGIN_API queryInterface(const TUID, void **obj) override
	{ *obj = this; return kResultOk; }
	uint32 PLUGIN_API addRef() override  { return 1; }
	uint32 PLUGIN_API release() override { return 1; }

	int32 PLUGIN_API getParameterCount() override { return int32(m_live.size()); }
	IParamValueQueue *PLUGIN_API getParameterData(int32 i) override
	{ return (i >= 0 && i < int32(m_live.size())) ? m_live[i] : nullptr; }
	IParamValueQueue *PLUGIN_API addParameterData(const ParamID &id, int32 &idx) override
	{ idx = 0; return get(id); }

	param_queue *get(ParamID id)
	{
		auto it = m_all.find(id);
		if (it == m_all.end())
			it = m_all.emplace(id, new param_queue(id)).first;
		param_queue *q = it->second;
		if (q->empty())
			m_live.push_back(q);
		return q;
	}
	void clear()
	{
		for (param_queue *q : m_live) q->clear();
		m_live.clear();
	}

private:
	std::map<ParamID, param_queue *> m_all;
	std::vector<param_queue *> m_live;
};

class event_list : public IEventList
{
public:
	tresult PLUGIN_API queryInterface(const TUID, void **obj) override
	{ *obj = this; return kResultOk; }
	uint32 PLUGIN_API addRef() override  { return 1; }
	uint32 PLUGIN_API release() override { return 1; }

	int32 PLUGIN_API getEventCount() override { return int32(m_ev.size()); }
	tresult PLUGIN_API getEvent(int32 i, Event &e) override
	{
		if (i < 0 || i >= int32(m_ev.size())) return kResultFalse;
		e = m_ev[i]; return kResultOk;
	}
	tresult PLUGIN_API addEvent(Event &e) override { m_ev.push_back(e); return kResultOk; }

	void clear() { m_ev.clear(); m_sysex.clear(); }
	std::vector<Event> m_ev;
	std::vector<std::vector<uint8>> m_sysex;   // 領域の持ち主
};

void write_wav(const std::string &path, const std::vector<int16_t> &pcm, uint32_t rate)
{
	std::FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) return;
	const uint32_t bytes = uint32_t(pcm.size() * 2);
	auto u32w = [&](uint32_t v) { uint8_t b[4] = { uint8_t(v), uint8_t(v >> 8),
	                                               uint8_t(v >> 16), uint8_t(v >> 24) };
	                              std::fwrite(b, 1, 4, f); };
	auto u16w = [&](uint16_t v) { uint8_t b[2] = { uint8_t(v), uint8_t(v >> 8) };
	                              std::fwrite(b, 1, 2, f); };
	std::fwrite("RIFF", 1, 4, f); u32w(36 + bytes); std::fwrite("WAVE", 1, 4, f);
	std::fwrite("fmt ", 1, 4, f); u32w(16); u16w(1); u16w(2);
	u32w(rate); u32w(rate * 4); u16w(4); u16w(16);
	std::fwrite("data", 1, 4, f); u32w(bytes);
	std::fwrite(pcm.data(), 1, bytes, f);
	std::fclose(f);
}

// ---- ホストがやりそうな乱暴を一通りやってみる。
// DAW によって呼ぶ順も回数も違うので、落ちないことをここで確かめておく

// getState / setState の受け皿
class mem_stream : public IBStream
{
public:
	tresult PLUGIN_API queryInterface(const TUID, void **obj) override
	{ *obj = this; return kResultOk; }
	uint32 PLUGIN_API addRef() override  { return 1; }
	uint32 PLUGIN_API release() override { return 1; }

	tresult PLUGIN_API read(void *buf, int32 n, int32 *got) override
	{
		const int32 k = std::min<int32>(n, int32(m_buf.size()) - m_pos);
		if (k > 0) std::memcpy(buf, m_buf.data() + m_pos, size_t(k));
		m_pos += std::max(k, 0);
		if (got) *got = std::max(k, 0);
		return kResultOk;
	}
	tresult PLUGIN_API write(void *buf, int32 n, int32 *put) override
	{
		const uint8 *p = static_cast<const uint8 *>(buf);
		m_buf.insert(m_buf.end(), p, p + n);
		m_pos = int32(m_buf.size());
		if (put) *put = n;
		return kResultOk;
	}
	tresult PLUGIN_API seek(int64 pos, int32 mode, int64 *result) override
	{
		if (mode == kIBSeekSet) m_pos = int32(pos);
		else if (mode == kIBSeekCur) m_pos += int32(pos);
		else m_pos = int32(m_buf.size()) + int32(pos);
		m_pos = std::clamp(m_pos, 0, int32(m_buf.size()));
		if (result) *result = m_pos;
		return kResultOk;
	}
	tresult PLUGIN_API tell(int64 *pos) override
	{ if (pos) *pos = m_pos; return kResultOk; }

	void rewind() { m_pos = 0; }
	size_t size() const { return m_buf.size(); }
	const std::vector<uint8> &bytes() const { return m_buf; }
	void assign(const std::vector<uint8> &b) { m_buf = b; m_pos = 0; }

private:
	std::vector<uint8> m_buf;
	int32 m_pos = 0;
};

// ---- 画面を窓に出してみる。
// ホストのふりをして親ウィンドウを作り、そこへプラグインの画面を貼る。
// 音は出さないが、LCD が動くよう process を実時間で回しておく
//
// The window itself is per platform (probe_host.h); standing in for a host
// otherwise means the same thing on both, so the rest is shared

std::atomic<bool> g_view_quit{false};

// ホストの受け口。プラグインが画面の操作を伝えてきたら書き出す
class log_handler : public IComponentHandler
{
public:
	tresult PLUGIN_API queryInterface(const TUID, void **obj) override { *obj = this; return kResultOk; }
	uint32 PLUGIN_API addRef() override  { return 1; }
	uint32 PLUGIN_API release() override { return 1; }
	tresult PLUGIN_API beginEdit(ParamID id) override
	{ std::printf("  beginEdit %u\n", unsigned(id)); begins++; return kResultOk; }
	tresult PLUGIN_API performEdit(ParamID id, ParamValue v) override
	{ std::printf("  performEdit %u %.4f\n", unsigned(id), v); performs++; return kResultOk; }
	tresult PLUGIN_API endEdit(ParamID id) override
	{ std::printf("  endEdit %u\n", unsigned(id)); ends++; return kResultOk; }
	tresult PLUGIN_API restartComponent(int32 flags) override
	{ std::printf("  restartComponent 0x%x\n", unsigned(flags)); restarts++; return kResultOk; }
	int begins = 0, performs = 0, ends = 0, restarts = 0;
};

int run_view(IComponent *comp, IAudioProcessor *proc, IEditController *ctrl, int seconds)
{
	std::printf("\n---- 画面を出してみる ----\n");
	if (!ctrl) { std::printf("NG: IEditController が無い\n"); return 1; }

	static log_handler handler;
	ctrl->setComponentHandler(&handler);
	IPlugView *view = ctrl->createView(ViewType::kEditor);
	if (!view) { std::printf("NG: 画面を作れない\n"); return 1; }
	std::printf("OK: createView\n");

	std::unique_ptr<probe_host> host(probe_host_create());
	if (view->isPlatformTypeSupported(host->platform_type()) != kResultTrue) {
		std::printf("NG: %s に対応していない\n", host->platform_type());
		view->release();
		return 1;
	}
	ViewRect vr{};
	view->getSize(&vr);
	std::printf("OK: 大きさ %d × %d、伸縮 %s\n", vr.getWidth(), vr.getHeight(),
	            view->canResize() == kResultTrue ? "できる" : "できない");

	if (!host->create(vr.getWidth(), vr.getHeight())) {
		std::printf("NG: 親の窓を作れない\n");
		view->release();
		return 1;
	}
	if (!host->attach(view)) {
		std::printf("NG: attached\n");
		host->destroy();
		view->release();
		return 1;
	}
	std::printf("OK: attached\n");
	host->show();

	// LCD が動くよう、実時間で process を回す
	std::thread pump([&] {
		std::vector<float> l(512), r(512);
		float *ch[2] = { l.data(), r.data() };
		AudioBusBuffers ab{};
		ab.numChannels = 2; ab.channelBuffers32 = ch;
		ProcessData pd{};
		pd.symbolicSampleSize = kSample32;
		pd.numSamples = 512; pd.numOutputs = 1; pd.outputs = &ab;
		while (!g_view_quit.load()) {
			proc->process(pd);
			// 512 / 44100 ≒ 11.6ms
			std::this_thread::sleep_for(std::chrono::milliseconds(11));
		}
	});

	host->pump(seconds);

	g_view_quit.store(true);
	pump.join();

	view->removed();
	std::printf("OK: removed\n");
	host->destroy();
	view->release();
	std::printf("画面の操作の知らせ: beginEdit %d / performEdit %d / endEdit %d\n",
	            handler.begins, handler.performs, handler.ends);
	std::printf("---- 画面はここまで ----\n");
	return handler.begins == handler.ends ? 0 : 1;
}

// ---- XG の値のパラメータ（--automation）

struct instance {
	IComponent *comp = nullptr;
	IAudioProcessor *proc = nullptr;
	IEditController *ctrl = nullptr;
};

bool make_instance(IPluginFactory *fac, const TUID cid, instance &in)
{
	if (fac->createInstance(reinterpret_cast<FIDString>(cid), reinterpret_cast<FIDString>(IComponent::iid.toTUID()),
	                        (void **)&in.comp) != kResultOk || !in.comp)
		return false;
	in.comp->queryInterface(IAudioProcessor::iid, (void **)&in.proc);
	in.comp->queryInterface(IEditController::iid, (void **)&in.ctrl);
	if (!in.proc || !in.ctrl)
		return false;
	in.comp->initialize(nullptr);
	return true;
}

void start_instance(instance &in, double rate, int block)
{
	SpeakerArrangement out_arr = SpeakerArr::kStereo;
	in.proc->setBusArrangements(nullptr, 0, &out_arr, 1);
	in.comp->activateBus(kAudio, kOutput, 0, true);
	ProcessSetup setup{};
	setup.processMode = kRealtime;
	setup.symbolicSampleSize = kSample32;
	setup.maxSamplesPerBlock = block;
	setup.sampleRate = rate;
	in.proc->setupProcessing(setup);
	in.comp->setActive(true);          // 起動が終わるまで待つ
	in.proc->setProcessing(true);
}

void stop_instance(instance &in)
{
	in.proc->setProcessing(false);
	in.comp->setActive(false);
	in.comp->terminate();
	in.proc->release();
	in.ctrl->release();
	in.comp->release();
	in = instance{};
}

// 無音の区間を secs 秒ぶん回す。first のときは changes を最初の区間に渡す
void run_blocks(instance &in, double rate, int block, double secs, param_changes *changes, int repeat_blocks = 1)
{
	std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
	float *ch[2] = { l.data(), r.data() };
	AudioBusBuffers ab{};
	ab.numChannels = 2;
	ab.channelBuffers32 = ch;
	event_list ev;
	param_changes none;
	ProcessData pd{};
	pd.processMode = kRealtime;
	pd.symbolicSampleSize = kSample32;
	pd.numSamples = block;
	pd.numOutputs = 1;
	pd.outputs = &ab;
	pd.inputEvents = &ev;
	const int blocks = int(secs * rate / block);
	for (int k = 0; k < blocks; k++) {
		pd.inputParameterChanges = (changes && k < repeat_blocks) ? changes : &none;
		in.proc->process(pd);
	}
}

// SysEx を 1 区間で流す（インサーションの種類を決めるのに使う）
void send_sysex(instance &in, int block, const std::vector<std::vector<uint8>> &messages)
{
	std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
	float *ch[2] = { l.data(), r.data() };
	AudioBusBuffers ab{};
	ab.numChannels = 2;
	ab.channelBuffers32 = ch;
	event_list ev;
	for (const std::vector<uint8> &m : messages) {
		ev.m_sysex.push_back(m);
	}
	for (std::vector<uint8> &m : ev.m_sysex) {
		Event e{};
		e.type = Event::kDataEvent;
		e.data.type = DataEvent::kMidiSysEx;
		e.data.size = uint32(m.size());
		e.data.bytes = m.data();
		ev.addEvent(e);
	}
	param_changes none;
	ProcessData pd{};
	pd.processMode = kRealtime;
	pd.symbolicSampleSize = kSample32;
	pd.numSamples = block;
	pd.numOutputs = 1;
	pd.outputs = &ab;
	pd.inputEvents = &ev;
	pd.inputParameterChanges = &none;
	in.proc->process(pd);
}

std::string title_of(IEditController *ctrl, int32 index, ParamID &id, ParameterInfo &info)
{
	if (ctrl->getParameterInfo(index, info) != kResultOk)
		return std::string();
	id = info.id;
	std::string t;
	for (int i = 0; i < 128 && info.title[i]; i++)
		t.push_back(info.title[i] < 128 ? char(info.title[i]) : '?');
	return t;
}

int run_automation(IPluginFactory *fac, const TUID cid, double rate, int block)
{
	std::printf("\n---- XG の値のパラメータ ----\n");
	int bad = 0;
	instance a;
	if (!make_instance(fac, cid, a)) { std::printf("NG: 作れない\n"); return 1; }
	static log_handler handler;
	a.ctrl->setComponentHandler(&handler);

	// 一覧。名前 → 番号
	std::map<std::string, ParamID> ids;
	std::map<ParamID, int> seen;
	int xg_count = 0;
	const int32 count = a.ctrl->getParameterCount();
	for (int32 i = 0; i < count; i++) {
		ParamID id = 0;
		ParameterInfo info{};
		const std::string t = title_of(a.ctrl, i, id, info);
		if (seen[id]++)
			{ std::printf("NG: 番号 %u が重なっている\n", unsigned(id)); bad++; }
		if (id >= 65536) {
			xg_count++;
			ids[t] = id;
			if (!(info.flags & ParameterInfo::kCanAutomate))
				{ std::printf("NG: %s がオートメーションできない\n", t.c_str()); bad++; }
		}
	}
	std::printf("パラメータ %d 本（XG の値 %d 本）\n", count, xg_count);

	auto show = [&](const char *name, double plain) {
		const ParamID id = ids[name];
		String128 str{};
		const ParamValue nv = a.ctrl->plainParamToNormalized(id, plain);
		a.ctrl->getParamStringByValue(id, nv, str);
		std::printf("  %-18s id %u  値 %g → \"", name, unsigned(id), plain);
		print16(str);
		std::printf("\"\n");
	};
	for (const char *n : { "A1 Volume", "A1 Pan", "B3 EQ Bass Freq", "D16 Note Shift", "INS1 Param 1", "INS4 Param 16" })
		if (!ids.count(n)) { std::printf("NG: %s が無い\n", n); bad++; }
	if (bad)
		return 1;
	show("A1 Volume", 100);
	show("A1 Pan", 0);
	show("A1 Pan", 40);
	show("B3 EQ Bass Gain", 70);
	show("B3 EQ Bass Freq", 12);
	show("Master Tune", 0x400 + 55);
	show("Master EQ Q 3", 7);

	start_instance(a, rate, block);
	run_blocks(a, rate, block, 1.0, nullptr);
	// インサーション 1 を DISTORTION（1 バイトのパラメータ）、2 を DELAY LCR（2 バイト）にしておく
	send_sysex(a, block, { { 0xf0, 0x43, 0x10, 0x4c, 0x03, 0x00, 0x00, 0x49, 0x00, 0xf7 },
	                       { 0xf0, 0x43, 0x10, 0x4c, 0x03, 0x01, 0x00, 0x05, 0x00, 0xf7 } });
	run_blocks(a, rate, block, 0.5, nullptr);

	// 値を送る。CC で入るもの（A1 Cutoff・A10 Attack・D16 Volume）と、パラメータチェンジで入るもの。
	// インサーションのパラメータは種類の範囲に対する割合（0-1000）。Drive 0-127 の 500 は 64 になり、読み戻すと 504
	struct target { const char *name; int value; int back; };
	const target T[] = {
		{ "A1 Cutoff", 20, 20 }, { "A10 Attack", 90, 90 }, { "D16 Volume", 50, 50 },
		{ "A1 EQ Bass Gain", 70, 70 }, { "B3 Pan", 0, 0 }, { "Reverb Return", 100, 100 }, { "Master EQ Gain 3", 58, 58 },
		{ "C5 Note Shift", 0x40 + 7, 0x40 + 7 }, { "Master Tune", 0x400 - 30, 0x400 - 30 },
		{ "INS1 Param 1", 500, 504 }, { "INS2 Param 1", 250, 250 }, { "INS2 Param 10", 1000, 1000 },
	};
	param_changes changes;
	for (const target &t : T)
		changes.get(ids[t.name])->add(0, a.ctrl->plainParamToNormalized(ids[t.name], t.value));
	// ホストが同じ値を区間ごとに送り続けるのをまねる（2 秒ぶん）
	run_blocks(a, rate, block, 2.0, &changes, 100000);
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));   // 見せる値の控えが古くなるのを待つ
	run_blocks(a, rate, block, 0.3, nullptr);

	auto check = [&](instance &in, const char *what) {
		int ng = 0;
		for (const target &t : T) {
			const ParamID id = ids[t.name];
			const ParamValue nv = in.ctrl->getParamNormalized(id);
			const int got = int(std::lround(in.ctrl->normalizedParamToPlain(id, nv)));
			if (got != t.back) {
				std::printf("NG: %s %s は %d（%d のはず）\n", what, t.name, got, t.back);
				ng++;
			}
			if (!std::strncmp(t.name, "INS", 3)) {
				String128 str{};
				in.ctrl->getParamStringByValue(id, nv, str);
				std::printf("  %s = ", t.name);
				print16(str);
				std::printf("\n");
			}
		}
		std::printf("%s: %d 個のうち %d 個が合った\n", what, int(sizeof(T) / sizeof(T[0])), int(sizeof(T) / sizeof(T[0])) - ng);
		return ng;
	};
	bad += check(a, "送った値を音源から読み戻す");

	// 保存して、別の実体に戻す
	mem_stream st;
	a.comp->getState(&st);
	std::printf("状態 %zu バイト\n", st.size());
	stop_instance(a);

	instance b;
	make_instance(fac, cid, b);
	b.ctrl->setComponentHandler(&handler);
	st.rewind();
	b.comp->setState(&st);
	start_instance(b, rate, block);
	run_blocks(b, rate, block, 1.5, nullptr);
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));
	bad += check(b, "状態を戻した実体");
	stop_instance(b);

	// 機械まるごとの状態を抜いて、XG の値の控えだけにする（版違いで読めなかったときのまね）
	const std::vector<uint8> &raw = st.bytes();
	auto i32 = [&](size_t at) { int32 v = 0; std::memcpy(&v, raw.data() + at, 4); return v; };
	size_t at = 8;                                   // 版と出力レベル
	const int32 packed = i32(at); at += 4 + size_t(packed);
	const int32 card = i32(at); at += 4 + size_t(card);
	const int32 setup = i32(at); at += 4;
	std::printf("控え %d バイト（機械まるごと %d バイト）\n", setup, packed);
	std::vector<uint8> only(raw.begin(), raw.begin() + 8);
	auto push32 = [&](int32 v) { uint8 b4[4]; std::memcpy(b4, &v, 4); only.insert(only.end(), b4, b4 + 4); };
	push32(0);
	push32(0);
	push32(setup);
	only.insert(only.end(), raw.begin() + long(at), raw.begin() + long(at) + setup);
	mem_stream st2;
	st2.assign(only);
	instance c;
	make_instance(fac, cid, c);
	c.comp->setState(&st2);
	start_instance(c, rate, block);
	run_blocks(c, rate, block, 4.0, nullptr);     // 控えは 31250bps の直列で流れる（数 KB で 2 秒ほど）
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));
	run_blocks(c, rate, block, 0.2, nullptr);
	bad += check(c, "XG の値の控えだけで戻した実体");
	stop_instance(c);

	std::printf("restartComponent %d 回\n", handler.restarts);
	std::printf("---- XG の値はここまで: %s ----\n", bad ? "NG あり" : "全部合った");
	return bad ? 1 : 0;
}

int run_torture(IPluginFactory *fac, const TUID cid)
{
	std::printf("\n---- 乱暴に扱ってみる ----\n");
	int bad = 0;

	// 1. 作って、初期化せずに捨てる
	for (int i = 0; i < 8; i++) {
		IComponent *c = nullptr;
		fac->createInstance(reinterpret_cast<FIDString>(cid),
		                    reinterpret_cast<FIDString>(IComponent::iid.toTUID()), (void **)&c);
		if (!c) { std::printf("NG: 作れない\n"); return 1; }
		c->release();
	}
	std::printf("OK: 初期化せずに 8 個作って捨てた\n");

	// 2. initialize / terminate を繰り返す
	{
		IComponent *c = nullptr;
		fac->createInstance(reinterpret_cast<FIDString>(cid),
		                    reinterpret_cast<FIDString>(IComponent::iid.toTUID()), (void **)&c);
		for (int i = 0; i < 3; i++) { c->initialize(nullptr); c->terminate(); }
		c->release();
		std::printf("OK: initialize/terminate を 3 往復\n");
	}

	// 3. 一通りの標本化周波数と大きさで、音を出さずに process を回す
	{
		IComponent *c = nullptr;
		fac->createInstance(reinterpret_cast<FIDString>(cid),
		                    reinterpret_cast<FIDString>(IComponent::iid.toTUID()), (void **)&c);
		IAudioProcessor *p = nullptr;
		c->queryInterface(IAudioProcessor::iid, (void **)&p);
		c->initialize(nullptr);

		const double rates[5] = { 22050, 44100, 48000, 88200, 192000 };
		for (double r : rates) {
			ProcessSetup su{};
			su.processMode = kRealtime;
			su.symbolicSampleSize = kSample32;
			su.maxSamplesPerBlock = 2048;
			su.sampleRate = r;
			if (p->setupProcessing(su) != kResultOk) { std::printf("NG: %.0f Hz\n", r); bad++; }
		}
		std::printf("OK: 22050 から 192000 まで setupProcessing\n");

		c->setActive(true);
		p->setProcessing(true);

		std::vector<float> l(2048), rr(2048);
		float *ch[2] = { l.data(), rr.data() };
		AudioBusBuffers ab{};
		ab.numChannels = 2; ab.channelBuffers32 = ch;
		ProcessData pd{};
		pd.symbolicSampleSize = kSample32;
		pd.numOutputs = 1; pd.outputs = &ab;

		// 長さ 0、バスなし、音声配列なし、64bit 指定 … どれも落ちてはいけない
		pd.numSamples = 0;                        p->process(pd);
		pd.numSamples = 64;  pd.numOutputs = 0;   p->process(pd);
		pd.numOutputs = 1;   ab.numChannels = 0;  p->process(pd);
		ab.numChannels = 2;
		pd.symbolicSampleSize = kSample64;        p->process(pd);
		pd.symbolicSampleSize = kSample32;
		std::printf("OK: 長さ 0 / バス無し / 64bit 指定でも落ちない\n");

		// setProcessing と setActive をばたばた切り替える
		for (int i = 0; i < 20; i++) {
			p->setProcessing(i & 1);
			pd.numSamples = 128;
			p->process(pd);
			c->setActive(!(i & 1));
		}
		std::printf("OK: setActive/setProcessing を 20 往復しながら process\n");

		p->setProcessing(false);
		c->setActive(false);
		c->terminate();
		p->release();
		c->release();
	}

	// 3.4 setActive(true) を抜けた直後の音。**新しい個体**でやる。
	// ホストは setActive を抜けたら演奏を始めてよいことになっているので、
	// 起動が終わっていなければ最初の音が溜められて曲の頭が崩れる（issue #19）
	{
		IComponent *c = nullptr;
		fac->createInstance(reinterpret_cast<FIDString>(cid),
		                    reinterpret_cast<FIDString>(IComponent::iid.toTUID()), (void **)&c);
		IAudioProcessor *p = nullptr;
		if (c) c->queryInterface(IAudioProcessor::iid.toTUID(), (void **)&p);
		if (c && p) {
			c->initialize(nullptr);
			c->activateBus(kAudio, kOutput, 0, true);
			for (int32 b = 0; b < c->getBusCount(kEvent, kInput); b++)
				c->activateBus(kEvent, kInput, b, true);
			ProcessSetup su{};
			su.processMode = kRealtime;
			su.symbolicSampleSize = kSample32;
			su.maxSamplesPerBlock = 512;
			su.sampleRate = 44100.0;
			p->setupProcessing(su);
			c->setActive(true);
			p->setProcessing(true);

			std::vector<float> l(512), rr(512);
			float *ch[2] = { l.data(), rr.data() };
			AudioBusBuffers ab{};
			ab.numChannels = 2; ab.channelBuffers32 = ch;
			event_list elist;
			Event ev{};
			ev.busIndex = 0; ev.sampleOffset = 0; ev.flags = Event::kIsLive;
			ev.type = Event::kNoteOnEvent;
			ev.noteOn.channel = 0; ev.noteOn.pitch = 60;
			ev.noteOn.velocity = 100.0f / 127.0f; ev.noteOn.noteId = -1;
			elist.addEvent(ev);

			ProcessData pd{};
			pd.symbolicSampleSize = kSample32;
			pd.numOutputs = 1; pd.outputs = &ab;
			pd.numSamples = 512;
			pd.inputEvents = &elist;

			// 100ms ぶん回して、音が出ているか見る
			double peak = 0;
			for (int i = 0; i < 9; i++) {
				std::fill(l.begin(), l.end(), 0.0f);
				std::fill(rr.begin(), rr.end(), 0.0f);
				p->process(pd);
				pd.inputEvents = nullptr;     // ノートオンは 1 回だけ
				for (int k = 0; k < 512; k++)
					peak = std::max(peak, double(std::fabs(l[k])));
			}
			if (peak < 0.001) {
				std::printf("NG: setActive(true) の直後のノートオンで音が出ない"
				            "（起動を待っていない。issue #19）\n");
				bad++;
			} else {
				std::printf("OK: setActive(true) の直後のノートオンで音が出る（頂 %.3f）\n", peak);
			}
			p->setProcessing(false);
			c->setActive(false);
			c->terminate();
		}
		if (p) p->release();
		if (c) c->release();
	}

	// 3.5 状態の保存と復元。**新しい個体**でやる。使い回すと起動の途中で
	// 止められていたりして、機械の中身が入らない
	{
		IComponent *c = nullptr;
		fac->createInstance(reinterpret_cast<FIDString>(cid),
		                    reinterpret_cast<FIDString>(IComponent::iid.toTUID()), (void **)&c);
		IAudioProcessor *p = nullptr;
		if (c) c->queryInterface(IAudioProcessor::iid.toTUID(), (void **)&p);
		if (c && p) {
			c->initialize(nullptr);
			ProcessSetup su{};
			su.processMode = kRealtime;
			su.symbolicSampleSize = kSample32;
			su.maxSamplesPerBlock = 512;
			su.sampleRate = 44100.0;
			p->setupProcessing(su);
			c->setActive(true);
			p->setProcessing(true);

			std::vector<float> l(512), rr(512);
			float *ch[2] = { l.data(), rr.data() };
			AudioBusBuffers ab{};
			ab.numChannels = 2; ab.channelBuffers32 = ch;
			ProcessData pd{};
			pd.symbolicSampleSize = kSample32;
			pd.numOutputs = 1; pd.outputs = &ab;
			pd.numSamples = 512;

			mem_stream st;
			for (int t = 0; t < 300; t++) {
				for (int i = 0; i < 20; i++)
					p->process(pd);
				st = mem_stream();
				if (c->getState(&st) != kResultOk)
					break;
				if (st.size() >= 1000)
					break;
				sleep_ms(50);
			}
			if (st.size() < 1000) {
				std::printf("NG: getState が %zu バイトしかない"
				            "（機械の中身が入っていない）\n", st.size());
				bad++;
			} else {
				st.rewind();
				if (c->setState(&st) != kResultOk) {
					std::printf("NG: setState\n");
					bad++;
				} else {
					std::printf("OK: 状態を %zu バイトで保存して読み戻した\n", st.size());
				}
			}
			p->setProcessing(false);
			c->setActive(false);
			c->terminate();
		}
		if (p) p->release();
		if (c) c->release();
	}

	// 3.6 FL Studio の「Reset plugin when FL Studio resets」の形（issue #9）。
	// 音声スレッドは process を回し続け、別のスレッドが保存のたびに
	// setProcessing(false) → setActive(false) → getState → setActive(true) → setProcessing(true)
	// を呼び、ときどき setState で戻す。機械に 2 つのスレッドが同時に触ると落ちるか、壊れた状態が出る
	{
		IComponent *c = nullptr;
		fac->createInstance(reinterpret_cast<FIDString>(cid),
		                    reinterpret_cast<FIDString>(IComponent::iid.toTUID()), (void **)&c);
		IAudioProcessor *p = nullptr;
		if (c) c->queryInterface(IAudioProcessor::iid.toTUID(), (void **)&p);
		if (c && p) {
			c->initialize(nullptr);
			ProcessSetup su{};
			su.processMode = kRealtime;
			su.symbolicSampleSize = kSample32;
			su.maxSamplesPerBlock = 256;
			su.sampleRate = 44100.0;
			p->setupProcessing(su);
			c->setActive(true);
			p->setProcessing(true);

			std::atomic<bool> quit{false};
			std::atomic<uint64_t> blocks{0};
			std::thread audio([&] {
				std::vector<float> l(256), rr(256);
				float *ch[2] = { l.data(), rr.data() };
				AudioBusBuffers ab{};
				ab.numChannels = 2; ab.channelBuffers32 = ch;
				ProcessData pd{};
				pd.symbolicSampleSize = kSample32;
				pd.numOutputs = 1; pd.outputs = &ab;
				pd.numSamples = 256;
				while (!quit.load()) {
					p->process(pd);          // 止めろと言われても呼び続ける
					blocks.fetch_add(1);
				}
			});
			// 起動を待つ
			for (int t = 0; t < 300; t++) {
				mem_stream s;
				c->getState(&s);
				if (s.size() >= 1000) break;
				sleep_ms(50);
			}
			int saved = 0, restored = 0, small = 0;
			const long long end = now_ms() + 8000;
			for (int round = 0; now_ms() < end; round++) {
				p->setProcessing(false);
				c->setActive(false);
				mem_stream st;
				if (c->getState(&st) == kResultOk && st.size() >= 1000) saved++; else small++;
				c->setActive(true);
				p->setProcessing(true);
				if (round % 3 == 2 && st.size() >= 1000) {
					st.rewind();
					if (c->setState(&st) == kResultOk) restored++;
				}
			}
			quit.store(true);
			audio.join();
			if (small) {
				std::printf("NG: 保存中のリセットで、中身の無い状態が %d 回\n", small);
				bad++;
			}
			std::printf("OK: 保存中のリセットを %d 回（戻し %d 回）、そのあいだ process %llu 回\n",
			            saved, restored, (unsigned long long)blocks.load());
			p->setProcessing(false);
			c->setActive(false);
			c->terminate();
		}
		if (p) p->release();
		if (c) c->release();
	}

	// 4. パラメータの問い合わせを全部
	{
		IComponent *c = nullptr;
		fac->createInstance(reinterpret_cast<FIDString>(cid),
		                    reinterpret_cast<FIDString>(IComponent::iid.toTUID()), (void **)&c);
		IEditController *e = nullptr;
		IMidiMapping *m = nullptr;
		c->queryInterface(IEditController::iid, (void **)&e);
		c->queryInterface(IMidiMapping::iid, (void **)&m);
		c->initialize(nullptr);

		const int32 n = e->getParameterCount();
		for (int32 i = 0; i < n; i++) {
			ParameterInfo pi{};
			if (e->getParameterInfo(i, pi) != kResultOk) { std::printf("NG: パラメータ %d\n", i); bad++; break; }
			String128 s{};
			e->getParamStringByValue(pi.id, 0.5, s);
			ParamValue v = 0.0;
			e->getParamValueByString(pi.id, s, v);
			if (pi.flags & ParameterInfo::kIsReadOnly)
				continue;              // 読むだけのものは書けなくて当たり前
			e->setParamNormalized(pi.id, 0.25);
			if (e->getParamNormalized(pi.id) != 0.25) { std::printf("NG: 値が残らない %u\n", unsigned(pi.id)); bad++; break; }
		}
		std::printf("OK: パラメータ %d 本を全部問い合わせた\n", n);

		// 範囲外
		ParameterInfo pi{};
		if (e->getParameterInfo(-1, pi) == kResultOk || e->getParameterInfo(n, pi) == kResultOk) {
			std::printf("NG: 範囲外のパラメータに答えてしまう\n"); bad++;
		}
		int mapped = 0;
		std::vector<ParamID> seen;
		for (int32 bus = 0; bus < 3; bus++)
			for (int16 ch = 0; ch < 16; ch++)
				for (int cc = 0; cc < 132; cc++) {
					ParamID id = 0;
					if (m->getMidiControllerAssignment(bus, ch, CtrlNumber(cc), id) == kResultTrue) {
						mapped++;
						seen.push_back(id);
					}
				}
		std::sort(seen.begin(), seen.end());
		if (std::adjacent_find(seen.begin(), seen.end()) != seen.end()) {
			std::printf("NG: 違う口・チャンネルが同じパラメータに割り当たっている\n"); bad++;
		}
		std::printf("OK: MIDI の割り当ては %d 通り（2 口 × 16ch × 131）\n", mapped);

		// Cubase がプログラムチェンジを渡す道。MIDI チャンネル → ユニット（getUnitByBus）→
		// そのユニットのプログラム一覧 → 同じユニットに属し kIsProgramChange の付いたパラメータ
		IUnitInfo *u = nullptr;
		c->queryInterface(IUnitInfo::iid, (void **)&u);
		if (!u) {
			std::printf("NG: IUnitInfo が無い（Cubase で MIDI のプログラムチェンジが捨てられる）\n"); bad++;
		} else {
			std::map<UnitID, ProgramListID> lists;
			for (int32 i = 0; i < u->getUnitCount(); i++) {
				UnitInfo ui{};
				if (u->getUnitInfo(i, ui) == kResultOk)
					lists[ui.id] = ui.programListId;
			}
			int32 programs = 0;
			for (int32 i = 0; i < u->getProgramListCount(); i++) {
				ProgramListInfo li{};
				if (u->getProgramListInfo(i, li) == kResultOk && li.id == 1)
					programs = li.programCount;
			}
			std::map<UnitID, int> pc_params;
			for (int32 i = 0; i < e->getParameterCount(); i++) {
				ParameterInfo pi2{};
				if (e->getParameterInfo(i, pi2) == kResultOk && (pi2.flags & ParameterInfo::kIsProgramChange))
					pc_params[pi2.unitId]++;
			}
			int good = 0, bus_count = c->getBusCount(kEvent, kInput);
			for (int32 bus = 0; bus < bus_count; bus++)
				for (int32 ch = 0; ch < 16; ch++) {
					UnitID unit = -1;
					if (u->getUnitByBus(kEvent, kInput, bus, ch, unit) != kResultTrue)
						continue;
					auto l = lists.find(unit);
					if (l == lists.end() || l->second == kNoProgramListId)
						continue;
					if (pc_params[unit] != 1)
						continue;
					// そのパラメータと IMidiMapping の 130 番が同じものか
					ParamID via_map = 0;
					m->getMidiControllerAssignment(bus, int16(ch), CtrlNumber(130), via_map);   // kCtrlProgramChange
					ParameterInfo pi3{};
					bool same = false;
					for (int32 i = 0; i < e->getParameterCount() && !same; i++)
						if (e->getParameterInfo(i, pi3) == kResultOk && pi3.id == via_map)
							same = (pi3.flags & ParameterInfo::kIsProgramChange) && pi3.unitId == unit;
					good += same;
				}
			if (good != bus_count * 16 || programs != 128) {
				std::printf("NG: プログラムチェンジのユニット %d / %d、一覧 %d 音\n", good, bus_count * 16, programs);
				bad++;
			} else {
				std::printf("OK: プログラムチェンジは %d 口 × 16ch ともユニットと 128 音の一覧に繋がる\n", bus_count);
			}
			u->release();
		}

		c->terminate();
		e->release();
		m->release();
		c->release();
	}

	// 5. 同時に 4 個。DAW で複数トラックに挿した形
	{
		IComponent *cs[4] = {};
		IAudioProcessor *ps[4] = {};
		for (int i = 0; i < 4; i++) {
			fac->createInstance(reinterpret_cast<FIDString>(cid),
			                    reinterpret_cast<FIDString>(IComponent::iid.toTUID()), (void **)&cs[i]);
			cs[i]->queryInterface(IAudioProcessor::iid, (void **)&ps[i]);
			cs[i]->initialize(nullptr);
			ProcessSetup su{};
			su.processMode = kRealtime; su.symbolicSampleSize = kSample32;
			su.maxSamplesPerBlock = 512; su.sampleRate = 48000;
			ps[i]->setupProcessing(su);
			cs[i]->setActive(true);
			ps[i]->setProcessing(true);
		}
		std::printf("4 個ぶん起動を待つ...");
		std::fflush(stdout);
		sleep_ms(12000);

		std::vector<float> l(512), rr(512);
		float *ch[2] = { l.data(), rr.data() };
		AudioBusBuffers ab{}; ab.numChannels = 2; ab.channelBuffers32 = ch;
		ProcessData pd{};
		pd.symbolicSampleSize = kSample32; pd.numSamples = 512;
		pd.numOutputs = 1; pd.outputs = &ab;

		const long long t0 = now_ms();
		double peak = 0.0;
		for (int blk = 0; blk < 200; blk++)
			for (int i = 0; i < 4; i++) {
				ps[i]->process(pd);
				for (float v : l) peak = std::max(peak, std::fabs(double(v)));
			}
		const long long t1 = now_ms();
		const double audio = 200.0 * 512.0 / 48000.0;
		std::printf(" 4 個同時に %.2f 秒ぶん作って実時間 %.2f 秒（1 個あたり CPU %.0f%%）\n",
		            audio, (t1 - t0) / 1000.0, 100.0 * (t1 - t0) / 1000.0 / audio / 4.0);

		for (int i = 0; i < 4; i++) {
			ps[i]->setProcessing(false);
			cs[i]->setActive(false);
			cs[i]->terminate();
			ps[i]->release();
			cs[i]->release();
		}
		std::printf("OK: 4 個同時に作って捨てた\n");
	}

	std::printf("---- 悪いところ %d 件 ----\n", bad);
	return bad ? 1 : 0;
}

} // namespace


int main(int argc, char **argv)
{
	smu2000::init_console_utf8();
	// プラグインが落ちても、どこまで進んだかが残るように
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	if (argc < 2) {
		std::fprintf(stderr,
			"使い方: vst3probe <DLL> [<MIDI> <出力 wav>] [--rate 48000] [--block 512] [--data-midi]\n"
			"          [--state <file>]（起動中に状態を戻してから鳴らす） [--save-state <file>]\n");
		return 1;
	}
	std::string dll = argv[1], mid, wav;
	double rate = 48000.0;
	int block = 512;
	double extra = 3.0;      // 曲の後ろに足す残響ぶん
	bool torture = false;
	bool adc_sine = false;
	bool one_bus = false;    // 比べる用。MIDI ファイルの口 B も A のバスへ流す
	bool data_midi = false;  // VSTHost のまね。チャンネルメッセージも DataEvent で渡す
	bool restart = false;    // DAW の「止めて再生」のまね（流す直前に setProcessing を切り入れする）
	bool automation = false;
	int  view_seconds = 0;
	// ホストが保存した状態を戻してから鳴らす（issue #51。foobar2000 の foo_midi は
	// 起動中のプラグインに状態を戻してから、すぐ曲頭を流す）
	std::string state_in, state_out;
	for (int i = 2; i < argc; i++) {
		if (!std::strcmp(argv[i], "--rate") && i + 1 < argc) rate = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--block") && i + 1 < argc) block = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--tail") && i + 1 < argc) extra = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--torture")) torture = true;
		else if (!std::strcmp(argv[i], "--adc-sine")) adc_sine = true;
		else if (!std::strcmp(argv[i], "--one-bus")) one_bus = true;
		else if (!std::strcmp(argv[i], "--data-midi")) data_midi = true;
		else if (!std::strcmp(argv[i], "--restart")) restart = true;
		else if (!std::strcmp(argv[i], "--automation")) automation = true;
		else if (!std::strcmp(argv[i], "--state") && i + 1 < argc) state_in = argv[++i];
		else if (!std::strcmp(argv[i], "--save-state") && i + 1 < argc) state_out = argv[++i];
		else if (!std::strcmp(argv[i], "--view")) view_seconds =
		    (i + 1 < argc && argv[i + 1][0] != '-') ? std::atoi(argv[++i]) : 20;
		else if (mid.empty()) mid = argv[i];
		else if (wav.empty()) wav = argv[i];
	}

	// ---- Open the module
	//
	// On Windows the argument is a DLL and the entry points are InitDll and
	// GetPluginFactory. On macOS it is the .vst3 directory, opened with CFBundle
	// the way a host opens it, and the entry point is bundleEntry
	bool (*init)() = nullptr;
	IPluginFactory *(PLUGIN_API *getf)() = nullptr;
#if defined(__linux__)
	void *dl_mod = nullptr;
#endif

#if defined(_WIN32)
	HMODULE lib = LoadLibraryA(dll.c_str());
	if (!lib) {
		std::fprintf(stderr, "DLL を読めない: %s (エラー %lu)\n", dll.c_str(), GetLastError());
		return 1;
	}
	init = reinterpret_cast<bool (*)()>(GetProcAddress(lib, "InitDll"));
	getf = reinterpret_cast<IPluginFactory *(PLUGIN_API *)()>(
		GetProcAddress(lib, "GetPluginFactory"));
#elif defined(__APPLE__)
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(
		nullptr, reinterpret_cast<const UInt8 *>(dll.c_str()), dll.size(), true);
	CFBundleRef bundle = url ? CFBundleCreate(nullptr, url) : nullptr;
	if (url)
		CFRelease(url);
	if (!bundle) {
		std::fprintf(stderr, "バンドルを開けない: %s\n", dll.c_str());
		return 1;
	}
	if (!CFBundleLoadExecutable(bundle)) {
		std::fprintf(stderr, "バンドルを読めない: %s\n", dll.c_str());
		return 1;
	}
	// bundleEntry is the macOS host's entry point, so call it as a host would.
	// There is no InitDll here; `init` stays null
	auto entry = reinterpret_cast<bool (*)(CFBundleRef)>(
		CFBundleGetFunctionPointerForName(bundle, CFSTR("bundleEntry")));
	if (entry)
		entry(bundle);
	getf = reinterpret_cast<IPluginFactory *(PLUGIN_API *)()>(
		CFBundleGetFunctionPointerForName(bundle, CFSTR("GetPluginFactory")));
#elif defined(__linux__)
	// Linux: the argument is the ELF .so itself (bundle Contents/x86_64-linux).
	// Hosts dlopen it and call GetPluginFactory directly; no InitDll.
	dl_mod = dlopen(dll.c_str(), RTLD_NOW);
	if (!dl_mod) {
		std::fprintf(stderr, "読めない: %s (%s)\n", dll.c_str(), dlerror());
		return 1;
	}
	getf = reinterpret_cast<IPluginFactory *(PLUGIN_API *)()>(
		dlsym(dl_mod, "GetPluginFactory"));
#endif

	if (!getf) {
		std::fprintf(stderr, "GetPluginFactory が無い\n");
		return 1;
	}
	if (init) init();

	IPluginFactory *fac = getf();
	if (!fac) { std::fprintf(stderr, "工場が空\n"); return 1; }

	PFactoryInfo fi{};
	fac->getFactoryInfo(&fi);
	std::printf("製作: %s  %s\n", fi.vendor, fi.url);
	std::printf("クラス数: %d\n", fac->countClasses());

	TUID cid{};
	for (int32 i = 0; i < fac->countClasses(); i++) {
		PClassInfo ci{};
		if (fac->getClassInfo(i, &ci) != kResultOk) continue;
		std::printf("  [%d] %s  種別 %s\n", i, ci.name, ci.category);
		if (!std::strcmp(ci.category, kVstAudioEffectClass))
			std::memcpy(cid, ci.cid, sizeof(TUID));
	}
	{
		IPluginFactory2 *f2 = nullptr;
		if (fac->queryInterface(IPluginFactory2::iid, (void **)&f2) == kResultOk && f2) {
			PClassInfo2 c2{};
			if (f2->getClassInfo2(0, &c2) == kResultOk)
				std::printf("  分類 %s  版 %s  SDK %s\n", c2.subCategories, c2.version,
				            c2.sdkVersion);
			f2->release();
		}
		IPluginFactory3 *f3 = nullptr;
		if (fac->queryInterface(IPluginFactory3::iid, (void **)&f3) == kResultOk && f3) {
			PClassInfoW cw{};
			if (f3->getClassInfoUnicode(0, &cw) == kResultOk) {
				std::printf("  Unicode 名 ");
				print16(cw.name);
				std::printf("\n");
			}
			f3->release();
		}
	}

	IComponent *comp = nullptr;
	if (fac->createInstance(reinterpret_cast<FIDString>(cid),
	                        reinterpret_cast<FIDString>(IComponent::iid.toTUID()),
	                        (void **)&comp) != kResultOk || !comp) {
		std::fprintf(stderr, "IComponent を作れない\n");
		return 1;
	}
	std::printf("IComponent を作れた\n");

	IAudioProcessor *proc = nullptr;
	IEditController *ctrl = nullptr;
	IMidiMapping    *map  = nullptr;
	comp->queryInterface(IAudioProcessor::iid, (void **)&proc);
	comp->queryInterface(IEditController::iid, (void **)&ctrl);
	comp->queryInterface(IMidiMapping::iid, (void **)&map);
	std::printf("IAudioProcessor %s / IEditController %s / IMidiMapping %s\n",
	            proc ? "あり" : "なし", ctrl ? "あり" : "なし", map ? "あり" : "なし");
	if (!proc) return 1;

	comp->initialize(nullptr);
	std::printf("音声出力バス %d / MIDI 入力バス %d / パラメータ %d\n",
	            comp->getBusCount(kAudio, kOutput), comp->getBusCount(kEvent, kInput),
	            ctrl ? ctrl->getParameterCount() : 0);

	if (map) {
		ParamID id = 0;
		if (map->getMidiControllerAssignment(0, 0, 7, id) == kResultTrue)
			std::printf("Ch1 CC7 -> パラメータ %u\n", unsigned(id));
	}

	SpeakerArrangement out_arr = SpeakerArr::kStereo;
	SpeakerArrangement in_arr = SpeakerArr::kStereo;
	std::printf("音声入力バス %d\n", comp->getBusCount(kAudio, kInput));
	if (adc_sine) {
		if (proc->setBusArrangements(&in_arr, 1, &out_arr, 1) != kResultTrue)
			std::printf("入力 1 つの並びを断られた\n");
		comp->activateBus(kAudio, kInput, 0, true);
	} else {
		proc->setBusArrangements(nullptr, 0, &out_arr, 1);
	}
	comp->activateBus(kAudio, kOutput, 0, true);
	for (int32 b = 0; b < comp->getBusCount(kEvent, kInput); b++)
		comp->activateBus(kEvent, kInput, b, true);

	ProcessSetup setup{};
	setup.processMode        = kOffline;
	setup.symbolicSampleSize = kSample32;
	setup.maxSamplesPerBlock  = block;
	setup.sampleRate         = rate;
	if (proc->setupProcessing(setup) != kResultOk) {
		std::fprintf(stderr, "setupProcessing に失敗\n");
		return 1;
	}
	// 起動がまだ終わらないうちに状態を戻す（ホストが曲を開いたときの順）
	if (!state_in.empty()) {
		std::vector<uint8> bytes;
		if (FILE *f = std::fopen(state_in.c_str(), "rb")) {
			uint8 buf[65536];
			size_t k;
			while ((k = std::fread(buf, 1, sizeof(buf), f)) > 0)
				bytes.insert(bytes.end(), buf, buf + k);
			std::fclose(f);
		}
		mem_stream st;
		st.assign(bytes);
		std::printf("状態を戻す: %zu バイト → %s\n", bytes.size(),
		            comp->setState(&st) == kResultOk ? "OK" : "NG");
	}
	comp->setActive(true);
	proc->setProcessing(true);
	std::printf("遅れ %u サンプル / 残響 %u サンプル\n",
	            proc->getLatencySamples(), proc->getTailSamples());

	if (view_seconds > 0) {
		proc->setProcessing(false);
		const int rc = run_view(comp, proc, ctrl, view_seconds);
		comp->setActive(false);
		comp->terminate();
		if (proc) proc->release();
		if (ctrl) ctrl->release();
		if (map)  map->release();
		comp->release();
		return rc;
	}

	if (automation) {
		proc->setProcessing(false);
		comp->setActive(false);
		comp->terminate();
		if (proc) proc->release();
		if (ctrl) ctrl->release();
		if (map)  map->release();
		comp->release();
		return run_automation(fac, cid, rate, block);
	}

	if (torture) {
		proc->setProcessing(false);
		comp->setActive(false);
		comp->terminate();
		if (proc) proc->release();
		if (ctrl) ctrl->release();
		if (map)  map->release();
		comp->release();
		return run_torture(fac, cid);
	}

	if (mid.empty() || wav.empty()) {
		std::printf("音は出していない（MIDI と出力先を渡すと鳴らす）\n");
		proc->setProcessing(false);
		comp->setActive(false);
		comp->terminate();
		comp->release();
		return 0;
	}

	std::vector<smf::event> events;
	std::string err;
	if (!smf::load(mid, events, err)) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	const double length = events.empty() ? 0.0 : events.back().time;
	std::printf("MIDI: %zu イベント、%.2f 秒\n", events.size(), length);

	// 起動が終わるまで無音を回す。プラグインは別スレッドで立ち上がっている
	std::vector<float> bl(block), br(block);
	float *chans[2] = { bl.data(), br.data() };
	AudioBusBuffers abuf{};
	abuf.numChannels = 2;
	abuf.silenceFlags = 0;
	abuf.channelBuffers32 = chans;

	event_list   elist;
	param_changes pchanges;

	ProcessData pd{};
	pd.processMode        = kOffline;
	pd.symbolicSampleSize = kSample32;
	pd.numSamples         = block;
	std::vector<float> il(block), ir(block);
	float *ichans[2] = { il.data(), ir.data() };
	AudioBusBuffers ibuf{};
	ibuf.numChannels = 2;
	ibuf.channelBuffers32 = ichans;
	pd.numInputs          = adc_sine ? 1 : 0;
	pd.inputs             = adc_sine ? &ibuf : nullptr;
	pd.numOutputs         = 1;
	pd.outputs            = &abuf;
	pd.inputEvents        = &elist;
	pd.inputParameterChanges = &pchanges;

	std::printf("起動待ち...");
	std::fflush(stdout);
	const long long t_wait = now_ms();
	for (;;) {
		sleep_ms(50);
		if (now_ms() - t_wait > 60000) {
			std::printf(" 60 秒待っても始まらない\n");
			break;
		}
		// パラメータの読み書きでは分からないので、鳴らして確かめる代わりに
		// 一定時間待つ。起動は実測 2 秒前後
		if (now_ms() - t_wait > 8000)
			break;
	}
	std::printf(" %ld ms\n", long(now_ms() - t_wait));

	// DAW で止めて再生し直したときと同じ状態にしてから流す
	if (restart) {
		proc->setProcessing(false);
		proc->setProcessing(true);
		std::printf("止めて再生し直した状態から流す\n");
	}

	const int64_t total = int64_t((length + extra) * rate);
	std::vector<int16_t> pcm;
	pcm.reserve(size_t(total) * 2);

	size_t next = 0;
	int64_t pos = 0;
	const long long t0 = now_ms();
	while (pos < total) {
		const int32 n = int32(std::min<int64_t>(block, total - pos));
		if (adc_sine)
			for (int32 i = 0; i < n; i++)
				il[size_t(i)] = ir[size_t(i)] = float(0.3 * std::sin(2 * 3.14159265358979 * 440.0 * double(pos + i) / rate));
		elist.clear();
		pchanges.clear();

		while (next < events.size() && events[next].time * rate < double(pos + n)) {
			const auto &e = events[next++];
			const int32 off = std::clamp(int32(e.time * rate - double(pos)), 0, n - 1);
			const std::vector<u8> &b = e.bytes;
			if (b.empty())
				continue;
			const uint8 st = b[0], ch = uint8(st & 15);
			// MIDI ファイルの口（FF 21）をそのままバスの番号にする。B はパート 17-32
			// MIDI ファイルの口指定（FF 21）を、そのままバスに使う。口 A-D の 4 本
			const int32 bus = one_bus ? 0 : int32(e.port < 4 ? e.port : 3);
			// CC などはプラグインが教える割り当てで、パラメータ番号に直す
			auto param = [&](int ctrl) {
				ParamID id = ParamID(ch * 131 + ctrl);
				if (map)
					map->getMidiControllerAssignment(bus, ch, CtrlNumber(ctrl), id);
				return id;
			};
			// VSTHost 1.58 のまね。チャンネルメッセージまで DataEvent に入れ、
			// プログラムチェンジのような 2 byte のものも 3 byte に詰めて渡してくる。
			// 余分な 00 をそのまま音源へ流すと走行状態のデータバイトになる
			if (data_midi && st >= 0x80 && st < 0xf0) {
				std::vector<uint8> raw(b.begin(), b.end());
				raw.resize(3, 0);
				elist.m_sysex.push_back(raw);
				Event ev{};
				ev.busIndex = bus; ev.sampleOffset = off; ev.flags = Event::kIsLive;
				ev.type = Event::kDataEvent;
				ev.data.size = uint32(elist.m_sysex.back().size());
				ev.data.type = DataEvent::kMidiSysEx;
				ev.data.bytes = elist.m_sysex.back().data();
				elist.addEvent(ev);
				continue;
			}
			if (st == 0xf0) {
				elist.m_sysex.push_back(std::vector<uint8>(b.begin(), b.end()));
				Event ev{};
				ev.busIndex = bus;
				ev.sampleOffset = off;
				ev.flags = Event::kIsLive;
				ev.type = Event::kDataEvent;
				ev.data.size = uint32(elist.m_sysex.back().size());
				ev.data.type = DataEvent::kMidiSysEx;
				ev.data.bytes = elist.m_sysex.back().data();
				elist.addEvent(ev);
			} else if ((st & 0xf0) == 0x90 && b.size() > 2 && b[2]) {
				Event ev{};
				ev.busIndex = bus; ev.sampleOffset = off; ev.flags = Event::kIsLive;
				ev.type = Event::kNoteOnEvent;
				ev.noteOn.channel = ch;
				ev.noteOn.pitch = int16(b[1]);
				ev.noteOn.velocity = float(b[2]) / 127.0f;
				ev.noteOn.noteId = -1;
				elist.addEvent(ev);
			} else if ((st & 0xf0) == 0x80 || ((st & 0xf0) == 0x90 && b.size() > 2)) {
				Event ev{};
				ev.busIndex = bus; ev.sampleOffset = off; ev.flags = Event::kIsLive;
				ev.type = Event::kNoteOffEvent;
				ev.noteOff.channel = ch;
				ev.noteOff.pitch = int16(b[1]);
				ev.noteOff.velocity = b.size() > 2 ? float(b[2]) / 127.0f : 0.0f;
				ev.noteOff.noteId = -1;
				elist.addEvent(ev);
			} else if ((st & 0xf0) == 0xa0 && b.size() > 2) {
				Event ev{};
				ev.busIndex = bus; ev.sampleOffset = off; ev.flags = Event::kIsLive;
				ev.type = Event::kPolyPressureEvent;
				ev.polyPressure.channel = ch;
				ev.polyPressure.pitch = int16(b[1]);
				ev.polyPressure.pressure = float(b[2]) / 127.0f;
				ev.polyPressure.noteId = -1;
				elist.addEvent(ev);
			} else if ((st & 0xf0) == 0xb0 && b.size() > 2) {
				pchanges.get(param(b[1]))->add(off, double(b[2]) / 127.0);
			} else if ((st & 0xf0) == 0xd0) {
				pchanges.get(param(128))->add(off, double(b[1]) / 127.0);
			} else if ((st & 0xf0) == 0xe0 && b.size() > 2) {
				const int bend = b[1] | (int(b[2]) << 7);
				pchanges.get(param(129))->add(off, double(bend) / 16383.0);
			} else if ((st & 0xf0) == 0xc0) {
				pchanges.get(param(130))->add(off, double(b[1]) / 127.0);
			}
		}

		pd.numSamples = n;
		proc->process(pd);

		for (int32 i = 0; i < n; i++) {
			const float l = std::clamp(bl[i], -1.0f, 1.0f);
			const float r = std::clamp(br[i], -1.0f, 1.0f);
			pcm.push_back(int16_t(std::lround(l * 32767.0f)));
			pcm.push_back(int16_t(std::lround(r * 32767.0f)));
		}
		pos += n;
	}
	const long long t1 = now_ms();

	write_wav(wav, pcm, uint32_t(rate));
	double peak = 0.0, sum = 0.0;
	for (int16_t v : pcm) { peak = std::max(peak, std::fabs(double(v))); sum += std::fabs(double(v)); }
	std::printf("書き出した: %s（%.1f 秒 / %.0f Hz、実時間 %.1f 秒）\n",
	            wav.c_str(), double(total) / rate, rate, (t1 - t0) / 1000.0);
	std::printf("最大 %.0f  平均 %.1f\n", peak, sum / std::max<size_t>(pcm.size(), 1));

	if (!state_out.empty()) {
		mem_stream st;
		if (comp->getState(&st) == kResultOk) {
			if (FILE *f = std::fopen(state_out.c_str(), "wb")) {
				std::fwrite(st.bytes().data(), 1, st.size(), f);
				std::fclose(f);
			}
			std::printf("状態を書き出した: %zu バイト\n", st.size());
		}
	}

	proc->setProcessing(false);
	comp->setActive(false);
	comp->terminate();
	if (proc) proc->release();
	if (ctrl) ctrl->release();
	if (map)  map->release();
	comp->release();
	return peak > 0.0 ? 0 : 2;
}
