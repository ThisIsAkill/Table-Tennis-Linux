// Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
// ui.cpp - see ui.h.

#include "ui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
extern const unsigned char tt_font_black_data[], tt_font_black_end[];
extern const unsigned char tt_font_regular_data[], tt_font_regular_end[];
}

namespace tt::ui {

// --- Palette, picked from the game's menus and HUD --------------------------------------------
ImU32 Col(int r, int g, int b, float a) {
  return IM_COL32(r, g, b, int(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f));
}
ImU32 Navy(float a) { return Col(10, 24, 44, a); }
ImU32 NavyLight(float a) { return Col(26, 58, 100, a); }
ImU32 Blue(float a) { return Col(56, 138, 205, a); }
ImU32 BlueLight(float a) { return Col(128, 194, 238, a); }
ImU32 Yellow(float a) { return Col(255, 204, 0, a); }
ImU32 Outline(float a) { return Col(16, 18, 28, a); }
ImU32 White(float a) { return Col(255, 255, 255, a); }
ImU32 Grey(float a) { return Col(158, 164, 174, a); }
ImU32 PanelDark(float a) { return Col(5, 9, 16, a); }
ImU32 FooterLight(float a) { return Col(212, 215, 220, a); }
ImU32 FooterText(float a) { return Col(28, 30, 36, a); }
ImU32 GreenA(float a) { return Col(90, 186, 40, a); }
ImU32 RedB(float a) { return Col(210, 44, 44, a); }
ImU32 Orange(float a) { return Col(255, 150, 30, a); }

// --- Easing -----------------------------------------------------------------------------------
float Clamp01(float x) { return std::clamp(x, 0.0f, 1.0f); }
float Stage(float t, float start, float duration) { return Clamp01((t - start) / duration); }
float EaseOutCubic(float x) { return 1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x); }
float EaseInCubic(float x) { return x * x * x; }
float EaseInOutSine(float x) { return -(std::cos(kPi * x) - 1.0f) * 0.5f; }
float EaseOutBack(float x) {
  const float c1 = 1.70158f;
  const float c3 = c1 + 1.0f;
  const float y = x - 1.0f;
  return 1.0f + c3 * y * y * y + c1 * y * y;
}

// --- Fonts ------------------------------------------------------------------------------------
namespace {
ImFont* g_heavy = nullptr;
ImFont* g_body = nullptr;
}  // namespace

void LoadFonts(ImFontAtlas* atlas) {
  ImFontConfig config;
  config.FontDataOwnedByAtlas = false;  // the data lives in the executable
  std::snprintf(config.Name, sizeof(config.Name), "Noto Sans Black");
  g_heavy = atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(tt_font_black_data),
                                        int(tt_font_black_end - tt_font_black_data), 0.0f, &config);
  std::snprintf(config.Name, sizeof(config.Name), "Noto Sans");
  g_body = atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(tt_font_regular_data),
                                       int(tt_font_regular_end - tt_font_regular_data), 0.0f, &config);
  if (!g_heavy || !g_body) {
    ImFont* fallback = atlas->AddFontDefault();
    if (!g_heavy) g_heavy = fallback;
    if (!g_body) g_body = fallback;
  }
}

ImFont* Heavy() { return g_heavy ? g_heavy : ImGui::GetFont(); }
ImFont* Body() { return g_body ? g_body : ImGui::GetFont(); }

// --- Text -------------------------------------------------------------------------------------
float CapsTop(float center_y, float px) { return center_y - 0.525f * px; }

float TextWidth(ImFont* font, float px, const char* text, float x_scale) {
  return font->CalcTextSizeA(px, FLT_MAX, 0.0f, text).x * x_scale;
}

void AddText(ImDrawList* dl, ImFont* font, float px, ImVec2 pos, ImU32 col, const char* text,
             float x_scale, float wrap_width) {
  const int first = dl->VtxBuffer.Size;
  dl->AddText(font, px, pos, col, text, nullptr, wrap_width > 0.0f ? wrap_width / x_scale : 0.0f);
  if (x_scale != 1.0f) {
    for (int i = first; i < dl->VtxBuffer.Size; ++i) {
      auto& v = dl->VtxBuffer[i];
      v.pos.x = pos.x + (v.pos.x - pos.x) * x_scale;
    }
  }
}

