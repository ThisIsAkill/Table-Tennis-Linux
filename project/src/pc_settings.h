// Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
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
// The UPDATES rows store check_updates / auto_update for the launcher and show the installed
// version (<install dir>/.tt-version). When the launcher has found a newer release
// (<install dir>/.tt-update.json) the version row offers it: A asks for confirmation, writes
// <install dir>/.tt-update-requested and quits; the launcher installs the update and restarts.
// UpdateToast announces such a release once on the main menu. The game itself does no
// networking. Test hook: TT_TEST_UPDATE_JSON=<path> reads that file as the update file.
//
// Values come from and are saved to the launcher's settings file next to the executable
// (tt-recomp.ini on Linux, settings.ini on Windows). Show FPS, fullscreen and the achievement sound apply at once; the
// others are read by the launcher and apply after a restart.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <rex/ui/imgui_dialog.h>

struct ImDrawList;

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

// ONLINE in the main menu: shows the online prompt (public room or a friend's private room)
// over the game's online menu.
void OpenOnlinePrompt();

// Live settings the page changes through the application.
struct LiveHooks {
  std::function<bool()> get_show_fps;
  std::function<void(bool)> set_show_fps;
  // Display effects that switch while the game runs (names as in tt-recomp.ini).
  std::function<void(const std::string&)> set_anti_aliasing;  // none, fxaa, fxaa_extreme
  std::function<void(const std::string&)> set_sharpening;     // bilinear, cas, fsr
  std::function<void(bool)> set_vsync;                         // rebuilds the swapchain
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
  void RequestUpdate();
  void Save();
  void Draw(ImGuiIO& io, float alpha);
  void DrawConfirm(ImDrawList* dl, float s, float cx, float cy, float alpha);
  // CHECK NOW: runs the launcher's update check while the page stays usable.
  void StartUpdateCheck();
  void PollUpdateCheck();
  // PLAYER NAME editor (D-pad like the room code entry, or the keyboard).
  void OpenNameEditor();
  void EditName(ImGuiIO& io, uint16_t pressed, uint16_t held, double now);
  void SaveName();
  void DrawNameEditor(ImDrawList* dl, float s, float cx, float cy, float alpha);
  // ENTER LINK CODE uses the same editor with the link code alphabet.
  void OpenLinkCodeEditor();

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
  bool confirm_ = false;  // "update now?" box showing
  double confirm_at_ = 0.0;
  bool naming_ = false;   // name editor showing
  bool editing_link_ = false;  // ... for ENTER LINK CODE instead of the player name
  double naming_at_ = 0.0;
  std::string name_edit_;
  int name_pos_ = 0;
};

// Connection quality to the other online player (bars, round trip, loss, a warning when the
// match will feel delayed), shown outside matches while online.
class ConnectionIndicator : public rex::ui::ImGuiDialog {
 public:
  explicit ConnectionIndicator(rex::ui::ImGuiDrawer* drawer);
  ~ConnectionIndicator() override;

 protected:
  void OnDraw(ImGuiIO& io) override;
};

// The ONLINE prompt: PLAY ANYONE (public room) or PLAY WITH A FRIEND (show or enter a room
// code), with the connection status; then the game's own PLAYER MATCH menu takes over.
class OnlinePrompt : public rex::ui::ImGuiDialog {
 public:
  explicit OnlinePrompt(rex::ui::ImGuiDrawer* drawer);
  ~OnlinePrompt() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  enum class Step { kUnavailable, kChoose, kFriend, kShowCode, kEnterCode };
  void Draw(ImGuiIO& io, float alpha);
  void Close(bool back_to_game);

  bool shown_ = false;
  double opened_at_ = 0.0;
  Step step_ = Step::kChoose;
  int selected_ = 0;
  int held_direction_ = 0;
  double next_repeat_ = 0.0;
  std::string code_edit_;
  int code_pos_ = 0;
};

// Front-end popup when the launcher reports a newer release (see the update notes in
// pc_settings.cpp). Shown once per new version per session, never in a match or over a page.
// It also warns once per session, before any update notice, when the engine reports that
// Vulkan runs on a software renderer (no GPU driver), which makes the game very slow.
class UpdateToast : public rex::ui::ImGuiDialog {
 public:
  explicit UpdateToast(rex::ui::ImGuiDrawer* drawer);
  ~UpdateToast() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  std::string shown_version_;
  double started_ = -1.0;
  bool gpu_warning_done_ = false;
  bool showing_gpu_warning_ = false;
};

}  // namespace tabletennis::pc_settings
