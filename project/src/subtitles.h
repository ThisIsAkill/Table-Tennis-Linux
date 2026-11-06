// Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
// SUBTITLES: captions for what the match announcer says (score calls, serves, faults, points,
// games, player introductions), and with ALL SOUNDS also short tags for crowd reactions.
//
// The announcer speaks through one function, sub_821B3AE0(announcer, "ANNOUNCER:<line>"), which
// the game calls with names built from templates such as "%s:Announcer_Point_%s"; this module
// overrides it and turns the line name into text. Players appear in line names by nationality
// (one player per nationality), so the text uses the player's name instead.
#pragma once

#include <string>

#include <rex/ui/imgui_dialog.h>

namespace tabletennis::subtitles {

// "off", "announcer" or "all" (the subtitles setting).
std::string Mode();

// Caption text for an announcer line name such as "ANNOUNCER:Announcer_Point_CHINESE", or empty
// if the line has no caption.
std::string AnnouncerText(const std::string& line);

// Caption tag for a crowd sound name such as "CROWD_RS:FAULT_OUT_FRNT01", or empty.
std::string CrowdText(const std::string& sound);

// Queues a caption (any thread). kind: 0 announcer, 1 sound tag.
void Show(const std::string& text, int kind);

// Draws the current captions at the bottom of the game picture.
class Overlay : public rex::ui::ImGuiDialog {
 public:
  explicit Overlay(rex::ui::ImGuiDrawer* drawer);
  ~Overlay() override;

 protected:
  void OnDraw(ImGuiIO& io) override;
};

}  // namespace tabletennis::subtitles
