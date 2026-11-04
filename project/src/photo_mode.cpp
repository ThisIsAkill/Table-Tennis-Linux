#include "generated/default/tabletennis_init.h"
#include "photo_mode.h"
#include "achievement_ui.h"
#include "haptics.h"

#include <imgui.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xsocket.h>
#include <rex/ui/presenter.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The game's camera hook: every frame the current camera matrix (r4: right, up, back and
// position rows of four floats; y is up, the camera looks along -back) passes through here on its
// way to the renderer. Photo mode hands it a matrix of its own instead.
extern "C" void __imp__sub_821B5A68(PPCContext& ctx, uint8_t* base);
// The camera's per-frame update, which writes that matrix.
extern "C" void __imp__sub_821709F8(PPCContext& ctx, uint8_t* base);

namespace tabletennis::photo {
namespace {

// The game's timer: +80 is the time warp the focus shots' slow motion uses (1 = normal speed).
constexpr uint32_t kTimeWarp = 0x825CAEB8 + 80;
// rage::sysParam "nohuddraw" (name at 0x825D0704, value pointer at +4): any value hides the HUD.
constexpr uint32_t kNoHudValue = 0x825D0708;
constexpr uint32_t kNoHudName = 0x8205B0F8;

constexpr uint16_t kL3 = 0x0040, kR3 = 0x0080, kLB = 0x0100, kRB = 0x0200;
constexpr uint16_t kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;
constexpr float kMoveSpeed = 2.5f;     // m/s
constexpr float kLookSpeed = 1.8f;     // rad/s
// Bounds that keep the free camera inside the hall (some venues have more courts around).
constexpr float kMaxDistance = 15.0f;  // horizontally from the table
constexpr float kMaxHeight = 8.0f;

using Clock = std::chrono::steady_clock;

struct Vec3 {
  float x, y, z;
};

std::mutex g_mutex;
std::function<rex::ui::Presenter*()> g_presenter;
std::atomic<bool> g_active{false};
std::atomic<bool> g_toggle_requested{false};
std::atomic<bool> g_help{true};
std::atomic<bool> g_reset_requested{false};
std::atomic<bool> g_save_requested{false};
// Pad input for the camera (normalised), written by FilterInput, read by the camera hook.
std::atomic<float> g_move_x{0}, g_move_z{0}, g_look_x{0}, g_look_y{0}, g_rise{0};
std::atomic<float> g_boost{1};
std::atomic<float> g_mouse_dx{0}, g_mouse_dy{0};  // from the overlay (UI thread)
// Messages for the overlay.
std::string g_notice;
Clock::time_point g_notice_until{};

// Camera state (game thread).
Vec3 g_pos{}, g_start_pos{};
float g_yaw = 0, g_pitch = 0, g_start_yaw = 0, g_start_pitch = 0;
bool g_camera_ready = false;
Clock::time_point g_last_frame{};
uint32_t g_scratch = 0;
uint32_t g_matrix = 0;  // the camera data matrix (guest address)
uint32_t g_saved_warp = 0, g_saved_nohud = 0;
bool g_applied = false;  // freeze + HUD applied in guest memory

void Notice(const std::string& text) {
  std::lock_guard lock(g_mutex);
  g_notice = text;
  g_notice_until = Clock::now() + std::chrono::seconds(3);
}

float LoadF(uint8_t* base, uint32_t address) {
  const uint32_t bits = REX_LOAD_U32(address);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
void StoreF(uint8_t* base, uint32_t address, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  REX_STORE_U32(address, bits);
}

bool InNetworkSession() {
  uint8_t kid[8];
  return rex::system::net::CurrentSessionKey(kid);
}

float Stick(int16_t v) {
  const float f = std::clamp(float(v) / 32767.0f, -1.0f, 1.0f);
  const float dead = 0.18f;
  if (std::fabs(f) < dead) return 0.0f;
  return (f - std::copysign(dead, f)) / (1.0f - dead);
}

// --- PNG (stored deflate blocks: no compression library needed) -------------------------------
uint32_t Crc32(const uint8_t* data, size_t size) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[n] = c;
    }
    return t;
  }();
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void PutBe32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 3; i >= 0; --i) out.push_back(uint8_t(v >> (8 * i)));
}

void Chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data) {
  PutBe32(png, uint32_t(data.size()));
  const size_t start = png.size();
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), data.begin(), data.end());
  PutBe32(png, Crc32(png.data() + start, png.size() - start));
}

