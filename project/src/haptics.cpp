#include "generated/default/tabletennis_init.h"
#include "haptics.h"
#include "intro_skip.h"

#include <SDL3/SDL.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

REXCVAR_DEFINE_BOOL(haptics, true, "Input",
                    "Controller pulse on every ball hit and a light tick when navigating menus");

extern "C" void __imp__sub_823F9318(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82567090(PPCContext& ctx, uint8_t* base);

namespace tabletennis::haptics {
namespace {
using Clock = std::chrono::steady_clock;

struct Pulse {
  uint16_t low;   // low-frequency motor
  uint16_t high;  // high-frequency motor
  uint32_t ms;
};
// Tunables. The Steam Deck renders mostly the low-frequency motor (the game's own rumble uses
// only that one), so every pulse drives it. Hit: strong and short; menu: a light tick.
constexpr Pulse kHitPulse{0xFFFF, 0xC000, 80};
constexpr Pulse kMenuPulse{0xFFFF, 0xFFFF, 55};
constexpr Pulse kSelectPulse{0xFFFF, 0xFFFF, 80};

std::mutex g_mutex;
Clock::time_point g_pulse_until{};
uint16_t g_game_low = 0, g_game_high = 0;  // the game's latest vibration request
std::atomic<int64_t> g_last_hud_ns{0};      // last update of the in-match HUD

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}

bool InMatch() {
  return NowNs() - g_last_hud_ns.load(std::memory_order_relaxed) < 500'000'000;
}

SDL_Gamepad* PlayerOnePad() {
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  SDL_Gamepad* pad = nullptr;
  if (ids && count > 0) pad = SDL_GetGamepadFromID(ids[0]);
  SDL_free(ids);
  return pad;
}

void Play(const Pulse& pulse, const char* what) {
  if (!REXCVAR_GET(haptics)) return;
  SDL_Gamepad* pad = PlayerOnePad();
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 6) {
    REXLOG_INFO("Tabletennis: haptics {} on \"{}\"", what, pad ? SDL_GetGamepadName(pad) : "no pad");
  }
  if (!pad) return;
  std::lock_guard lock(g_mutex);
  // Never weaken a stronger rumble the game is playing right now.
  const uint16_t low = std::max(pulse.low, g_game_low);
  const uint16_t high = std::max(pulse.high, g_game_high);
  SDL_RumbleGamepad(pad, low, high, pulse.ms);
  g_pulse_until = Clock::now() + std::chrono::milliseconds(pulse.ms);
}

constexpr uint16_t kDpad = 0x000F;  // XINPUT up/down/left/right
constexpr uint16_t kButtonA = 0x1000, kButtonB = 0x2000;
constexpr int16_t kStickOn = 20000, kStickOff = 12000;
struct PadState {
  uint16_t buttons = 0;
  int stick_x = 0, stick_y = 0;  // -1/0/+1 with hysteresis
};
PadState g_pads[4];
}  // namespace

bool MatchInPlay() { return InMatch(); }

void OnPadSample(uint32_t user, uint16_t buttons, int16_t thumb_lx, int16_t thumb_ly) {
  if (user >= 4) return;
  PadState& state = g_pads[user];
  auto axis = [](int16_t v, int previous) {
    if (v > kStickOn) return 1;
    if (v < -kStickOn) return -1;
    if (previous != 0 && std::abs(int(v)) > kStickOff) return previous;
    return 0;
  };
  const int x = axis(thumb_lx, state.stick_x), y = axis(thumb_ly, state.stick_y);
  const uint16_t pressed = buttons & ~state.buttons;
  const bool stick_moved = (x != 0 && x != state.stick_x) || (y != 0 && y != state.stick_y);
  state.stick_x = x;
  state.stick_y = y;
  state.buttons = buttons;
  if (user != 0 || InMatch() || !intro::MainMenuReached()) return;
  if (pressed & (kButtonA | kButtonB)) {
    Play(kSelectPulse, "select");
  } else if ((pressed & kDpad) || stick_moved) {
    Play(kMenuPulse, "menu");
  }
}

}  // namespace tabletennis::haptics

using namespace tabletennis::haptics;

// Flash variable writes: the HUD's rallyCount changes on every ball hit, and the serve meter
// is written every frame while a match is in play.
extern "C" void sub_823F9318(PPCContext& ctx, uint8_t* base) {
  if (const uint32_t name = ctx.r4.u32) {
    const char* text = reinterpret_cast<const char*>(base + name);
    if (std::strcmp(text, "rallyCount") == 0) {
      Play(kHitPulse, "hit");
    } else if (std::strcmp(text, "ServeMeterX") == 0) {
      g_last_hud_ns.store(NowNs(), std::memory_order_relaxed);
    }
  }
  __imp__sub_823F9318(ctx, base);
}

// The game's XInputSetState wrapper (r3 = user, r4 = XINPUT_VIBRATION). Remember its request,
// and while one of our pulses is running do not let a "motors off" request cut it short.
extern "C" void sub_82567090(PPCContext& ctx, uint8_t* base) {
  const uint32_t vibration = ctx.r4.u32;
  if (vibration) {
    const uint16_t low = REX_LOAD_U16(vibration + 0);
    const uint16_t high = REX_LOAD_U16(vibration + 2);
    std::lock_guard lock(g_mutex);
    g_game_low = low;
    g_game_high = high;
    if (low == 0 && high == 0 && Clock::now() < g_pulse_until) {
      ctx.r3.u64 = 0;  // ERROR_SUCCESS
      return;
    }
  }
  __imp__sub_82567090(ctx, base);
}
