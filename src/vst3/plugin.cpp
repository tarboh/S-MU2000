// license:BSD-3-Clause
//
// S-MU2000 の VST3 プラグイン。
//
// Steinberg の SDK のうち **インターフェース定義（pluginterfaces, MIT）だけ**
// を使い、土台（public.sdk, GPLv3）は使っていない。だから配線は全部ここにある。
// third_party/vst3/README.md に経緯がある。
//
// 作りは単一コンポーネント（single component effect）。IComponent と
// IEditController を 1 つのクラスが両方受け持つ。画面は持たない。
//
// MIDI の入り方は 2 通りある。VST3 はここが独特で、
//   ・ノートオン／オフ、ポリプレッシャ、システムエクスクルーシブ … イベント
//   ・コントロールチェンジ、ピッチベンド、プログラムチェンジ  … パラメータ
// になる。後者のために IMidiMapping で「チャンネル×番号 → パラメータ番号」を
// 教えてやる必要がある。受け取った側でまた MIDI のバイト列に組み直して音源へ渡す。
//
// MIDI の入力バスは **4 本**。実機の MIDI IN A-D（パート 1-16 / 17-32 / 33-48 / 49-64）
// に当たる。パラメータも口ごとに 16ch × 131 本ずつ持つ（一覧には並べない）。
//
// それとは別に、XG の値（パートの音量・フィルタ・EG・EQ、マスター EQ など）を名前付きの
// パラメータとして見せる（automation.h、doc/automation.md）。ホストのオートメーションで
// 動かせ、画面（パネル・PC の窓）で触った値は beginEdit / performEdit / endEdit でホストへ伝える。

#include "automation.h"
#include "automation_host.h"
#include "engine.h"
#include "pc_order.h"
#include "state.h"
#include "view.h"
#include "ui/xg_state.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstunits.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include "compat/platform.h"      // windows.h (lean) for QueryPerformanceCounter
#include "compat/crash_log.h"
#endif

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>      // CFBundleRef, the host's handle
#include <mach/mach_time.h>                     // mach_absolute_time
#elif defined(__linux__)
#include <time.h>                               // clock_gettime
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

// ---- このプラグインを表す番号。一度決めたら変えられない
//      （変えるとホストが別物とみなし、保存した曲から見つからなくなる）
static const FUID kProcessorUID(0x5D2E4B70, 0xA1C34F92, 0x8B0E7A61, 0x4D553000);

constexpr const char *kPlugName   = "S-MU2000";
constexpr const char *kVendor     = "tarboh";
constexpr const char *kVersion    = "0.1.0.0";

// ---- A clock for measuring the load
//
// In the shape QueryPerformanceCounter has: a constant frequency and a tick
// count, so "busy seconds" and "did this block miss its deadline" are the same
// arithmetic on both platforms. Only ever used to report load -- nothing here
// reaches the audio path, so the exact rate does not matter.
//
//   Windows … QueryPerformanceCounter
//   macOS   … mach_absolute_time, scaled to nanoseconds by mach_timebase_info
//   Linux   … clock_gettime(CLOCK_MONOTONIC), already nanoseconds
int64 perf_frequency()
{
#if defined(_WIN32)
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	return f.QuadPart;
#else
	return 1000000000;      // perf_ticks() below hands back nanoseconds
#endif
}

uint64 perf_ticks()
{
#if defined(_WIN32)
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return uint64(t.QuadPart);
#elif defined(__linux__)
	struct timespec ts{};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return uint64(ts.tv_sec) * 1000000000u + uint64(ts.tv_nsec);
#else
	// The timebase is constant on a given machine, so resolve it once
	static const double scale = [] {
		mach_timebase_info_data_t tb{};
		mach_timebase_info(&tb);
		return double(tb.numer) / double(tb.denom);
	}();
	return uint64(double(mach_absolute_time()) * scale);
#endif
}

// MIDI のコントロール番号は 0-127 のほか、
//   128 チャンネルプレッシャ / 129 ピッチベンド / 130 プログラムチェンジ
// まである。チャンネルごとにこれだけのパラメータを並べる
constexpr int32 kCtrlCount = 131;
constexpr int32 kChannels  = 16;
constexpr int32 kMidiParams = kChannels * kCtrlCount;

// 画面を持たないので、ホストの汎用パネルに出す物がこれだけ要る。
//   出力レベル … 音源の外で掛ける素の掛け算
//   状態      … 起動中か、鳴る用意ができたか、ROM が無いか。読むだけ
constexpr ParamID kGainId   = 4096;
constexpr ParamID kStatusId = 4097;

// MIDI IN B-D（パート 17-64）のぶん。A の 0-2095 と Output / Status の番号は
// **保存した曲が覚えているので動かさない**。B 以降は離れた所から 8192 刻みで並べる。
// C・D は実機では USB だけの口
constexpr int32   kPorts      = mu2000::MIDI_PORTS;
constexpr ParamID kPortBase[4] = { 0, 8192, 16384, 24576 };
// MIDI の口のパラメータと Output / Status。XG の値はその後ろに並べる（xg_first）
constexpr int32   kMidiParamCount = kPorts * kMidiParams + 2;
constexpr int32   kXgFirst        = kMidiParamCount;

namespace autom = smu2000::automation;

int32 param_count() { return kMidiParamCount + int32(autom::entries().size()); }

// XG の値のユニット。パートごとに 1 つと、マスター。MIDI のチャンネルのユニット（1-64）とは別
constexpr UnitID kXgPartUnit   = 1000;     // + パート番号
constexpr UnitID kXgMasterUnit = 2000;
constexpr UnitID kXgInsUnit    = 3000;     // + インサーションの番号（0-3）


ParamID param_of(int32 port, int32 ch, int32 ctrl)
{
	return ParamID(kPortBase[port & 3] + ch * kCtrlCount + ctrl);
}

// ---- ユニットとプログラム一覧（IUnitInfo）
//
// Cubase は MIDI のプログラムチェンジを IMidiMapping では流さない。
// 「MIDI チャンネル → ユニット」を getUnitByBus で引き、そのユニットに属していて
// kIsProgramChange の印が付いたパラメータへ、プログラム一覧の番号として渡してくる。
// 印もユニットも無いと黙って捨てる。REAPER などは IMidiMapping の 130 番で流すので、
// そちらはそのまま残す。
//
// ユニットは根（0）の下に、口 × チャンネルの 64 個（1-64）。一覧は 128 音の 1 つを共有する
constexpr ProgramListID kProgramList = 1;
constexpr int32 kPrograms = 128;

UnitID unit_of(int32 port, int32 ch)
{
	return UnitID(1 + (port & 3) * kChannels + ch);
}

// パラメータ番号を、口・チャンネル・番号と m_value の位置に戻す。
// MIDI のパラメータでなければ false
bool midi_param(ParamID id, int32 &port, int32 &ch, int32 &ctrl, int32 &slot)
{
	int32 x = 0;
	port = -1;
	for (int32 p = 0; p < kPorts; p++)
		if (id >= kPortBase[p] && id < kPortBase[p] + ParamID(kMidiParams)) {
			port = p;
			x = int32(id - kPortBase[p]);
			break;
		}
	if (port < 0)
		return false;
	ch   = x / kCtrlCount;
	ctrl = x % kCtrlCount;
	slot = port * kMidiParams + x;
	return true;
}

// ピッチベンドのパラメータか（これだけ 14bit で、目盛りの付け方が違う）
bool is_bend(ParamID id)
{
	int32 port, ch, ctrl, slot;
	return midi_param(id, port, ch, ctrl, slot) && ctrl == 129;
}

// パラメータの初期値。ホストが起動時にこれを送ってくることがあるので、
// 「初期値と同じ値が来たら何も送らない」ようにするための表でもある
double default_of(int32 ctrl)
{
	switch (ctrl) {
	case 7:   return 100.0 / 127.0;   // ボリューム
	case 10:  return  64.0 / 127.0;   // パン（中央）
	case 11:  return 1.0;             // エクスプレッション
	case 129: return 0.5;             // ピッチベンド（中央）
	default:  return 0.0;
	}
}

