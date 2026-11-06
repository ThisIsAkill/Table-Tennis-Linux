// Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
// screens.cpp - the six screens of the installer, drawn with the widgets from ui.h.

#include "app.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

namespace tt {
namespace fs = std::filesystem;
using namespace ui;
using Style = ui::Ui::ButtonStyle;

namespace {

constexpr int kStepCount = 4;

std::string FormatDuration(float seconds) {
  if (seconds < 0.0f) return "estimating";
  const int total = int(seconds + 0.5f);
  if (total < 60) return "less than a minute";
  const int minutes = (total + 30) / 60;
  if (minutes < 60) return std::to_string(minutes) + (minutes == 1 ? " minute" : " minutes");
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%d h %02d min", minutes / 60, minutes % 60);
  return buffer;
}

std::string FormatClock(float seconds) {
  const int total = int(seconds);
  char buffer[32];
  if (total >= 3600) {
    std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%d:%02d", total / 60, total % 60);
  }
  return buffer;
}

// A caption in the game's small heavy blue style ("FOUND ON THIS COMPUTER").
void Caption(ui::Frame& f, float x, float y, const char* text, ImU32 col = 0) {
  const float px = 13.0f * f.s;
  AddText(f.dl, Heavy(), px, ImVec2(x, y), col ? col : BlueLight(), Upper(text).c_str(), kCondensed);
}

void Headline(ui::Frame& f, float x, float y, float px, const char* text, ImU32 col = 0) {
  AddOutlinedText(f.dl, Heavy(), px, ImVec2(x, y), col ? col : Yellow(), Outline(), px * 0.06f, Upper(text).c_str(),
                  kCondensed);
}

void HeadlineCentered(ui::Frame& f, float cx, float y, float px, const char* text, ImU32 col = 0) {
  const std::string upper = Upper(text);
  const float w = TextWidth(Heavy(), px, upper.c_str(), kCondensed);
  AddOutlinedText(f.dl, Heavy(), px, ImVec2(cx - w * 0.5f, y), col ? col : Yellow(), Outline(), px * 0.06f,
                  upper.c_str(), kCondensed);
}

void BodyText(ui::Frame& f, float x, float y, float w, const char* text, ImU32 col = 0, float px = 0.0f) {
  if (px <= 0.0f) px = 16.0f * f.s;
  AddText(f.dl, Body(), px, ImVec2(x, y), col ? col : White(0.88f), text, 1.0f, w);
}

float BodyTextCentered(ui::Frame& f, float cx, float y, float w, const char* text, ImU32 col = 0, float px = 0.0f) {
  if (px <= 0.0f) px = 16.0f * f.s;
  // Centre each wrapped line by measuring the block, then drawing it left-aligned within it.
  const ImVec2 size = Body()->CalcTextSizeA(px, FLT_MAX, w, text);
  AddText(f.dl, Body(), px, ImVec2(cx - std::min(size.x, w) * 0.5f, y), col ? col : White(0.88f), text, 1.0f, w);
  return size.y;
}

}  // namespace

// ==============================================================================================
// 1. Welcome: drop zone, browse, discs found on this computer
// ==============================================================================================
void App::DrawWelcome() {
  ui::Frame& f = ui.f;
  ImDrawList* dl = f.dl;
  const float s = f.s;
  ui.Header("Table Tennis", "Linux and Steam Deck setup", 1, kStepCount);
  const Rect c = ui.Content();
  const float in = EaseOutCubic(Stage(SinceEnter(), 0.0f, 0.3f));
  const float rise = (1.0f - in) * 14.0f * s;
  const bool stacked = c.w() < 900.0f * s;

  // Left: what this does, and the drop zone.
  const float gap = 28.0f * s;
  const float left_w = stacked ? c.w() : c.w() * 0.58f;
  const float right_x = stacked ? c.x0 : c.x0 + left_w + gap;
  const float right_w = stacked ? c.w() : c.w() - left_w - gap;

  float y = c.y0 + rise;
  const char* intro =
      "This builds the Linux port of Rockstar Games presents Table Tennis from your own Xbox 360 disc image, "
      "then adds it to Steam so it plays like any other game.";
  BodyText(f, c.x0 + 2.0f * s, y, left_w, intro, White(0.9f), 17.0f * s);
  y += WrappedHeight(Body(), 17.0f * s, left_w, intro) + 22.0f * s;

  const float drop_h = std::min(300.0f * s, c.y1 - y - 60.0f * s);
  const Rect drop{c.x0, y, c.x0 + left_w, y + drop_h};
  {
    const float r = 16.0f * s;
    const float pulse = 0.5f + 0.5f * std::sin(f.t * 2.2f);
    const bool hovered = drop.contains(f.mouse);
    dl->AddRectFilled(ImVec2(drop.x0, drop.y0 + 4.0f * s), ImVec2(drop.x1, drop.y1 + 4.0f * s), Col(0, 0, 0, 0.3f), r);
    dl->AddRectFilled(drop.min(), drop.max(), hovered ? Col(26, 58, 100, 0.6f) : PanelDark(0.82f), r);
    // Dashed rim.
    const ImU32 dash = Blue(0.55f + 0.35f * pulse);
    const float dash_len = 14.0f * s, dash_gap = 9.0f * s;
    auto dashed = [&](ImVec2 a, ImVec2 b) {
      const float len = std::hypot(b.x - a.x, b.y - a.y);
      const ImVec2 dir((b.x - a.x) / len, (b.y - a.y) / len);
      for (float d = 0.0f; d < len; d += dash_len + dash_gap) {
        const float e = std::min(len, d + dash_len);
        dl->AddLine(ImVec2(a.x + dir.x * d, a.y + dir.y * d), ImVec2(a.x + dir.x * e, a.y + dir.y * e), dash, 2.0f * s);
      }
    };
    const float inset = 8.0f * s;
    dashed(ImVec2(drop.x0 + inset + r, drop.y0 + inset), ImVec2(drop.x1 - inset - r, drop.y0 + inset));
    dashed(ImVec2(drop.x1 - inset, drop.y0 + inset + r), ImVec2(drop.x1 - inset, drop.y1 - inset - r));
    dashed(ImVec2(drop.x1 - inset - r, drop.y1 - inset), ImVec2(drop.x0 + inset + r, drop.y1 - inset));
    dashed(ImVec2(drop.x0 + inset, drop.y1 - inset - r), ImVec2(drop.x0 + inset, drop.y0 + inset + r));
    // Disc with a paddle, bobbing gently.
    const float cx = (drop.x0 + drop.x1) * 0.5f;
    const float bob = std::sin(f.t * 1.6f) * 3.0f * s;
    const float icon_y = drop.y0 + drop_h * 0.34f + bob;
    DiscIcon(dl, ImVec2(cx, icon_y), 44.0f * s, Col(180, 200, 220), 1.0f);
    Paddle(dl, ImVec2(cx + 44.0f * s, icon_y + 22.0f * s), 40.0f * s, -0.5f, RedB(), Col(190, 150, 90), 1.0f);
    HeadlineCentered(f, cx, drop.y0 + drop_h * 0.34f + 62.0f * s, 30.0f * s, "Drop your Table Tennis ISO here");
    const float or_px = 14.0f * s;
    const char* or_text = "OR";
    const float or_w = TextWidth(Heavy(), or_px, or_text, kCondensed);
    const float or_y = drop.y0 + drop_h * 0.34f + 112.0f * s;
    AddText(dl, Heavy(), or_px, ImVec2(cx - or_w * 0.5f, or_y), Grey(), or_text, kCondensed);
    dl->AddLine(ImVec2(cx - or_w * 0.5f - 60.0f * s, or_y + or_px * 0.55f), ImVec2(cx - or_w * 0.5f - 12.0f * s, or_y + or_px * 0.55f), White(0.2f), 1.0f);
    dl->AddLine(ImVec2(cx + or_w * 0.5f + 12.0f * s, or_y + or_px * 0.55f), ImVec2(cx + or_w * 0.5f + 60.0f * s, or_y + or_px * 0.55f), White(0.2f), 1.0f);
    const float bw = 200.0f * s, bh = 48.0f * s;
    const Rect browse{cx - bw * 0.5f, or_y + 30.0f * s, cx + bw * 0.5f, or_y + 30.0f * s + bh};
    if (ui.Button("browse", browse, browse_open ? "Choosing..." : "Browse...", Style::Secondary, !browse_open)) {
      OpenBrowse();
    }
  }
  y = drop.y1 + 14.0f * s;
  if (f.t < notice_until && !notice.empty()) {
    const float a = Clamp01((notice_until - f.t) / 0.5f);
    BodyText(f, c.x0 + 2.0f * s, y, left_w, notice.c_str(), Orange(a), 15.0f * s);
  } else {
    BodyText(f, c.x0 + 2.0f * s, y, left_w,
             "You need the .iso (or .chd) of your own USA / Europe retail disc (Title ID 545407DF). It is only read, never changed, "
             "and nothing from it leaves this computer.",
             Grey(), 14.0f * s);
  }

  // Right: discs the scan turned up.
  float ry = stacked ? drop.y1 + 60.0f * s : c.y0 + rise;
  Caption(f, right_x + 2.0f * s, ry, "Found on this computer");
  ry += 26.0f * s;
  if (!scan_finished) {
    Spinner(dl, ImVec2(right_x + 22.0f * s, ry + 18.0f * s), 12.0f * s, f.t, BlueLight());
    BodyText(f, right_x + 48.0f * s, ry + 9.0f * s, right_w - 48.0f * s, "Looking in Downloads, Desktop, Games and removable drives...", Grey(), 15.0f * s);
  } else if (found.empty()) {
    const Rect none{right_x, ry, right_x + right_w, ry + 92.0f * s};
    ui.Panel(none, 0.7f);
    BodyText(f, none.x0 + 18.0f * s, none.y0 + 14.0f * s, none.w() - 36.0f * s,
             "No disc image larger than 1 GB was found in the usual places. Drop the .iso or .chd here, or plug in the SD card or "
             "drive that holds it and restart the setup.",
             Grey(), 14.5f * s);
  } else {
    const int shown = std::min<int>(int(found.size()), 4);
    const float card_h = 84.0f * s;
    for (int i = 0; i < shown; ++i) {
      const sys::FoundIso& iso_found = found[size_t(i)];
      const Rect card{right_x, ry, right_x + right_w, ry + card_h};
      const std::string id = "found-" + std::to_string(i);
      const bool use = ui.Card(id, card, false, true, [&](const Rect& r, bool lit, float alpha) {
        const float cy = (r.y0 + r.y1) * 0.5f;
        DiscIcon(dl, ImVec2(r.x0 + 40.0f * s, cy), 24.0f * s, lit ? Col(200, 215, 230) : Col(150, 170, 195), alpha);
        const float text_x = r.x0 + 78.0f * s;
        const float chip_px = 14.0f * s;
        const char* chip_text = "USE IT";
        const float chip_w = TextWidth(Heavy(), chip_px, chip_text, kCondensed) + 30.0f * s;
        const float text_w = r.x1 - 16.0f * s - chip_w - 12.0f * s - text_x;
        const float name_px = 19.0f * s;
        const std::string name = Fit(Heavy(), name_px, kCondensed, text_w, Upper(iso_found.path.filename().string()));
        AddOutlinedText(dl, Heavy(), name_px, ImVec2(text_x, r.y0 + 16.0f * s), lit ? Yellow(alpha) : White(alpha), Outline(alpha),
                        1.3f * s, name.c_str(), kCondensed);
        const std::string where = "Found " + iso_found.where + "  \xc2\xb7  " + sys::FormatBytes(iso_found.bytes);
        AddText(dl, Body(), 14.0f * s, ImVec2(text_x, r.y0 + 44.0f * s), Grey(alpha), Fit(Body(), 14.0f * s, 1.0f, text_w, where).c_str());
        // "USE IT" chip.
        const float chip_h = 32.0f * s;
        const Rect chip{r.x1 - 16.0f * s - chip_w, cy - chip_h * 0.5f, r.x1 - 16.0f * s, cy + chip_h * 0.5f};
        dl->AddRectFilled(chip.min(), chip.max(), lit ? Yellow(alpha) : NavyLight(alpha), chip_h * 0.5f);
        dl->AddRect(chip.min(), chip.max(), lit ? Col(0, 0, 0, 0.3f * alpha) : Blue(0.8f * alpha), chip_h * 0.5f, 0, 1.2f * s);
        AddText(dl, Heavy(), chip_px, ImVec2(chip.x0 + 15.0f * s, CapsTop(cy, chip_px)), lit ? Col(20, 24, 36, alpha) : White(alpha),
                chip_text, kCondensed);
      });
      if (use) ChooseIso(iso_found.path);
      ry += card_h + 10.0f * s;
    }
    if (int(found.size()) > shown) {
      char more[64];
      std::snprintf(more, sizeof(more), "%d more not shown. Use Browse... to pick one of them.", int(found.size()) - shown);
      BodyText(f, right_x + 2.0f * s, ry + 4.0f * s, right_w, more, Grey(), 13.5f * s);
    }
  }

  ui.Footer({{"Select", "A", GreenA()}, {"Quit", "B", RedB()}}, "NOTHING FROM THE DISC IS UPLOADED OR SHARED");
}

// ==============================================================================================
// 2. Check: verifying the chosen image
// ==============================================================================================
void App::DrawCheck() {
  ui::Frame& f = ui.f;
  ImDrawList* dl = f.dl;
  const float s = f.s;
  ui.Header("Table Tennis", "Checking your disc", 2, kStepCount);
  const Rect c = ui.Content();
  const float in = EaseOutCubic(Stage(SinceEnter(), 0.0f, 0.3f));
  const float rise = (1.0f - in) * 14.0f * s;

  const float panel_w = std::min(760.0f * s, c.w());
  const float panel_h = std::min(400.0f * s, c.h());
  const Rect p{(f.W - panel_w) * 0.5f, c.y0 + (c.h() - panel_h) * 0.5f + rise, (f.W + panel_w) * 0.5f,
               c.y0 + (c.h() - panel_h) * 0.5f + rise + panel_h};
  ui.Panel(p);
  const float cx = (p.x0 + p.x1) * 0.5f;
  const std::string name = iso.filename().string();

  if (!check_result) {
    Spinner(dl, ImVec2(cx, p.y0 + 96.0f * s), 44.0f * s, f.t, BlueLight());
    HeadlineCentered(f, cx, p.y0 + 166.0f * s, 34.0f * s, "Checking your disc");
    BodyTextCentered(f, cx, p.y0 + 218.0f * s, p.w() - 80.0f * s,
                     ("Reading " + name + " to make sure it is the supported Table Tennis disc. The image is opened read-only.").c_str(),
                     White(0.85f), 16.0f * s);
    const float bar_y = p.y0 + 270.0f * s;
    const float bar_w = 360.0f * s;
    dl->AddRectFilled(ImVec2(cx - bar_w * 0.5f, bar_y), ImVec2(cx + bar_w * 0.5f, bar_y + 6.0f * s), White(0.1f), 3.0f * s);
    const float k = std::fmod(f.t * 0.6f, 1.0f);
    const float x0 = cx - bar_w * 0.5f + bar_w * k;
    dl->PushClipRect(ImVec2(cx - bar_w * 0.5f, bar_y), ImVec2(cx + bar_w * 0.5f, bar_y + 6.0f * s), true);
    dl->AddRectFilled(ImVec2(x0 - bar_w * 0.3f, bar_y), ImVec2(x0, bar_y + 6.0f * s), Yellow(0.9f), 3.0f * s);
    dl->PopClipRect();
    BodyTextCentered(f, cx, p.y0 + 300.0f * s, p.w() - 80.0f * s, FitPath(Body(), 13.0f * s, p.w() - 80.0f * s, iso.string()).c_str(), Grey(), 13.0f * s);
    ui.Footer({{"Back", "B", RedB()}});
    return;
  }

  const iso::Result& r = *check_result;
  const bool ok = r.verdict == iso::Verdict::Ok;
  const float pop = EaseOutBack(Stage(f.t - check_done_at, 0.0f, 0.45f));
  const ImVec2 badge(cx, p.y0 + 86.0f * s);
  const float br = 40.0f * s * pop;
  dl->AddCircleFilled(badge, br + 4.0f * s, Outline(0.9f), 48);
  dl->AddCircleFilled(badge, br, ok ? GreenA() : RedB(), 48);
  dl->AddCircle(badge, br, White(0.4f), 48, 2.0f * s);
  if (ok) {
    Tick(dl, badge, br * 1.2f, White(), 6.0f * s * std::max(pop, 0.1f));
  } else {
    Cross(dl, badge, br * 1.1f, White(), 6.0f * s * std::max(pop, 0.1f));
  }
  HeadlineCentered(f, cx, p.y0 + 146.0f * s, 34.0f * s, r.headline.c_str(), ok ? Yellow() : Col(255, 160, 150));
  float ty = p.y0 + 198.0f * s;
  ty += BodyTextCentered(f, cx, ty, p.w() - 100.0f * s, r.detail.c_str(), White(0.88f), 16.5f * s) + 10.0f * s;
  const std::string file_line = name + "  \xc2\xb7  " + sys::FormatBytes(iso_bytes) + "  \xc2\xb7  " + sys::DescribeLocation(iso);
  BodyTextCentered(f, cx, ty, p.w() - 80.0f * s, file_line.c_str(), Grey(), 13.5f * s);

  const float bw = 230.0f * s, bh = 50.0f * s, by = p.y1 - 24.0f * s - bh;
  if (ok) {
    // Continue by itself after a moment; A goes straight on.
    const float wait = 2.5f;
    const float left = wait - (f.t - check_done_at);
    const Rect go{cx - bw - 8.0f * s, by, cx - 8.0f * s, by + bh};
    const Rect other{cx + 8.0f * s, by, cx + 8.0f * s + bw, by + bh};
    ui.focus.SetIfNone("continue");
    if (ui.Button("continue", go, "Continue", Style::Primary, true, "A") || left <= 0.0f) {
      Enter(Screen::Options);
      return;
    }
    if (ui.Button("other", other, "Choose another", Style::Secondary)) Enter(Screen::Welcome);
    // Countdown ring on the continue button.
    const float k = Clamp01(left / wait);
    dl->PathArcTo(ImVec2(go.x1 - 18.0f * s, (go.y0 + go.y1) * 0.5f), 7.0f * s, -kPi * 0.5f, -kPi * 0.5f + 2.0f * kPi * (1.0f - k), 24);
    dl->PathStroke(Col(20, 24, 36, 0.8f), ImDrawFlags_None, 2.5f * s);
  } else {
    const Rect other{cx - bw - 8.0f * s, by, cx - 8.0f * s, by + bh};
    const Rect again{cx + 8.0f * s, by, cx + 8.0f * s + bw, by + bh};
    ui.focus.SetIfNone("other");
    if (ui.Button("other", other, "Choose another", Style::Primary, true, "A")) Enter(Screen::Welcome);
    if (ui.Button("again", again, "Check again", Style::Secondary)) StartCheck();
    if (!r.raw.empty()) {
      BodyTextCentered(f, cx, p.y1 - 24.0f * s - bh - 24.0f * s, p.w() - 80.0f * s, Fit(Body(), 12.0f * s, 1.0f, p.w() - 80.0f * s, "Reported: " + r.raw).c_str(), Grey(0.7f), 12.0f * s);
    }
  }
  ui.Footer({{"Accept", "A", GreenA()}, {"Back", "B", RedB()}});
}

// ==============================================================================================
// 3. Options: where to install, Steam, shortcut, what to expect
// ==============================================================================================
void App::DrawOptions() {
  ui::Frame& f = ui.f;
  ImDrawList* dl = f.dl;
  const float s = f.s;
  ui.Header("Table Tennis", "Setup options", 3, kStepCount);
  const Rect c = ui.Content();
  const float in = EaseOutCubic(Stage(SinceEnter(), 0.0f, 0.3f));
  const float rise = (1.0f - in) * 14.0f * s;
  const bool stacked = c.w() < 940.0f * s;
  const float gap = 28.0f * s;
  const float left_w = stacked ? c.w() : c.w() * 0.5f;
  const float right_x = stacked ? c.x0 : c.x0 + left_w + gap;
  const float right_w = stacked ? c.w() : c.w() - left_w - gap;

  // Refresh free space now and then (drives come and go).
  static float next_refresh = 0.0f;
  if (f.t > next_refresh) {
    next_refresh = f.t + 3.0f;
    for (Location& loc : locations) {
      if (!loc.prefix.empty()) loc.free_bytes = sys::FreeBytes(loc.prefix);
    }
    home_free = sys::FreeBytes(sys::HomeDir());
  }

  // --- Left: install location cards ---
  float y = c.y0 + rise;
  Caption(f, c.x0 + 2.0f * s, y, "Install location");
  y += 24.0f * s;
  const float card_h = 70.0f * s;
  for (size_t i = 0; i < locations.size(); ++i) {
    Location& loc = locations[i];
    const Rect card{c.x0, y, c.x0 + left_w, y + card_h};
    const bool selected = int(i) == location;
    const bool picked = ui.Card(loc.id, card, selected, loc.enabled, [&](const Rect& r, bool lit, float alpha) {
      const float tx = r.x0 + 20.0f * s;
      const float title_px = 18.0f * s;
      ImU32 title_col = selected || lit ? Yellow(alpha) : White(alpha);
      std::string title = loc.title;
      AddOutlinedText(dl, Heavy(), title_px, ImVec2(tx, r.y0 + 11.0f * s), title_col, Outline(alpha), 1.2f * s, title.c_str(), kCondensed);
      float after_title = tx + TextWidth(Heavy(), title_px, title.c_str(), kCondensed) + 12.0f * s;
      if (loc.recommended) {
        const float tag_px = 11.0f * s;
        const char* tag = "RECOMMENDED";
        const float tw = TextWidth(Heavy(), tag_px, tag, kCondensed) + 14.0f * s;
        const Rect t{after_title, r.y0 + 12.0f * s, after_title + tw, r.y0 + 30.0f * s};
        dl->AddRectFilled(t.min(), t.max(), Yellow(alpha), 9.0f * s);
        AddText(dl, Heavy(), tag_px, ImVec2(t.x0 + 7.0f * s, CapsTop((t.y0 + t.y1) * 0.5f, tag_px)), Col(20, 24, 36, alpha), tag, kCondensed);
      }
      // Free space on the right.
      std::string right_text;
      ImU32 right_col = loc.enabled ? BlueLight(alpha) : Orange(alpha * 1.6f);
      if (!loc.enabled) {
        right_text = loc.disabled_reason;
      } else if (!loc.prefix.empty()) {
        right_text = sys::FormatBytes(loc.free_bytes) + " FREE";
      }
      if (!right_text.empty()) {
        const float rpx = 13.0f * s;
        const std::string upper = Upper(right_text);
        const float rw = TextWidth(Heavy(), rpx, upper.c_str(), kCondensed);
        AddText(dl, Heavy(), rpx, ImVec2(r.x1 - 18.0f * s - rw, r.y0 + 14.0f * s), right_col, upper.c_str(), kCondensed);
      }
      // Path and note.
      const float sub_px = 13.5f * s;
      std::string sub = loc.prefix.empty() ? loc.note : FitPath(Body(), sub_px, r.w() * 0.5f, loc.prefix.string()) + "  \xc2\xb7  " + loc.note;
      AddText(dl, Body(), sub_px, ImVec2(tx, r.y0 + 41.0f * s), Grey(alpha), Fit(Body(), sub_px, 1.0f, r.w() - 40.0f * s, sub).c_str());
    });
    if (picked) {
      if (loc.custom) {
        OpenFolderDialog();
      } else {
        location = int(i);
        cache_on_target = home_free < kSetupBytes && CurrentLocation().device != home_device;
      }
    }
    y += card_h + 8.0f * s;
  }
  BodyText(f, c.x0 + 4.0f * s, y + 4.0f * s, left_w - 8.0f * s,
           "Your disc image stays where it is; the game reads it while playing. Keeping the ISO on an SD card and installing "
           "to internal storage is fine.",
           Grey(), 13.5f * s);
  y += 50.0f * s;

  // --- Right: toggles and expectations ---
  float ry = stacked ? y + 20.0f * s : c.y0 + rise;
  Caption(f, right_x + 2.0f * s, ry, "After the build");
  ry += 24.0f * s;
  const float toggle_h = 64.0f * s;
  ui.Toggle("steam", {right_x, ry, right_x + right_w, ry + toggle_h}, "Add to Steam as a non-Steam game",
            is_steamos ? "Play it from your Steam library and in Game Mode on the Steam Deck."
                       : "Play it from your Steam library, with Steam Input for your controller.",
            steam);
  ry += toggle_h + 8.0f * s;
  ui.Toggle("desktop", {right_x, ry, right_x + right_w, ry + toggle_h}, "Desktop shortcut",
            "A Table Tennis entry in your applications menu and on the desktop.", desktop);
  ry += toggle_h + 8.0f * s;
  const bool other_drive = CurrentLocation().device != 0 && CurrentLocation().device != home_device;
  if (other_drive) {
    const std::string sub = "Put the ~10 GB of build files next to the game instead of in your home folder (" +
                            sys::FormatBytes(home_free) + " free). Needs a Linux filesystem such as ext4, not exFAT.";
    const float cache_h = 76.0f * s;
    ui.Toggle("cache", {right_x, ry, right_x + right_w, ry + cache_h}, "Keep build files on the same drive", sub.c_str(),
              cache_on_target);
    ry += cache_h + 8.0f * s;
  }

  // What to expect.
  ry += 8.0f * s;
  Caption(f, right_x + 2.0f * s, ry, "What to expect");
  ry += 24.0f * s;
  const float info_h = 150.0f * s;
  const Rect info{right_x, ry, right_x + right_w, ry + info_h};
  ui.Panel(info, 0.8f);
  {
    const fs::path cache_dir = CacheDir().value_or(sys::HomeDir() / ".cache" / "tt-recomp");
    const std::string space_line = "About 10 GB of space while setting up, for build tools and caches (in " +
                                   FitPath(Body(), 14.0f * s, right_w * 0.5f, cache_dir.string()) +
                                   "). The installed game itself is small.";
    const char* lines[4] = {"20 to 40 minutes the first time; the game is compiled on this computer.", space_line.c_str(),
                            "Missing build tools are downloaded automatically, one time.",
                            "Nothing from the disc is uploaded. You can keep using the computer meanwhile."};
    float ly = info.y0 + 14.0f * s;
    for (const char* line : lines) {
      dl->AddCircleFilled(ImVec2(info.x0 + 20.0f * s, ly + 9.0f * s), 3.0f * s, Yellow(), 8);
      BodyText(f, info.x0 + 34.0f * s, ly, info.w() - 50.0f * s, line, White(0.85f), 14.0f * s);
      ly += WrappedHeight(Body(), 14.0f * s, info.w() - 50.0f * s, line) + 6.0f * s;
    }
  }
  ry = info.y1 + 16.0f * s;

  // Install button, with the reason when it cannot go ahead.
  std::string why;
  const bool can_install = EnoughSpace(&why) && !config.backend.empty();
  if (config.backend.empty()) why = "The setup script was not found next to the installer.";
  const float bw = 260.0f * s, bh = 54.0f * s;
  const float by = std::max(ry, c.y1 - bh);
  const Rect install{right_x + right_w - bw, by, right_x + right_w, by + bh};
  if (ui.Button("install", install, "Install", Style::Primary, can_install, "A")) StartInstall();
  const Rect back{right_x, by + 4.0f * s, right_x + 120.0f * s, by + bh - 4.0f * s};
  if (ui.Button("back", back, "Back", Style::Quiet)) Enter(Screen::Welcome);
  if (!can_install) {
    BodyText(f, back.x1 + 14.0f * s, by + 4.0f * s, install.x0 - back.x1 - 28.0f * s, why.c_str(), Orange(), 13.0f * s);
  }
  if (f.t < notice_until && !notice.empty()) {
    BodyText(f, c.x0 + 2.0f * s, c.y1 - 20.0f * s, left_w, notice.c_str(), Orange(Clamp01((notice_until - f.t) / 0.5f)), 14.0f * s);
  }
  ui.Footer({{"Accept", "A", GreenA()}, {"Back", "B", RedB()}}, iso.filename().string().c_str());
}

// ==============================================================================================
// 4. Install: steps, progress, log
// ==============================================================================================
void App::DrawLog(const Rect& r, float alpha) {
  ui::Frame& f = ui.f;
  ImDrawList* dl = f.dl;
  const float s = f.s;
  dl->AddRectFilled(r.min(), r.max(), Col(0, 0, 0, 0.55f * alpha), 6.0f * s);
  dl->AddRect(r.min(), r.max(), NavyLight(0.8f * alpha), 6.0f * s, 0, 1.0f * s);
  const float px = 13.0f * s;
  const float line_h = px * 1.45f;
  const int visible = std::max(1, int((r.h() - 16.0f * s) / line_h));
  const int count = int(inst.log.size());
  const float max_scroll = float(std::max(0, count - visible));
  if (r.contains(f.mouse) && f.wheel != 0.0f) {
    inst.log_scroll -= f.wheel * 3.0f;
    inst.log_follow = inst.log_scroll >= max_scroll - 0.5f;
  }
  if (inst.log_follow) inst.log_scroll = max_scroll;
  inst.log_scroll = std::clamp(inst.log_scroll, 0.0f, max_scroll);
  dl->PushClipRect(ImVec2(r.x0 + 2.0f * s, r.y0 + 2.0f * s), ImVec2(r.x1 - 2.0f * s, r.y1 - 2.0f * s), true);
  const int first = int(inst.log_scroll);
  float y = r.y0 + 8.0f * s - (inst.log_scroll - float(first)) * line_h;
  const float text_w = r.w() - 24.0f * s;
  for (int i = first; i < count && y < r.y1; ++i) {
    const std::string& line = inst.log[size_t(i)];
    ImU32 col = White(0.75f * alpha);
    if (line.rfind("== ", 0) == 0) col = Yellow(alpha);
    else if (line.rfind("Warning:", 0) == 0) col = Orange(alpha);
    else if (line.rfind("Error:", 0) == 0) col = Col(255, 120, 120, alpha);
    AddText(dl, Body(), px, ImVec2(r.x0 + 12.0f * s, y), col, Fit(Body(), px, 1.0f, text_w, line).c_str());
    y += line_h;
  }
  dl->PopClipRect();
  if (count > visible) {
    const float tx = r.x1 - 8.0f * s;
    const float track = r.h() - 12.0f * s;
    const float thumb = std::max(16.0f * s, track * float(visible) / float(count));
    const float ty = r.y0 + 6.0f * s + (track - thumb) * (max_scroll > 0 ? inst.log_scroll / max_scroll : 0.0f);
    dl->AddRectFilled(ImVec2(tx, r.y0 + 6.0f * s), ImVec2(tx + 3.0f * s, r.y1 - 6.0f * s), White(0.08f * alpha), 1.5f * s);
    dl->AddRectFilled(ImVec2(tx, ty), ImVec2(tx + 3.0f * s, ty + thumb), White(0.5f * alpha), 1.5f * s);
  }
}

void App::DrawInstall() {
  ui::Frame& f = ui.f;
  ImDrawList* dl = f.dl;
  const float s = f.s;
  ui.Header("Table Tennis", "Building your game", 4, kStepCount);
  const Rect c = ui.Content();
  const float in = EaseOutCubic(Stage(SinceEnter(), 0.0f, 0.3f));
  const float rise = (1.0f - in) * 14.0f * s;
  const bool stacked = c.w() < 900.0f * s;
  const float gap = 28.0f * s;
  const float left_w = stacked ? c.w() : std::min(400.0f * s, c.w() * 0.36f);
  const float right_x = stacked ? c.x0 : c.x0 + left_w + gap;
  const float right_w = stacked ? c.w() : c.w() - left_w - gap;
  const float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - inst.started).count();

