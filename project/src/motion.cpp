// Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
#include "motion.h"
#include "haptics.h"
#include "time_now.h"

#include <SDL3/SDL.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

REXCVAR_DEFINE_BOOL(motion_swing, false, "Input",
                    "Play shots with a flick of a controller that has a gyro (MOTION SWING)");
REXCVAR_DEFINE_INT32(motion_swing_sensitivity, 3, "Input",
                     "How light a flick plays a shot: 1 (strong flicks only) to 5 (light flicks)");
REXCVAR_DEFINE_BOOL(motion_swing_invert, false, "Input",
                    "Swap forward and back (topspin and backspin) for MOTION SWING");

namespace tabletennis::motion {
namespace {

constexpr uint16_t kButtonA = 0x1000, kButtonB = 0x2000, kButtonX = 0x4000, kButtonY = 0x8000;
// Angular speed (rad/s) a flick must reach, by sensitivity 1..5. A relaxed wrist flick peaks
// around 6-10 rad/s; turning the controller to look at it stays under 2.
constexpr float kThreshold[5] = {9.0f, 7.0f, 5.5f, 4.2f, 3.2f};
constexpr float kDominance = 1.6f;      // the main axis must beat the other by this factor
constexpr int64_t kHoldNs = 110'000'000;      // the shot button stays down this long
constexpr int64_t kCooldownNs = 350'000'000;  // no second shot from the same flick

std::atomic<bool> g_gyro_available{false};
SDL_Gamepad* g_enabled_pad = nullptr;  // pad whose gyro is switched on (input thread only)
int64_t g_hold_until = 0, g_ready_at = 0;
uint16_t g_held_button = 0;

// Test hook: TT_TEST_MOTION=topspin|backspin|leftspin|rightspin plays that flick every 3 s
// during a match, through the same detector, as a synthetic gyro sample.
const char* TestFlick() {
  static const char* value = std::getenv("TT_TEST_MOTION");
  return value && *value ? value : nullptr;
}

bool ReadGyro(float rate[3]) {
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  SDL_Gamepad* pad = (ids && count > 0) ? SDL_GetGamepadFromID(ids[0]) : nullptr;
  SDL_free(ids);
  if (!pad || !SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) {
    g_gyro_available.store(false);
    return false;
  }
  g_gyro_available.store(true);
  if (pad != g_enabled_pad) {
    SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
    g_enabled_pad = pad;
    REXLOG_INFO("Tabletennis: motion swing uses the gyro of \"{}\"", SDL_GetGamepadName(pad));
  }
  return SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, rate, 3);
}

bool SyntheticGyro(const char* flick, int64_t now, float rate[3]) {
  // A 100 ms flick at 8 rad/s every 3 s.
  if ((now / 1'000'000) % 3000 >= 100) {
    rate[0] = rate[1] = rate[2] = 0.0f;
    return true;
  }
  rate[0] = rate[1] = rate[2] = 0.0f;
  if (std::strcmp(flick, "topspin") == 0) rate[0] = -8.0f;
  if (std::strcmp(flick, "backspin") == 0) rate[0] = 8.0f;
  if (std::strcmp(flick, "leftspin") == 0) rate[1] = 8.0f;
  if (std::strcmp(flick, "rightspin") == 0) rate[1] = -8.0f;
  return true;
}

// SDL gyro axes for a gamepad held in front of you: x points right (pitch), y up (yaw), z toward
// the player (roll); positive is counter-clockwise looking down the axis. So pitch > 0 tips the
// top toward the player (a chop: backspin) and yaw > 0 turns it left.
uint16_t Classify(const float rate[3]) {
  const int sensitivity = std::clamp(int(REXCVAR_GET(motion_swing_sensitivity)), 1, 5);
  const float threshold = kThreshold[sensitivity - 1];
  float pitch = rate[0];
  const float yaw = rate[1];
  if (REXCVAR_GET(motion_swing_invert)) pitch = -pitch;
  const float ap = std::fabs(pitch), ay = std::fabs(yaw);
  if (ap >= threshold && ap >= kDominance * ay) return pitch > 0 ? kButtonY : kButtonA;
  if (ay >= threshold && ay >= kDominance * ap) return yaw > 0 ? kButtonX : kButtonB;
  return 0;
}

}  // namespace

bool GyroAvailable() { return g_gyro_available.load(); }

uint16_t AddSwingButtons(uint32_t user, uint16_t buttons) {
  const char* test = TestFlick();
  if (user != 0 || (!REXCVAR_GET(motion_swing) && !test)) return buttons;
  const int64_t now = NowNs();
  float rate[3];
  const bool have = test ? SyntheticGyro(test, now, rate) : ReadGyro(rate);
  // Only shots: outside a rally a flick must not press A or B in a menu.
  if (!have || !haptics::MatchInPlay()) {
    g_hold_until = 0;
    return buttons;
  }
  if (now >= g_ready_at) {
    if (const uint16_t button = Classify(rate)) {
      g_held_button = button;
      g_hold_until = now + kHoldNs;
      g_ready_at = now + kCooldownNs;
      static std::atomic<int> logged{0};
      if (logged.fetch_add(1) < 8) {
        REXLOG_INFO("Tabletennis: motion swing flick -> button {:04X} (pitch {:.1f}, yaw {:.1f})",
                    button, rate[0], rate[1]);
      }
    }
  }
  if (now < g_hold_until) buttons |= g_held_button;
  return buttons;
}

}  // namespace tabletennis::motion
