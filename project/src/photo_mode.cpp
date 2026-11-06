// Copyright (c) 2026 Akhil Moola (ThisIsAkill). Portions copyright (c) 2026 davidptrssvea.
// MIT License, see LICENSE.
#include "generated/default/tabletennis_init.h"
#include "photo_mode.h"
#include "achievement_ui.h"
#include "haptics.h"
#include "pc_features.h"

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
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The game's camera hook: every frame the current camera matrix (r4: right, up, back and
// position rows of four floats; y is up, the camera looks along -back) passes through here after
// the frame has taken its copy. Photo mode uses it for the toggle, the freeze and the effects.
extern "C" void __imp__sub_821B5A68(PPCContext& ctx, uint8_t* base);
// The camera's per-frame update, which writes that matrix. The free camera replaces it right after.
extern "C" void __imp__sub_821709F8(PPCContext& ctx, uint8_t* base);
// Registers a lighting preset's tunables (preset in r3); among them the post-processing values the
// pong_postfx shader reads every frame. Called once per preset while the game loads.
extern "C" void __imp__sub_821DAEF0(PPCContext& ctx, uint8_t* base);

namespace tabletennis::photo {
namespace {

// The game's timer: +80 is the time warp the focus shots' slow motion uses (1 = normal speed).
constexpr uint32_t kTimeWarp = 0x825CAEB8 + 80;
// rage::sysParam "nohuddraw" (name at 0x825D0704, value pointer at +4): any value hides the HUD.
constexpr uint32_t kNoHudValue = 0x825D0708;
constexpr uint32_t kNoHudName = 0x8205B0F8;

// Post-processing fields of a lighting preset (PostExposure, PostBloomIntensity,
// PostColorCorrect (rgb multiplier, 0.5 = neutral), PostdeSat (saturation), PostContrast).
constexpr uint32_t kPostExposure = 5764, kPostBloom = 5784, kPostColor = 5792;
constexpr uint32_t kPostSaturation = 5824, kPostContrast = 5860;

constexpr uint16_t kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008;
constexpr uint16_t kBack = 0x0020, kL3 = 0x0040, kR3 = 0x0080, kLB = 0x0100, kRB = 0x0200;
constexpr uint16_t kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;
constexpr float kMoveSpeed = 2.5f;  // m/s
constexpr float kLookSpeed = 1.8f;  // rad/s
// Bounds that keep the free camera inside the hall (some venues have more courts around).
constexpr float kMaxDistance = 15.0f;  // horizontally from the table
constexpr float kMaxHeight = 8.0f;

using Clock = std::chrono::steady_clock;

struct Vec3 {
  float x, y, z;
};

// --- Options -------------------------------------------------------------------------------------
enum OptionId {
  kFov, kTilt, kFilter, kExposure, kContrast, kSaturation, kTemperature, kBloom, kVignette,
  kFrame, kBorder, kOptionCount
};

struct Filter {
  const char* name;
  float saturation, contrast, exposure;
  float tint[3];
};
constexpr Filter kFilters[] = {
    {"NONE", 1.0f, 1.0f, 1.0f, {1.0f, 1.0f, 1.0f}},
    {"BLACK & WHITE", 0.0f, 1.1f, 1.0f, {1.0f, 1.0f, 1.0f}},
    // The game desaturates after tinting, so sepia keeps a little colour of a strong warm tint.
    {"SEPIA", 0.25f, 1.05f, 1.0f, {1.55f, 1.08f, 0.58f}},
    {"NOIR", 0.0f, 1.55f, 0.9f, {1.0f, 1.0f, 1.0f}},
    {"WARM", 1.05f, 1.0f, 1.0f, {1.14f, 1.0f, 0.84f}},
    {"COOL", 0.95f, 1.0f, 1.0f, {0.86f, 0.98f, 1.16f}},
    {"VIVID", 1.5f, 1.15f, 1.0f, {1.0f, 1.0f, 1.0f}},
    {"FADED", 0.55f, 0.8f, 1.12f, {1.04f, 1.0f, 0.96f}},
    {"MATCH NIGHT", 1.2f, 1.3f, 0.85f, {0.95f, 0.97f, 1.1f}},
};
constexpr int kFilterCount = int(sizeof(kFilters) / sizeof(kFilters[0]));

struct Frame {
  const char* name;
  float aspect;  // width / height; 0 = the picture as it is
};
constexpr Frame kFrames[] = {{"ORIGINAL", 0.0f}, {"2.39:1 CINEMA", 2.39f}, {"1.85:1", 1.85f},
                             {"16:9", 16.0f / 9.0f}, {"4:3", 4.0f / 3.0f}, {"1:1 SQUARE", 1.0f},
                             {"4:5 PORTRAIT", 0.8f}, {"9:16 VERTICAL", 9.0f / 16.0f}};
constexpr int kFrameCount = int(sizeof(kFrames) / sizeof(kFrames[0]));
constexpr const char* kBorders[] = {"NONE", "WHITE", "BLACK", "THIN WHITE"};
constexpr int kBorderCount = 4;

struct OptionInfo {
  const char* label;
  int min, max, initial;
};
constexpr OptionInfo kOptionInfo[kOptionCount] = {
    {"ZOOM", 5, 50, 10},  // tenths: 1.0X is the game's own view
    {"TILT", -45, 45, 0},
    {"FILTER", 0, kFilterCount - 1, 0},
    {"EXPOSURE", -10, 10, 0},
    {"CONTRAST", -10, 10, 0},
    {"SATURATION", -10, 10, 0},
    {"TEMPERATURE", -10, 10, 0},
    {"BLOOM", 0, 10, 3},
    {"VIGNETTE", 0, 10, 0},
    {"FRAME", 0, kFrameCount - 1, 0},
    {"BORDER", 0, kBorderCount - 1, 0},
};
constexpr int kOptionStep[kOptionCount] = {2, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1};

std::array<std::atomic<int>, kOptionCount> g_options;
std::atomic<int> g_selected{kFilter};
std::atomic<bool> g_options_ready{false};

void ResetOptions() {
  for (int i = 0; i < kOptionCount; ++i) g_options[i].store(kOptionInfo[i].initial);
  g_options_ready.store(true);
}

std::string OptionValue(int id) {
  const int v = g_options[id].load();
  switch (id) {
    case kFov:
      return std::to_string(v / 10) + "." + std::to_string(v % 10) + "X";
    case kTilt:
      return (v > 0 ? "+" : "") + std::to_string(v);
    case kFilter:
      return kFilters[std::clamp(v, 0, kFilterCount - 1)].name;
    case kFrame:
      return kFrames[std::clamp(v, 0, kFrameCount - 1)].name;
    case kBorder:
      return kBorders[std::clamp(v, 0, kBorderCount - 1)];
    case kBloom:
    case kVignette:
      return v == 0 ? "OFF" : std::to_string(v);
    default:
      return v == 0 ? "0" : (v > 0 ? "+" : "") + std::to_string(v);
  }
}

void ChangeOption(int id, int direction) {
  const OptionInfo& info = kOptionInfo[id];
  const int span = info.max - info.min + 1;
  int v = g_options[id].load() + direction * kOptionStep[id];
  if (id == kFov) {  // zoom steps of about 10%
    const int now = g_options[id].load();
    v = int(std::lround(direction > 0 ? now * 1.1f : now / 1.1f));
    if (v == now) v += direction;
  }
  if (id == kFilter || id == kFrame || id == kBorder) {
    v = info.min + ((v - info.min) % span + span) % span;  // lists wrap around
  } else {
    v = std::clamp(v, info.min, info.max);
  }
  g_options[id].store(v);
}

// --- Shared state --------------------------------------------------------------------------------
std::mutex g_mutex;
std::function<rex::ui::Presenter*()> g_presenter;
std::atomic<bool> g_active{false};
std::atomic<bool> g_toggle_requested{false};
std::atomic<bool> g_ui_visible{true};
std::atomic<bool> g_reset_requested{false};
std::atomic<bool> g_save_requested{false};
// Pad input for the camera (normalised), written by FilterInput, read by the camera hook.
std::atomic<float> g_move_x{0}, g_move_z{0}, g_look_x{0}, g_look_y{0}, g_rise{0};
std::atomic<float> g_boost{1};
std::atomic<float> g_mouse_dx{0}, g_mouse_dy{0};  // from the overlay (UI thread)
std::string g_notice;
Clock::time_point g_notice_until{};

// Lighting presets (game thread after loading) and their values when photo mode started.
std::vector<uint32_t> g_post_presets;
struct PostValues {
  float exposure, bloom, color[3], saturation, contrast;
};
std::vector<PostValues> g_post_saved;

// Camera state (game thread).
Vec3 g_pos{}, g_start_pos{};
float g_yaw = 0, g_pitch = 0, g_start_yaw = 0, g_start_pitch = 0;
bool g_camera_ready = false;
Clock::time_point g_last_frame{};
uint32_t g_scratch = 0;
uint32_t g_matrix = 0;  // the camera data matrix (guest address)
uint32_t g_saved_warp = 0, g_saved_nohud = 0;
bool g_applied = false;  // freeze, HUD and effects applied in guest memory

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

// --- Framing (the overlay previews it; the saved picture gets it for real) ----------------------
// Vignette darkening at a point, with x, y in -1..1 across the framed picture.
float Vignette(float x, float y, int strength) {
  if (strength <= 0) return 1.0f;
  const float r = std::sqrt((x * x + y * y) * 0.5f);  // 0 centre .. 1 corners
  const float t = std::clamp((r - 0.35f) / 0.65f, 0.0f, 1.0f);
  return 1.0f - float(strength) * 0.085f * t * t * (3.0f - 2.0f * t);
}

// The part of a w x h picture the chosen frame keeps (centred).
void FrameRect(int frame, float w, float h, float* x0, float* y0, float* fw, float* fh) {
  const float aspect = kFrames[std::clamp(frame, 0, kFrameCount - 1)].aspect;
  *fw = w;
  *fh = h;
  if (aspect > 0.0f) {
    if (w / h > aspect) {
      *fw = h * aspect;
    } else {
      *fh = w / aspect;
    }
  }
  *x0 = (w - *fw) * 0.5f;
  *y0 = (h - *fh) * 0.5f;
}

float BorderThickness(int border, float fh) {
  if (border == 0) return 0.0f;
  return fh * (border == 3 ? 0.012f : 0.035f);
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

// rgb: tightly packed rows of width * 3 bytes.
bool WritePng(const std::filesystem::path& path, uint32_t width, uint32_t height,
              const std::vector<uint8_t>& rgb) {
  std::vector<uint8_t> raw;  // filter byte + RGB per row
  raw.reserve(size_t(height) * (size_t(width) * 3 + 1));
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);
    const uint8_t* row = rgb.data() + size_t(y) * width * 3;
    raw.insert(raw.end(), row, row + size_t(width) * 3);
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
  PutBe32(header, width);
  PutBe32(header, height);
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

// Crops to the frame, darkens the edges and draws the border, as the overlay showed it.
std::vector<uint8_t> FramedPicture(const rex::ui::RawImage& image, uint32_t* out_w,
                                   uint32_t* out_h) {
  float x0, y0, fw, fh;
  FrameRect(g_options[kFrame].load(), float(image.width), float(image.height), &x0, &y0, &fw, &fh);
  const uint32_t w = std::max(1u, uint32_t(fw)), h = std::max(1u, uint32_t(fh));
  const uint32_t ox = uint32_t(x0), oy = uint32_t(y0);
  const int vignette = g_options[kVignette].load();
  const int border = g_options[kBorder].load();
  const uint32_t thick = uint32_t(BorderThickness(border, float(h)));
  const uint8_t border_value = border == 2 ? 0 : 245;
  std::vector<uint8_t> rgb(size_t(w) * h * 3);
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* src = image.data.data() + size_t(y + oy) * image.stride + size_t(ox) * 4;
    uint8_t* dst = rgb.data() + size_t(y) * w * 3;
    const float ny = (float(y) + 0.5f) / float(h) * 2.0f - 1.0f;
    for (uint32_t x = 0; x < w; ++x) {
      const bool in_border = thick && (x < thick || y < thick || x >= w - thick || y >= h - thick);
      if (in_border) {
        dst[x * 3 + 0] = dst[x * 3 + 1] = dst[x * 3 + 2] = border_value;
        continue;
      }
      const float k = Vignette((float(x) + 0.5f) / float(w) * 2.0f - 1.0f, ny, vignette);
      for (int c = 0; c < 3; ++c) dst[x * 3 + c] = uint8_t(std::lround(src[x * 4 + c] * k));
    }
  }
  *out_w = w;
  *out_h = h;
  return rgb;
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
    uint32_t w = 0, h = 0;
    const std::vector<uint8_t> rgb = FramedPicture(image, &w, &h);
    char name[64];
    const std::time_t now = std::time(nullptr);
    std::strftime(name, sizeof(name), "tabletennis-%Y%m%d-%H%M%S.png", std::localtime(&now));
    const auto path = rex::filesystem::GetExecutableFolder() / "photos" / name;
    if (WritePng(path, w, h, rgb)) {
      REXLOG_INFO("Tabletennis: photo saved to {} ({}x{})", path.string(), w, h);
      Notice("SAVED photos/" + std::string(name) + " (" + std::to_string(w) + "x" +
             std::to_string(h) + ")");
    } else {
      Notice("COULD NOT SAVE THE PICTURE");
    }
  }).detach();
}