  // --- Left: steps ---
  const float steps_h = stacked ? 0.0f : c.h() - 10.0f * s;
  if (!stacked) {
    const Rect panel{c.x0, c.y0 + rise, c.x0 + left_w, c.y0 + rise + steps_h};
    ui.Panel(panel);
    Caption(f, panel.x0 + 20.0f * s, panel.y0 + 16.0f * s, "Steps");
    const float row_h = std::min(46.0f * s, (panel.h() - 50.0f * s) / float(std::max<size_t>(1, inst.steps.size())));
    float y = panel.y0 + 44.0f * s;
    for (size_t i = 0; i < inst.steps.size(); ++i) {
      const InstallStep& step = inst.steps[i];
      const bool active = step.state == InstallStep::State::Active;
      const bool done = step.state == InstallStep::State::Done;
      const float cy = y + row_h * 0.5f;
      if (active) {
        dl->AddRectFilled(ImVec2(panel.x0 + 10.0f * s, y + 3.0f * s), ImVec2(panel.x1 - 10.0f * s, y + row_h - 3.0f * s), Col(40, 96, 160, 0.40f), 6.0f * s);
        dl->AddRectFilled(ImVec2(panel.x0 + 10.0f * s, y + 3.0f * s), ImVec2(panel.x0 + 14.0f * s, y + row_h - 3.0f * s), Yellow(), 2.0f * s);
      }
      const ImVec2 mark(panel.x0 + 36.0f * s, cy);
      const float mr = 10.0f * s;
      if (done) {
        dl->AddCircleFilled(mark, mr, GreenA(), 20);
        Tick(dl, mark, mr * 1.3f, White(), 2.4f * s);
      } else if (active) {
        Spinner(dl, mark, mr * 0.9f, f.t, Yellow());
      } else {
        dl->AddCircle(mark, mr * 0.8f, White(0.25f), 20, 1.5f * s);
      }
      const float px = 16.0f * s;
      const ImU32 col = active ? Yellow() : done ? White(0.9f) : Grey(0.7f);
      const std::string title = Fit(Heavy(), px, kCondensed, panel.w() - 80.0f * s, Upper(step.title));
      AddOutlinedText(dl, Heavy(), px, ImVec2(panel.x0 + 56.0f * s, CapsTop(cy, px)), col, Outline(), active ? 1.4f * s : 0.0f, title.c_str(), kCondensed);
      y += row_h;
    }
  }