void AddOutlinedText(ImDrawList* dl, ImFont* font, float px, ImVec2 pos, ImU32 col,
                     ImU32 outline_col, float outline, const char* text, float x_scale) {
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

std::string Upper(std::string_view text) {
  std::string out(text);
  for (auto& c : out) {
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  }
  return out;
}

std::string Fit(ImFont* font, float px, float x_scale, float max_width, std::string text) {
  if (TextWidth(font, px, text.c_str(), x_scale) <= max_width) return text;
  while (!text.empty()) {
    text.pop_back();
    while (!text.empty() && (uint8_t(text.back()) & 0xC0) == 0x80) text.pop_back();
    std::string candidate = text + "...";
    if (TextWidth(font, px, candidate.c_str(), x_scale) <= max_width) return candidate;
  }
  return text;
}

std::string FitPath(ImFont* font, float px, float max_width, std::string path) {
  if (TextWidth(font, px, path.c_str()) <= max_width) return path;
  while (!path.empty()) {
    const size_t slash = path.find('/', 1);
    if (slash == std::string::npos) break;
    path = path.substr(slash);
    const std::string candidate = "..." + path;
    if (TextWidth(font, px, candidate.c_str()) <= max_width) return candidate;
  }
  return Fit(font, px, 1.0f, max_width, "..." + path);
}

float WrappedHeight(ImFont* font, float px, float wrap_width, const char* text) {
  return font->CalcTextSizeA(px, FLT_MAX, wrap_width, text).y;
}

// --- Shapes -----------------------------------------------------------------------------------
void Halftone(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 col, float spacing, float phase,
              float round_right, bool grow_left_to_right) {
  const float width = max.x - min.x;
  if (width <= 0.0f || spacing <= 1.0f) return;
  const float cy = (min.y + max.y) * 0.5f;
  const float offset = std::fmod(phase, spacing * 2.0f);
  int row = 0;
  for (float y = min.y + spacing * 0.5f; y < max.y; y += spacing * 0.8f, ++row) {
    const float stagger = (row & 1) ? spacing * 0.5f : 0.0f;
    for (float x = min.x - spacing * 2.0f + stagger + offset; x < max.x + spacing; x += spacing) {
      float u = Clamp01((x - min.x) / width);
      if (!grow_left_to_right) u = 1.0f - u;
      const float grow = u * u * (3.0f - 2.0f * u);
      const float radius = spacing * 0.40f * grow;
      if (radius < 0.5f) continue;
      if (round_right > 0.0f && x > max.x - round_right) {
        const float dx = x - (max.x - round_right);
        const float dy = y - cy;
        if (dx * dx + dy * dy > (round_right - radius) * (round_right - radius)) continue;
      }
      dl->AddCircleFilled(ImVec2(x, y), radius, col, 10);
    }
  }
}

void ButtonGlyph(ImDrawList* dl, ImVec2 center, float radius, ImU32 fill, const char* letter,
                 float alpha) {
  dl->AddCircleFilled(center, radius, Col(20, 22, 28, 0.9f * alpha), 24);
  dl->AddCircleFilled(center, radius * 0.84f, fill, 24);
  dl->AddCircle(center, radius * 0.84f, White(0.35f * alpha), 24, radius * 0.12f);
  const float px = radius * 1.55f;
  const float w = TextWidth(Heavy(), px, letter);
  AddText(dl, Heavy(), px, ImVec2(center.x - w * 0.5f, CapsTop(center.y, px)), White(alpha), letter);
}

void Tick(ImDrawList* dl, ImVec2 c, float size, ImU32 col, float thickness) {
  const ImVec2 a(c.x - size * 0.45f, c.y + size * 0.02f);
  const ImVec2 b(c.x - size * 0.12f, c.y + size * 0.36f);
  const ImVec2 d(c.x + size * 0.5f, c.y - size * 0.34f);
  dl->PathLineTo(a);
  dl->PathLineTo(b);
  dl->PathLineTo(d);
  dl->PathStroke(col, ImDrawFlags_None, thickness);
}

void Cross(ImDrawList* dl, ImVec2 c, float size, ImU32 col, float thickness) {
  const float h = size * 0.4f;
  dl->AddLine(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), col, thickness);
  dl->AddLine(ImVec2(c.x - h, c.y + h), ImVec2(c.x + h, c.y - h), col, thickness);
}