// --- Game-side effects ---------------------------------------------------------------------------
// Enter/leave (game thread): freeze time, hide the HUD and remember the lighting presets' values;
// on leaving put everything back.
void Apply(uint8_t* base, bool on) {
  if (on == g_applied) return;
  if (on) {
    g_saved_warp = REX_LOAD_U32(kTimeWarp);
    g_saved_nohud = REX_LOAD_U32(kNoHudValue);
    g_post_saved.clear();
    for (uint32_t preset : g_post_presets) {
      PostValues v;
      v.exposure = LoadF(base, preset + kPostExposure);
      v.bloom = LoadF(base, preset + kPostBloom);
      for (int c = 0; c < 3; ++c) v.color[c] = LoadF(base, preset + kPostColor + 4 * c);
      v.saturation = LoadF(base, preset + kPostSaturation);
      v.contrast = LoadF(base, preset + kPostContrast);
      g_post_saved.push_back(v);
    }
  } else {
    REX_STORE_U32(kTimeWarp, g_saved_warp);
    REX_STORE_U32(kNoHudValue, g_saved_nohud);
    for (size_t i = 0; i < g_post_saved.size() && i < g_post_presets.size(); ++i) {
      const uint32_t preset = g_post_presets[i];
      const PostValues& v = g_post_saved[i];
      StoreF(base, preset + kPostExposure, v.exposure);
      StoreF(base, preset + kPostBloom, v.bloom);
      for (int c = 0; c < 3; ++c) StoreF(base, preset + kPostColor + 4 * c, v.color[c]);
      StoreF(base, preset + kPostSaturation, v.saturation);
      StoreF(base, preset + kPostContrast, v.contrast);
    }
    g_camera_ready = false;
  }
  g_applied = on;
}