  // --- Right: progress ---
  float ry = c.y0 + rise;
  const InstallStep* active_step = inst.current >= 0 ? &inst.steps[size_t(inst.current)] : nullptr;
  {
    char percent[16];
    std::snprintf(percent, sizeof(percent), "%d%%", int(inst.shown * 100.0f + 0.5f));
    const float big_px = 72.0f * s;
    Headline(f, right_x - 2.0f * s, ry - 8.0f * s, big_px, percent);
    const float pw = TextWidth(Heavy(), big_px, percent, kCondensed);
    const std::string step_title = active_step ? Upper(active_step->title) : std::string("STARTING...");
    const float st_px = 22.0f * s;
    AddOutlinedText(dl, Heavy(), st_px, ImVec2(right_x + pw + 18.0f * s, ry + 14.0f * s), White(), Outline(), 1.4f * s,
                    Fit(Heavy(), st_px, kCondensed, right_w - pw - 20.0f * s, step_title).c_str(), kCondensed);
    const std::string timing = "Elapsed " + FormatClock(elapsed) + "   \xc2\xb7   " +
                               (inst.remaining_seconds >= 0.0f ? "About " + FormatDuration(inst.remaining_seconds) + " left" : "Estimating the time left");
    AddText(dl, Body(), 15.0f * s, ImVec2(right_x + pw + 18.0f * s, ry + 46.0f * s), Grey(), timing.c_str());
    ry += 92.0f * s;

    // The bar: a navy trough with a yellow fill and a sheen that sweeps across while working.
    const float bar_h = 22.0f * s;
    const Rect bar{right_x, ry, right_x + right_w, ry + bar_h};
    dl->AddRectFilled(bar.min(), bar.max(), Navy(), bar_h * 0.5f);
    dl->AddRect(bar.min(), bar.max(), NavyLight(), bar_h * 0.5f, 0, 1.5f * s);
    const float fill_w = std::max(bar_h, bar.w() * inst.shown);
    dl->PushClipRect(bar.min(), bar.max(), true);
    dl->AddRectFilled(bar.min(), ImVec2(bar.x0 + fill_w, bar.y1), Yellow(), bar_h * 0.5f);
    dl->AddRectFilled(bar.min(), ImVec2(bar.x0 + fill_w, (bar.y0 + bar.y1) * 0.5f), White(0.22f), bar_h * 0.5f, ImDrawFlags_RoundCornersTop);
    if (inst.shown < 1.0f) {
      const float sx = bar.x0 + std::fmod(f.t * 220.0f * s, fill_w + 80.0f * s) - 40.0f * s;
      const ImVec2 quad[4] = {ImVec2(sx - 10.0f * s, bar.y0), ImVec2(sx + 14.0f * s, bar.y0), ImVec2(sx + 2.0f * s, bar.y1), ImVec2(sx - 22.0f * s, bar.y1)};
      dl->PushClipRect(bar.min(), ImVec2(bar.x0 + fill_w, bar.y1), true);
      dl->AddConvexPolyFilled(quad, 4, White(0.35f));
      dl->PopClipRect();
    }
    dl->PopClipRect();
    ry += bar_h + 14.0f * s;
    // The latest line of output.
    const std::string last = inst.last_line.empty() ? std::string("Working...") : inst.last_line;
    AddText(dl, Body(), 14.0f * s, ImVec2(right_x + 2.0f * s, ry), Grey(), Fit(Body(), 14.0f * s, 1.0f, right_w, last).c_str());
    ry += 30.0f * s;
  }