// A ping-pong ball circling the centre with a fading trail, instead of a plain spinner.
void Spinner(ImDrawList* dl, ImVec2 center, float radius, float t, ImU32 col) {
  const float a = ((col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
  const ImU32 rgb = col & ~IM_COL32_A_MASK;
  dl->AddCircle(center, radius, (rgb | (ImU32(int(a * 0.18f * 255)) << IM_COL32_A_SHIFT)), 48, radius * 0.16f);
  const float head = t * 4.2f;
  const int trail = 14;
  for (int i = trail; i >= 0; --i) {
    const float ang = head - i * 0.17f;
    const float k = 1.0f - float(i) / float(trail + 1);
    const ImVec2 p(center.x + std::cos(ang) * radius, center.y + std::sin(ang) * radius);
    const ImU32 c = rgb | (ImU32(int(a * k * k * 255)) << IM_COL32_A_SHIFT);
    dl->AddCircleFilled(p, radius * (0.12f + 0.16f * k), c, 16);
  }
  const ImVec2 p(center.x + std::cos(head) * radius, center.y + std::sin(head) * radius);
  dl->AddCircleFilled(p, radius * 0.3f, White(a), 20);
  dl->AddCircle(p, radius * 0.3f, Col(0, 0, 0, 0.35f * a), 20, radius * 0.05f);
}

void Chevron(ImDrawList* dl, ImVec2 c, float size, bool down, ImU32 col, float thickness) {
  const float h = size * 0.5f;
  const float dir = down ? 1.0f : -1.0f;
  dl->PathLineTo(ImVec2(c.x - h, c.y - h * 0.5f * dir));
  dl->PathLineTo(ImVec2(c.x, c.y + h * 0.5f * dir));
  dl->PathLineTo(ImVec2(c.x + h, c.y - h * 0.5f * dir));
  dl->PathStroke(col, ImDrawFlags_None, thickness);
}

void Triangle(ImDrawList* dl, ImVec2 center, float size, bool up, ImU32 col) {
  const float h = size, w = size * 1.15f;
  if (up) {
    dl->AddTriangleFilled(ImVec2(center.x, center.y - h * 0.5f),
                          ImVec2(center.x + w * 0.5f, center.y + h * 0.5f),
                          ImVec2(center.x - w * 0.5f, center.y + h * 0.5f), col);
  } else {
    dl->AddTriangleFilled(ImVec2(center.x - w * 0.5f, center.y - h * 0.5f),
                          ImVec2(center.x + w * 0.5f, center.y - h * 0.5f),
                          ImVec2(center.x, center.y + h * 0.5f), col);
  }
}

void FolderIcon(ImDrawList* dl, ImVec2 c, float size, ImU32 col) {
  const float w = size, h = size * 0.74f;
  const ImVec2 p0(c.x - w * 0.5f, c.y - h * 0.5f + size * 0.1f);
  const ImVec2 p1(c.x + w * 0.5f, c.y + h * 0.5f);
  dl->AddRectFilled(ImVec2(p0.x, p0.y - size * 0.1f), ImVec2(p0.x + w * 0.42f, p0.y + size * 0.1f), col, size * 0.06f);
  dl->AddRectFilled(p0, p1, col, size * 0.08f);
}

void DiscIcon(ImDrawList* dl, ImVec2 c, float radius, ImU32 col, float alpha) {
  dl->AddCircleFilled(c, radius, col, 48);
  dl->AddCircle(c, radius, White(0.5f * alpha), 48, radius * 0.05f);
  dl->AddCircleFilled(c, radius * 0.30f, Navy(alpha), 32);
  dl->AddCircleFilled(c, radius * 0.12f, White(0.8f * alpha), 24);
  // A pair of reflections like light on a disc.
  dl->PathArcTo(c, radius * 0.66f, -2.4f, -1.5f, 16);
  dl->PathStroke(White(0.45f * alpha), ImDrawFlags_None, radius * 0.1f);
  dl->PathArcTo(c, radius * 0.66f, 0.7f, 1.6f, 16);
  dl->PathStroke(White(0.25f * alpha), ImDrawFlags_None, radius * 0.1f);
}

void Paddle(ImDrawList* dl, ImVec2 c, float size, float angle, ImU32 rubber, ImU32 handle, float alpha) {
  const float ca = std::cos(angle), sa = std::sin(angle);
  auto rot = [&](float x, float y) { return ImVec2(c.x + x * ca - y * sa, c.y + x * sa + y * ca); };
  // Handle: a slim rounded quad below the blade.
  const float hw = size * 0.13f, h0 = size * 0.35f, h1 = size * 0.95f;
  const ImVec2 quad[4] = {rot(-hw, h0), rot(hw, h0), rot(hw * 0.8f, h1), rot(-hw * 0.8f, h1)};
  dl->AddConvexPolyFilled(quad, 4, handle);
  dl->AddCircleFilled(rot(0, h1), hw * 0.8f, handle, 12);
  // Blade.
  dl->AddCircleFilled(rot(0, 0), size * 0.48f, Outline(alpha), 40);
  dl->AddCircleFilled(rot(0, 0), size * 0.44f, rubber, 40);
  dl->AddCircle(rot(0, 0), size * 0.36f, White(0.12f * alpha), 40, size * 0.02f);
}

// --- Focus ------------------------------------------------------------------------------------
void Focus::BeginFrame(Frame&) {
  previous_ = std::move(items_);
  items_.clear();
}

bool Focus::Register(const std::string& id, const Rect& rect, bool enabled) {
  items_.push_back({id, rect, enabled});
  return enabled && id == focused_;
}

void Focus::EndFrame(Frame& f) {
  auto find = [&](const std::string& id) -> const Item* {
    for (const auto& item : items_) {
      if (item.id == id && item.enabled) return &item;
    }
    return nullptr;
  };
  const Item* current = find(focused_);
  if (!current) {
    // Nothing (enabled) has the focus any more: take the first enabled widget.
    focused_.clear();
    for (const auto& item : items_) {
      if (item.enabled) {
        focused_ = item.id;
        current = &item;
        break;
      }
    }
  }
  had_focused_ = current != nullptr;
  if (f.nav == Nav::None || !current) return;

  const ImVec2 from = current->rect.center();
  const Item* best = nullptr;
  float best_score = FLT_MAX;
  for (const auto& item : items_) {
    if (!item.enabled || item.id == current->id) continue;
    const ImVec2 to = item.rect.center();
    const float dx = to.x - from.x, dy = to.y - from.y;
    float primary = 0.0f, secondary = 0.0f;
    switch (f.nav) {
      case Nav::Up: primary = -dy; secondary = std::fabs(dx); break;
      case Nav::Down: primary = dy; secondary = std::fabs(dx); break;
      case Nav::Left: primary = -dx; secondary = std::fabs(dy); break;
      case Nav::Right: primary = dx; secondary = std::fabs(dy); break;
      default: break;
    }
    // Rects that overlap on the cross axis count as straight ahead.
    const bool vertical = f.nav == Nav::Up || f.nav == Nav::Down;
    const float overlap = vertical
        ? std::min(current->rect.x1, item.rect.x1) - std::max(current->rect.x0, item.rect.x0)
        : std::min(current->rect.y1, item.rect.y1) - std::max(current->rect.y0, item.rect.y0);
    if (primary <= 1.0f) continue;
    const float score = primary + (overlap > 0.0f ? 0.0f : secondary * 2.5f);
    if (score < best_score) {
      best_score = score;
      best = &item;
    }
  }
  if (best) focused_ = best->id;
}

// --- Widgets ----------------------------------------------------------------------------------
namespace {
std::string g_pressed_id;
}

Ui::Hit Ui::Interact(const std::string& id, const Rect& r, bool enabled) {
  Hit hit;
  if (inert) return hit;
  hit.focused = focus.Register(id, r, enabled);
  if (!enabled) return hit;
  hit.hovered = r.contains(f.mouse) && f.pointer_mode;
  if (hit.hovered && f.mouse_moved) focus.Set(id);
  if (hit.hovered && f.mouse_clicked) g_pressed_id = id;
  hit.pressed = g_pressed_id == id && f.mouse_down;
  if (g_pressed_id == id && f.mouse_released) {
    if (hit.hovered) hit.activated = true;
    g_pressed_id.clear();
  }
  if (hit.focused && f.activate) hit.activated = true;
  if (hit.activated) activated_this_frame = id;
  return hit;
}

void Ui::FocusRing(const Rect& r, float rounding, float alpha) {
  const float s = f.s;
  const float pulse = 0.75f + 0.25f * std::sin(f.t * 5.0f);
  f.dl->AddRect(ImVec2(r.x0 - 3.0f * s, r.y0 - 3.0f * s), ImVec2(r.x1 + 3.0f * s, r.y1 + 3.0f * s),
                Yellow(alpha * pulse), rounding + 3.0f * s, 0, 2.5f * s);
}

bool Ui::Button(const std::string& id, const Rect& r, const char* label, ButtonStyle style,
                bool enabled, const char* glyph_letter) {
  const Hit hit = Interact(id, r, enabled);
  ImDrawList* dl = f.dl;
  const float s = f.s;
  const float alpha = enabled ? 1.0f : 0.4f;
  const float rounding = r.h() * 0.5f;
  const bool lit = hit.hovered || (hit.focused && !f.pointer_mode);
  const float press = hit.pressed ? 1.0f : 0.0f;

  ImU32 fill, text_col, rim;
  switch (style) {
    case ButtonStyle::Primary:
      fill = lit ? Col(255, 220, 60, alpha) : Yellow(alpha);
      if (press > 0) fill = Col(220, 170, 0, alpha);
      text_col = Col(20, 24, 36, alpha);
      rim = Col(0, 0, 0, 0.35f * alpha);
      break;
    case ButtonStyle::Danger:
      fill = lit ? Col(70, 24, 30, alpha) : Col(44, 16, 22, alpha);
      text_col = Col(255, 170, 170, alpha);
      rim = RedB(0.8f * alpha);
      break;
    case ButtonStyle::Quiet:
      fill = lit ? NavyLight(0.45f * alpha) : Col(0, 0, 0, 0.0f);
      text_col = lit ? White(alpha) : Grey(alpha);
      rim = lit ? Blue(0.5f * alpha) : White(0.18f * alpha);
      break;
    case ButtonStyle::Secondary:
    default:
      fill = lit ? Col(40, 96, 160, alpha) : NavyLight(alpha);
      if (press > 0) fill = Navy(alpha);
      text_col = White(alpha);
      rim = Blue(0.8f * alpha);
      break;
  }
  // Shadow, body, lit upper half, rim.
  if (style != ButtonStyle::Quiet || lit) {
    dl->AddRectFilled(ImVec2(r.x0, r.y0 + 3.0f * s), ImVec2(r.x1, r.y1 + 3.0f * s), Col(0, 0, 0, 0.30f * alpha), rounding);
  }
  dl->AddRectFilled(r.min(), r.max(), fill, rounding);
  if (style != ButtonStyle::Quiet) {
    dl->AddRectFilled(r.min(), ImVec2(r.x1, (r.y0 + r.y1) * 0.5f), White(0.10f * alpha), rounding,
                      ImDrawFlags_RoundCornersTop);
  }
  dl->AddRect(r.min(), r.max(), rim, rounding, 0, 1.5f * s);

  const float px = std::min(r.h() * 0.42f, 24.0f * s);
  const std::string text = Upper(label);
  float glyph_w = 0.0f;
  const float glyph_r = px * 0.5f;
  if (glyph_letter) glyph_w = glyph_r * 2.0f + px * 0.4f;
  const float tw = TextWidth(Heavy(), px, text.c_str(), kCondensed);
  const float total = tw + glyph_w;
  const float x = r.x0 + (r.w() - total) * 0.5f;
  const float cy = (r.y0 + r.y1) * 0.5f;
  AddText(dl, Heavy(), px, ImVec2(x, CapsTop(cy, px)), text_col, text.c_str(), kCondensed);
  if (glyph_letter) {
    const bool a = std::strcmp(glyph_letter, "A") == 0;
    const bool b = std::strcmp(glyph_letter, "B") == 0;
    const ImU32 col = a ? GreenA(alpha) : b ? RedB(alpha) : Blue(alpha);
    ButtonGlyph(dl, ImVec2(x + tw + px * 0.4f + glyph_r, cy), glyph_r, col, glyph_letter, alpha);
  }
  if (hit.focused && !f.pointer_mode) FocusRing(r, rounding, alpha);
  return hit.activated;
}

bool Ui::Card(const std::string& id, const Rect& r, bool selected, bool enabled,
              const std::function<void(const Rect&, bool, float)>& draw_body) {
  const Hit hit = Interact(id, r, enabled);
  ImDrawList* dl = f.dl;
  const float s = f.s;
  const float alpha = enabled ? 1.0f : 0.45f;
  const bool lit = hit.hovered || (hit.focused && !f.pointer_mode);
  const float rounding = 8.0f * s;
  dl->AddRectFilled(ImVec2(r.x0, r.y0 + 3.0f * s), ImVec2(r.x1, r.y1 + 3.0f * s), Col(0, 0, 0, 0.28f), rounding);
  ImU32 fill = selected ? Col(40, 96, 160, 0.42f) : (lit ? Col(26, 50, 84, 0.85f) : PanelDark(0.88f));
  if (hit.pressed) fill = Col(40, 96, 160, 0.6f);
  dl->AddRectFilled(r.min(), r.max(), fill, rounding);
  dl->AddRect(r.min(), r.max(), selected ? Blue(0.9f) : (lit ? Blue(0.7f) : NavyLight(0.8f)), rounding, 0, 1.5f * s);
  if (selected) {
    dl->AddRectFilled(ImVec2(r.x0, r.y0 + 8.0f * s), ImVec2(r.x0 + 4.0f * s, r.y1 - 8.0f * s), Yellow(), 2.0f * s);
  }
  dl->PushClipRect(r.min(), r.max(), true);
  draw_body(r, lit, alpha);
  dl->PopClipRect();
  if (hit.focused && !f.pointer_mode) FocusRing(r, rounding, 1.0f);
  return hit.activated;
}

bool Ui::Toggle(const std::string& id, const Rect& r, const char* label, const char* subtitle,
                bool& value, bool enabled) {
  const Hit hit = Interact(id, r, enabled);
  if (hit.activated) value = !value;
  ImDrawList* dl = f.dl;
  const float s = f.s;
  const float alpha = enabled ? 1.0f : 0.45f;
  const bool lit = hit.hovered || (hit.focused && !f.pointer_mode);
  const float rounding = 8.0f * s;
  dl->AddRectFilled(r.min(), r.max(), lit ? Col(26, 50, 84, 0.7f) : PanelDark(0.7f), rounding);
  dl->AddRect(r.min(), r.max(), lit ? Blue(0.7f) : NavyLight(0.7f), rounding, 0, 1.2f * s);

  // Switch on the right.
  const float sw_w = 58.0f * s, sw_h = 30.0f * s;
  const float sx1 = r.x1 - 18.0f * s, sx0 = sx1 - sw_w;
  const float cy = (r.y0 + r.y1) * 0.5f;
  const Rect track{sx0, cy - sw_h * 0.5f, sx1, cy + sw_h * 0.5f};
  dl->AddRectFilled(track.min(), track.max(), value ? Blue(alpha) : Col(60, 66, 78, alpha), sw_h * 0.5f);
  dl->AddRect(track.min(), track.max(), value ? BlueLight(0.8f * alpha) : White(0.2f * alpha), sw_h * 0.5f, 0, 1.2f * s);
  const float knob_x = value ? sx1 - sw_h * 0.5f : sx0 + sw_h * 0.5f;
  dl->AddCircleFilled(ImVec2(knob_x, cy), sw_h * 0.38f, Col(0, 0, 0, 0.3f * alpha), 20);
  dl->AddCircleFilled(ImVec2(knob_x, cy - 1.0f * s), sw_h * 0.38f, value ? Yellow(alpha) : Grey(alpha), 20);
  const char* state = value ? "ON" : "OFF";
  const float state_px = 13.0f * s;
  const float state_w = TextWidth(Heavy(), state_px, state, kCondensed);
  AddText(dl, Heavy(), state_px, ImVec2(sx0 - 10.0f * s - state_w, CapsTop(cy, state_px)),
          value ? BlueLight(alpha) : Grey(0.8f * alpha), state, kCondensed);

  // Label and subtitle.
  const float text_x = r.x0 + 18.0f * s;
  const float text_w = sx0 - 10.0f * s - state_w - 16.0f * s - text_x;
  const float label_px = 19.0f * s;
  const std::string label_text = Fit(Heavy(), label_px, kCondensed, text_w, Upper(label));
  if (subtitle && *subtitle) {
    AddOutlinedText(dl, Heavy(), label_px, ImVec2(text_x, r.y0 + 12.0f * s), lit ? Yellow(alpha) : White(alpha),
                    Outline(alpha), 1.2f * s, label_text.c_str(), kCondensed);
    const float sub_px = 14.0f * s;
    dl->PushClipRect(ImVec2(text_x, r.y0), ImVec2(text_x + text_w, r.y1), true);
    AddText(dl, Body(), sub_px, ImVec2(text_x, r.y0 + 38.0f * s), Grey(alpha), subtitle, 1.0f, text_w);
    dl->PopClipRect();
  } else {
    AddOutlinedText(dl, Heavy(), label_px, ImVec2(text_x, CapsTop(cy, label_px)), lit ? Yellow(alpha) : White(alpha),
                    Outline(alpha), 1.2f * s, label_text.c_str(), kCondensed);
  }
  if (hit.focused && !f.pointer_mode) FocusRing(r, rounding, 1.0f);
  return hit.activated;
}

bool Ui::Checkbox(const std::string& id, const Rect& r, const char* label, bool& value, bool enabled) {
  const Hit hit = Interact(id, r, enabled);
  if (hit.activated) value = !value;
  ImDrawList* dl = f.dl;
  const float s = f.s;
  const float alpha = enabled ? 1.0f : 0.45f;
  const bool lit = hit.hovered || (hit.focused && !f.pointer_mode);
  const float box = 22.0f * s;
  const float cy = (r.y0 + r.y1) * 0.5f;
  const Rect b{r.x0 + 6.0f * s, cy - box * 0.5f, r.x0 + 6.0f * s + box, cy + box * 0.5f};
  dl->AddRectFilled(b.min(), b.max(), value ? Blue(alpha) : Col(40, 46, 58, alpha), 5.0f * s);
  dl->AddRect(b.min(), b.max(), lit ? Yellow(alpha) : BlueLight(0.6f * alpha), 5.0f * s, 0, 1.4f * s);
  if (value) Tick(dl, b.center(), box * 0.8f, White(alpha), 2.6f * s);
  const float px = 15.0f * s;
  AddText(dl, Body(), px, ImVec2(b.x1 + 12.0f * s, CapsTop(cy, px) + px * 0.03f), lit ? White(alpha) : Grey(alpha), label,
          1.0f, r.x1 - (b.x1 + 12.0f * s));
  if (hit.focused && !f.pointer_mode) FocusRing(r, 6.0f * s, 1.0f);
  return hit.activated;
}

bool Ui::Field(const std::string& id, const Rect& r, const char* label, const char* value,
               const char* action_label, bool enabled) {
  const Hit hit = Interact(id, r, enabled);
  ImDrawList* dl = f.dl;
  const float s = f.s;
  const float alpha = enabled ? 1.0f : 0.45f;
  const bool lit = hit.hovered || (hit.focused && !f.pointer_mode);
  const float rounding = 8.0f * s;
  dl->AddRectFilled(r.min(), r.max(), lit ? Col(26, 50, 84, 0.7f) : PanelDark(0.7f), rounding);
  dl->AddRect(r.min(), r.max(), lit ? Blue(0.7f) : NavyLight(0.7f), rounding, 0, 1.2f * s);
  const float label_px = 12.0f * s;
  AddText(dl, Heavy(), label_px, ImVec2(r.x0 + 18.0f * s, r.y0 + 9.0f * s), BlueLight(alpha), Upper(label).c_str(), kCondensed);
  // Action chip on the right.
  const float chip_px = 13.0f * s;
  const std::string chip = Upper(action_label);
  const float chip_w = TextWidth(Heavy(), chip_px, chip.c_str(), kCondensed) + 28.0f * s;
  const float chip_h = 28.0f * s;
  const float cy = (r.y0 + r.y1) * 0.5f;
  const Rect c{r.x1 - 14.0f * s - chip_w, cy - chip_h * 0.5f, r.x1 - 14.0f * s, cy + chip_h * 0.5f};
  dl->AddRectFilled(c.min(), c.max(), lit ? Yellow(alpha) : NavyLight(alpha), chip_h * 0.5f);
  dl->AddRect(c.min(), c.max(), lit ? Col(0, 0, 0, 0.3f * alpha) : Blue(0.8f * alpha), chip_h * 0.5f, 0, 1.2f * s);
  AddText(dl, Heavy(), chip_px, ImVec2(c.x0 + 14.0f * s, CapsTop(cy, chip_px)), lit ? Col(20, 24, 36, alpha) : White(alpha),
          chip.c_str(), kCondensed);
  // Value.
  const float value_px = 17.0f * s;
  const float value_w = c.x0 - 16.0f * s - (r.x0 + 18.0f * s);
  const std::string shown = FitPath(Body(), value_px, value_w, value);
  AddText(dl, Body(), value_px, ImVec2(r.x0 + 18.0f * s, r.y0 + 27.0f * s), White(alpha), shown.c_str());
  if (hit.focused && !f.pointer_mode) FocusRing(r, rounding, 1.0f);
  return hit.activated;
}

// --- Chrome -----------------------------------------------------------------------------------
Rect Ui::Content() const {
  const float s = f.s;
  const float margin = 40.0f * s;
  const float width = std::min(f.W - 2.0f * margin, 1200.0f * s);
  const float x0 = (f.W - width) * 0.5f;
  return {x0, 118.0f * s, x0 + width, f.H - 76.0f * s};
}

void Ui::Background() {
  if (f.background) return;  // main.cpp blits the cached texture before the draw lists
  BackgroundShapes();
}

void Ui::BackgroundShapes() {
  ImDrawList* dl = f.dl;
  const float W = f.W, H = f.H, s = f.s, t = f.t;
  // Navy gradient.
  dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H * 0.55f), Col(14, 32, 58), Col(14, 32, 58),
                              Col(8, 18, 34), Col(8, 18, 34));
  dl->AddRectFilledMultiColor(ImVec2(0, H * 0.55f), ImVec2(W, H), Col(8, 18, 34), Col(8, 18, 34),
                              Col(3, 7, 14), Col(3, 7, 14));

  // The game's big light-blue swoosh across the top, behind the header band.
  {
    const int n = 40;
    ImVec2 pts[n];
    for (int i = 0; i < n; ++i) {
      const float u = float(i) / float(n - 1);
      const float x = -0.1f * W + 1.25f * W * u;
      const float y = H * (0.085f - 0.07f * std::sin(u * kPi) + 0.05f * u);
      pts[i] = ImVec2(x, y);
    }
    dl->AddPolyline(pts, n, Col(150, 200, 236, 0.16f), ImDrawFlags_None, 76.0f * s);
    dl->AddPolyline(pts, n, Col(190, 225, 245, 0.10f), ImDrawFlags_None, 3.0f * s);
    // Dots on the right end of the band, growing toward the edge.
    Halftone(dl, ImVec2(W * 0.55f, H * 0.03f), ImVec2(W, H * 0.21f), Navy(0.28f), 11.0f * s, t * 3.0f * s);
  }
  // A quieter dark band low on the left with light dots fading in at the edge.
  {
    const int n = 30;
    ImVec2 pts[n];
    for (int i = 0; i < n; ++i) {
      const float u = float(i) / float(n - 1);
      pts[i] = ImVec2(-0.05f * W + 0.75f * W * u, H * (0.80f + 0.12f * u * u));
    }
    dl->AddPolyline(pts, n, Col(40, 90, 150, 0.10f), ImDrawFlags_None, 54.0f * s);
    Halftone(dl, ImVec2(0, H * 0.70f), ImVec2(W * 0.30f, H * 0.98f), BlueLight(0.16f), 10.0f * s, -t * 2.0f * s, 0.0f, false);
  }
  // A faint table in perspective, low in the picture.
  {
    const float cx = W * 0.5f, base = H * 0.93f, top = H * 0.60f;
    const float half_near = W * 0.36f, half_far = W * 0.19f;
    const ImVec2 q[4] = {ImVec2(cx - half_far, top), ImVec2(cx + half_far, top),
                         ImVec2(cx + half_near, base), ImVec2(cx - half_near, base)};
    dl->AddConvexPolyFilled(q, 4, Col(40, 120, 190, 0.10f));
    dl->AddPolyline(q, 4, White(0.09f), ImDrawFlags_Closed, 2.0f * s);
    dl->AddLine(ImVec2(cx, top), ImVec2(cx, base), White(0.06f), 1.5f * s);
    const float mid = (top + base) * 0.5f, half_mid = (half_near + half_far) * 0.5f;
    dl->AddRectFilled(ImVec2(cx - half_mid - 6.0f * s, mid - 14.0f * s), ImVec2(cx + half_mid + 6.0f * s, mid),
                      White(0.05f));
    dl->AddLine(ImVec2(cx - half_mid - 6.0f * s, mid - 14.0f * s), ImVec2(cx + half_mid + 6.0f * s, mid - 14.0f * s),
                White(0.08f), 1.5f * s);
  }
  // Vignette.
  dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W * 0.25f, H), Col(0, 0, 0, 0.35f), Col(0, 0, 0, 0.0f),
                              Col(0, 0, 0, 0.0f), Col(0, 0, 0, 0.35f));
  dl->AddRectFilledMultiColor(ImVec2(W * 0.75f, 0), ImVec2(W, H), Col(0, 0, 0, 0.0f), Col(0, 0, 0, 0.35f),
                              Col(0, 0, 0, 0.35f), Col(0, 0, 0, 0.0f));
}