// The look options through the game's own post-processing, so the screen shows them live.
void ApplyLook(uint8_t* base) {
  const Filter& filter = kFilters[std::clamp(g_options[kFilter].load(), 0, kFilterCount - 1)];
  const float exposure = filter.exposure * std::pow(2.0f, float(g_options[kExposure].load()) * 0.12f);
  const float contrast = filter.contrast * (1.0f + float(g_options[kContrast].load()) * 0.05f);
  const float saturation =
      std::max(0.0f, filter.saturation * (1.0f + float(g_options[kSaturation].load()) * 0.1f));
  const float t = float(g_options[kTemperature].load()) * 0.035f;
  const float tint[3] = {filter.tint[0] * (1.0f + t), filter.tint[1] * (1.0f + t * 0.15f),
                         filter.tint[2] * (1.0f - t)};
  const float bloom = float(g_options[kBloom].load()) / 3.0f;
  for (size_t i = 0; i < g_post_saved.size() && i < g_post_presets.size(); ++i) {
    const uint32_t preset = g_post_presets[i];
    const PostValues& v = g_post_saved[i];
    StoreF(base, preset + kPostExposure, v.exposure * exposure);
    StoreF(base, preset + kPostBloom, v.bloom * bloom);
    for (int c = 0; c < 3; ++c) StoreF(base, preset + kPostColor + 4 * c, v.color[c] * tint[c]);
    StoreF(base, preset + kPostSaturation, v.saturation * saturation);
    StoreF(base, preset + kPostContrast, v.contrast * contrast);
  }
}