bool WritePng(const std::filesystem::path& path, const rex::ui::RawImage& image) {
  std::vector<uint8_t> raw;  // filter byte + RGB per row
  raw.reserve(size_t(image.height) * (size_t(image.width) * 3 + 1));
  for (uint32_t y = 0; y < image.height; ++y) {
    raw.push_back(0);
    const uint8_t* row = image.data.data() + size_t(y) * image.stride;
    for (uint32_t x = 0; x < image.width; ++x) raw.insert(raw.end(), row + x * 4, row + x * 4 + 3);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  size_t at = 0;
  while (true) {
    const size_t n = std::min<size_t>(65535, raw.size() - at);
    const bool last = at + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + ptrdiff_t(at), raw.begin() + ptrdiff_t(at + n));
    at += n;
    if (last) break;
  }
  uint32_t a = 1, b = 0;  // Adler-32
  for (uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  PutBe32(z, (b << 16) | a);
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> header;
  PutBe32(header, image.width);
  PutBe32(header, image.height);
  header.insert(header.end(), {8, 2, 0, 0, 0});  // 8-bit RGB
  Chunk(png, "IHDR", header);
  Chunk(png, "IDAT", z);
  Chunk(png, "IEND", {});
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
  return bool(file);
}

void SavePicture() {
  rex::ui::Presenter* presenter = g_presenter ? g_presenter() : nullptr;
  if (!presenter) {
    Notice("CANNOT SAVE PICTURES HERE");
    return;
  }
  std::thread([presenter] {
    rex::ui::RawImage image;
    if (!presenter->CaptureGuestOutput(image) || !image.width) {
      Notice("COULD NOT CAPTURE THE PICTURE");
      return;
    }
    char name[64];
    const std::time_t now = std::time(nullptr);
    std::strftime(name, sizeof(name), "tabletennis-%Y%m%d-%H%M%S.png", std::localtime(&now));
    const auto path = rex::filesystem::GetExecutableFolder() / "photos" / name;
    if (WritePng(path, image)) {
      REXLOG_INFO("Tabletennis: photo saved to {} ({}x{})", path.string(), image.width,
                  image.height);
      Notice("SAVED photos/" + std::string(name) + " (" + std::to_string(image.width) + "x" +
             std::to_string(image.height) + ")");
    } else {
      Notice("COULD NOT SAVE THE PICTURE");
    }
  }).detach();
}

// Enter/leave (game thread, from the camera hook): freeze time, hide the HUD, and back.
void Apply(uint8_t* base, bool on) {
  if (on == g_applied) return;
  if (on) {
    g_saved_warp = REX_LOAD_U32(kTimeWarp);
    g_saved_nohud = REX_LOAD_U32(kNoHudValue);
  } else {
    REX_STORE_U32(kTimeWarp, g_saved_warp);
    REX_STORE_U32(kNoHudValue, g_saved_nohud);
    g_camera_ready = false;
  }
  g_applied = on;
}

void BuildMatrix(uint8_t* base, uint32_t out, uint32_t original) {
  std::memcpy(base + out, base + original, 64);  // keeps the w components
  const float cp = std::cos(g_pitch), sp = std::sin(g_pitch);
  const Vec3 look{cp * std::sin(g_yaw), sp, cp * std::cos(g_yaw)};
  const Vec3 back{-look.x, -look.y, -look.z};
  // right = world up x back, up = back x right (the game's right-handed rows).
  const float rl = std::sqrt(back.z * back.z + back.x * back.x);
  const Vec3 right{back.z / rl, 0.0f, -back.x / rl};
  const Vec3 up{back.y * right.z - back.z * right.y, back.z * right.x - back.x * right.z,
                back.x * right.y - back.y * right.x};
  const Vec3 rows[4] = {right, up, back, g_pos};
  for (int r = 0; r < 4; ++r) {
    StoreF(base, out + r * 16 + 0, rows[r].x);
    StoreF(base, out + r * 16 + 4, rows[r].y);
    StoreF(base, out + r * 16 + 8, rows[r].z);
  }
}

// Test hook: TT_TEST_PHOTO=<amount> turns photo mode on 4 s into the first rally, backs the
// camera away, up and to the side for 2 s (amount 1 = full stick), saves a picture and leaves.
void TestScript() {
  static const bool enabled = std::getenv("TT_TEST_PHOTO") != nullptr;
  if (!enabled) return;
  static Clock::time_point first_rally{};
  static int step = 0;
  const auto now = Clock::now();
  if (step == 0 && tabletennis::haptics::MatchInPlay()) {
    first_rally = now;
    step = 1;
  }
  const float t = std::chrono::duration<float>(now - first_rally).count();
  if (step == 1 && t > 4.0f) {
    RequestToggle();
    step = 2;
  } else if (step == 2 && t > 5.0f && t < 7.0f) {
    const float amount = float(std::atof(std::getenv("TT_TEST_PHOTO")));  // 1 = full
    g_move_z.store(-amount);  // back away
    g_rise.store(0.6f * amount);
    g_look_x.store(0.3f * amount);
  } else if (step == 2 && t >= 7.0f) {
    g_move_z.store(0.0f);
    g_rise.store(0.0f);
    g_look_x.store(0.0f);
    g_save_requested.store(true);
    step = 3;
  } else if (step == 3 && t > 10.0f) {
    RequestToggle();
    step = 4;
  }
}

}  // namespace

