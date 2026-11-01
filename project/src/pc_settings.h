// pc_settings.h - the PC SETTINGS page behind the OPTIONS entry of the game's menus.
//
// project/tools/patch_menu.py adds "PC SETTINGS" below AUDIO OPTIONS in the front end's
// OPTIONS list and in the in-match pause menu's OPTIONS list; accepting it sets the Flash
// global GLOBAL_PC_SETTINGS in that movie. PollFrontend (the front end's per-frame update) and
// PollPause (the pause menu's update) consume the flag and open this page, drawn in the style
// of the game's own option screens over the menu. While the page is open the game receives no
// pad input: D-pad up/down picks a row, left/right (or A) changes its value, X restores the
// defaults, B returns to the game's OPTIONS list.
//
// Values come from and are saved to the launcher's settings file next to the executable
// (tt-recomp.ini on Linux, settings.ini on Windows). Show FPS, fullscreen and the achievement sound apply at once; the
// others are read by the launcher and apply after a restart.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <rex/ui/imgui_dialog.h>

#include "achievement_ui.h"

struct PPCContext;

namespace rex::ui {
class Window;
}

namespace tabletennis::pc_settings {

// Guest thread: called every front-end frame once the main menu has been reached.
void PollFrontend(PPCContext& ctx, uint8_t* base, uint32_t movie);

// Guest thread: called from the in-match pause menu's update (sub_823132F0) with its object.
void PollPause(PPCContext& ctx, uint8_t* base, uint32_t pause);

// Guest thread: called for every pad sample before the game sees it. Returns true when `pad`
// was changed (the page owns the pad while it is open, and until its buttons are released).
bool FilterInput(uint32_t user, achievement_ui::PadSample& pad);

bool IsOpen();

// Live settings the page changes through the application.
struct LiveHooks {
  std::function<bool()> get_show_fps;
  std::function<void(bool)> set_show_fps;
};

class SettingsPage : public rex::ui::ImGuiDialog {
 public:
  SettingsPage(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, LiveHooks hooks);
  ~SettingsPage() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void Open();
  void Change(int row, int delta);
  void RestoreDefaults();
  void ApplyLive(int row);
  void Save();
  void Draw(ImGuiIO& io, float alpha);

  rex::ui::Window* window_ = nullptr;
  LiveHooks hooks_;
  bool shown_ = false;
  double opened_at_ = 0.0;
  int selected_ = 0;
  int held_direction_ = 0;
  double next_repeat_ = 0.0;
  std::string save_error_;
  std::string notice_;
  double notice_until_ = 0.0;
};

}  // namespace tabletennis::pc_settings