void BuildMatrix(uint8_t* base, uint32_t out, uint32_t original) {
  std::memcpy(base + out, base + original, 64);  // keeps the w components
  const float cp = std::cos(g_pitch), sp = std::sin(g_pitch);
  const Vec3 look{cp * std::sin(g_yaw), sp, cp * std::cos(g_yaw)};
  const Vec3 back{-look.x, -look.y, -look.z};
  // right = world up x back, up = back x right (the game's right-handed rows).
  const float rl = std::sqrt(back.z * back.z + back.x * back.x);
  Vec3 right{back.z / rl, 0.0f, -back.x / rl};
  Vec3 up{back.y * right.z - back.z * right.y, back.z * right.x - back.x * right.z,
          back.x * right.y - back.y * right.x};
  // TILT rolls the picture around the view axis.
  const float roll = float(g_options[kTilt].load()) * 3.14159265f / 180.0f;
  if (roll != 0.0f) {
    const float c = std::cos(roll), s = std::sin(roll);
    const Vec3 r2{c * right.x + s * up.x, c * right.y + s * up.y, c * right.z + s * up.z};
    const Vec3 u2{-s * right.x + c * up.x, -s * right.y + c * up.y, -s * right.z + c * up.z};
    right = r2;
    up = u2;
  }
  // ZOOM changes the field of view where the renderer builds the projection (sub_82153598).
  const Vec3 rows[4] = {right, up, back, g_pos};
  for (int r = 0; r < 4; ++r) {
    StoreF(base, out + r * 16 + 0, rows[r].x);
    StoreF(base, out + r * 16 + 4, rows[r].y);
    StoreF(base, out + r * 16 + 8, rows[r].z);
  }
}

