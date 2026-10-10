// license:BSD-3-Clause
#pragma once
#include <string>
#include <vector>

namespace ui {
enum class audio_driver { native, directsound, asio };
inline const char *audio_driver_name(audio_driver driver)
{
	if (driver == audio_driver::directsound) return "DirectSound";
	if (driver == audio_driver::asio) return "ASIO";
#if defined(_WIN32)
	return "WASAPI";
#elif defined(__APPLE__)
	return "CoreAudio";
#else
	return "ALSA";
#endif
}
inline bool supported_audio_driver(audio_driver driver)
{
#if defined(_WIN32) && defined(SMU2000_ASIO)
	return int(driver) >= 0 && int(driver) <= 2;
#else
	return driver == audio_driver::native;
#endif
}
#if defined(_WIN32) && defined(SMU2000_ASIO)
std::vector<std::string> portaudio_output_list(audio_driver);
#endif
} // namespace ui