void set_str(String128 dst, const char *ascii)
{
	int i = 0;
	for (; ascii[i] && i < 127; i++)
		dst[i] = TChar(uint8(ascii[i]));
	dst[i] = 0;
}


// ---- 本体

class mu_plugin : public IComponent, public IAudioProcessor,
                  public IEditController, public IMidiMapping, public IUnitInfo
{
public:
	mu_plugin()
	{
		for (int32 i = 0; i < kPorts * kMidiParams; i++)
			m_value[i] = default_of(i % kCtrlCount);
		m_engine.panel().set_gain(1.0f);
		m_gain_now = 1.0f;
		m_msgs.reserve(8192);
		m_engine.set_output_rate(smu2000::vst3::NATIVE_RATE);
		m_qpc_freq = perf_frequency();
		smu2000::vst3::engine::trace("create", this);

		// 画面で値を触ったら、ホストへ伝える
		m_engine.set_edit_handlers(
			[this](const xg::param &p, int part, int value) { on_gui_edit(p, part, value); },
			[this](bool closing) { on_gui_idle(closing); },
			[this](u32 addr, int, int value) { on_gui_edit_raw(addr, value); });
	}

	virtual ~mu_plugin()
	{
		smu2000::vst3::engine::trace("destroy", this);
		m_engine.set_edit_handlers(nullptr, nullptr);
		if (m_handler)
			m_handler->release();
	}

	// ---- FUnknown

	tresult PLUGIN_API queryInterface(const TUID _iid, void **obj) override
	{
		if (FUnknownPrivate::iidEqual(_iid, FUnknown::iid) ||
		    FUnknownPrivate::iidEqual(_iid, IComponent::iid)) {
			addRef(); *obj = static_cast<IComponent *>(this); return kResultOk;
		}
		if (FUnknownPrivate::iidEqual(_iid, IPluginBase::iid)) {
			// IComponent と IEditController の両方が IPluginBase を持つので、
			// どちらの側か決めてやらないと曖昧になる
			addRef(); *obj = static_cast<IPluginBase *>(static_cast<IComponent *>(this));
			return kResultOk;
		}
		if (FUnknownPrivate::iidEqual(_iid, IAudioProcessor::iid)) {
			addRef(); *obj = static_cast<IAudioProcessor *>(this); return kResultOk;
		}
		if (FUnknownPrivate::iidEqual(_iid, IEditController::iid)) {
			addRef(); *obj = static_cast<IEditController *>(this); return kResultOk;
		}
		if (FUnknownPrivate::iidEqual(_iid, IMidiMapping::iid)) {
			addRef(); *obj = static_cast<IMidiMapping *>(this); return kResultOk;
		}
		if (FUnknownPrivate::iidEqual(_iid, IUnitInfo::iid)) {
			addRef(); *obj = static_cast<IUnitInfo *>(this); return kResultOk;
		}
		*obj = nullptr;
		return kNoInterface;
	}

	uint32 PLUGIN_API addRef() override
	{ return uint32(FUnknownPrivate::atomicAdd(m_refs, 1)); }

	uint32 PLUGIN_API release() override
	{
		if (FUnknownPrivate::atomicAdd(m_refs, -1) == 0) { delete this; return 0; }
		return uint32(m_refs);
	}

	// ---- IPluginBase

	tresult PLUGIN_API initialize(FUnknown *) override
	{
		// ROM 読みと起動（音にして 4 秒ぶんの空回し）は時間がかかるので、
		// ここでは走らせるだけ。終わるまでは無音を返す
		smu2000::vst3::engine::trace("initialize", this);
		m_engine.unpark();
		m_engine.start();
		return kResultOk;
	}

	// ホストからもらったものはここで手放す（VST3 の決まり）。ホストは terminate の後で
	// IComponentHandler を消してから本体を release することがあり（FL Studio）、持ったままだと
	// 最後の release で消えた物を触って落ちる。この DLL のコードを走るスレッドもここで止める
	// （本体が手放されないまま DLL を外されても、消えたコードを走らないように）
	tresult PLUGIN_API terminate() override
	{
		smu2000::vst3::engine::trace("terminate", this);
		end_all_edits();
		if (IComponentHandler *h = m_handler) {
			m_handler = nullptr;
			h->release();
		}
		m_engine.park();
		smu2000::vst3::engine::trace("terminate done", this);
		return kResultOk;
	}

	// ---- IComponent

	tresult PLUGIN_API getControllerClassId(TUID) override
	{
		// 単一コンポーネント。ホストはこの同じ物から IEditController を取る
		return kNotImplemented;
	}

	tresult PLUGIN_API setIoMode(IoMode) override { return kNotImplemented; }

	int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override
	{
		if (type == kAudio) return 1;          // 出力 1 つと、A/D INPUT の入力 1 つ
		if (type == kEvent) return dir == kInput  ? kPorts : 0;
		return 0;
	}

	tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index,
	                              BusInfo &bus) override
	{
		if (type == kAudio && dir == kOutput && index == 0) {
			bus.mediaType    = kAudio;
			bus.direction    = kOutput;
			bus.channelCount = 2;
			set_str(bus.name, "Stereo Out");
			bus.busType = kMain;
			bus.flags   = BusInfo::kDefaultActive;
			return kResultOk;
		}
		// A/D INPUT。サンプリングで録る音。補助の入力（サイドチェーン）として出す
		if (type == kAudio && dir == kInput && index == 0) {
			bus.mediaType    = kAudio;
			bus.direction    = kInput;
			bus.channelCount = 2;
			set_str(bus.name, "A/D Input");
			bus.busType = kAux;
			bus.flags   = 0;
			return kResultOk;
		}
		if (type == kEvent && dir == kInput && index >= 0 && index < kPorts) {
			bus.mediaType    = kEvent;
			bus.direction    = kInput;
			bus.channelCount = 16;
			// 実機の MIDI IN A-D。B はパート 17-32、C は 33-48、D は 49-64 に届く。
			// C・D は実機では USB だけの口
			static const char *NAMES[4] = { "MIDI In A (Part 1-16)", "MIDI In B (Part 17-32)",
			                                "MIDI In C (Part 33-48)", "MIDI In D (Part 49-64)" };
			set_str(bus.name, NAMES[index]);
			bus.busType = index == 0 ? kMain : kAux;
			bus.flags   = BusInfo::kDefaultActive;
			return kResultOk;
		}
		return kInvalidArgument;
	}

	tresult PLUGIN_API getRoutingInfo(RoutingInfo &, RoutingInfo &) override
	{ return kNotImplemented; }

	tresult PLUGIN_API activateBus(MediaType type, BusDirection dir, int32 index, TBool state) override
	{
		smu2000::vst3::engine::trace("activateBus", this, type * 100 + dir * 10 + index, state);
		return kResultOk;
	}

	tresult PLUGIN_API setActive(TBool state) override
	{
		smu2000::vst3::engine::trace("setActive", this, state);
		if (state) {
			// **ここで起動を待ちきる。**setActive は本スレッドで呼ばれ、時間がかかって
			// よいところなので、ここで待たないとホストは起動中の機械へ MIDI を流し始める。
			// 流された分は溜めてあとでまとめて出すので、曲の頭が崩れる（issue #19）
			if (!m_engine.wait_ready(30000))
				m_engine.log_line("起動が終わらないまま演奏に入る");
		} else {
			m_hush.store(true);
			m_engine.set_processing(false);
			report();
		}
		return kResultOk;
	}

	// 画面が無いので、間に合っていたかどうかは止めるときに記録へ書く
	void report()
	{
		if (!m_produced)
			return;
		const double audio = double(m_produced) / m_rate;
		const double busy  = double(m_busy_ticks) / double(m_qpc_freq);
		char line[256];
		std::snprintf(line, sizeof(line),
		              "%.0f Hz で %.0f 秒ぶん: CPU %.1f%%、1 ブロックの最悪 %.2f ms、"
		              "間に合わなかった回数 %llu",
		              m_rate, audio, 100.0 * busy / audio,
		              1000.0 * double(m_worst_ticks) / double(m_qpc_freq),
		              (unsigned long long)m_late);
		m_engine.log_line(line);
		// 1 ブロックに MIDI が溜めきれないほど届いた（8192 件）ときは、捨てた数を書く
		if (m_dropped) {
			std::snprintf(line, sizeof(line), "1 ブロックの MIDI が多すぎて捨てたメッセージ %llu 件",
			              (unsigned long long)m_dropped);
			m_engine.log_line(line);
		}
		m_busy_ticks = m_produced = m_worst_ticks = m_late = m_dropped = 0;
		// 口ごとの内訳（A B C D）
		auto four = [](const std::atomic<uint32_t> *a, char *out, size_t n) {
			std::snprintf(out, n, "A %u / B %u / C %u / D %u",
			              a[0].load(std::memory_order_relaxed), a[1].load(std::memory_order_relaxed),
			              a[2].load(std::memory_order_relaxed), a[3].load(std::memory_order_relaxed));
		};
		char a[96], b[96], c[96], d[96], e[96], f[96], g[96];
		four(m_got_on, f, sizeof(f));
		four(m_got_off, g, sizeof(g));
		four(m_q_ctrl, a, sizeof(a));
		four(m_q_unit, b, sizeof(b));
		four(m_got_cc, c, sizeof(c));
		four(m_got_pc, d, sizeof(d));
		four(m_got_ev, e, sizeof(e));
		char line2[900];
		std::snprintf(line2, sizeof(line2),
		              "口ごとの内訳: CC の対応の問い合わせ [%s]、音色のユニットの問い合わせ [%s]、"
		              "届いた CC 等 [%s]、届いたプログラムチェンジ [%s]、届いたイベント（ノート等） [%s]、"
		              "うちノートオン [%s]、ノートオフ [%s]",
		              a, b, c, d, e, f, g);
		m_engine.log_line(line2);
	}

	tresult PLUGIN_API setState(IBStream *stream) override
	{
		if (!stream)
			return kResultFalse;
		int32 version = 0, got = 0;
		float gain = 1.0f;
		if (stream->read(&version, sizeof(version), &got) != kResultOk || got != sizeof(version))
			return kResultOk;   // 空でも困らない
		if (stream->read(&gain, sizeof(gain), &got) == kResultOk && got == sizeof(gain) &&
		    gain >= 0.0f && gain <= 1.0f)
			m_engine.panel().set_gain(gain);
		if (version < 2)
			return kResultOk;   // 古い形。出力レベルだけ

		// 機械まるごとの状態。詰めた形で入っている（起動中に保存された曲では長さが 0）
		int32 packed_size = 0;
		std::vector<u8> blob;
		if (stream->read(&packed_size, sizeof(packed_size), &got) == kResultOk &&
		    got == sizeof(packed_size) && packed_size > 0 && packed_size <= (64 << 20)) {
			std::vector<uint8_t> packed(static_cast<size_t>(packed_size));
			if (stream->read(packed.data(), packed_size, &got) != kResultOk || got != packed_size ||
			    !state_unpack(packed.data(), packed.size(), blob))
				blob.clear();
		}

		// 版 3 から: 差していた SmartMedia のファイル（UTF-8）。無くなっていたら差さない
		std::string card;
		if (version >= 3) {
			int32 len = 0;
			if (stream->read(&len, sizeof(len), &got) == kResultOk && got == sizeof(len) && len > 0 && len < 4096) {
				std::string path(size_t(len), '\0');
				if (stream->read(path.data(), len, &got) == kResultOk && got == len)
					card = path;
			}
		}
		// 版 4 から: XG の値だけの控え（engine::save_xg_setup）。機械まるごとの状態が読めないときに使う
		std::vector<uint8_t> setup;
		if (version >= 4) {
			int32 len = 0;
			if (stream->read(&len, sizeof(len), &got) == kResultOk && got == sizeof(len) &&
			    len > 0 && len < (1 << 20)) {
				setup.resize(size_t(len));
				if (stream->read(setup.data(), len, &got) != kResultOk || got != len)
					setup.clear();
			}
		}

		// 起動が終わっていないと戻せない。終わるまで待つ
		m_engine.wait_ready(3000);
		if (!blob.empty() || !setup.empty())
			m_engine.load_state(blob.empty() ? nullptr : blob.data(), blob.size(),
			                    setup.empty() ? nullptr : setup.data(), setup.size());
		m_engine.publish_xg_now();   // 戻した値を bridge にも。古い写しが flood を呼ぶ

		if (!card.empty()) {
			std::string err;
			if (!m_engine.card_insert(card, err))
				m_engine.log_line(("SmartMedia を差せない: " + err).c_str());
		} else if (version < 3 && !m_engine.card_path().empty()) {
			m_engine.card_eject();
		}
		// XG の値が替わったので、ホストの持っている値を読み直させる
		m_xg.forget_recent();
		if (m_handler)
			m_handler->restartComponent(kParamValuesChanged);
		return kResultOk;
	}

	tresult PLUGIN_API getState(IBStream *stream) override
	{
		// **機械まるごと**（CPU・RAM・SWP30・LCD）を入れる。だから曲を
		// 開き直しても、音色もエフェクトもそのまま戻る
		if (!stream)
			return kResultFalse;
		int32 version = 4;
		float gain = m_engine.panel().gain();
		m_engine.card_flush();   // プロジェクトを保存するときに、カードのファイルも揃える
		int32 written = 0;
		stream->write(&version, sizeof(version), &written);
		stream->write(&gain, sizeof(gain), &written);

		const std::vector<uint8_t> blob = m_engine.save_state();
		if (blob.empty())
			return kResultOk;                 // まだ起動中など
		const std::vector<u8> packed = state_pack(blob);
		int32 n = int32(packed.size());
		stream->write(&n, sizeof(n), &written);
		stream->write(const_cast<uint8_t *>(packed.data()), n, &written);
		// 版 3: 差している SmartMedia のファイル。中身はプロジェクトに入れない（16MB から 128MB あるので）
		const std::string card = m_engine.card_path();
		int32 len = int32(card.size());
		stream->write(&len, sizeof(len), &written);
		if (len)
			stream->write(const_cast<char *>(card.data()), len, &written);
		// 版 4: XG の値だけの控え。S-MU2000 の版が変わって機械まるごとの状態が読めなくなっても、
		// これで音色とエフェクトの設定は戻る（数 KB）
		const std::vector<uint8_t> setup = m_engine.save_xg_setup();
		int32 sn = int32(setup.size());
		stream->write(&sn, sizeof(sn), &written);
		if (sn)
			stream->write(const_cast<uint8_t *>(setup.data()), sn, &written);
		return kResultOk;
	}

	// ---- IAudioProcessor

	tresult PLUGIN_API setBusArrangements(SpeakerArrangement *inputs, int32 numIns,
	                                      SpeakerArrangement *outputs, int32 numOuts) override
	{
		smu2000::vst3::engine::trace("setBusArrangements", this, numIns, numOuts);
		if (numOuts == 1 && outputs[0] == SpeakerArr::kStereo &&
		    (numIns == 0 || (numIns == 1 && inputs[0] == SpeakerArr::kStereo)))
			return kResultTrue;
		return kResultFalse;
	}

	tresult PLUGIN_API getBusArrangement(BusDirection dir, int32 index,
	                                     SpeakerArrangement &arr) override
	{
		if (index == 0) { arr = SpeakerArr::kStereo; return kResultOk; }
		return kInvalidArgument;
	}

	tresult PLUGIN_API canProcessSampleSize(int32 size) override
	{ return size == kSample32 ? kResultTrue : kResultFalse; }

	uint32 PLUGIN_API getLatencySamples() override { return m_engine.latency_samples(); }

	tresult PLUGIN_API setupProcessing(ProcessSetup &setup) override
	{
		smu2000::vst3::engine::trace("setupProcessing", this, (long long)setup.sampleRate, setup.maxSamplesPerBlock);
		m_rate = setup.sampleRate;
		m_engine.set_output_rate(m_rate);
		return kResultOk;
	}

	tresult PLUGIN_API setProcessing(TBool state) override
	{
		smu2000::vst3::engine::trace("setProcessing", this, state);
		if (!state)
			m_hush.store(true);
		// 動いているあいだ、機械に触れてよいのは音声スレッドだけ
		m_engine.set_processing(state != 0);
		return kResultOk;
	}

	uint32 PLUGIN_API getTailSamples() override
	{
		// 残響がある。4 秒みておく
		return uint32(m_rate * 4.0);
	}

	tresult PLUGIN_API process(ProcessData &data) override;

	// ---- IEditController

	tresult PLUGIN_API setComponentState(IBStream *) override { return kResultOk; }

	int32 PLUGIN_API getParameterCount() override { return param_count(); }

	tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo &info) override
	{
		if (index < 0 || index >= param_count())
			return kInvalidArgument;
		// XG の値（後ろに並べてある）
		if (index >= kXgFirst) {
			const autom::entry &e = autom::entries()[size_t(index - kXgFirst)];
			std::memset(&info, 0, sizeof(info));
			info.id = e.id;
			set_str(info.title, e.name.c_str());
			set_str(info.shortTitle, e.name.c_str());
			// インサーションのパラメータは種類で範囲が変わるので、目盛りは付けない（割合で連続）
			info.stepCount = e.k == autom::kind::insertion ? 0 : autom::steps(e);
			info.defaultNormalizedValue = autom::to_normalized(e, autom::def(e));
			info.unitId = e.k == autom::kind::insertion ? kXgInsUnit + e.block
			            : e.is_part ? kXgPartUnit + e.part : kXgMasterUnit;
			info.flags = ParameterInfo::kCanAutomate;
			return kResultOk;
		}
		// 並びは A の 2096 本、Output、Status、B・C・D の 2096 本ずつ。
		// 前からあるものの位置を変えないよう、B 以降は後ろに足した
		if (index == kMidiParams || index == kMidiParams + 1) {
			std::memset(&info, 0, sizeof(info));
			if (index == kMidiParams) {
				info.id = kGainId;
				set_str(info.title, "Output");
				set_str(info.shortTitle, "Out");
				set_str(info.units, "%");
				info.defaultNormalizedValue = 1.0;
				info.flags = ParameterInfo::kCanAutomate;
			} else {
				info.id = kStatusId;
				set_str(info.title, "Status");
				set_str(info.shortTitle, "Stat");
				info.stepCount = 2;
				info.flags = ParameterInfo::kIsReadOnly;
			}
			return kResultOk;
		}
		const int32 after = index - kMidiParams - 2;      // Output / Status の後ろ
		const int32 port = index < kMidiParams ? 0 : 1 + after / kMidiParams;
		const int32 x = port ? after % kMidiParams : index;
		const int32 ch = x / kCtrlCount, ctrl = x % kCtrlCount;

		// B 以降は頭に口の字を付ける（A は前からの名前のまま）
		static const char *PRE[4] = { "", "B ", "C ", "D " };
		const char *pre = PRE[port & 3];
		char name[64];
		if (ctrl < 128)      std::snprintf(name, sizeof(name), "%sCh%d CC%d", pre, ch + 1, ctrl);
		else if (ctrl == 128) std::snprintf(name, sizeof(name), "%sCh%d Aftertouch", pre, ch + 1);
		else if (ctrl == 129) std::snprintf(name, sizeof(name), "%sCh%d Pitch Bend", pre, ch + 1);
		else                  std::snprintf(name, sizeof(name), "%sCh%d Program", pre, ch + 1);

		std::memset(&info, 0, sizeof(info));
		info.id = param_of(port, ch, ctrl);
		set_str(info.title, name);
		set_str(info.shortTitle, name);
		info.stepCount = (ctrl == 129) ? 0 : 127;   // ピッチベンドだけ連続
		info.defaultNormalizedValue = default_of(ctrl);
		info.unitId = 0;   // kRootUnitId
		// 4192 本もあるので、一覧に並べさせない
		info.flags = ParameterInfo::kCanAutomate | ParameterInfo::kIsHidden;
		// プログラムチェンジはそのチャンネルのユニットに属させ、印を付ける（Cubase 向け。上の unit_of）
		if (ctrl == 130) {
			info.unitId = unit_of(port, ch);
			info.flags |= ParameterInfo::kIsProgramChange | ParameterInfo::kIsList;
		}
		return kResultOk;
	}

	tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue v, String128 str) override
	{
		if (id == kStatusId) {
			// ここが唯一の「表に見える」窓口。無音の理由がこれで分かる
			const int k = int(std::lround(v * 2.0));
			set_str(str, k == 0 ? "Booting" : k == 1 ? "Ready" : "No ROM");
			return kResultOk;
		}
		if (id == kGainId) {
			char g[32];
			std::snprintf(g, sizeof(g), "%.0f", v * 100.0);
			set_str(str, g);
			return kResultOk;
		}
		if (const autom::entry *e = xg_entry(id)) {
			set_str(str, autom::text(*e, autom::to_value(*e, v), m_xg.view_ram()).c_str());
			return kResultOk;
		}
		int32 port, ch, ctrl, slot;
		if (!midi_param(id, port, ch, ctrl, slot))
			return kInvalidArgument;
		char buf[32];
		if (ctrl == 129)
			std::snprintf(buf, sizeof(buf), "%+d", int(std::lround(v * 16383.0)) - 8192);
		else
			std::snprintf(buf, sizeof(buf), "%d", int(std::lround(v * 127.0)));
		set_str(str, buf);
		return kResultOk;
	}

	tresult PLUGIN_API getParamValueByString(ParamID id, TChar *str, ParamValue &v) override
	{
		if (!str)
			return kInvalidArgument;
		if (id == kGainId || id == kStatusId)
			return kResultFalse;
		char buf[32];
		int i = 0;
		for (; i < 31 && str[i]; i++)
			buf[i] = char(str[i]);
		buf[i] = 0;
		if (const autom::entry *e = xg_entry(id)) {
			int value = 0;
			if (!autom::parse(*e, buf, value, m_xg.view_ram()))
				return kResultFalse;
			v = autom::to_normalized(*e, value);
			return kResultOk;
		}
		int32 port, ch, ctrl, slot;
		if (!midi_param(id, port, ch, ctrl, slot))
			return kInvalidArgument;
		const double plain = std::atof(buf);
		v = ctrl == 129 ? std::clamp((plain + 8192.0) / 16383.0, 0.0, 1.0)
		                : std::clamp(plain / 127.0, 0.0, 1.0);
		return kResultOk;
	}

	ParamValue PLUGIN_API normalizedParamToPlain(ParamID id, ParamValue v) override
	{
		if (id == kGainId)   return v * 100.0;
		if (id == kStatusId) return std::lround(v * 2.0);
		if (const autom::entry *e = xg_entry(id)) return autom::to_value(*e, v);
		return is_bend(id) ? std::lround(v * 16383.0) - 8192.0
		                   : std::lround(v * 127.0);
	}

	ParamValue PLUGIN_API plainParamToNormalized(ParamID id, ParamValue plain) override
	{
		if (id == kGainId)   return std::clamp(plain / 100.0, 0.0, 1.0);
		if (id == kStatusId) return std::clamp(plain / 2.0, 0.0, 1.0);
		if (const autom::entry *e = xg_entry(id)) return autom::to_normalized(*e, autom::clamp_value(*e, plain));
		return is_bend(id) ? std::clamp((plain + 8192.0) / 16383.0, 0.0, 1.0)
		                   : std::clamp(plain / 127.0, 0.0, 1.0);
	}

	ParamValue PLUGIN_API getParamNormalized(ParamID id) override
	{
		if (id == kGainId)
			return m_engine.panel().gain();
		if (id == kStatusId) {
			switch (m_engine.state()) {
			case smu2000::vst3::status::loading: return 0.0;
			case smu2000::vst3::status::ready:   return 0.5;
			default:                             return 1.0;
			}
		}
		const int xi = autom::index_of(uint32_t(id));
		if (xi >= 0)
			return m_xg.shown_normalized(xi);
		int32 port, ch, ctrl, slot;
		return midi_param(id, port, ch, ctrl, slot) ? m_value[slot] : 0.0;
	}

	tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue v) override
	{
		if (id == kGainId) { m_engine.panel().set_gain(float(std::clamp(v, 0.0, 1.0))); return kResultOk; }
		if (id == kStatusId)
			return kResultFalse;   // 読むだけ
		const int xi = autom::index_of(uint32_t(id));
		if (xi >= 0) {
			// 音源へは process に来る値で入れる。ここは表示のための控えだけ
			m_xg.remember(xi, autom::to_value(autom::entries()[size_t(xi)], v), v);
			return kResultOk;
		}
		int32 port, ch, ctrl, slot;
		if (!midi_param(id, port, ch, ctrl, slot))
			return kInvalidArgument;
		m_value[slot] = v;
		return kResultOk;
	}

	tresult PLUGIN_API setComponentHandler(IComponentHandler *handler) override
	{
		smu2000::vst3::engine::trace("setComponentHandler", this, handler != nullptr);
		if (handler == m_handler)
			return kResultOk;
		end_all_edits();
		if (handler)
			handler->addRef();
		if (m_handler)
			m_handler->release();
		m_handler = handler;
		return kResultOk;
	}

	// 画面。実機のフロントパネル風。中身は gui.exe と同じ ui::panel
	IPlugView *PLUGIN_API createView(FIDString name) override
	{
		smu2000::vst3::engine::trace("createView", this);
		if (name && std::strcmp(name, ViewType::kEditor) != 0)
			return nullptr;
		// 画面は本体の参照を持つ（ホストが本体を先に手放しても、画面が消えるまで engine を残す）
		return new smu2000::vst3::plug_view(m_engine, static_cast<IEditController *>(this));
	}

	// 音量は bridge が 1 つだけ持つ。Output パラメータも画面のつまみも同じ値

	// ---- IMidiMapping

	tresult PLUGIN_API getMidiControllerAssignment(int32 busIndex, int16 channel,
	                                               CtrlNumber ctrl, ParamID &id) override
	{
		if (busIndex < 0 || busIndex >= kPorts || channel < 0 || channel >= kChannels)
			return kResultFalse;
		if (ctrl < 0 || ctrl >= kCtrlCount)
			return kResultFalse;
		m_q_ctrl[busIndex].fetch_add(1, std::memory_order_relaxed);
		id = param_of(busIndex, channel, ctrl);
		return kResultTrue;
	}

	// ---- IUnitInfo（Cubase のプログラムチェンジ。上の unit_of）

	int32 PLUGIN_API getUnitCount() override { return 1 + kPorts * kChannels + 64 + 1 + 4; }

	tresult PLUGIN_API getUnitInfo(int32 unitIndex, UnitInfo &info) override
	{
		if (unitIndex < 0 || unitIndex >= getUnitCount())
			return kInvalidArgument;
		std::memset(&info, 0, sizeof(info));
		if (unitIndex == 0) {
			info.id = kRootUnitId;
			info.parentUnitId = kNoParentUnitId;
			set_str(info.name, "Root");
			info.programListId = kNoProgramListId;
			return kResultOk;
		}
		// XG の値のユニット（パート 64 とマスター）
		if (unitIndex > kPorts * kChannels) {
			const int32 j = unitIndex - 1 - kPorts * kChannels;
			char name[32];
			if (j < 64)
				std::snprintf(name, sizeof(name), "XG Part %c%d", char('A' + j / 16), j % 16 + 1);
			else if (j == 64)
				std::snprintf(name, sizeof(name), "XG Master");
			else
				std::snprintf(name, sizeof(name), "XG Insertion %d", j - 64);
			info.id = j < 64 ? kXgPartUnit + j : j == 64 ? kXgMasterUnit : kXgInsUnit + (j - 65);
			info.parentUnitId = kRootUnitId;
			set_str(info.name, name);
			info.programListId = kNoProgramListId;
			return kResultOk;
		}
		const int32 port = (unitIndex - 1) / kChannels, ch = (unitIndex - 1) % kChannels;
		char name[32];
		std::snprintf(name, sizeof(name), "%c Ch%d", char('A' + port), ch + 1);
		info.id = unit_of(port, ch);
		info.parentUnitId = kRootUnitId;
		set_str(info.name, name);
		info.programListId = kProgramList;
		return kResultOk;
	}

	int32 PLUGIN_API getProgramListCount() override { return 1; }

	tresult PLUGIN_API getProgramListInfo(int32 listIndex, ProgramListInfo &info) override
	{
		if (listIndex != 0)
			return kInvalidArgument;
		std::memset(&info, 0, sizeof(info));
		info.id = kProgramList;
		set_str(info.name, "Program");
		info.programCount = kPrograms;
		return kResultOk;
	}

	tresult PLUGIN_API getProgramName(ProgramListID listId, int32 programIndex, String128 name) override
	{
		if (listId != kProgramList || programIndex < 0 || programIndex >= kPrograms)
			return kInvalidArgument;
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%03d", programIndex + 1);
		set_str(name, buf);
		return kResultOk;
	}

	tresult PLUGIN_API getProgramInfo(ProgramListID, int32, Steinberg::Vst::CString, String128) override
	{ return kNotImplemented; }

	tresult PLUGIN_API hasProgramPitchNames(ProgramListID, int32) override { return kResultFalse; }

	tresult PLUGIN_API getProgramPitchName(ProgramListID, int32, int16, String128) override
	{ return kNotImplemented; }

	UnitID PLUGIN_API getSelectedUnit() override { return kRootUnitId; }

	tresult PLUGIN_API selectUnit(UnitID) override { return kResultOk; }

	tresult PLUGIN_API getUnitByBus(MediaType type, BusDirection dir, int32 busIndex,
	                                int32 channel, UnitID &unitId) override
	{
		if (type != kEvent || dir != kInput || busIndex < 0 || busIndex >= kPorts ||
		    channel < 0 || channel >= kChannels)
			return kResultFalse;
		m_q_unit[busIndex].fetch_add(1, std::memory_order_relaxed);
		unitId = unit_of(busIndex, channel);
		return kResultTrue;
	}

	tresult PLUGIN_API setUnitProgramData(int32, int32, IBStream *) override
	{ return kNotImplemented; }

