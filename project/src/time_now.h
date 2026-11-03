// time_now.h - a monotonic timestamp in nanoseconds, shared by the game-side modules that pace or
// time-stamp their own work (frame pacing, haptics, the settings-page input).

#pragma once

#include <chrono>
#include <cstdint>

namespace tabletennis {

inline int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace tabletennis
