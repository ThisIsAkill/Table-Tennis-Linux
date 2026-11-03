// frame_pacer.h - frame pacing and frame-time statistics.
//
// Adapted from David Petersson's Table-Tennis-Rockstar-PC (MIT, PR #6 and v1.1.2): the frame is
// held to an absolute schedule before it is swapped and kicked to the GPU (native edit in
// sub_8235B750, the game's D3D Swap), so frames reach the screen at fixed intervals however long
// the game's CPU work took. A late frame goes at once and the schedule restarts from it, with no
// extra wait after a stall and no burst of short frames to catch up. The DXGI present hook and the
// Windows timer setup of the original are not needed here.

#pragma once

#include <cstddef>
#include <cstdint>

namespace tabletennis::pacing {

// Called on the guest render thread at the start of the game's Swap function, before the frame
// goes to the GPU: waits for the frame's deadline at the framerate setting (0 = unlimited).
void BeforeSwap();

// Called when a frame reaches the GPU (VdSwap): records the interval for the statistics and logs
// a summary every five seconds.
void RecordGameFrame();

// Called by the game's wait for the GPU's write-back value (native edit in sub_82359DC0) before
// each poll: on the Xbox its delay loop paused the hardware thread, on a PC it spins, so this
// sleeps briefly instead (gpu_wait_pause_us; 0 spins as before).
void GpuWaitPause();

struct Stats {
  float fps = 0.0f;     // over the last ~2 seconds
  float avg_ms = 0.0f;  // mean frame interval
  float p99_ms = 0.0f;  // 99th percentile interval (the "1% low")
  float max_ms = 0.0f;  // worst interval
};
Stats GetStats();

}  // namespace tabletennis::pacing
