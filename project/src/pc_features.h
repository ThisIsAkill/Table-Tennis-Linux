// Copyright (c) 2026 Akhil Moola (ThisIsAkill). Portions copyright (c) 2026 davidptrssvea.
// MIT License, see LICENSE.
// pc_features.h - the picture on screens that are not 16:9 (PICTURE in PC SETTINGS).
//
// Adapted from David Petersson's Table-Tennis-Rockstar-PC (MIT), v1.1.0. The native edits in
// project/tools/native_edits.json call CameraAspect, VideoAspect, FixUi, EndUi and Scissor from
// the generated code; SetSurfaceSize follows the window, so a change applies at once.
//
// Picture modes: kAuto fills screens wider than 16:9 and letterboxes narrower ones, kLetterbox
// keeps the original 16:9 picture with bars, kFill always fills (wider: the camera widens and the
// menus stay 16:9 in the middle; narrower: the camera shows more at the top and bottom and the
// menus stretch slightly).

#pragma once

#include <cstdint>
#include <string>

struct PPCContext;

namespace tabletennis::pc {

enum class Picture { kAuto, kLetterbox, kFill };

// "auto", "letterbox" or "fill" (anything else is auto). Thread-safe.
void SetPicture(const std::string& name);
// The window's drawable size in pixels. Thread-safe; ignores a zero size (minimized).
void SetSurfaceSize(uint32_t width, uint32_t height);
// A match is being played (the wider FILL picture is used only then; menus stay 16:9).
void SetGameplay(bool active);
// True while a match is being played, between points included (set by SetGameplay).
bool InGameplay();

// Aspect ratio of the picture the game draws into, for overlays laid out inside it: 16:9 unless
// FILL stretches it to a narrower window.
double OverlayAspect();

// Called from the generated code (native edits).
double CameraAspect(double original);
double VideoAspect(double original);
void FixUi(PPCContext& ctx, uint8_t* base, uint32_t matrix, double width);
void EndUi(PPCContext& ctx, uint8_t* base);
void Scissor(PPCContext& ctx);

}  // namespace tabletennis::pc
