// Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
// ui.h - the installer's look and its small immediate-mode widget set.
//
// Everything is drawn straight into ImGui draw lists in the style of the game's menus (and of
// the port's achievement screens): navy pills with a lit upper half, blue halftone swooshes,
// heavy condensed yellow headings with a dark outline, and a light footer bar with the
// "ACCEPT (A)" style button hints. Widgets register themselves with a focus system so one code
// path serves mouse, touch, keyboard and gamepad.
#pragma once

#include <imgui.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Texture;

namespace tt::ui {

// --- Palette ---------------------------------------------------------------------------------
ImU32 Col(int r, int g, int b, float a = 1.0f);
ImU32 Navy(float a = 1.0f);
ImU32 NavyLight(float a = 1.0f);
ImU32 Blue(float a = 1.0f);
ImU32 BlueLight(float a = 1.0f);
ImU32 Yellow(float a = 1.0f);
ImU32 Outline(float a = 1.0f);
ImU32 White(float a = 1.0f);
ImU32 Grey(float a = 1.0f);
ImU32 PanelDark(float a = 1.0f);
ImU32 FooterLight(float a = 1.0f);
ImU32 FooterText(float a = 1.0f);
ImU32 GreenA(float a = 1.0f);
ImU32 RedB(float a = 1.0f);
ImU32 Orange(float a = 1.0f);

constexpr float kCondensed = 0.84f;  // horizontal squeeze for the heavy headline face
constexpr float kPi = 3.14159265f;

// --- Easing ----------------------------------------------------------------------------------
float Clamp01(float x);
float Stage(float t, float start, float duration);
float EaseOutCubic(float x);
float EaseInCubic(float x);
float EaseInOutSine(float x);
float EaseOutBack(float x);

// --- Fonts (loaded once from the embedded Noto Sans data) -----------------------------------
void LoadFonts(ImFontAtlas* atlas);
ImFont* Heavy();
ImFont* Body();

// --- Text ------------------------------------------------------------------------------------
float CapsTop(float center_y, float px);
float TextWidth(ImFont* font, float px, const char* text, float x_scale = 1.0f);
void AddText(ImDrawList* dl, ImFont* font, float px, ImVec2 pos, ImU32 col, const char* text,
             float x_scale = 1.0f, float wrap_width = 0.0f);
void AddOutlinedText(ImDrawList* dl, ImFont* font, float px, ImVec2 pos, ImU32 col,
                     ImU32 outline_col, float outline, const char* text, float x_scale = 1.0f);
std::string Upper(std::string_view text);
std::string Fit(ImFont* font, float px, float x_scale, float max_width, std::string text);
// Shortens a path from the left ("…/Games/TableTennis") until it fits.
std::string FitPath(ImFont* font, float px, float max_width, std::string path);
// Height of `text` wrapped at `wrap_width`.
float WrappedHeight(ImFont* font, float px, float wrap_width, const char* text);

// --- Shapes ----------------------------------------------------------------------------------
void Halftone(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 col, float spacing, float phase,
              float round_right = 0.0f, bool grow_left_to_right = true);
void ButtonGlyph(ImDrawList* dl, ImVec2 center, float radius, ImU32 fill, const char* letter,
                 float alpha);
void Tick(ImDrawList* dl, ImVec2 center, float size, ImU32 col, float thickness);
void Cross(ImDrawList* dl, ImVec2 center, float size, ImU32 col, float thickness);
void Spinner(ImDrawList* dl, ImVec2 center, float radius, float t, ImU32 col);
void Chevron(ImDrawList* dl, ImVec2 center, float size, bool down, ImU32 col, float thickness);
void Triangle(ImDrawList* dl, ImVec2 center, float size, bool up, ImU32 col);
void FolderIcon(ImDrawList* dl, ImVec2 center, float size, ImU32 col);
void DiscIcon(ImDrawList* dl, ImVec2 center, float radius, ImU32 col, float alpha);
void Paddle(ImDrawList* dl, ImVec2 center, float size, float angle, ImU32 rubber, ImU32 handle, float alpha);

// --- Frame context, input and focus ---------------------------------------------------------
enum class Nav { None, Up, Down, Left, Right };

struct Rect {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  float w() const { return x1 - x0; }
  float h() const { return y1 - y0; }
  ImVec2 min() const { return {x0, y0}; }
  ImVec2 max() const { return {x1, y1}; }
  ImVec2 center() const { return {(x0 + x1) * 0.5f, (y0 + y1) * 0.5f}; }
  bool contains(ImVec2 p) const { return p.x >= x0 && p.x < x1 && p.y >= y0 && p.y < y1; }
};

struct Frame {
  ImDrawList* dl = nullptr;
  float W = 0, H = 0;   // window size in pixels
  float s = 1.0f;       // scale: 1.0 at 1280x800
  float t = 0.0f;       // seconds since start
  float dt = 0.0f;
  ImVec2 mouse{-1, -1};
  bool mouse_moved = false;
  bool mouse_clicked = false;   // left button pressed this frame
  bool mouse_down = false;
  bool mouse_released = false;
  float wheel = 0.0f;
  Nav nav = Nav::None;          // one navigation step this frame
  bool activate = false;        // Enter / Space / A
  bool back = false;            // Escape / Backspace / B
  bool pointer_mode = true;     // last input came from the mouse or touch (hides focus ring)
  SDL_Texture* icon = nullptr;  // the project icon, as a renderer texture
  int icon_w = 0, icon_h = 0;
  SDL_Texture* background = nullptr;  // the static backdrop, rendered once per window size
};

// Keeps the focused widget across frames and resolves navigation between the widgets that
// registered during the previous frame.
class Focus {
 public:
  struct Item {
    std::string id;
    Rect rect;
    bool enabled = true;
  };
  void BeginFrame(Frame& f);
  void EndFrame(Frame& f);
  // Called by widgets. Returns true when this widget is focused.
  bool Register(const std::string& id, const Rect& rect, bool enabled);
  void Set(const std::string& id) { focused_ = id; }
  void SetIfNone(const std::string& id) { if (focused_.empty() || !had_focused_) focused_ = id; }
  const std::string& Current() const { return focused_; }
  void Clear() { focused_.clear(); }
  // When a modal is open only widgets registered after BeginModal take part.
  void BeginModal() { modal_depth_++; items_.clear(); }
  void EndModal() { modal_depth_--; }
  bool InModal() const { return modal_depth_ > 0; }
  // Screen changes reset the memory of what was on screen.
  void Reset() { items_.clear(); previous_.clear(); focused_.clear(); had_focused_ = false; }