void Init(std::function<rex::ui::Presenter*()> presenter) { g_presenter = std::move(presenter); }

void RequestToggle() { g_toggle_requested.store(true); }

bool Active() { return g_active.load(); }

bool FilterInput(uint32_t user, achievement_ui::PadSample& pad) {
  if (user != 0) return false;
  static uint16_t last = 0;
  const uint16_t pressed = pad.buttons & ~last;
  last = pad.buttons;
  const bool sticks_clicked =
      (pad.buttons & (kL3 | kR3)) == (kL3 | kR3) && (pressed & (kL3 | kR3));
  if (sticks_clicked) RequestToggle();
  if (!g_active.load()) return false;
  if (pressed & kB) RequestToggle();
  if (pressed & kA) g_help.store(!g_help.load());
  if (pressed & kY) g_reset_requested.store(true);
  if (pressed & kX) g_save_requested.store(true);
  if (!std::getenv("TT_TEST_PHOTO")) {
    g_move_x.store(Stick(pad.thumb_lx));
    g_move_z.store(Stick(pad.thumb_ly));
    g_look_x.store(Stick(pad.thumb_rx));
    g_look_y.store(Stick(pad.thumb_ry));
    g_rise.store((float(pad.right_trigger) - float(pad.left_trigger)) / 255.0f);
  }
  g_boost.store((pad.buttons & kRB) ? 3.0f : (pad.buttons & kLB) ? 0.3f : 1.0f);
  // The frozen game sees an idle pad.
  pad = achievement_ui::PadSample{};
  return true;
}

Overlay::Overlay(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}
Overlay::~Overlay() = default;

void Overlay::OnDraw(ImGuiIO& io) {
  std::string notice;
  {
    std::lock_guard lock(g_mutex);
    if (Clock::now() < g_notice_until) notice = g_notice;
  }
  const bool active = g_active.load();
  if (!active && notice.empty()) return;
  if (active && (io.MouseDown[0] || io.MouseDown[1])) {
    g_mouse_dx.store(g_mouse_dx.load() + io.MouseDelta.x);
    g_mouse_dy.store(g_mouse_dy.load() + io.MouseDelta.y);
  }
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float s = io.DisplaySize.y / 720.0f;
  const float x0 = 28.0f * s;
  float y = io.DisplaySize.y - 30.0f * s;
  ImFont* body = achievement_ui::BodyFont();
  const float body_px = 18.0f * s;
  auto line = [&](const std::string& text, ImU32 col) {
    y -= body_px * 1.35f;
    dl->AddText(body, body_px, ImVec2(x0 + 1.5f * s, y + 1.5f * s), IM_COL32(0, 0, 0, 200),
                text.c_str());
    dl->AddText(body, body_px, ImVec2(x0, y), col, text.c_str());
  };
  if (!notice.empty()) line(notice, IM_COL32(255, 204, 0, 255));
  if (active && g_help.load()) {
    const ImU32 col = IM_COL32(235, 237, 240, 255);
    line("A hide this help    B (or both sticks) leave", col);
    line("X save picture    Y reset view", col);
    line("LT / RT down and up    LB slow, RB fast    mouse drag: look", col);
    line("Left stick move    Right stick look", col);
    const float title_px = 30.0f * s;
    ImFont* title = achievement_ui::HeadlineFont(title_px);
    y -= title_px * 1.25f;
    dl->AddText(title, title_px, ImVec2(x0 + 2.0f * s, y + 2.0f * s), IM_COL32(0, 0, 0, 220),
                "PHOTO MODE");
    dl->AddText(title, title_px, ImVec2(x0, y), IM_COL32(255, 204, 0, 255), "PHOTO MODE");
  }
}

}  // namespace tabletennis::photo

