// license:BSD-3-Clause

#include "sdl_popup.h"

#include "ui/imgui_shell_sdl.h"
#include "ui/rom_locate.h"
#include "ui/texts.h"

#include <algorithm>
#include <memory>
#include <mutex>

namespace ui {
namespace sdl_popup {

namespace {

constexpr int ROW_H = 26, PAD_X = 12, GUTTER = 22;

} // namespace

struct dialog_state {
	std::atomic<bool> done{ false };
	std::mutex        mutex;
	std::string       path;
};

static void SDLCALL dialog_done(void *ud, const char *const *list, int)
{
	auto *st = static_cast<std::shared_ptr<dialog_state> *>(ud);
	std::lock_guard<std::mutex> hold((*st)->mutex);
	if (list && *list)
		(*st)->path = *list;
	(*st)->done.store(true);
	delete st;   // one-shot: the wait below always consumes exactly once
}

static std::string file_dialog(SDL_Window *win, std::atomic<bool> &quit, bool save,
                               const file_filter *filters, int nfilters,
                               const std::string &defloc)
{
	auto *held = new std::shared_ptr<dialog_state>(std::make_shared<dialog_state>());
	std::shared_ptr<dialog_state> st = *held;
	std::vector<SDL_DialogFileFilter> f;
	for (int i = 0; i < nfilters; i++)
		f.push_back({ filters[i].name, filters[i].pattern });
	if (save)
		SDL_ShowSaveFileDialog(dialog_done, held, win, f.data(), int(f.size()),
		                       defloc.empty() ? nullptr : defloc.c_str());
	else
		SDL_ShowOpenFileDialog(dialog_done, held, win, f.data(), int(f.size()),
		                       defloc.empty() ? nullptr : defloc.c_str(), false);
	while (!st->done.load() && !quit.load()) {
		SDL_Event ev;
		if (!SDL_WaitEventTimeout(&ev, 50))
			continue;
		if (ev.type == SDL_EVENT_QUIT) {
			quit.store(true);
			break;
		}
		if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE)
			break;   // stop waiting; a late answer lands in shared state
	}
	std::lock_guard<std::mutex> hold(st->mutex);
	return st->path;
}

std::string open_file(SDL_Window *win, std::atomic<bool> &quit,
                      const file_filter *filters, int nfilters,
                      const std::string &defloc)
{
	return file_dialog(win, quit, false, filters, nfilters, defloc);
}

std::string save_file(SDL_Window *win, std::atomic<bool> &quit,
                      const file_filter *filters, int nfilters,
                      const std::string &defloc)
{
	return file_dialog(win, quit, true, filters, nfilters, defloc);
}

int message_box(SDL_Window *win, const char *title, const char *text,
                unsigned flags,
                std::initializer_list<SDL_MessageBoxButtonData> buttons)
{
	std::vector<SDL_MessageBoxButtonData> b(buttons);
	SDL_MessageBoxData d{};
	d.window = win;
	d.flags = flags;
	d.title = title;
	d.message = text;
	d.numbuttons = int(b.size());
	d.buttons = b.data();
	int id = -1;
	SDL_ShowMessageBox(&d, &id);
	return id;
}

// The menu through an ImDrawList. Same geometry, colors, hit-testing,
// keyboard and cancel semantics on every platform.
int run(SDL_Window *win, imshell::sdl_state &st, ImGuiContext *ctx,
              const im::fonts &fonts, int ww, int wh,
              std::function<void(ImDrawList *)> behind, std::atomic<bool> &quit,
              const std::vector<item> &items, int x, int y, int &sub_chosen)
{
	sub_chosen = -1;
	if (!ctx || !st.ctx)
		return -1;
	ImGui::SetCurrentContext(ctx);

	ImFont *font = fonts.label;
	const float px = fonts.label_px;
	// Measured before the first NewFrame (no current font yet), so use an
	// explicit atlas font; build it if nothing rendered yet.
	ImFont *measure = font;
	if (!measure) {
		ImGuiIO &mio = ImGui::GetIO();
		if (!mio.Fonts->IsBuilt())
			mio.Fonts->Build();
		if (!mio.Fonts->Fonts.empty())
			measure = mio.Fonts->Fonts[0];
	}
	auto text_w = [&](const std::string &t) {
		if (measure)
			return measure->CalcTextSizeA(px, FLT_MAX, 0.0f, t.c_str()).x;
		return float(t.size()) * 8.0f;
	};
	double w = 0;
	for (const item &it : items) {
		if (it.separator)
			continue;
		w = std::max(w, double(text_w(it.submenu ? it.label + "  >" : it.label)));
	}
	const int mw = int(w) + PAD_X * 2 + GUTTER + 16;
	const int mh = int(items.size()) * ROW_H + 12;
	x = std::clamp(x, 0, std::max(0, ww - mw));
	y = std::clamp(y, 0, std::max(0, wh - mh));

	const ImU32 col_dim    = IM_COL32(0, 0, 0, 89);       // 0.35 alpha
	const ImU32 col_box    = IM_COL32(41, 41, 43, 255);
	const ImU32 col_edge   = IM_COL32(140, 140, 148, 255);
	const ImU32 col_hover  = IM_COL32(64, 115, 191, 255);
	const ImU32 col_text   = IM_COL32(235, 235, 235, 255);
	const ImU32 col_dis    = IM_COL32(128, 128, 128, 255);
	const ImU32 col_check  = IM_COL32(235, 235, 235, 255);

	int hover = -1;
	auto repaint = [&] {
		imshell::sdl_begin(ctx);
		behind(ImGui::GetBackgroundDrawList());
		ImDrawList *dl = ImGui::GetForegroundDrawList();
		// Dim the panel behind the menu.
		dl->AddRectFilled(ImVec2(0, 0), ImVec2(float(ww), float(wh)), col_dim);
		// Box.
		dl->AddRectFilled(ImVec2(float(x), float(y)),
		                  ImVec2(float(x + mw), float(y + mh)), col_box);
		dl->AddRect(ImVec2(float(x) + 0.5f, float(y) + 0.5f),
		            ImVec2(float(x + mw) - 1.0f, float(y + mh) - 1.0f), col_edge);
		for (size_t i = 0; i < items.size(); i++) {
			const int ry = y + 6 + int(i) * ROW_H;
			const item &it = items[i];
			if (it.separator) {
				dl->AddLine(ImVec2(float(x + 8), float(ry + ROW_H / 2)),
				            ImVec2(float(x + mw - 8), float(ry + ROW_H / 2)), col_edge);
				continue;
			}
			if (int(i) == hover && it.enabled)
				dl->AddRectFilled(ImVec2(float(x + 3), float(ry)),
				                  ImVec2(float(x + mw - 3), float(ry + ROW_H)), col_hover);
			std::string text = it.label;
			if (it.submenu)
				text += "  >";
			const ImU32 tc = it.enabled ? col_text : col_dis;
			if (font)
				dl->AddText(font, px,
				            ImVec2(float(x + PAD_X + GUTTER), float(ry + ROW_H / 2) - px * 0.5f),
				            tc, text.c_str());
			else
				dl->AddText(ImVec2(float(x + PAD_X + GUTTER), float(ry + 5)), tc,
				            text.c_str());
			if (it.checked)
				dl->AddRectFilled(
				    ImVec2(float(x + PAD_X + 2), float(ry + ROW_H / 2 - 4)),
				    ImVec2(float(x + PAD_X + 10), float(ry + ROW_H / 2 + 4)), col_check);
		}
		imshell::sdl_present(st, win);
	};
	auto at = [&](int mx, int my) {
		if (mx < x || mx >= x + mw || my < y)
			return -1;
		const int i = (my - y - 6) / ROW_H;
		if (i < 0 || i >= int(items.size()))
			return -1;
		return i;
	};
	auto choose = [&](int i) {
		if (i < 0 || !items[size_t(i)].enabled || items[size_t(i)].separator)
			return -1;
		if (items[size_t(i)].submenu)
			sub_chosen = items[size_t(i)].sub;
		return items[size_t(i)].id;
	};

	repaint();
	while (!quit.load()) {
		SDL_Event ev;
		if (!SDL_WaitEventTimeout(&ev, 30))
			continue;
		ImGui_ImplSDL3_ProcessEvent(&ev);
		// Only this window's events. The plug-in shares the queue with its
		// PC windows and the host may share the process.
		if (ev.type != SDL_EVENT_QUIT) {
			Uint32 id = 0;
			switch (ev.type) {
			case SDL_EVENT_MOUSE_MOTION:      id = ev.motion.windowID; break;
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
			case SDL_EVENT_MOUSE_BUTTON_UP:   id = ev.button.windowID; break;
			case SDL_EVENT_MOUSE_WHEEL:       id = ev.wheel.windowID; break;
			case SDL_EVENT_KEY_DOWN:
			case SDL_EVENT_KEY_UP:            id = ev.key.windowID; break;
			case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			case SDL_EVENT_WINDOW_RESIZED:
			case SDL_EVENT_WINDOW_FOCUS_LOST: id = ev.window.windowID; break;
			default: break;
			}
			Uint32 me = win ? SDL_GetWindowID(win) : 0;
			if (id && me && id != me)
				continue;
		}
		switch (ev.type) {
		case SDL_EVENT_QUIT:
			quit.store(true);
			return -1;
		case SDL_EVENT_MOUSE_MOTION: {
			const int i = at(int(ev.motion.x), int(ev.motion.y));
			const int h = (i >= 0 && !items[size_t(i)].separator) ? i : -1;
			if (h != hover) {
				hover = h;
				repaint();
			}
			break;
		}
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
			if (ev.button.button == SDL_BUTTON_RIGHT)
				return -1;
			if (ev.button.button == SDL_BUTTON_LEFT) {
				const int i = at(int(ev.button.x), int(ev.button.y));
				if (i < 0)
					return -1;   // clicked outside: cancel
				const int id = choose(i);
				if (id >= 0 || items[size_t(i)].submenu)
					return id;
			}
			break;
		case SDL_EVENT_KEY_DOWN:
			if (ev.key.key == SDLK_ESCAPE)
				return -1;
			if (ev.key.key == SDLK_UP || ev.key.key == SDLK_DOWN) {
				const int d = ev.key.key == SDLK_DOWN ? 1 : -1;
				int i = hover;
				for (size_t k = 0; k < items.size(); k++) {
					i = (i + d + int(items.size())) % int(items.size());
					if (!items[size_t(i)].separator && items[size_t(i)].enabled)
						break;
				}
				hover = i;
				repaint();
			} else if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER) {
				if (hover >= 0) {
					const int id = choose(hover);
					if (id >= 0 || items[size_t(hover)].submenu)
						return id;
				}
			}
			break;
		default:
			break;
		}
	}
	return -1;
}