  // Details toggle and cancel, then the log when open.
  const float bh = 44.0f * s;
  const Rect details{right_x, ry, right_x + 190.0f * s, ry + bh};
  if (ui.Button("details", details, inst.details_open ? "Hide details" : "Show details", Style::Secondary)) {
    inst.details_open = !inst.details_open;
  }
  Chevron(dl, ImVec2(details.x1 - 22.0f * s, (details.y0 + details.y1) * 0.5f), 10.0f * s, !inst.details_open, White(0.8f), 2.0f * s);
  const Rect cancel{right_x + right_w - 150.0f * s, ry, right_x + right_w, ry + bh};
  const bool cancelling = process && process->Cancelling();
  if (ui.Button("cancel", cancel, cancelling ? "Stopping..." : "Cancel", Style::Danger, !cancelling)) {
    Ask({"Stop the setup?", "The game is being built. Stopping now keeps what was downloaded, so a later attempt is faster, but nothing will be playable yet.",
         "Stop setup", "Keep going", true, [this] { CancelInstall(); }});
  }
  ry += bh + 12.0f * s;
  // Warnings: a yellow strip that does not interrupt anything.
  if (!inst.warnings.empty()) {
    const std::string& last_warning = inst.warnings.back();
    const float wpx = 14.0f * s;
    const float wh = WrappedHeight(Body(), wpx, right_w - 70.0f * s, last_warning.c_str()) + 18.0f * s;
    const Rect w{right_x, ry, right_x + right_w, ry + wh};
    dl->AddRectFilled(w.min(), w.max(), Col(120, 80, 10, 0.45f), 6.0f * s);
    dl->AddRect(w.min(), w.max(), Orange(0.8f), 6.0f * s, 0, 1.0f * s);
    dl->AddTriangleFilled(ImVec2(w.x0 + 20.0f * s, w.y0 + 10.0f * s), ImVec2(w.x0 + 30.0f * s, w.y0 + 26.0f * s), ImVec2(w.x0 + 10.0f * s, w.y0 + 26.0f * s), Orange());
    AddText(dl, Heavy(), 12.0f * s, ImVec2(w.x0 + 17.5f * s, w.y0 + 11.5f * s), Col(60, 40, 0), "!");
    BodyText(f, w.x0 + 44.0f * s, w.y0 + 9.0f * s, right_w - 70.0f * s, last_warning.c_str(), White(0.92f), wpx);
    ry = w.y1 + 12.0f * s;
  }

