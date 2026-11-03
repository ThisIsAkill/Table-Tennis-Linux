// frame_pacer.cpp - see frame_pacer.h.

#include "frame_pacer.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

REXCVAR_DEFINE_INT32(gpu_wait_pause_us, 100, "Video",
                     "Sleep before each poll of the GPU's write-back value in the game's GPU wait "
                     "(microseconds; 0 = spin like the original delay loop)");
REXCVAR_DECLARE(int32_t, framerate);
REXCVAR_DECLARE(bool, pace_before_swap);

namespace tabletennis::pacing {
namespace {

using Clock = std::chrono::steady_clock;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

// Absolute deadlines (upstream's frame_schedule.h).
class Schedule {
 public:
  int64_t Next(int64_t now, int32_t fps) {
    if (fps <= 0) {
      initialized_ = false;
      return now;
    }
    const int64_t period = 1'000'000'000LL / fps;
    if (!initialized_ || period != period_) {
      period_ = period;
      deadline_ = now;
      initialized_ = true;
    }
    deadline_ += period;
    // A frame that missed its deadline is already late: submit it now and restart the schedule
    // from here, so there is neither a full wait after a long stall nor a burst of short frames.
    if (deadline_ < now) deadline_ = now;
    return deadline_;
  }

 private:
  int64_t period_ = 0, deadline_ = 0;
  bool initialized_ = false;
};

// Sleep until shortly before the deadline, then spin: sleeps overshoot by up to a millisecond.
constexpr int64_t kSpinMarginNs = 600'000;

void WaitUntil(int64_t deadline_ns) {
  const int64_t coarse = deadline_ns - kSpinMarginNs - NowNs();
  if (coarse > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(coarse));
  while (NowNs() < deadline_ns) {
#if defined(_M_X64) || defined(__x86_64__)
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
  }
}

Schedule g_schedule;  // guest render thread only

// Frame intervals, written by the guest render thread.
class History {
 public:
  void Record(int64_t now_ns) {
    if (last_ns_) {
      std::lock_guard lock(mutex_);
      values_[next_ % values_.size()] = float(double(now_ns - last_ns_) / 1e6);
      ++next_;
    }
    last_ns_ = now_ns;
  }

  Stats Summarize(size_t frames) const {
    std::vector<float> v;
    {
      std::lock_guard lock(mutex_);
      const size_t count = std::min({frames, values_.size(), next_});
      for (size_t i = 0; i < count; ++i) v.push_back(values_[(next_ - count + i) % values_.size()]);
    }
    Stats stats;
    if (v.empty()) return stats;
    double sum = 0.0;
    for (float ms : v) {
      sum += ms;
      stats.max_ms = std::max(stats.max_ms, ms);
    }
    stats.avg_ms = float(sum / v.size());
    stats.fps = stats.avg_ms > 0.0f ? 1000.0f / stats.avg_ms : 0.0f;
    std::sort(v.begin(), v.end());
    stats.p99_ms = v[std::min(v.size() - 1, v.size() * 99 / 100)];
    return stats;
  }

 private:
  mutable std::mutex mutex_;
  std::array<float, 600> values_{};
  size_t next_ = 0;
  int64_t last_ns_ = 0;
};

History g_history;

}  // namespace

void BeforeSwap() {
  if (!REXCVAR_GET(pace_before_swap)) return;
  const int32_t fps = REXCVAR_GET(framerate);
  WaitUntil(g_schedule.Next(NowNs(), fps));
}

void RecordGameFrame() {
  const int64_t now = NowNs();
  g_history.Record(now);
  static int64_t last_log = now;
  if (now - last_log >= 5'000'000'000LL) {
    last_log = now;
    const Stats s = g_history.Summarize(300);
    REXLOG_INFO("Tabletennis: guest frames {:.1f}/s, interval avg {:.2f} ms, 1% {:.2f} ms, max {:.2f} ms",
                s.fps, s.avg_ms, s.p99_ms, s.max_ms);
  }
}

Stats GetStats() { return g_history.Summarize(120); }

void GpuWaitPause() {
  const int32_t us = REXCVAR_GET(gpu_wait_pause_us);
  if (us > 0) std::this_thread::sleep_for(std::chrono::microseconds(us));
}

}  // namespace tabletennis::pacing
