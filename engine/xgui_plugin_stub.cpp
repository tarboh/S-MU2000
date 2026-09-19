// engine/xgui_plugin_stub.cpp — plugin-build shim for the smu2000_engine target ONLY
// (sources stay under src/; this TU exists in engine/CMakeLists.txt, never in the
// Makefile/VST3/native builds, which link the real src/ui/xg_ui.cpp).
//
// src/vst3/engine.cpp (5901ea5) calls ui::xgui::set_voice_rom() so the imgui
// "一覧/エディタ" windows can show voice names and pictures (gui.exe / upstream VST3).
// That definition lives in src/ui/xg_ui.cpp, which includes imgui.h — forbidden in the
// plugin targets (hard rule #6: GUI-ON adds only gdi32/comdlg32; no imgui/GL/Skia).
// The plugin's native GDI panel (SMU2000_VST2/ui + src/ui/panel|editor|effects|layout|svg)
// never reads the voice ROM, so the plugin-side implementation is a no-op sink:
// engine.cpp's call compiles and links, does nothing, and no graphics code is pulled in.
//
// Deliberately does NOT include ui/xg_ui.h (that drags imgui.h). This redeclaration is
// signature-identical to xg_ui.h:44 (u8 = std::uint8_t via src/compat/mamecompat.h),
// so it mangles to the same symbol the engine references. If that declaration ever
// changes, the plugin link fails with LNK2019 on this name — update both.

#include <cstdint>
#include <memory>
#include <vector>

namespace ui {
namespace xgui {

void set_voice_rom(std::shared_ptr<const std::vector<std::uint8_t>> /*rom*/) {}

}  // namespace xgui
}  // namespace ui