  if (inst.details_open) {
    const Rect log{right_x, ry, right_x + right_w, c.y1 - 6.0f * s};
    if (log.h() > 40.0f * s) DrawLog(log, 1.0f);
  } else {
    // A quiet reminder while the long steps run.
    BodyText(f, right_x + 2.0f * s, ry + 6.0f * s, right_w,
             "You can keep using the computer. The first build downloads the tools it needs and compiles the whole game; "
             "later installs reuse them and finish in a few minutes.",
             Grey(0.8f), 14.0f * s);
  }
  const std::string log_hint = process && !process->LogFile().empty() ? "Log: " + process->LogFile().string() : std::string();
  ui.Footer({{"Stop", "B", RedB()}}, log_hint.c_str());
}

// ==============================================================================================
// 5. Done
// ==============================================================================================
void App::DrawDone() {
  ui::Frame& f = ui.f;
  ImDrawList* dl = f.dl;
  const float s = f.s;
  ui.Header("Table Tennis", "Ready to play", 4, kStepCount);
  const Rect c = ui.Content();
  const float t = SinceEnter();
  const float in = EaseOutCubic(Stage(t, 0.0f, 0.3f));
  const float rise = (1.0f - in) * 14.0f * s;
  const float panel_w = std::min(820.0f * s, c.w());
  const Rect p{(f.W - panel_w) * 0.5f, c.y0 + rise, (f.W + panel_w) * 0.5f, c.y1};
  ui.Panel(p);
  const float cx = (p.x0 + p.x1) * 0.5f;

  // Badge: a green disc with a tick, popping in, with a few halftone dots fanning out.
  const float pop = EaseOutBack(Stage(t, 0.1f, 0.5f));
  const ImVec2 badge(cx, p.y0 + 78.0f * s);
  const float br = 42.0f * s * pop;
  Halftone(dl, ImVec2(cx + 60.0f * s, badge.y - 30.0f * s), ImVec2(p.x1 - 20.0f * s, badge.y + 30.0f * s), BlueLight(0.25f * in), 10.0f * s, t * 6.0f * s);
  Halftone(dl, ImVec2(p.x0 + 20.0f * s, badge.y - 30.0f * s), ImVec2(cx - 60.0f * s, badge.y + 30.0f * s), BlueLight(0.25f * in), 10.0f * s, -t * 6.0f * s, 0.0f, false);
  dl->AddCircleFilled(badge, br + 4.0f * s, Outline(0.9f), 48);
  dl->AddCircleFilled(badge, br, GreenA(), 48);
  dl->AddCircle(badge, br, White(0.4f), 48, 2.0f * s);
  Tick(dl, badge, br * 1.2f, White(), 6.0f * s * std::max(pop, 0.1f));

  HeadlineCentered(f, cx, p.y0 + 136.0f * s, 46.0f * s, UpdateMode() ? "Update installed" : "Ready to play");
  const fs::path dir = inst.done_dir.empty() ? Prefix() : fs::path(inst.done_dir);
  const std::string where = "Installed to " + dir.string();
  BodyTextCentered(f, cx, p.y0 + 200.0f * s, p.w() - 80.0f * s, where.c_str(), Grey(), 14.5f * s);

  const float pw = 300.0f * s, ph = 64.0f * s;
  const Rect play{cx - pw * 0.5f, p.y0 + 236.0f * s, cx + pw * 0.5f, p.y0 + 236.0f * s + ph};
  if (ui.Button("play", play, "Play", Style::Primary, true, "A")) Play();
  const float sw = 240.0f * s, sh = 46.0f * s;
  const Rect open{cx - sw * 0.5f, play.y1 + 14.0f * s, cx + sw * 0.5f, play.y1 + 14.0f * s + sh};
  if (ui.Button("open", open, "Open install folder", Style::Secondary)) OpenInstallFolder();

  // Where to find it.
  float y = open.y1 + 26.0f * s;
  const float col_w = p.w() - 80.0f * s;
  const float x = cx - col_w * 0.5f;
  // Show the Steam card only for what the backend reports (older backends report nothing).
  const bool steam_added = steam_result.empty() ? steam : steam_result == "added";
  const bool steam_requested = steam_result == "requested";
  if (steam_added || steam_requested) {
    const Rect card{x, y, x + col_w, y + 66.0f * s};
    dl->AddRectFilled(card.min(), card.max(), Col(26, 58, 100, 0.45f), 8.0f * s);
    dl->AddRect(card.min(), card.max(), Blue(0.6f), 8.0f * s, 0, 1.2f * s);
    AddOutlinedText(dl, Heavy(), 16.0f * s, ImVec2(card.x0 + 18.0f * s, card.y0 + 12.0f * s), Yellow(), Outline(), 1.2f * s,
                    steam_added ? "ADDED TO STEAM AS A NON-STEAM GAME" : "SENT TO STEAM", kCondensed);
    BodyText(f, card.x0 + 18.0f * s, card.y0 + 36.0f * s, col_w - 36.0f * s,
             is_steamos ? "In Game Mode: Library > Non-Steam. In Desktop Mode it is in your Steam library."
                        : "Find it in your Steam library under Non-Steam games.",
             White(0.85f), 14.0f * s);
    y = card.y1 + 10.0f * s;
  }
  if (desktop) {
    BodyText(f, x + 2.0f * s, y, col_w, "A Table Tennis shortcut was added to your applications menu and desktop.", Grey(), 13.5f * s);
    y += 24.0f * s;
  }
  if (!notes.empty()) {
    y += 4.0f * s;
    for (size_t i = 0; i < notes.size() && i < 3; ++i) {
      const std::string& note = notes[i];
      const float nh = WrappedHeight(Body(), 13.5f * s, col_w - 44.0f * s, note.c_str()) + 14.0f * s;
      if (y + nh > p.y1 - 10.0f * s) break;
      const Rect n{x, y, x + col_w, y + nh};
      dl->AddRectFilled(n.min(), n.max(), Col(120, 80, 10, 0.35f), 6.0f * s);
      dl->AddRect(n.min(), n.max(), Orange(0.7f), 6.0f * s, 0, 1.0f * s);
      dl->AddTriangleFilled(ImVec2(n.x0 + 18.0f * s, n.y0 + 8.0f * s), ImVec2(n.x0 + 27.0f * s, n.y0 + 23.0f * s), ImVec2(n.x0 + 9.0f * s, n.y0 + 23.0f * s), Orange());
      BodyText(f, n.x0 + 38.0f * s, n.y0 + 7.0f * s, col_w - 44.0f * s, note.c_str(), White(0.9f), 13.5f * s);
      y = n.y1 + 8.0f * s;
    }
  }
  if (f.t < notice_until && !notice.empty()) {
    BodyTextCentered(f, cx, p.y1 - 30.0f * s, p.w() - 80.0f * s, notice.c_str(), Orange(), 14.0f * s);
  }
  ui.Footer({{"Play", "A", GreenA()}, {"Close", "B", RedB()}}, "IN THE GAME, PRESS ESCAPE TWICE TO QUIT");
}

