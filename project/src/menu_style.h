// Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
// menu_style.h - small drawing helpers shared by the game-styled overlays (the PC settings page
// in pc_settings.cpp and the achievements popup and list in achievement_ui.cpp): the menu colour
// palette and text drawn in the game's own style. Header-only; everything is inline.

#pragma once

#include <imgui.h>

#include <algorithm>
#include <cfloat>

namespace tabletennis::menu_style {

// The game's menu palette. Alpha is 0..1.
inline ImU32 Col(int r, int g, int b, float a = 1.0f) {
  return IM_COL32(r, g, b, int(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f));
}
inline ImU32 Navy(float a = 1.0f) { return Col(10, 24, 44, a); }
inline ImU32 NavyLight(float a = 1.0f) { return Col(26, 58, 100, a); }
inline ImU32 Blue(float a = 1.0f) { return Col(56, 138, 205, a); }
inline ImU32 BlueLight(float a = 1.0f) { return Col(128, 194, 238, a); }
inline ImU32 Yellow(float a = 1.0f) { return Col(255, 204, 0, a); }
inline ImU32 Outline(float a = 1.0f) { return Col(16, 18, 28, a); }
inline ImU32 White(float a = 1.0f) { return Col(255, 255, 255, a); }
inline ImU32 Grey(float a = 1.0f) { return Col(158, 164, 174, a); }

// The top of a capital letter drawn at `px`, for vertically centring a line on `center_y`.
inline float CapsTop(float center_y, float px) { return center_y - 0.525f * px; }

inline float TextWidth(ImFont* font, float px, const char* text, float x_scale = 1.0f) {
  return font->CalcTextSizeA(px, FLT_MAX, 0.0f, text).x * x_scale;
}

// Draws text, then squeezes the glyphs it added horizontally around the text origin.
inline void AddText(ImDrawList* dl, ImFont* font, float px, ImVec2 pos, ImU32 col, const char* text,
                    float x_scale = 1.0f, float wrap_width = 0.0f) {
  const int first = dl->VtxBuffer.Size;
  dl->AddText(font, px, pos, col, text, nullptr, wrap_width > 0.0f ? wrap_width / x_scale : 0.0f);
  if (x_scale != 1.0f) {
    for (int i = first; i < dl->VtxBuffer.Size; ++i) {
      auto& v = dl->VtxBuffer[i];
      v.pos.x = pos.x + (v.pos.x - pos.x) * x_scale;
    }
  }
}

// AddText with an 8-direction outline behind it.
inline void AddOutlinedText(ImDrawList* dl, ImFont* font, float px, ImVec2 pos, ImU32 col,
                            ImU32 outline_col, float outline, const char* text, float x_scale = 1.0f) {
  if (outline > 0.0f) {
    static const float kDirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1},
                                      {0.7f, 0.7f}, {-0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}};
    for (const auto& d : kDirs) {
      AddText(dl, font, px, ImVec2(pos.x + d[0] * outline, pos.y + d[1] * outline), outline_col,
              text, x_scale);
    }
  }
  AddText(dl, font, px, pos, col, text, x_scale);
}

}  // namespace tabletennis::menu_style