private:
	// process の中で時刻順に並べ直すための入れ物。
	// 短いもの（XG の値のパラメータチェンジも）は中に持ち、ホストのシステムエクスクルーシブはホストの領域を指す
	struct msg {
		int32          off;
		int32          seq;
		uint8          port;          // 0 が MIDI IN A、1 が B、2 が C、3 が D
		uint8          n;
		uint8          b[16];
		const uint8   *sysex;
		uint32         sysex_len;
		uint8          rank = smu2000::vst3::RANK_OTHER;   // 同じ時刻のなかの順番（pc_order.h。リセットが先、次にバンクセレクト、プログラムチェンジ）
	};

	void queue(int32 port, int32 off, uint8 a, uint8 b = 0, uint8 c = 0, int n = 3)
	{
		if (m_msgs.size() >= m_msgs.capacity()) {
			m_dropped++;
			return;
		}
		// 鳴らしたチャンネルを覚えておく。止めるときはここだけに流す（下の m_hush）
		if ((a & 0xf0) == 0x90 && c)
			m_sounded[port & 3] |= uint16(1u << (a & 15));
		msg m{ off, int32(m_msgs.size()), uint8(port), uint8(n), {}, nullptr, 0 };
		m.b[0] = a; m.b[1] = b; m.b[2] = c;
		m_msgs.push_back(m);
	}

	void queue_bytes(int32 port, int32 off, const uint8 *bytes, int n)
	{
		if (n <= 0 || n > 16)
			return;
		if (m_msgs.size() >= m_msgs.capacity()) {
			m_dropped++;
			return;
		}
		msg m{ off, int32(m_msgs.size()), uint8(port), uint8(n), {}, nullptr, 0 };
		std::memcpy(m.b, bytes, size_t(n));
		m_msgs.push_back(m);
	}

	// ---- XG の値のパラメータ（automation_host.h）

	static const autom::entry *xg_entry(ParamID id)
	{
		const int i = autom::index_of(uint32_t(id));
		return i >= 0 ? &autom::entries()[size_t(i)] : nullptr;
	}

	// 画面で値を触った（画面の糸）。ホストのオートメーションへ伝える
	void on_gui_edit(const xg::param &p, int part, int value)
	{
		bool began = false;
		const int i = m_xg.gui_edit(p, part, value, began);
		if (i < 0 || !m_handler)
			return;
		const autom::entry &e = autom::entries()[size_t(i)];
		if (began)
			m_handler->beginEdit(e.id);
		m_handler->performEdit(e.id, autom::to_normalized(e, value));
	}

	// 画面でインサーションのパラメータを触った（インサーションの設定の窓）
	void on_gui_edit_raw(u32 addr, int raw)
	{
		bool began = false;
		int value = 0;
		const int i = m_xg.gui_edit_raw(addr, raw, value, began);
		if (i < 0 || !m_handler)
			return;
		const autom::entry &e = autom::entries()[size_t(i)];
		if (began)
			m_handler->beginEdit(e.id);
		m_handler->performEdit(e.id, autom::to_normalized(e, value));
	}

	// 画面の 1 コマ。しばらく触られていない値の操作を終える
	void on_gui_idle(bool closing)
	{
		m_xg.gui_idle(closing, [this](int i) {
			if (m_handler)
				m_handler->endEdit(autom::entries()[size_t(i)].id);
		});
	}

	void end_all_edits() { on_gui_idle(true); }

	// ホストから来た XG の値（音声の糸）。値が変わったところだけ、CC かパラメータチェンジにして流す
	void xg_points(int i, IParamValueQueue *pq)
	{
		const autom::entry &e = autom::entries()[size_t(i)];
		const int32 np = pq->getPointCount();
		for (int32 k = 0; k < np; k++) {
			int32 off = 0;
			ParamValue v = 0.0;
			if (pq->getPoint(k, off, v) != kResultOk)
				continue;
			m_xg.host_value(i, autom::to_value(e, v), [&](int port, const uint8_t *bytes, int n) {
				queue_bytes(port, off, bytes, n);
			});
		}
	}

	smu2000::vst3::engine m_engine;
	autom::host           m_xg{m_engine};
	std::vector<msg>      m_msgs;
	// **口ごとの内訳**（report でログへ）。ホストが複数の口を正しく使っているかを見る。
	// SONAR で口 B のプログラムチェンジが口 A に届いた件の調べ用。
	// [口]。q_ctrl は CC の対応の問い合わせ、q_unit はプログラムチェンジのユニットの問い合わせ
	// （どちらも本スレッド）、got_* は届いたもの（音声の糸）
	std::atomic<uint32_t> m_q_ctrl[kPorts] = {}, m_q_unit[kPorts] = {};
	std::atomic<uint32_t> m_got_cc[kPorts] = {}, m_got_pc[kPorts] = {}, m_got_ev[kPorts] = {};
	// ノートオン・ノートオフ（ベロシティ 0 のノートオンを含む）。数が合わなければ、
	// ホストから届く前に離しが落ちている
	std::atomic<uint32_t> m_got_on[kPorts] = {}, m_got_off[kPorts] = {};
	IComponentHandler    *m_handler = nullptr;
	double                m_rate = smu2000::vst3::NATIVE_RATE;
	double                m_value[kPorts * kMidiParams] = {};
	// 出力レベルは bridge が持つ。ここは 1 サンプルずつ寄せる途中の値
	float                 m_gain_now = 1.0f;
	std::atomic<bool>     m_hush{false};
	// 音を出したチャンネル（口ごとに 16 ビット）。止めるときに流す先を絞る
	std::atomic<uint16>   m_sounded[kPorts] = {};
	// 間に合っているかの記録。音声スレッドだけが触る
	uint64                m_busy_ticks = 0, m_produced = 0, m_worst_ticks = 0, m_late = 0;
	uint64                m_dropped = 0;       // 溜めきれずに捨てた MIDI（report で書く）
	int64                 m_qpc_freq = 1;
	int32                 m_refs = 1;
};