// ==============================================================================================
// 6. Error
// ==============================================================================================
void App::DrawError() {
  ui::Frame& f = ui.f;
  ImDrawList* dl = f.dl;
  const float s = f.s;
  ui.Header("Table Tennis", "Setup stopped", 4, kStepCount);
  const Rect c = ui.Content();
  const float t = SinceEnter();
  const float in = EaseOutCubic(Stage(t, 0.0f, 0.3f));
  const float rise = (1.0f - in) * 14.0f * s;
  const float panel_w = std::min(900.0f * s, c.w());
  const Rect p{(f.W - panel_w) * 0.5f, c.y0 + rise, (f.W + panel_w) * 0.5f, c.y1};
  ui.Panel(p);

  const float pop = EaseOutBack(Stage(t, 0.1f, 0.5f));
  const ImVec2 badge(p.x0 + 70.0f * s, p.y0 + 64.0f * s);
  const float br = 32.0f * s * pop;
  dl->AddCircleFilled(badge, br + 4.0f * s, Outline(0.9f), 48);
  dl->AddCircleFilled(badge, br, RedB(), 48);
  dl->AddCircle(badge, br, White(0.4f), 48, 2.0f * s);
  Cross(dl, badge, br * 1.1f, White(), 5.0f * s * std::max(pop, 0.1f));

  const float tx = p.x0 + 124.0f * s;
  const float tw = p.x1 - 30.0f * s - tx;
  Headline(f, tx, p.y0 + 30.0f * s, 36.0f * s, "Something went wrong");
  const std::string step_text = inst.current >= 0 ? "While: " + inst.steps[size_t(inst.current)].title : std::string();
  float y = p.y0 + 80.0f * s;
  if (!step_text.empty()) {
    BodyText(f, tx, y, tw, step_text.c_str(), BlueLight(), 14.0f * s);
    y += 24.0f * s;
  }
  // The backend's message in a highlighted box.
  const std::string message = inst.error.empty() ? std::string("No details were reported.") : inst.error;
  const float mh = WrappedHeight(Body(), 16.0f * s, tw - 36.0f * s, message.c_str()) + 26.0f * s;
  const Rect m{tx, y, p.x1 - 30.0f * s, y + mh};
  dl->AddRectFilled(m.min(), m.max(), Col(90, 20, 28, 0.45f), 8.0f * s);
  dl->AddRect(m.min(), m.max(), RedB(0.7f), 8.0f * s, 0, 1.2f * s);
  BodyText(f, m.x0 + 18.0f * s, m.y0 + 13.0f * s, tw - 36.0f * s, message.c_str(), White(0.95f), 16.0f * s);
  y = m.y1 + 14.0f * s;
  BodyText(f, tx, y, tw,
           "Nothing on your computer was harmed. Downloads and finished build steps are kept, so trying again picks up where "
           "it stopped. If it keeps failing, copy the log and include it in a bug report.",
           Grey(), 14.0f * s);
  y += 54.0f * s;

  const float bh = 48.0f * s;
  float bx = tx;
  const Rect retry{bx, y, bx + 190.0f * s, y + bh};
  if (ui.Button("retry", retry, "Try again", Style::Primary, true, "A")) StartInstall();
  bx = retry.x1 + 12.0f * s;
  const Rect details{bx, y, bx + 190.0f * s, y + bh};
  if (ui.Button("details", details, show_error_details ? "Hide details" : "Show details", Style::Secondary)) {
    show_error_details = !show_error_details;
    inst.details_open = show_error_details;
  }
  bx = details.x1 + 12.0f * s;
  const Rect copy{bx, y, bx + 150.0f * s, y + bh};
  if (ui.Button("copy", copy, "Copy log", Style::Secondary)) CopyLog();
  bx = copy.x1 + 12.0f * s;
  const Rect back{bx, y + 4.0f * s, bx + 130.0f * s, y + bh - 4.0f * s};
  if (UpdateMode()) {
    const Rect current{bx, y + 4.0f * s, bx + 260.0f * s, y + bh - 4.0f * s};
    if (ui.Button("back", current, "Play the current version", Style::Quiet)) FinishUpdate(10);
  } else if (ui.Button("back", back, "Options", Style::Quiet)) {
    Enter(Screen::Options);
  }
  y += bh + 12.0f * s;
  if (process && !process->LogFile().empty()) {
    const std::string saved = "Log saved to " + process->LogFile().string();
    BodyText(f, tx, y, tw, saved.c_str(), Grey(0.8f), 13.0f * s);
    y += 24.0f * s;
  }
  if (f.t < notice_until && !notice.empty()) {
    BodyText(f, tx, y, tw, notice.c_str(), Orange(), 13.5f * s);
  }
  if (show_error_details) {
    const Rect log{p.x0 + 24.0f * s, y + 8.0f * s, p.x1 - 24.0f * s, p.y1 - 16.0f * s};
    if (log.h() > 40.0f * s) {
      inst.log_follow = true;
      DrawLog(log, 1.0f);
    }
  }
  ui.Footer({{"Try again", "A", GreenA()}, {"Back", "B", RedB()}});
}