// Test hook: TT_TEST_PHOTO=<amount> turns photo mode on 4 s into the first rally, backs the
// camera away, up and to the side for 2 s (amount 1 = full stick), saves a picture and leaves.
// TT_TEST_PHOTO_OPTIONS="filter=2,frame=1,vignette=6,..." sets options first (option names in
// lower case with _ for spaces).
std::atomic<bool> g_test_options_applied{false};
void TestScript() {
  static const char* amount_text = std::getenv("TT_TEST_PHOTO");
  if (!amount_text) return;
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
  } else if (step == 2 && t > 5.0f && t < 5.1f) {
    static bool baseline = false;
    if (!baseline) {
      baseline = true;
      g_save_requested.store(true);  // the same moment before any option
    }
  } else if (step == 2 && t > 5.6f && t < 6.5f) {
    static bool options_set = false;
    if (!options_set) {
      options_set = true;
      if (const char* spec = std::getenv("TT_TEST_PHOTO_OPTIONS")) {
        std::string text(spec);
        for (size_t at = 0; at < text.size();) {
          const size_t end = std::min(text.find(',', at), text.size());
          const std::string item = text.substr(at, end - at);
          const size_t eq = item.find('=');
          if (eq != std::string::npos) {
            for (int i = 0; i < kOptionCount; ++i) {
              std::string name = kOptionInfo[i].label;
              for (char& c : name) c = c == ' ' ? '_' : char(std::tolower(c));
              if (name == item.substr(0, eq)) g_options[i].store(std::stoi(item.substr(eq + 1)));
            }
          }
          at = end + 1;
        }
      }
      g_test_options_applied.store(true);
    }
    const float amount = float(std::atof(amount_text));
    g_move_z.store(-amount);
    g_rise.store(0.6f * amount);
    g_look_x.store(0.3f * amount);
  } else if (step == 2 && t >= 6.5f) {
    g_test_options_applied.store(true);
    g_move_z.store(0.0f);
    g_rise.store(0.0f);
    g_look_x.store(0.0f);
    g_save_requested.store(true);
    step = 3;
  } else if (step == 3 && t > 7.5f && std::getenv("TT_TEST_PHOTO_OPTIONS2")) {
    // A second set of options and picture in the same moment, for comparisons.
    std::string text(std::getenv("TT_TEST_PHOTO_OPTIONS2"));
    for (size_t at = 0; at < text.size();) {
      const size_t end = std::min(text.find(',', at), text.size());
      const std::string item = text.substr(at, end - at);
      const size_t eq = item.find('=');
      for (int i = 0; i < kOptionCount && eq != std::string::npos; ++i) {
        std::string name = kOptionInfo[i].label;
        for (char& c : name) c = c == ' ' ? '_' : char(std::tolower(c));
        if (name == item.substr(0, eq)) g_options[i].store(std::stoi(item.substr(eq + 1)));
      }
      at = end + 1;
    }
    step = 5;
  } else if (step == 5 && t > 9.0f) {
    g_save_requested.store(true);
    step = 3;
    unsetenv("TT_TEST_PHOTO_OPTIONS2");
  } else if (step == 3 && t > 10.5f) {
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
  static uint16_t repeat_button = 0;
  static Clock::time_point repeat_at{};
  const uint16_t pressed = pad.buttons & ~last;
  last = pad.buttons;
  const bool sticks_clicked =
      (pad.buttons & (kL3 | kR3)) == (kL3 | kR3) && (pressed & (kL3 | kR3));
  if (sticks_clicked) RequestToggle();
  if (!g_active.load()) return false;
  if (pressed & kB) RequestToggle();
  if (pressed & kA) g_ui_visible.store(!g_ui_visible.load());
  if (pressed & kY) g_reset_requested.store(true);
  if (pressed & kX) g_save_requested.store(true);
  if (pressed & kBack) {
    ResetOptions();
    Notice("ALL OPTIONS RESET");
  }
  // D-pad: choose an option and change it, repeating while held.
  const auto now = Clock::now();
  uint16_t dpad = pressed & (kDpadUp | kDpadDown | kDpadLeft | kDpadRight);
  if (dpad) {
    repeat_button = dpad;
    repeat_at = now + std::chrono::milliseconds(400);
  } else if (repeat_button && (pad.buttons & repeat_button) && now >= repeat_at) {
    dpad = repeat_button;
    repeat_at = now + std::chrono::milliseconds(90);
  } else if (!(pad.buttons & repeat_button)) {
    repeat_button = 0;
  }
  if (dpad & kDpadUp) g_selected.store((g_selected.load() + kOptionCount - 1) % kOptionCount);
  if (dpad & kDpadDown) g_selected.store((g_selected.load() + 1) % kOptionCount);
  if (dpad & kDpadLeft) ChangeOption(g_selected.load(), -1);
  if (dpad & kDpadRight) ChangeOption(g_selected.load(), 1);
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
  if (active) {
    if (io.MouseDown[0] || io.MouseDown[1]) {
      g_mouse_dx.store(g_mouse_dx.load() + io.MouseDelta.x);
      g_mouse_dy.store(g_mouse_dy.load() + io.MouseDelta.y);
    }
    // Keyboard: Tab / Shift+Tab choose an option, - and = (or [ and ]) change it.
    if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
      const int step = io.KeyShift ? kOptionCount - 1 : 1;
      g_selected.store((g_selected.load() + step) % kOptionCount);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) {
      ChangeOption(g_selected.load(), -1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_RightBracket)) {
      ChangeOption(g_selected.load(), 1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
      g_save_requested.store(true);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_H)) g_ui_visible.store(!g_ui_visible.load());
  }
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  // The game's picture inside the window (letterboxed like the presenter does).
  const float aspect = float(tabletennis::pc::OverlayAspect());
  const float pic_w = std::min(io.DisplaySize.x, io.DisplaySize.y * aspect);
  const float pic_h = pic_w / aspect;
  const float pic_x = (io.DisplaySize.x - pic_w) * 0.5f;
  const float pic_y = (io.DisplaySize.y - pic_h) * 0.5f;
  const float s = pic_h / 720.0f;

  if (active) {
    // Framing preview: bars outside the frame, vignette, border.
    float fx, fy, fw, fh;
    FrameRect(g_options[kFrame].load(), pic_w, pic_h, &fx, &fy, &fw, &fh);
    fx += pic_x;
    fy += pic_y;
    const ImU32 bar = IM_COL32(0, 0, 0, 255);
    if (fy > pic_y) {
      dl->AddRectFilled(ImVec2(pic_x, pic_y), ImVec2(pic_x + pic_w, fy), bar);
      dl->AddRectFilled(ImVec2(pic_x, fy + fh), ImVec2(pic_x + pic_w, pic_y + pic_h), bar);
    }
    if (fx > pic_x) {
      dl->AddRectFilled(ImVec2(pic_x, pic_y), ImVec2(fx, pic_y + pic_h), bar);
      dl->AddRectFilled(ImVec2(fx + fw, pic_y), ImVec2(pic_x + pic_w, pic_y + pic_h), bar);
    }
    if (const int vignette = g_options[kVignette].load()) {
      constexpr int kCols = 32, kRows = 18;
      for (int j = 0; j < kRows; ++j) {
        for (int i = 0; i < kCols; ++i) {
          float xs[2] = {float(i) / kCols, float(i + 1) / kCols};
          float ys[2] = {float(j) / kRows, float(j + 1) / kRows};
          auto col = [&](float u, float v) {
            const float k = Vignette(u * 2.0f - 1.0f, v * 2.0f - 1.0f, vignette);
            return IM_COL32(0, 0, 0, int(std::clamp(1.0f - k, 0.0f, 1.0f) * 255.0f));
          };
          dl->AddRectFilledMultiColor(ImVec2(fx + xs[0] * fw, fy + ys[0] * fh),
                                      ImVec2(fx + xs[1] * fw, fy + ys[1] * fh),
                                      col(xs[0], ys[0]), col(xs[1], ys[0]), col(xs[1], ys[1]),
                                      col(xs[0], ys[1]));
        }
      }
    }
    if (const int border = g_options[kBorder].load()) {
      const float t = BorderThickness(border, fh);
      const ImU32 c = border == 2 ? IM_COL32(0, 0, 0, 255) : IM_COL32(245, 245, 245, 255);
      dl->AddRectFilled(ImVec2(fx, fy), ImVec2(fx + fw, fy + t), c);
      dl->AddRectFilled(ImVec2(fx, fy + fh - t), ImVec2(fx + fw, fy + fh), c);
      dl->AddRectFilled(ImVec2(fx, fy), ImVec2(fx + t, fy + fh), c);
      dl->AddRectFilled(ImVec2(fx + fw - t, fy), ImVec2(fx + fw, fy + fh), c);
    }
  }

  ImFont* body = achievement_ui::BodyFont();
  const float body_px = 17.0f * s;
  auto shadowed = [&](ImFont* font, float px, ImVec2 p, ImU32 col, const char* text) {
    dl->AddText(font, px, ImVec2(p.x + 1.5f * s, p.y + 1.5f * s), IM_COL32(0, 0, 0, 210), text);
    dl->AddText(font, px, p, col, text);
  };
  if (!notice.empty()) {
    shadowed(body, body_px, ImVec2(pic_x + 28.0f * s, pic_y + pic_h - 40.0f * s),
             IM_COL32(255, 204, 0, 255), notice.c_str());
  }
  if (!active || !g_ui_visible.load()) return;

  // Options panel on the right.
  const float panel_w = 330.0f * s, row_h = 27.0f * s;
  const float px1 = pic_x + pic_w - 24.0f * s, px0 = px1 - panel_w;
  const float title_px = 28.0f * s;
  const float py0 = pic_y + 70.0f * s;
  const float panel_h = title_px * 1.6f + row_h * kOptionCount + 16.0f * s;
  dl->AddRectFilled(ImVec2(px0, py0), ImVec2(px1, py0 + panel_h), IM_COL32(6, 8, 12, 200),
                    6.0f * s);
  ImFont* title_font = achievement_ui::HeadlineFont(title_px);
  shadowed(title_font, title_px, ImVec2(px0 + 16.0f * s, py0 + 8.0f * s),
           IM_COL32(255, 204, 0, 255), "PHOTO MODE");
  const int selected = g_selected.load();
  float y = py0 + title_px * 1.6f;
  for (int i = 0; i < kOptionCount; ++i) {
    const bool hot = i == selected;
    if (hot) {
      dl->AddRectFilled(ImVec2(px0 + 6.0f * s, y), ImVec2(px1 - 6.0f * s, y + row_h),
                        IM_COL32(255, 255, 255, 30), 3.0f * s);
    }
    const ImU32 col = hot ? IM_COL32(255, 204, 0, 255) : IM_COL32(222, 224, 228, 255);
    const float ty = y + (row_h - body_px) * 0.5f;
    dl->AddText(body, body_px, ImVec2(px0 + 16.0f * s, ty), col, kOptionInfo[i].label);
    std::string value = OptionValue(i);
    if (hot) value = "< " + value + " >";
    const float vw = body->CalcTextSizeA(body_px, FLT_MAX, 0.0f, value.c_str()).x;
    dl->AddText(body, body_px, ImVec2(px1 - 16.0f * s - vw, ty), col, value.c_str());
    y += row_h;
  }
  // Controls along the bottom left.
  const char* help[] = {
      "Left stick move   Right stick / mouse drag look   LT / RT down, up   LB slow  RB fast",
      "D-pad choose and change an option (Tab and - = on a keyboard)",
      "X save picture   Y reset camera   Back reset options   A hide all this   B leave",
  };
  float hy = pic_y + pic_h - 30.0f * s - (notice.empty() ? 0.0f : 26.0f * s);
  for (int i = 2; i >= 0; --i) {
    hy -= body_px * 1.35f;
    shadowed(body, body_px, ImVec2(pic_x + 28.0f * s, hy), IM_COL32(235, 237, 240, 255), help[i]);
  }
}

}  // namespace tabletennis::photo