tresult PLUGIN_API mu_plugin::process(ProcessData &data)
{
	// 64bit 浮動小数は受けないと答えてある。それでも来たら音を出さない
	// （倍精度の配列を単精度として書けば壊れる）
	AudioBusBuffers *out = (data.numOutputs > 0 &&
	                        data.symbolicSampleSize == kSample32) ? &data.outputs[0] : nullptr;
	float *left  = (out && out->numChannels > 0) ? out->channelBuffers32[0] : nullptr;
	float *right = (out && out->numChannels > 1) ? out->channelBuffers32[1] : left;
	// A/D INPUT（補助の入力）。繋がっていなければ無し
	AudioBusBuffers *in = (data.numInputs > 0 && data.inputs &&
	                       data.symbolicSampleSize == kSample32) ? &data.inputs[0] : nullptr;
	const float *in_l = (in && in->numChannels > 0 && in->channelBuffers32) ? in->channelBuffers32[0] : nullptr;
	const float *in_r = (in && in->numChannels > 1 && in->channelBuffers32) ? in->channelBuffers32[1] : in_l;

	const uint64 t0 = perf_ticks();

	// ホストが止めたときは、鳴らしたチャンネルだけを黙らせる。全チャンネルへ流すと
	// 1 口につき 192 バイト＝61ms ぶんの直列になり、次に再生した最初の音がそのぶん遅れる（issue #15）
	if (m_hush.exchange(false)) {
		uint16 mask[kPorts];
		bool any = false;
		for (int32 p = 0; p < kPorts; p++) {
			mask[p] = m_sounded[p].exchange(0);
			any = any || mask[p];
		}
		if (any)
			m_engine.all_notes_off(mask, kPorts);
	}

	// ---- まず、この区間に来た MIDI を全部集める

	m_msgs.clear();

	// 最初の区間の頭で RAM から種を仕込む（foo_midi らの再生頭のパラメータ
	// 再送を直列に載せる前に弾く。automation_host.h の seed_values）
	if (!m_xg.seeded())
		m_xg.seed_values();

	m_xg.begin_block();
	if (IParameterChanges *changes = data.inputParameterChanges) {
		const int32 nq = changes->getParameterCount();
		for (int32 q = 0; q < nq; q++) {
			IParamValueQueue *pq = changes->getParameterData(q);
			if (!pq)
				continue;
			const ParamID id = pq->getParameterId();
			if (id == kGainId) {
				// 出力レベルは音源に流さない。外で掛ける。最後の値だけ見る
				int32 off = 0;
				ParamValue v = 0.0;
				if (pq->getPointCount() > 0 &&
				    pq->getPoint(pq->getPointCount() - 1, off, v) == kResultOk)
					m_engine.panel().set_gain(float(std::clamp(v, 0.0, 1.0)));
				continue;
			}
			const int xi = autom::index_of(uint32_t(id));
			if (xi >= 0) {
				xg_points(xi, pq);
				continue;
			}
			int32 port, ch, ctrl, slot;
			if (!midi_param(id, port, ch, ctrl, slot))
				continue;
			const int32 np = pq->getPointCount();
			(ctrl == 130 ? m_got_pc : m_got_cc)[port].fetch_add(uint32_t(np), std::memory_order_relaxed);
			for (int32 p = 0; p < np; p++) {
				int32 off = 0;
				ParamValue v = 0.0;
				if (pq->getPoint(p, off, v) != kResultOk)
					continue;
				m_value[slot] = v;
				// ここで「前と同じ値だから」と捨ててはいけない。RPN/NRPN は
				// CC101=0, CC100=0, CC6=n のように同じ値を続けて送ることに
				// 意味があり、捨てるとピッチベンド幅などが化ける

				if (ctrl < 128) {
					queue(port, off, uint8(0xb0 | ch), uint8(ctrl),
					      uint8(std::clamp(int(std::lround(v * 127.0)), 0, 127)));
				} else if (ctrl == 128) {
					queue(port, off, uint8(0xd0 | ch),
					      uint8(std::clamp(int(std::lround(v * 127.0)), 0, 127)), 0, 2);
				} else if (ctrl == 129) {
					const int bend = std::clamp(int(std::lround(v * 16383.0)), 0, 16383);
					queue(port, off, uint8(0xe0 | ch), uint8(bend & 127), uint8(bend >> 7));
				} else {
					// プログラムチェンジ。バンクセレクトより先に並ばないよう、下でまとめて直す（pc_order.h）
					queue(port, off, uint8(0xc0 | ch),
					      uint8(std::clamp(int(std::lround(v * 127.0)), 0, 127)), 0, 2);
				}
			}
		}
	}

	// ここまでがパラメータから作ったメッセージ。プログラムチェンジを、同じブロックのバンクセレクトの後ろへ
	// （REAPER はプログラムチェンジだけブロックの頭で渡してくる。プルリクエスト #143）
	smu2000::vst3::order_program_changes(m_msgs, m_msgs.size());

	if (IEventList *events = data.inputEvents) {
		const int32 n = events->getEventCount();
		for (int32 i = 0; i < n; i++) {
			Event e;
			if (events->getEvent(i, e) != kResultOk)
				continue;
			const int32 off = e.sampleOffset;
			const int32 port = (e.busIndex >= 0 && e.busIndex < kPorts) ? e.busIndex : 0;
			m_got_ev[port].fetch_add(1, std::memory_order_relaxed);
			switch (e.type) {
			case Event::kNoteOnEvent: {
				(e.noteOn.velocity > 0.0f ? m_got_on : m_got_off)[port].fetch_add(1, std::memory_order_relaxed);
				const int v = std::clamp(int(std::lround(e.noteOn.velocity * 127.0)), 1, 127);
				queue(port, off, uint8(0x90 | (e.noteOn.channel & 15)),
				      uint8(e.noteOn.pitch & 127), uint8(v));
				break;
			}
			case Event::kNoteOffEvent: {
				m_got_off[port].fetch_add(1, std::memory_order_relaxed);
				const int v = std::clamp(int(std::lround(e.noteOff.velocity * 127.0)), 0, 127);
				queue(port, off, uint8(0x80 | (e.noteOff.channel & 15)),
				      uint8(e.noteOff.pitch & 127), uint8(v));
				break;
			}
			case Event::kPolyPressureEvent: {
				const int v = std::clamp(int(std::lround(e.polyPressure.pressure * 127.0)), 0, 127);
				queue(port, off, uint8(0xa0 | (e.polyPressure.channel & 15)),
				      uint8(e.polyPressure.pitch & 127), uint8(v));
				break;
			}
			case Event::kDataEvent: {
				if (e.data.type != DataEvent::kMidiSysEx || !e.data.bytes || !e.data.size)
					break;
				// ホストによっては、チャンネルメッセージまでここに入れてくる
				// （VSTHost 1.58 はプログラムチェンジを C0 xx 00 と 3 byte にして渡す）。
				// そのまま音源へ流すと、余分な 00 が走行状態のデータバイトになり、
				// 続けてプログラム 0 が入って音色が戻ってしまう。頭がチャンネルの
				// ステータスならシステムエクスクルーシブではないので、
				// 決まった長さだけ取り出して流し、後ろの詰め物は捨てる
				const uint8 first = e.data.bytes[0];
				if (first >= 0x80 && first < 0xf0) {
					for (uint32 at = 0; at < e.data.size;) {
						const uint8 st = e.data.bytes[at];
						if (st < 0x80 || st >= 0xf0)
							break;      // 詰め物。ここから先は読まない
						const uint32 len = uint32(smu2000::vst3::midi_length(st));
						if (at + len > e.data.size)
							break;
						queue(port, off, st,
						      len > 1 ? e.data.bytes[at + 1] : uint8(0),
						      len > 2 ? e.data.bytes[at + 2] : uint8(0), int(len));
						at += len;
					}
					break;
				}
				// 本物のシステムエクスクルーシブ（と、その途中の切れ端）はバイト列のまま
				if (m_msgs.size() < m_msgs.capacity())
					m_msgs.push_back({ off, int32(m_msgs.size()), uint8(port), 0, { 0, 0, 0 },
					                   e.data.bytes, e.data.size,
					                   uint8(smu2000::vst3::is_reset_sysex(e.data.bytes, e.data.size) ? smu2000::vst3::RANK_RESET : smu2000::vst3::RANK_OTHER) });
				else
					m_dropped++;
				break;
			}
			default:
				break;
			}
		}
	}

	// 時刻順。同じ時刻なら rank の順（リセット、バンクセレクト、プログラムチェンジ、そのほか。pc_order.h）。
	// リセットを先にするのは（is_reset_sysex。パラメータから作った音色の指定が
	// SysEx のリセットより先に並ぶので、そのままだとリセットが音色を消す。issue #51）
	std::sort(m_msgs.begin(), m_msgs.end(), [](const msg &a, const msg &b) {
		if (a.off != b.off)
			return a.off < b.off;
		if (a.rank != b.rank)
			return a.rank < b.rank;
		return a.seq < b.seq;
	});

	// ---- 時刻順に、音を作りながら流し込む

	const int32 n = data.numSamples;
	int32 done = 0;
	for (const msg &m : m_msgs) {
		const int32 at = std::clamp(m.off, done, n);
		if (at > done && left) {
			m_engine.fill(left + done, right + done, at - done,
			              in_l ? in_l + done : nullptr, in_r ? in_r + done : nullptr);
			done = at;
		} else if (at > done) {
			done = at;
		}
		if (m.sysex)
			m_engine.midi(m.sysex, m.sysex_len, m.port);
		else
			m_engine.midi(m.b, m.n, m.port);
	}
	if (left && done < n)
		m_engine.fill(left + done, right + done, n - done,
		              in_l ? in_l + done : nullptr, in_r ? in_r + done : nullptr);

	// 出力レベル。一気に変えると音が跳ねるので 1 サンプルずつ寄せる
	if (left) {
		const float target = m_engine.panel().gain();
		if (target != m_gain_now || target != 1.0f) {
			const float step = 1.0f / 512.0f;
			for (int32 i = 0; i < n; i++) {
				if (m_gain_now < target) m_gain_now = std::min(target, m_gain_now + step);
				else if (m_gain_now > target) m_gain_now = std::max(target, m_gain_now - step);
				left[i]  *= m_gain_now;
				right[i] *= m_gain_now;
			}
		}
	}

	// 間に合っているかの目安。setActive(false) のときに記録へ書く
	const uint64 took = perf_ticks() - t0;
	m_busy_ticks += took;
	m_produced   += uint64(n);
	if (took > m_worst_ticks) m_worst_ticks = took;
	if (double(took) / double(m_qpc_freq) > double(n) / m_rate) m_late++;

	if (out) {
		out->silenceFlags = 0;
		// 片側しか無いホストのために、右が左と同じ配列でも困らないようにしてある
		if (out->numChannels > 2)
			for (int32 c = 2; c < out->numChannels; c++)
				std::memset(out->channelBuffers32[c], 0, size_t(n) * sizeof(float));
	}
	return kResultOk;
}