// ==============================================================================================
// Modal confirmation and the typed-path fallback
// ==============================================================================================
void App::DrawModal() {
  ui::Frame& f = ui.f;
  ImDrawList* dl = f.dl;
  const float s = f.s;
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(f.W, f.H), Col(0, 0, 0, 0.6f));
  const float w = std::min(560.0f * s, f.W - 40.0f * s);
  const float body_px = 16.0f * s;
  const float body_h = WrappedHeight(Body(), body_px, w - 60.0f * s, modal->body.c_str());
  const float h = 150.0f * s + body_h;
  const Rect m{(f.W - w) * 0.5f, (f.H - h) * 0.5f, (f.W + w) * 0.5f, (f.H + h) * 0.5f};
  dl->AddRectFilled(ImVec2(m.x0, m.y0 + 6.0f * s), ImVec2(m.x1, m.y1 + 6.0f * s), Col(0, 0, 0, 0.5f), 12.0f * s);
  dl->AddRectFilled(m.min(), m.max(), Navy(0.98f), 12.0f * s);
  dl->AddRectFilled(m.min(), ImVec2(m.x1, m.y0 + 54.0f * s), NavyLight(0.35f), 12.0f * s, ImDrawFlags_RoundCornersTop);
  dl->AddRect(m.min(), m.max(), modal->danger ? RedB(0.8f) : Blue(0.8f), 12.0f * s, 0, 1.6f * s);
  Headline(f, m.x0 + 28.0f * s, m.y0 + 14.0f * s, 26.0f * s, modal->title.c_str());
  BodyText(f, m.x0 + 30.0f * s, m.y0 + 70.0f * s, w - 60.0f * s, modal->body.c_str(), White(0.9f), body_px);

  ui.focus.BeginModal();
  const float bh = 46.0f * s, bw = 170.0f * s;
  const float by = m.y1 - 24.0f * s - bh;
  const Rect cancel{m.x1 - 28.0f * s - bw, by, m.x1 - 28.0f * s, by + bh};
  const Rect confirm{cancel.x0 - 12.0f * s - bw, by, cancel.x0 - 12.0f * s, by + bh};
  ui.focus.SetIfNone("modal-cancel");
  if (ui.Button("modal-confirm", confirm, modal->confirm_label.c_str(), modal->danger ? Style::Danger : Style::Primary)) {
    auto action = modal->on_confirm;
    modal.reset();
    if (action) action();
  }
  if (modal && ui.Button("modal-cancel", cancel, modal->cancel_label.c_str(), Style::Secondary, true, "B")) {
    modal.reset();
  }
  ui.focus.EndModal();
}

