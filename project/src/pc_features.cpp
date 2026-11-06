// Copyright (c) 2026 Akhil Moola (ThisIsAkill). Portions copyright (c) 2026 davidptrssvea.
// MIT License, see LICENSE.
// pc_features.cpp - see pc_features.h.
//
// The camera, video and UI clipping hooks are David Petersson's (Table-Tennis-Rockstar-PC, MIT,
// v1.1.0); the picture modes, narrower screens and the live letterbox switch are this port's.

#include "generated/default/tabletennis_init.h"
#include "pc_features.h"
#include "screen_layout.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

namespace tabletennis::pc {
namespace {

std::atomic<double> g_window_aspect{layout::kBaseAspect};
std::atomic<int> g_picture{int(Picture::kAuto)};
// False in menus, loading and pause; true only while a match is being played (set every match
// frame by the sub_82160698 native edit, cleared by ObserveFrontend when a menu is input-ready).
// The wider FILL picture is used only during a match; menus keep their 16:9 proportions.
std::atomic<bool> g_gameplay{false};
std::atomic<uint32_t> g_device{0};
std::mutex g_letterbox_mutex;
int g_letterbox_applied = -1;  // under g_letterbox_mutex

// Whether the PICTURE setting wants to fill this window shape, menus aside.
bool WantsFill() {
  const double window = g_window_aspect.load();
  switch (Picture(g_picture.load())) {
    case Picture::kLetterbox:
      return false;
    case Picture::kFill:
      return std::abs(window - layout::kBaseAspect) > 0.01;
    case Picture::kAuto:
    default:
      return window > layout::kBaseAspect + 0.01;
  }
}

// Whether the picture fills the window right now: only while a match is being played. Menus,
// loading and pause keep 16:9 (pillarboxed on a wider screen) so their artwork is undistorted.
bool Fills() { return WantsFill() && g_gameplay.load(); }

double PictureAspect() { return Fills() ? g_window_aspect.load() : layout::kBaseAspect; }

// The presenter letterboxes the 16:9 picture unless it fills the window; it reads the setting
// on every paint, so this applies at once.
void UpdateLetterbox() {
  const int letterbox = Fills() ? 0 : 1;
  std::lock_guard lock(g_letterbox_mutex);
  if (letterbox == g_letterbox_applied) return;
  g_letterbox_applied = letterbox;
  rex::cvar::SetFlagByName("present_letterbox", letterbox ? "true" : "false");
  REXLOG_INFO("Tabletennis: picture {} (window aspect {:.4f})", letterbox ? "letterboxed" : "fills the window",
              g_window_aspect.load());
}

struct Clip {
  uint32_t device;
  int left, top, right, bottom;
  std::array<int, 4> old;
};
thread_local std::vector<Clip> g_clips;

void EmitScissor(const PPCContext& ctx, uint8_t* base, uint32_t device, const std::array<int, 4>& rect) {
  PPCContext call = ctx;
  call.r3.u64 = device;
  call.r4.s64 = rect[0];
  call.r5.s64 = rect[1];
  call.r6.s64 = rect[2];
  call.r7.s64 = rect[3];
  sub_823537B0(call, base);
}

void StoreFloat(uint8_t* base, uint32_t address, float value) {
  PPCRegister data{};
  data.f32 = value;
  REX_STORE_U32(address, data.u32);
}

}  // namespace

void SetPicture(const std::string& name) {
  const Picture picture = name == "letterbox" ? Picture::kLetterbox
                          : name == "fill"    ? Picture::kFill
                                              : Picture::kAuto;
  g_picture.store(int(picture));
  UpdateLetterbox();
}

void SetSurfaceSize(uint32_t width, uint32_t height) {
  if (!width || !height) return;  // keep the last valid shape while minimized
  g_window_aspect.store(double(width) / double(height));
  UpdateLetterbox();
}

void SetGameplay(bool active) {
  if (g_gameplay.exchange(active) == active) return;
  UpdateLetterbox();
  REXLOG_INFO("Tabletennis: picture context {} (aspect {:.4f})", active ? "gameplay" : "menu",
              PictureAspect());
}

double OverlayAspect() {
  const double aspect = PictureAspect();
  return aspect < layout::kBaseAspect ? aspect : layout::kBaseAspect;
}

double CameraAspect(double original) { return layout::Camera(original, PictureAspect()); }

// Movie quads keep the video's own proportions on a wider or narrower picture.
double VideoAspect(double original) { return original * (PictureAspect() / layout::kBaseAspect); }

// Before the game draws a 2D layer: on a picture wider than 16:9, squeeze the layer back to 16:9
// in the middle and clip drawing to that area, so artwork meant to be off screen stays hidden.
void FixUi(PPCContext& ctx, uint8_t* base, uint32_t matrix, double width) {
  const float scale = float(layout::UiScale(PictureAspect()));
  const uint32_t device = g_device.load();
  if (scale > 0.9999f || width <= 0 || !device) {
    g_clips.push_back({});
    return;
  }
  const uint32_t top_left = REX_LOAD_U32(device + 11524), bottom_right = REX_LOAD_U32(device + 11528);
  const int x = int(REX_LOAD_U32(device + 12808)), y = int(REX_LOAD_U32(device + 12812));
  const int w = int(REX_LOAD_U32(device + 12816)), h = int(REX_LOAD_U32(device + 12820));
  const int inset = int(w * (1.0f - scale) * 0.5f + 0.5f);
  g_clips.push_back({device, x + inset, y, x + w - inset, y + h,
                     {int(top_left & 0x7fff), int((top_left >> 16) & 0x7fff), int(bottom_right & 0x7fff),
                      int((bottom_right >> 16) & 0x7fff)}});
  EmitScissor(ctx, base, device, g_clips.back().old);
  StoreFloat(base, matrix + 24, scale);
  StoreFloat(base, matrix + 40, float(width * (1.0 - scale) * 0.5));
}

// After the layer: restore the scissor rectangle it started with.
void EndUi(PPCContext& ctx, uint8_t* base) {
  if (g_clips.empty()) return;
  const Clip clip = g_clips.back();
  g_clips.pop_back();
  if (clip.device) EmitScissor(ctx, base, clip.device, clip.old);
}

// Every scissor the game sets: remember the device, and keep it inside the 16:9 area while a
// squeezed layer is drawing.
void Scissor(PPCContext& ctx) {
  g_device.store(ctx.r3.u32);
  if (g_clips.empty() || !g_clips.back().device) return;
  const Clip& c = g_clips.back();
  const layout::Rect rect = layout::Intersect({ctx.r4.s32, ctx.r5.s32, ctx.r6.s32, ctx.r7.s32},
                                              {c.left, c.top, c.right, c.bottom});
  ctx.r4.s64 = rect.left;
  ctx.r5.s64 = rect.top;
  ctx.r6.s64 = rect.right;
  ctx.r7.s64 = rect.bottom;
}

}  // namespace tabletennis::pc