// ---- 工場。ホストはまずこれを取りに来る

class factory : public IPluginFactory3
{
public:
	tresult PLUGIN_API queryInterface(const TUID _iid, void **obj) override
	{
		if (FUnknownPrivate::iidEqual(_iid, FUnknown::iid) ||
		    FUnknownPrivate::iidEqual(_iid, IPluginFactory::iid) ||
		    FUnknownPrivate::iidEqual(_iid, IPluginFactory2::iid) ||
		    FUnknownPrivate::iidEqual(_iid, IPluginFactory3::iid)) {
			addRef(); *obj = static_cast<IPluginFactory3 *>(this); return kResultOk;
		}
		*obj = nullptr;
		return kNoInterface;
	}

	uint32 PLUGIN_API addRef() override  { return uint32(FUnknownPrivate::atomicAdd(m_refs, 1)); }
	uint32 PLUGIN_API release() override { return uint32(FUnknownPrivate::atomicAdd(m_refs, -1)); }

	tresult PLUGIN_API getFactoryInfo(PFactoryInfo *info) override
	{
		if (!info)
			return kInvalidArgument;
		std::memset(static_cast<void *>(info), 0, sizeof(*info));
		std::strncpy(info->vendor, kVendor, PFactoryInfo::kNameSize - 1);
		std::strncpy(info->url, "https://github.com/tarboh/S-MU2000", PFactoryInfo::kURLSize - 1);
		info->flags = PFactoryInfo::kUnicode;
		return kResultOk;
	}