void App::DrawCustomPathEditor() {
  ui::Frame& f = ui.f;
  ImDrawList* dl = f.dl;
  const float s = f.s;
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(f.W, f.H), Col(0, 0, 0, 0.6f));
  const float w = std::min(640.0f * s, f.W - 40.0f * s), h = 200.0f * s;
  const Rect m{(f.W - w) * 0.5f, (f.H - h) * 0.5f, (f.W + w) * 0.5f, (f.H + h) * 0.5f};
  dl->AddRectFilled(m.min(), m.max(), Navy(0.98f), 12.0f * s);
  dl->AddRect(m.min(), m.max(), Blue(0.8f), 12.0f * s, 0, 1.6f * s);
  Headline(f, m.x0 + 28.0f * s, m.y0 + 14.0f * s, 24.0f * s, "Install folder");
  BodyText(f, m.x0 + 30.0f * s, m.y0 + 52.0f * s, w - 60.0f * s,
           "No folder dialog is available on this system. Type the folder to install into:", White(0.85f), 14.0f * s);

  // A borderless ImGui window carries the text field; everything else is custom drawn.
  ImGui::SetNextWindowPos(ImVec2(m.x0 + 30.0f * s, m.y0 + 84.0f * s));
  ImGui::SetNextWindowSize(ImVec2(w - 60.0f * s, 40.0f * s));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * s, 9.0f * s));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f * s);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.02f, 0.04f, 0.07f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.22f, 0.54f, 0.80f, 1.0f));
  ImGui::Begin("##path", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
  ImGui::PushFont(Body(), 16.0f * s);
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (!ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
  const bool entered = ImGui::InputText("##folder", custom_path_buffer, sizeof(custom_path_buffer), ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::PopFont();
  ImGui::End();
  ImGui::PopStyleColor(4);
  ImGui::PopStyleVar(4);

  ui.focus.BeginModal();
  const float bh = 42.0f * s, bw = 150.0f * s;
  const float by = m.y1 - 20.0f * s - bh;
  const Rect cancel{m.x1 - 28.0f * s - bw, by, m.x1 - 28.0f * s, by + bh};
  const Rect use{cancel.x0 - 12.0f * s - bw, by, cancel.x0 - 12.0f * s, by + bh};
  const bool was_inert = ui.inert;
  ui.inert = false;
  if (ui.Button("path-use", use, "Use this folder", Style::Primary) || entered) {
    if (custom_path_buffer[0]) UseCustomFolder(custom_path_buffer);
  }
  if (ui.Button("path-cancel", cancel, "Cancel", Style::Secondary) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    custom_path_editing = false;
  }
  ui.inert = was_inert;
  ui.focus.EndModal();
}

}  // namespace tt