using namespace tabletennis::photo;

extern "C" void sub_821DAEF0(PPCContext& ctx, uint8_t* base) {
  const uint32_t preset = ctx.r3.u32;
  __imp__sub_821DAEF0(ctx, base);
  if (std::find(g_post_presets.begin(), g_post_presets.end(), preset) == g_post_presets.end()) {
    g_post_presets.push_back(preset);
  }
}

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
      g_ui_visible.store(true);
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
  ApplyLook(base);
  if (!g_scratch) g_scratch = rex::system::kernel_memory()->SystemHeapAlloc(64, 16);
  if (!g_camera_ready) {
    // Start from the game's camera (the camera update hook only takes over once this is set).
    g_start_pos = {LoadF(base, matrix + 48), LoadF(base, matrix + 52), LoadF(base, matrix + 56)};
    const Vec3 back{LoadF(base, matrix + 32), LoadF(base, matrix + 36), LoadF(base, matrix + 40)};
    g_start_pitch = std::asin(std::clamp(-back.y, -1.0f, 1.0f));
    g_start_yaw = std::atan2(-back.x, -back.z);
    g_pos = g_start_pos;
    g_yaw = g_start_yaw;
    g_pitch = g_start_pitch;
    if (!g_options_ready.load()) ResetOptions();  // later visits keep the look and framing
    g_last_frame = Clock::now();
    g_matrix = matrix;
    g_camera_ready = true;
  }
  if (g_save_requested.exchange(false)) SavePicture();
  __imp__sub_821B5A68(ctx, base);
}

