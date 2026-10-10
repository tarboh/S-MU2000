// license:BSD-3-Clause
// Build with the platform's audio_out.cpp; --devices also exercises real
// playback devices with silence. Menu tests require neither ROMs nor hardware.
#include "ui/audio_output_switch.h"
#include "ui/menu.h"

#include <stdexcept>
#include <source_location>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

static void require(bool ok, const std::source_location where = std::source_location::current())
{
	if (!ok) throw std::runtime_error(std::string(where.file_name()) + ":" + std::to_string(where.line()) + ": audio check failed");
}

static const ui::menu_item &menu_item(const std::vector<ui::menu_group> &groups, int id)
{
	for (const auto &group : groups)
		for (const auto &item : group.items)
			if (!item.separator && item.id == id) return item;
	throw std::runtime_error("Missing menu command: " + std::to_string(id));
}

#if defined(_WIN32) && defined(SMU2000_ASIO)
static void driver_streams(const std::string &baseline)
{
	ui::audio_out out;
	const ui::audio_out::fill_fn silence = [](s16 *dst, u32 n) { std::memset(dst, 0, size_t(n) * 4); };
	const auto wait_frames = [&] {
		for (int i = 0; i < 100 && out.running() && !out.produced(); i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		require(out.running() && out.produced() > 0);
	};
	ui::audio_output_config current;
	current.device = baseline;
	require(ui::switch_audio_output(out, silence, current, current).selected);
	for (auto driver : {ui::audio_driver::directsound, ui::audio_driver::asio}) {
		const auto devices = ui::audio_out::list(driver);
		for (const auto &name : devices) std::cout << ui::audio_driver_name(driver) << " device: " << name << std::endl;
		ui::audio_output_config next;
		next.preferences.stream.driver = driver;
		if (driver == ui::audio_driver::asio) {
			// A registered driver need not have its hardware connected. Pick an
			// installed software driver for this opt-in Windows hardware test.
			for (const auto &name : devices) if (name == "FL Studio ASIO" || name == "Steinberg built-in ASIO Driver") { next.device = name; break; }
			if (next.device.empty()) continue;
		}
		auto opened = ui::switch_audio_output(out, silence, next, current);
		if (!opened.selected) std::cerr << opened.error << std::endl;
		require(opened.selected);
		current = next;
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
		wait_frames();
		require(!out.stream_info().rates.empty());
		std::cout << "Opened " << ui::audio_driver_name(driver) << ": " << out.device_name() << " / " << out.stream_info().rate << " Hz" << std::endl;
		next.preferences.stream.left = 1; next.preferences.stream.right = 0;
		next.preferences.stream.sample_rate = out.stream_info().rate;
		if (driver == ui::audio_driver::asio) {
			require(!out.stream_info().buffers.empty());
			next.preferences.stream.buffer_frames = out.stream_info().buffers.front();
		}
		require(ui::switch_audio_output(out, silence, next, current).selected);
		current = next;
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		wait_frames();
		auto bad = next; bad.device = "S-MU2000 nonexistent driver 907278";
		const auto rollback = ui::switch_audio_output(out, silence, bad, current);
		require(!rollback.selected && rollback.restored && !rollback.error.empty());
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		wait_frames();
	}
	ui::audio_output_config native;
	require(ui::switch_audio_output(out, silence, native, current).selected);
	out.stop();
	std::cout << "DirectSound/ASIO playback, format changes and rollback: PASS" << std::endl;
}
#endif

int main(int argc, char **argv)
{
	try {
	ui::init_lang("en");
	ui::menu_state s;
	s.audio_ready = true;
	s.audio_outs = { "Speakers", "USB headphones" };
	s.audio_name = "USB headphones";
	const auto phones = ui::menu_phones(s);
	require(phones.front().title == "Audio output device");
	require(!phones.front().items[0].checked);
	require(phones.front().items[3].checked);
	require(phones.front().items[3].id == ui::ID_AUDIO_BASE + 1);
	require(menu_item(phones, ui::ID_OUTPUT_DIGITAL).checked); // digital output is preserved
	s.analog = true;
	const auto analog = ui::menu_phones(s);
	require(menu_item(analog, ui::ID_OUTPUT_ANALOG).checked);
	require(!menu_item(analog, ui::ID_OUTPUT_DIGITAL).checked);
	// A newly enumerated snapshot reflects unplug/replug and changed ordering.
	s.audio_outs = { "USB headphones", "HDMI monitor" };
	const auto updated = ui::menu_audio_output(s);
	require(updated.items[2].checked);
	require(updated.items[3].label == "HDMI monitor");
	require(phones.front().items[2].label == "Speakers"); // old snapshot stays stable
	s.audio_name.clear();
	require(ui::menu_audio_output(s).items[0].checked);
	s.audio_ready = false;
	for (const auto &item : ui::menu_audio_output(s).items)
		require(!item.enabled || item.separator);
	s.audio_outs.clear();
	require(ui::menu_audio_output(s).items.back().label == "(No playback devices)");
	std::cout << "Audio output menus: PASS\n";
#if defined(_WIN32) && defined(SMU2000_ASIO)
	if (argc >= 2 && !std::strcmp(argv[1], "--drivers")) { driver_streams(argc > 2 ? argv[2] : ""); return 0; }
#endif
	if (argc < 2 || std::strcmp(argv[1], "--devices"))
		return 0;

	ui::audio_out out;
	const ui::audio_out::fill_fn silence = [](s16 *o, u32 n) { std::memset(o, 0, size_t(n) * 4); };
	std::string error;
	const std::string baseline = argc > 2 ? argv[2] : "";
	const bool started = out.start(20, silence, error, false, baseline);
	if (!started) std::cerr << error << std::endl;
	require(started);
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	require(out.produced() > 0);
	// Selecting a missing endpoint must fail and restore the previous stream.
	const auto failed = ui::switch_audio_output(out, 20, silence, false,
		"S-MU2000 test missing endpoint 907278", baseline);
	require(!failed.selected && failed.restored && !failed.error.empty());
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	require(out.produced() > 0);
	for (const auto &name : baseline.empty() ? ui::audio_out::list() : std::vector<std::string>{baseline}) {
		const auto selected = ui::switch_audio_output(out, 20, silence, false, name, baseline);
		require(selected.selected);
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		require(out.produced() > 0);
		std::cout << "Opened: " << name << '\n';
	}
	require(ui::switch_audio_output(out, 20, silence, false, baseline, baseline).selected);
	const auto unavailable = ui::switch_audio_output(out, 20, silence, false,
		"S-MU2000 test missing endpoint 907278", "S-MU2000 test missing endpoint 907279");
	require(!unavailable.selected && !unavailable.restored && !unavailable.error.empty());
	require(ui::switch_audio_output(out, 20, silence, false, baseline, baseline).selected);
	// Failed format/routing changes must restore every old stream setting.
	ui::audio_output_config old;
	old.device = baseline;
	old.preferences.stream.strict = true;
	old.preferences.stream.buffer_frames = 512;
	old.preferences.stream.left = 1;
	old.preferences.stream.right = 0;
	require(ui::switch_audio_output(out, silence, old, old).selected);
	const int rate = out.stream_info().rate;
	auto bad = old;
	bad.preferences.stream.sample_rate = 12345;
	const auto restored = ui::switch_audio_output(out, silence, bad, old);
	require(!restored.selected && restored.restored && out.stream_info().rate == rate);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	require(out.produced() > 0 && out.running());
	ui::audio_reopen_job job;
	job.start([&] { return ui::switch_audio_output(out, silence, old, old); });
	require(job.busy());
	require(job.join().selected && !job.busy());
	auto exclusive = old;
	exclusive.preferences.exclusive = true;
	const auto opened = ui::switch_audio_output(out, silence, exclusive, old);
	require(opened.selected || opened.restored);
	if (opened.selected) {
		const auto supported_rates = out.stream_info().rates;
		for (int supported : supported_rates) {
			auto next = exclusive;
			next.preferences.stream.sample_rate = supported;
			const auto result = ui::switch_audio_output(out, silence, next, exclusive);
			if (!result.selected) std::cerr << "Exclusive " << supported << " Hz: " << result.error << std::endl;
			require(result.selected || result.restored);
			if (result.selected) {
				require(out.stream_info().rate == supported);
				exclusive = next;
				std::cout << "Exclusive stream: " << supported << " Hz\n";
			}
		}
	}
	out.stop();
	std::cout << "Audio output switching and rollback: PASS\n";
	} catch (const std::exception &error) { std::cerr << error.what() << std::endl; return 1; }
}