void Ui::Header(const char* title, const char* caption, int step, int step_count) {
  ImDrawList* dl = f.dl;
  const float W = f.W, s = f.s, t = f.t;
  const Rect content = Content();
  const float h = 66.0f * s;
  const float y0 = 32.0f * s, y1 = y0 + h;
  const float cy = (y0 + y1) * 0.5f;
  const float x0 = content.x0 - 18.0f * s;
  // Navy band running off the right edge, lit upper half, swoosh dots in the tail.
  dl->AddRectFilled(ImVec2(x0, y0 + 4.0f * s), ImVec2(W + h, y1 + 4.0f * s), Col(0, 0, 0, 0.35f), h * 0.5f,
                    ImDrawFlags_RoundCornersLeft);
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(W + h, y1), Navy(0.97f), h * 0.5f, ImDrawFlags_RoundCornersLeft);
  dl->PushClipRect(ImVec2(x0, y0), ImVec2(W, y1), true);
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(W, cy), NavyLight(0.25f), h * 0.5f, ImDrawFlags_RoundCornersTopLeft);
  Halftone(dl, ImVec2(content.x1 - 150.0f * s, y0), ImVec2(W, y1), BlueLight(0.42f), 9.0f * s, t * 4.0f * s);
  dl->PopClipRect();
  dl->AddLine(ImVec2(x0 + h * 0.3f, y1 + 1.0f * s), ImVec2(W, y1 + 1.0f * s), Blue(0.7f), 2.0f * s);

  // Icon disc at the left end.
  float x = content.x0 + 2.0f * s;
  {
    const float r = h * 0.36f;
    const ImVec2 c(x + r, cy);
    dl->AddCircleFilled(c, r + 3.0f * s, Outline(0.9f), 32);
    if (f.icon) {
      dl->AddImageRounded(reinterpret_cast<ImTextureID>(f.icon), ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r),
                          ImVec2(0, 0), ImVec2(1, 1), White(), r);
    } else {
      Paddle(dl, c, r * 1.3f, -0.6f, RedB(), Col(190, 150, 90), 1.0f);
    }
    dl->AddCircle(c, r + 1.0f * s, White(0.5f), 32, 1.5f * s);
    x += r * 2.0f + 16.0f * s;
  }
  // Title and caption.
  const float title_px = 40.0f * s;
  const std::string title_text = Upper(title);
  AddOutlinedText(dl, Heavy(), title_px, ImVec2(x, CapsTop(cy, title_px)), Yellow(), Outline(), 2.6f * s,
                  title_text.c_str(), kCondensed);
  x += TextWidth(Heavy(), title_px, title_text.c_str(), kCondensed) + 18.0f * s;
  if (caption && *caption) {
    dl->AddRectFilled(ImVec2(x, cy - 14.0f * s), ImVec2(x + 2.0f * s, cy + 14.0f * s), Yellow(0.8f));
    x += 14.0f * s;
    const float cap_px = 15.0f * s;
    AddText(dl, Heavy(), cap_px, ImVec2(x, CapsTop(cy, cap_px)), BlueLight(), Upper(caption).c_str(), kCondensed);
  }
  // Step pips on the right.
  if (step_count > 0) {
    char line[32];
    std::snprintf(line, sizeof(line), "STEP %d OF %d", step, step_count);
    const float px = 13.0f * s;
    const float w = TextWidth(Heavy(), px, line, kCondensed);
    const float right = content.x1 - 12.0f * s;
    AddOutlinedText(dl, Heavy(), px, ImVec2(right - w, CapsTop(cy - 9.0f * s, px)), White(), Outline(), 1.2f * s, line,
                    kCondensed);
    const float pip_w = 22.0f * s, gap = 5.0f * s;
    float px0 = right - (pip_w * step_count + gap * (step_count - 1));
    for (int i = 1; i <= step_count; ++i) {
      const ImU32 c = i < step ? Yellow(0.9f) : i == step ? Yellow() : White(0.25f);
      dl->AddRectFilled(ImVec2(px0, cy + 8.0f * s), ImVec2(px0 + pip_w, cy + 12.0f * s), c, 2.0f * s);
      px0 += pip_w + gap;
    }
  }
}