void alert(SDL_Window *win, const char *title, const std::string &text)
{
	message_box(win, title, text.c_str(), SDL_MESSAGEBOX_WARNING,
	            { { 0, 0, "OK" } });
}

bool confirm(SDL_Window *win, const char *title, const std::string &text,
             const char *ok_label)
{
	const SDL_MessageBoxButtonData buttons[] = {
		{ SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT |
		  SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Cancel" },
		{ 0, 1, ok_label },
	};
	return message_box(win, title, text.c_str(), SDL_MESSAGEBOX_WARNING,
	                   { buttons[0], buttons[1] }) == 1;
}

} // namespace sdl_popup

// ROM の場所なしで起動したとき（ui/rom_locate.h）。窓はまだ無いので、SDL の映像だけ起こして
// 案内のメッセージと、フォルダを選ぶ窓（xdg-desktop-portal か zenity。無ければ選べない）を出す
bool ask_roms_folder(const std::string &message, std::string &picked)
{
	if (!SDL_Init(SDL_INIT_VIDEO))
		return false;
	const SDL_MessageBoxButtonData buttons[] = {
		{ SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, UI_TEXT(dlg_roms_quit, "Quit") },
		{ SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, UI_TEXT(dlg_roms_pick, "Select ROM folder...") },
	};
	if (sdl_popup::message_box(nullptr, "S-MU2000", message.c_str(), SDL_MESSAGEBOX_INFORMATION,
	                           { buttons[0], buttons[1] }) != 1)
		return false;
	auto *held = new std::shared_ptr<sdl_popup::dialog_state>(std::make_shared<sdl_popup::dialog_state>());
	std::shared_ptr<sdl_popup::dialog_state> st = *held;
	SDL_ShowOpenFolderDialog(sdl_popup::dialog_done, held, nullptr, nullptr, false);
	while (!st->done.load()) {
		SDL_Event ev;
		if (SDL_WaitEventTimeout(&ev, 50) && ev.type == SDL_EVENT_QUIT)
			return false;
	}
	std::lock_guard<std::mutex> hold(st->mutex);
	picked = st->path;
	return !picked.empty();
}

} // namespace ui