	int32 PLUGIN_API countClasses() override { return 1; }

	tresult PLUGIN_API getClassInfo(int32 index, PClassInfo *info) override
	{
		if (index != 0 || !info)
			return kInvalidArgument;
		std::memset(static_cast<void *>(info), 0, sizeof(*info));
		std::memcpy(info->cid, kProcessorUID.toTUID(), sizeof(TUID));
		info->cardinality = PClassInfo::kManyInstances;
		std::strncpy(info->category, kVstAudioEffectClass, PClassInfo::kCategorySize - 1);
		std::strncpy(info->name, kPlugName, PClassInfo::kNameSize - 1);
		return kResultOk;
	}

	tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2 *info) override
	{
		if (index != 0 || !info)
			return kInvalidArgument;
		std::memset(static_cast<void *>(info), 0, sizeof(*info));
		std::memcpy(info->cid, kProcessorUID.toTUID(), sizeof(TUID));
		info->cardinality = PClassInfo::kManyInstances;
		std::strncpy(info->category, kVstAudioEffectClass, PClassInfo::kCategorySize - 1);
		std::strncpy(info->name, kPlugName, PClassInfo::kNameSize - 1);
		info->classFlags = 0;
		std::strncpy(info->subCategories, PlugType::kInstrumentSynth,
		             PClassInfo2::kSubCategoriesSize - 1);
		std::strncpy(info->vendor, kVendor, PClassInfo2::kVendorSize - 1);
		std::strncpy(info->version, kVersion, PClassInfo2::kVersionSize - 1);
		std::strncpy(info->sdkVersion, kVstVersionString, PClassInfo2::kVersionSize - 1);
		return kResultOk;
	}

	tresult PLUGIN_API getClassInfoUnicode(int32 index, PClassInfoW *info) override
	{
		if (index != 0 || !info)
			return kInvalidArgument;
		std::memset(static_cast<void *>(info), 0, sizeof(*info));
		std::memcpy(info->cid, kProcessorUID.toTUID(), sizeof(TUID));
		info->cardinality = PClassInfo::kManyInstances;
		std::strncpy(info->category, kVstAudioEffectClass, PClassInfo::kCategorySize - 1);
		set_str16(info->name, kPlugName, PClassInfo::kNameSize);
		info->classFlags = 0;
		std::strncpy(info->subCategories, PlugType::kInstrumentSynth,
		             PClassInfo2::kSubCategoriesSize - 1);
		set_str16(info->vendor, kVendor, PClassInfo2::kVendorSize);
		set_str16(info->version, kVersion, PClassInfo2::kVersionSize);
		set_str16(info->sdkVersion, kVstVersionString, PClassInfo2::kVersionSize);
		return kResultOk;
	}

	tresult PLUGIN_API setHostContext(FUnknown *) override { return kResultOk; }

	tresult PLUGIN_API createInstance(FIDString cid, FIDString _iid, void **obj) override
	{
		if (!cid || !_iid || !obj)
			return kInvalidArgument;
		if (!FUnknownPrivate::iidEqual(cid, kProcessorUID.toTUID()))
			return kNoInterface;
		mu_plugin *p = new mu_plugin;
		const tresult r = p->queryInterface(_iid, obj);
		p->release();
		return r;
	}

