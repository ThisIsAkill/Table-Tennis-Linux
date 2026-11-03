// screen_layout.h - picture geometry for screens that are not 16:9.
//
// Adapted from David Petersson's Table-Tennis-Rockstar-PC (MIT), v1.1.0, where the 3D camera
// follows the window's shape and menus keep their proportions in a centered 16:9 area. This
// version also handles screens narrower than 16:9 (the Steam Deck's 16:10) for FILL.

#pragma once

#include <algorithm>

namespace tabletennis::pc::layout {

constexpr double kBaseAspect = 16.0 / 9.0;

// Horizontal scale for the game's 2D layers so that they keep 16:9 proportions in a window of
// `aspect` (1 for 16:9 and narrower; the menus there stretch slightly with FILL).
inline double UiScale(double aspect) { return kBaseAspect / std::max(kBaseAspect, aspect); }

// The aspect value the game's camera should use for a picture of `aspect`. The game keeps its
// horizontal field of view, so wider screens show more at the sides and narrower ones more at
// the top and bottom. Values outside (1, 2) are not the widescreen camera and stay as they are.
inline double Camera(double original, double aspect) {
  return original > 1.0 && original < 2.0 ? original * (aspect / kBaseAspect) : original;
}

struct Rect {
  int left, top, right, bottom;
};

inline Rect Intersect(Rect a, Rect b) {
  a.left = std::clamp(a.left, b.left, b.right);
  a.top = std::clamp(a.top, b.top, b.bottom);
  a.right = std::clamp(a.right, a.left, b.right);
  a.bottom = std::clamp(a.bottom, a.top, b.bottom);
  return a;
}

}  // namespace tabletennis::pc::layout