// The camera's update (camera object in r3), which writes the matrix the frame renders with.
// While photo mode is on, the free camera's matrix and lens replace it right after.
extern "C" void sub_821709F8(PPCContext& ctx, uint8_t* base) {
  __imp__sub_821709F8(ctx, base);
  if (!g_active.load() || !g_camera_ready || !g_matrix || !g_scratch) return;
  const auto now = Clock::now();
  if (g_reset_requested.exchange(false)) {
    g_pos = g_start_pos;
    g_yaw = g_start_yaw;
    g_pitch = g_start_pitch;
    g_options[kFov].store(kOptionInfo[kFov].initial);
    g_options[kTilt].store(0);
  }
  const float dt = std::min(0.1f, std::chrono::duration<float>(now - g_last_frame).count());
  g_last_frame = now;
  // Look: right stick and mouse drag; slower when zoomed in.
  const float zoom = std::clamp(10.0f / float(g_options[kFov].load()), 0.2f, 1.5f);
  g_yaw -= (g_look_x.load() * kLookSpeed * dt + g_mouse_dx.exchange(0.0f) * 0.004f) * zoom;
  g_pitch += (g_look_y.load() * kLookSpeed * dt - g_mouse_dy.exchange(0.0f) * 0.004f) * zoom;
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

// ZOOM: the renderer rebuilds a view's perspective projection in sub_82153598 from the field of
// view in degrees at view+824 (found by David Petersson for the Windows build). Zoom narrows or
// widens it for that call only and puts the game's value back, so frames never compound and
// culling follows the zoomed view.
extern "C" void __imp__sub_82153598(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82153598(PPCContext& ctx, uint8_t* base) {
  const uint32_t view = ctx.r3.u32;
  const float magnification = float(g_options[kFov].load()) / 10.0f;
  if (!g_active.load() || !view || magnification == 1.0f) {
    __imp__sub_82153598(ctx, base);
    return;
  }
  const uint32_t original = REX_LOAD_U32(view + 824);
  const float degrees = LoadF(base, view + 824);
  if (std::isfinite(degrees) && degrees > 0.0f && degrees < 180.0f) {
    constexpr double kRadians = 3.14159265358979323846 / 180.0;
    const double zoomed = 2.0 * std::atan(std::tan(degrees * kRadians / 2.0) / magnification) / kRadians;
    StoreF(base, view + 824, float(std::clamp(zoomed, 5.0, 150.0)));
  }
  __imp__sub_82153598(ctx, base);
  REX_STORE_U32(view + 824, original);
}