private:
	static void set_str16(char16 *dst, const char *ascii, int max)
	{
		int i = 0;
		for (; ascii[i] && i < max - 1; i++)
			dst[i] = char16(uint8(ascii[i]));
		dst[i] = 0;
	}

	int32 m_refs = 1;
};

factory g_factory;

} // namespace


// ---- DLL の出口
//
// The entry points differ by platform: a Windows DLL is opened with
// LoadLibrary and announces InitDll/ExitDll, while macOS loads the bundle with
// CFBundle and looks for bundleEntry/bundleExit. Both still have to hand back
// the same factory.

extern "C" {

SMTG_EXPORT_SYMBOL IPluginFactory *PLUGIN_API GetPluginFactory()
{
	g_factory.addRef();
	return &g_factory;
}

// PLUGIN_API is __stdcall only on 32-bit Windows, where the dllexport comes
// out decorated as _GetPluginFactory@0 and hosts calling GetProcAddress("GetPluginFactory")
// find nothing (x64 spells it plain). Alias the undecorated name onto it.
#if defined(_MSC_VER) && defined(_M_IX86)
#pragma comment(linker, "/EXPORT:GetPluginFactory=_GetPluginFactory@0")
#endif

#if defined(_WIN32)

__declspec(dllexport) bool InitDll()
{
	smu2000::vst3::engine::trace("InitDll");
	return true;
}
// DLL を外す直前。手放されずに残った本体があれば、そのスレッドを止めておく
// （DllMain の中ではスレッドを待てないので、ここでやる）
__declspec(dllexport) bool ExitDll()
{
	smu2000::vst3::engine::trace("ExitDll");
	smu2000::vst3::engine::park_all();
	smu2000::vst3::engine::trace("ExitDll done");
	return true;
}

// 落ちたときの記録（crash.txt）を、DLL が入った時に仕掛け、外れる時に必ず外す。DllMain の中なので、
// やるのは受け口の付け外しだけ（ファイルもスレッドも触らない）
BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
		smu2000::crash_log::install_plugin(self);
	else if (reason == DLL_PROCESS_DETACH)
		smu2000::crash_log::uninstall_plugin();
	return TRUE;
}

#elif defined(__APPLE__)

SMTG_EXPORT_SYMBOL bool bundleEntry(CFBundleRef bundle);
SMTG_EXPORT_SYMBOL bool bundleExit(void);

SMTG_EXPORT_SYMBOL bool bundleEntry(CFBundleRef bundle)
{
	(void)bundle;
	return true;
}

SMTG_EXPORT_SYMBOL bool bundleExit(void)
{
	smu2000::vst3::engine::park_all();
	return true;
}

#endif

}