 private:
  std::string focused_;
  std::vector<Item> items_;
  std::vector<Item> previous_;
  bool had_focused_ = false;
  int modal_depth_ = 0;
};

// Per-frame widget state shared by the screens.
struct Ui {
  Frame f;
  Focus focus;
  bool inert = false;  // widgets draw but ignore input (under a modal)
  std::string activated_this_frame;

  // --- widgets; `id` must be unique per screen ---
  enum class ButtonStyle { Primary, Secondary, Quiet, Danger };
  bool Button(const std::string& id, const Rect& r, const char* label, ButtonStyle style,
              bool enabled = true, const char* glyph_letter = nullptr);
  // A large selectable card; returns true when activated. `selected` draws it as the current
  // choice. `draw_body` paints the inside (text etc.) and receives the alpha for disabled cards.
  bool Card(const std::string& id, const Rect& r, bool selected, bool enabled,
            const std::function<void(const Rect&, bool focused, float alpha)>& draw_body);
  bool Toggle(const std::string& id, const Rect& r, const char* label, const char* subtitle,
              bool& value, bool enabled = true);
  // A row that looks like a text field; activating it is up to the caller.
  bool Field(const std::string& id, const Rect& r, const char* label, const char* value,
             const char* action_label, bool enabled = true);
  bool Checkbox(const std::string& id, const Rect& r, const char* label, bool& value, bool enabled = true);

  // Returns the hover/focus/press state without drawing (for custom widgets).
  struct Hit {
    bool hovered = false, focused = false, pressed = false, activated = false;
  };
  Hit Interact(const std::string& id, const Rect& r, bool enabled);

  // --- chrome ---
  // The backdrop: the cached texture when main.cpp has rendered one, else the shapes.
  void Background();
  void BackgroundShapes();
  // Header band with the game-style title; `caption` is the small line under it.
  void Header(const char* title, const char* caption, int step, int step_count);
  struct FooterHint {
    const char* label;
    const char* glyph;  // "A", "B", "X", "Y" or nullptr
    ImU32 colour;
  };
  void Footer(const std::vector<FooterHint>& right, const char* left_text = nullptr);
  // Content area between header and footer.
  Rect Content() const;
  // Dark panel with a thin rim.
  void Panel(const Rect& r, float alpha = 1.0f);
  void FocusRing(const Rect& r, float rounding, float alpha = 1.0f);
};

}  // namespace tt::ui