void Ui::Footer(const std::vector<FooterHint>& right, const char* left_text) {
  ImDrawList* dl = f.dl;
  const float s = f.s;
  const Rect content = Content();
  const float h = 40.0f * s;
  const float y1 = f.H - 22.0f * s, y0 = y1 - h;
  const float cy = (y0 + y1) * 0.5f;
  dl->AddRectFilled(ImVec2(content.x0, y0 + 3.0f * s), ImVec2(content.x1, y1 + 3.0f * s), Col(0, 0, 0, 0.3f), h * 0.5f);
  dl->AddRectFilled(ImVec2(content.x0, y0), ImVec2(content.x1, y1), FooterLight(0.96f), h * 0.5f);
  const float px = 16.0f * s;
  float x = content.x1 - 22.0f * s;
  for (const auto& hint : right) {
    const float glyph_r = 11.0f * s;
    if (hint.glyph) {
      ButtonGlyph(dl, ImVec2(x - glyph_r, cy), glyph_r, hint.colour, hint.glyph, 1.0f);
      x -= glyph_r * 2.0f + 8.0f * s;
    }
    const std::string label = Upper(hint.label);
    const float w = TextWidth(Heavy(), px, label.c_str(), kCondensed);
    AddText(dl, Heavy(), px, ImVec2(x - w, CapsTop(cy, px)), FooterText(), label.c_str(), kCondensed);
    x -= w + 30.0f * s;
  }
  if (left_text && *left_text) {
    const float lpx = 13.0f * s;
    const std::string text = Fit(Heavy(), lpx, kCondensed, x - content.x0 - 40.0f * s, left_text);
    AddText(dl, Heavy(), lpx, ImVec2(content.x0 + 24.0f * s, CapsTop(cy, lpx)), FooterText(0.7f), text.c_str(), kCondensed);
  }
}

void Ui::Panel(const Rect& r, float alpha) {
  const float s = f.s;
  f.dl->AddRectFilled(ImVec2(r.x0, r.y0 + 4.0f * s), ImVec2(r.x1, r.y1 + 4.0f * s), Col(0, 0, 0, 0.3f * alpha), 10.0f * s);
  f.dl->AddRectFilled(r.min(), r.max(), PanelDark(0.90f * alpha), 10.0f * s);
  f.dl->AddRect(r.min(), r.max(), NavyLight(0.8f * alpha), 10.0f * s, 0, 1.5f * s);
}

}  // namespace tt::ui