using namespace tabletennis::photo;

extern "C" void sub_821B5A68(PPCContext& ctx, uint8_t* base) {
  const uint32_t matrix = ctx.r4.u32;
  TestScript();
  if (g_toggle_requested.exchange(false)) {
    if (g_active.load()) {
      g_active.store(false);
      Notice("PHOTO MODE OFF");
      REXLOG_INFO("Tabletennis: photo mode off");
    } else if (InNetworkSession()) {
      Notice("PHOTO MODE IS NOT AVAILABLE IN NETWORK MATCHES");
    } else if (!tabletennis::haptics::MatchInPlay()) {
      Notice("PHOTO MODE WORKS DURING A MATCH");
    } else {
      g_active.store(true);
      g_help.store(true);
      REXLOG_INFO("Tabletennis: photo mode on");
    }
  }
  const bool active = g_active.load();
  Apply(base, active);
  if (!active || !matrix) {
    __imp__sub_821B5A68(ctx, base);
    return;
  }
  // Keep the action frozen: the game writes the time warp for slow motion, so every frame.
  StoreF(base, kTimeWarp, 0.0001f);
  REX_STORE_U32(kNoHudValue, kNoHudName);
  if (!g_scratch) g_scratch = rex::system::kernel_memory()->SystemHeapAlloc(64, 16);
  if (!g_scratch) {
    __imp__sub_821B5A68(ctx, base);
    return;
  }
  const auto now = Clock::now();
  if (!g_camera_ready) {
    // Start from the game's camera (the producer hook only takes over once this is set).
    g_start_pos = {LoadF(base, matrix + 48), LoadF(base, matrix + 52), LoadF(base, matrix + 56)};
    const Vec3 back{LoadF(base, matrix + 32), LoadF(base, matrix + 36), LoadF(base, matrix + 40)};
    g_start_pitch = std::asin(std::clamp(-back.y, -1.0f, 1.0f));
    g_start_yaw = std::atan2(-back.x, -back.z);
    g_pos = g_start_pos;
    g_yaw = g_start_yaw;
    g_pitch = g_start_pitch;
    g_last_frame = now;
    g_matrix = matrix;
    g_camera_ready = true;
  }
  if (g_save_requested.exchange(false)) SavePicture();
  __imp__sub_821B5A68(ctx, base);
}

// The camera's update (camera object in r3), which writes the matrix the frame renders with.
// While photo mode is on, the free camera's matrix replaces it right after.
extern "C" void sub_821709F8(PPCContext& ctx, uint8_t* base) {
  __imp__sub_821709F8(ctx, base);
  if (!g_active.load() || !g_camera_ready || !g_matrix || !g_scratch) return;
  const auto now = Clock::now();
  if (g_reset_requested.exchange(false)) {
    g_pos = g_start_pos;
    g_yaw = g_start_yaw;
    g_pitch = g_start_pitch;
  }
  const float dt = std::min(0.1f, std::chrono::duration<float>(now - g_last_frame).count());
  g_last_frame = now;
  // Look: right stick and mouse drag.
  g_yaw -= g_look_x.load() * kLookSpeed * dt + g_mouse_dx.exchange(0.0f) * 0.004f;
  g_pitch += g_look_y.load() * kLookSpeed * dt - g_mouse_dy.exchange(0.0f) * 0.004f;
  g_pitch = std::clamp(g_pitch, -1.45f, 1.45f);
  // Move: left stick on the ground plane along the view, triggers up and down.
  const float speed = kMoveSpeed * g_boost.load() * dt;
  const float fx = std::sin(g_yaw), fz = std::cos(g_yaw);
  const float mx = g_move_x.load(), mz = g_move_z.load();
  g_pos.x += (fx * mz - fz * mx) * speed;
  g_pos.z += (fz * mz + fx * mx) * speed;
  g_pos.y = std::clamp(g_pos.y + g_rise.load() * speed, 0.1f, kMaxHeight);
  const float distance = std::sqrt(g_pos.x * g_pos.x + g_pos.z * g_pos.z);
  if (distance > kMaxDistance) {
    g_pos.x *= kMaxDistance / distance;
    g_pos.z *= kMaxDistance / distance;
  }
  BuildMatrix(base, g_scratch, g_matrix);
  std::memcpy(base + g_matrix, base + g_scratch, 64);
}
