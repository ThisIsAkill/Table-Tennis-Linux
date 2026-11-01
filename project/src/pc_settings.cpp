// pc_settings.cpp - see pc_settings.h.

#include "pc_settings.h"

#include "generated/default/tabletennis_init.h"
#include "haptics.h"
#include "intro_skip.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/window.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

REXCVAR_DECLARE(bool, achievement_sound);
REXCVAR_DECLARE(bool, haptics);
REXCVAR_DECLARE(bool, show_fps);
REXCVAR_DECLARE(double, display_aspect);

namespace tabletennis::pc_settings {
namespace {

// --- Settings ---------------------------------------------------------------------------------
// kLive: takes effect at once. kRestart: read by the launcher at the next start. kSaved: only
// stored for the launcher (update checks). kInfo: no value to change (the version row).
enum class Apply { kLive, kRestart, kSaved, kInfo };

struct Choice {
  const char* value;  // as written to tt-recomp.ini
  const char* label;  // as shown on the page
};

struct Row {
  const char* key;
  const char* label;
  const char* help;
  Apply apply;
  std::vector<Choice> choices;
  int fallback;     // launcher default when the ini has no value
  int current = 0;  // chosen value
  int running = 0;  // value this session started with (restart rows)
};

enum RowId {
  kResolution, kFullscreen, kPicture, kAntiAliasing, kShowFps, kHaptics, kAchievementSound,
  // UPDATES section
  kCheckUpdates, kAutoUpdate, kVersion,
};

std::vector<Row> MakeRows() {
  return {
      {"resolution", "RESOLUTION", "Window size and rendering resolution.", Apply::kRestart,
       {{"720p", "1280 X 720"}, {"800p", "1280 X 800"}, {"1080p", "1920 X 1080"},
        {"1440p", "2560 X 1440"}, {"4k", "3840 X 2160"}},
       2},
      {"fullscreen", "FULLSCREEN", "Fill the whole display instead of a window.", Apply::kLive,
       {{"false", "OFF"}, {"true", "ON"}},
       0},
      {"aspect", "PICTURE", "Letterbox keeps the game's 16:9 picture. Fill stretches it to the "
       "screen's shape and widens the 3D view (experimental).", Apply::kRestart,
       {{"letterbox", "LETTERBOX"}, {"fill", "FILL SCREEN"}},
       0},
      {"aa", "ANTI-ALIASING", "Smooths jagged edges with FXAA after each frame.", Apply::kRestart,
       {{"none", "OFF"}, {"fxaa", "FXAA"}, {"fxaa_extreme", "FXAA EXTREME"}},
       0},
      {"show_fps", "SHOW FPS", "Frame rate counter in the top left corner (F10).", Apply::kLive,
       {{"false", "OFF"}, {"true", "ON"}},
       0},
      {"haptics", "HAPTICS", "Controller pulse on every ball hit and a light tick in menus.",
       Apply::kLive,
       {{"false", "OFF"}, {"true", "ON"}},
       1},
      {"achievement_sound", "ACHIEVEMENT SOUND", "Play a chime when an achievement unlocks.",
       Apply::kLive,
       {{"false", "OFF"}, {"true", "ON"}},
       1},
      {"check_updates", "CHECK FOR UPDATES", "Look for a new version each time the game starts.",
       Apply::kSaved,
       {{"false", "OFF"}, {"true", "ON"}},
       1},
      {"auto_update", "AUTOMATIC UPDATES", "Install new versions when the game starts.",
       Apply::kSaved,
       {{"false", "OFF"}, {"true", "ON"}},
       0},
      {"", "VERSION", "", Apply::kInfo, {}, 0},
  };
}

// UI thread only.
std::vector<Row> g_rows;
bool g_loaded = false;

// The launcher's settings file: tt-recomp.ini for tt-recomp.sh, settings.ini for the Windows
// launcher (same keys; it ignores the ones it does not offer).
std::filesystem::path IniPath() {
#if defined(_WIN32)
  return rex::filesystem::GetExecutableFolder() / "settings.ini";
#else
  return rex::filesystem::GetExecutableFolder() / "tt-recomp.ini";
#endif
}

// --- Updates ----------------------------------------------------------------------------------
// The game does no networking. The launcher writes <install dir>/.tt-update.json when a newer
// release exists (possibly a few seconds after the game started), and runs the update after the
// game exits if <install dir>/.tt-update-requested is present. <install dir>/.tt-version holds
// the installed version; without it this is a development build and updates are not offered.
// UI thread only.
struct UpdateInfo {
  std::string version;
  std::string title;
  std::string published;
  std::vector<std::string> notes;
};
std::optional<UpdateInfo> g_update;

std::filesystem::path InstallDir() { return rex::filesystem::GetExecutableFolder(); }

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

// Installed version, or empty for a development build.
const std::string& InstalledVersion() {
  static const std::string version = [] {
    std::string line = ReadFile(InstallDir() / ".tt-version");
    line = line.substr(0, line.find_first_of("\r\n"));
    const auto first = line.find_first_not_of(" \t");
    const auto last = line.find_last_not_of(" \t");
    return first == std::string::npos ? std::string() : line.substr(first, last - first + 1);
  }();
  return version;
}

std::vector<int> VersionParts(std::string version) {
  if (!version.empty() && (version[0] == 'v' || version[0] == 'V')) version.erase(0, 1);
  std::vector<int> parts;
  size_t i = 0;
  while (i < version.size() && std::isdigit(uint8_t(version[i]))) {
    int value = 0;
    while (i < version.size() && std::isdigit(uint8_t(version[i]))) value = value * 10 + (version[i++] - '0');
    parts.push_back(value);
    if (i < version.size() && version[i] == '.') ++i;
  }
  return parts;
}

bool IsNewer(const std::string& candidate, const std::string& installed) {
  auto a = VersionParts(candidate), b = VersionParts(installed);
  if (a.empty() || b.empty()) return false;
  a.resize(std::max(a.size(), b.size()), 0);
  b.resize(a.size(), 0);
  return a > b;
}

// Minimal JSON reading for the launcher's flat update file: string values and one array of
// strings, located by key.
bool JsonString(const std::string& s, size_t& i, std::string& out) {
  while (i < s.size() && std::isspace(uint8_t(s[i]))) ++i;
  if (i >= s.size() || s[i] != '"') return false;
  ++i;
  while (i < s.size()) {
    const char c = s[i++];
    if (c == '"') return true;
    if (c != '\\') {
      out += c;
      continue;
    }
    if (i >= s.size()) break;
    const char e = s[i++];
    switch (e) {
      case 'n': out += ' '; break;
      case 't': out += ' '; break;
      case 'r': break;
      case 'u': {
        if (i + 4 > s.size()) return false;
        const unsigned code = unsigned(std::strtoul(s.substr(i, 4).c_str(), nullptr, 16));
        i += 4;
        if (code < 0x80) {
          out += char(code);
        } else if (code < 0x800) {
          out += char(0xC0 | (code >> 6));
          out += char(0x80 | (code & 0x3F));
        } else {
          out += char(0xE0 | (code >> 12));
          out += char(0x80 | ((code >> 6) & 0x3F));
          out += char(0x80 | (code & 0x3F));
        }
        break;
      }
      default: out += e; break;
    }
  }
  return false;
}

// Position just after `"key":`, or npos.
size_t JsonValueAt(const std::string& s, const char* key) {
  const std::string quoted = std::string("\"") + key + "\"";
  size_t i = s.find(quoted);
  if (i == std::string::npos) return i;
  i += quoted.size();
  while (i < s.size() && std::isspace(uint8_t(s[i]))) ++i;
  return i < s.size() && s[i] == ':' ? i + 1 : std::string::npos;
}

std::optional<UpdateInfo> ParseUpdate(const std::string& s) {
  UpdateInfo info;
  for (auto [key, field] : {std::pair{"version", &info.version}, std::pair{"title", &info.title},
                            std::pair{"published", &info.published}}) {
    size_t i = JsonValueAt(s, key);
    if (i != std::string::npos) JsonString(s, i, *field);
  }
  if (size_t i = JsonValueAt(s, "notes"); i != std::string::npos) {
    while (i < s.size() && std::isspace(uint8_t(s[i]))) ++i;
    if (i < s.size() && s[i] == '[') {
      ++i;
      std::string note;
      while (JsonString(s, i, note)) {
        info.notes.push_back(note);
        note.clear();
        while (i < s.size() && (std::isspace(uint8_t(s[i])) || s[i] == ',')) ++i;
      }
    }
  }
  if (info.version.empty()) return std::nullopt;
  return info;
}

// Called every UI frame: looks at the update file every 2 s for the first minute.
// TT_TEST_UPDATE_JSON=<path> reads that file instead (test hook).
void PollUpdate() {
  static double started = -1.0, next = 0.0;
  static bool done = false;
  static std::filesystem::file_time_type seen{};
  if (done) return;
  const double now = ImGui::GetTime();
  if (started < 0.0) started = now;
  if (now < next) return;
  next = now + 2.0;
  if (now - started > 60.0) {
    done = true;
    return;
  }
  const char* test_path = std::getenv("TT_TEST_UPDATE_JSON");
  const std::filesystem::path path = test_path && test_path[0] ? std::filesystem::path(test_path)
                                                               : InstallDir() / ".tt-update.json";
  std::error_code ec;
  const auto written = std::filesystem::last_write_time(path, ec);
  if (ec || written == seen) return;
  seen = written;
  auto info = ParseUpdate(ReadFile(path));
  if (!info) {
    REXLOG_WARN("Tabletennis: cannot read the update file {}", path.string());
    return;
  }
  if (InstalledVersion().empty()) {
    REXLOG_INFO("Tabletennis: update {} available, not offered in a development build", info->version);
    done = true;
    return;
  }
  if (!IsNewer(info->version, InstalledVersion())) return;
  REXLOG_INFO("Tabletennis: update available: {} (installed {})", info->version, InstalledVersion());
  g_update = std::move(info);
  done = true;
}

bool WriteUpdateRequest(const std::string& version) {
  const auto path = InstallDir() / ".tt-update-requested";
  std::ofstream out(path, std::ios::trunc);
  out << version << '\n';
  out.close();
  return bool(out);
}

// Front end showing (not a match, not the pause menu): the front-end update ran within the last
// half second, the pause menu's not within two seconds, and no rally is in play.
std::atomic<int64_t> g_frontend_tick_ns{0};
std::atomic<int64_t> g_pause_tick_ns{0};
int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
bool InFrontEnd() {
  const int64_t now = NowNs();
  return intro::MainMenuReached() && now - g_frontend_tick_ns.load() < 500'000'000 &&
         now - g_pause_tick_ns.load() > 2'000'000'000 && !haptics::MatchInPlay();
}

std::string Upper(std::string text) {
  for (auto& c : text) {
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  }
  return text;
}

std::string Trim(std::string text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = text.find_last_not_of(" \t\r\n");
  text = text.substr(first, last - first + 1);
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
    text = text.substr(1, text.size() - 2);
  }
  return text;
}

std::map<std::string, std::string> ReadIni() {
  std::map<std::string, std::string> values;
  std::ifstream file(IniPath());
  std::string line;
  while (std::getline(file, line)) {
    const auto key_end = line.find('=');
    if (key_end == std::string::npos) continue;
    const std::string key = Trim(line.substr(0, key_end));
    if (key.empty() || key[0] == '#' || key[0] == ';') continue;
    values[key] = Trim(line.substr(key_end + 1));
  }
  return values;
}

int FindChoice(const Row& row, const std::string& value) {
  for (size_t i = 0; i < row.choices.size(); ++i) {
    if (value == row.choices[i].value) return int(i);
  }
  return -1;
}

// --- Controller tap ---------------------------------------------------------------------------
enum : uint16_t {
  kPadUp = 0x0001,
  kPadDown = 0x0002,
  kPadLeft = 0x0004,
  kPadRight = 0x0008,
  kPadA = 0x1000,
  kPadB = 0x2000,
  kPadX = 0x4000,
};
constexpr int16_t kStickThreshold = 18000;

struct UserPad {
  uint16_t last = 0;
  uint16_t ignore = 0;          // held when the page opened; ignored until released
  bool swallow_until_release = false;
};

std::mutex g_pad_mutex;
std::array<UserPad, 4> g_users;
std::atomic<bool> g_open{false};
std::atomic<uint16_t> g_pressed{0};  // new presses since the UI thread last looked
std::atomic<uint16_t> g_held{0};

uint16_t Buttons(const achievement_ui::PadSample& pad) {
  uint16_t buttons = pad.buttons;
  if (pad.thumb_ly > kStickThreshold) buttons |= kPadUp;
  if (pad.thumb_ly < -kStickThreshold) buttons |= kPadDown;
  if (pad.thumb_lx < -kStickThreshold) buttons |= kPadLeft;
  if (pad.thumb_lx > kStickThreshold) buttons |= kPadRight;
  return buttons;
}

bool PadIdle(const achievement_ui::PadSample& pad) {
  return Buttons(pad) == 0 && pad.left_trigger < 30 && pad.right_trigger < 30;
}

void SetOpen(bool open) {
  std::lock_guard lock(g_pad_mutex);
  for (auto& user : g_users) {
    // Opening: the A that chose the menu entry must not also change a value. Closing: the
    // B that closed the page must not also leave the game's OPTIONS menu.
    user.ignore = open ? user.last : 0;
    user.swallow_until_release = !open;
  }
  g_pressed.store(0);
  g_held.store(0);
  g_open.store(open);
}

// --- Drawing helpers --------------------------------------------------------------------------
ImU32 Col(int r, int g, int b, float a) {
  return IM_COL32(r, g, b, int(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f));
}
constexpr float kCondensed = 0.84f;

float CapsTop(float center_y, float px) { return center_y - 0.525f * px; }

float TextWidth(ImFont* font, float px, const char* text, float x_scale = 1.0f) {
  return font->CalcTextSizeA(px, FLT_MAX, 0.0f, text).x * x_scale;
}

void AddText(ImDrawList* dl, ImFont* font, float px, ImVec2 pos, ImU32 col, const char* text,
             float x_scale = 1.0f, float wrap_width = 0.0f) {
  const int first = dl->VtxBuffer.Size;
  dl->AddText(font, px, pos, col, text, nullptr, wrap_width > 0.0f ? wrap_width / x_scale : 0.0f);
  if (x_scale != 1.0f) {
    for (int i = first; i < dl->VtxBuffer.Size; ++i) {
      auto& v = dl->VtxBuffer[i];
      v.pos.x = pos.x + (v.pos.x - pos.x) * x_scale;
    }
  }
}

void AddOutlinedText(ImDrawList* dl, ImFont* font, float px, ImVec2 pos, ImU32 col, ImU32 outline_col,
                     float outline, const char* text, float x_scale = 1.0f) {
  static const float kDirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1},
                                    {0.7f, 0.7f}, {-0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}};
  for (const auto& d : kDirs) {
    AddText(dl, font, px, ImVec2(pos.x + d[0] * outline, pos.y + d[1] * outline), outline_col, text,
            x_scale);
  }
  AddText(dl, font, px, pos, col, text, x_scale);
}

void Arrow(ImDrawList* dl, ImVec2 center, float size, bool right, ImU32 col) {
  const float d = right ? 1.0f : -1.0f;
  dl->AddTriangleFilled(ImVec2(center.x + d * size * 0.5f, center.y),
                        ImVec2(center.x - d * size * 0.5f, center.y - size * 0.6f),
                        ImVec2(center.x - d * size * 0.5f, center.y + size * 0.6f), col);
}

void FaceButton(ImDrawList* dl, ImVec2 center, float radius, ImU32 fill, const char* letter, float alpha) {
  dl->AddCircleFilled(center, radius, Col(20, 22, 28, 0.9f * alpha), 24);
  dl->AddCircleFilled(center, radius * 0.82f, fill, 24);
  const float px = radius * 1.5f;
  ImFont* font = achievement_ui::HeadlineFont(px);
  const float w = TextWidth(font, px, letter);
  AddText(dl, font, px, ImVec2(center.x - w * 0.5f, CapsTop(center.y, px)), Col(255, 255, 255, alpha),
          letter);
}

// The game's menus sit at a slight angle, rising to the right.
void Rotate(ImDrawList* dl, int first_vertex, ImVec2 pivot, float radians) {
  const float c = std::cos(radians), s = std::sin(radians);
  for (int i = first_vertex; i < dl->VtxBuffer.Size; ++i) {
    auto& v = dl->VtxBuffer[i];
    const float x = v.pos.x - pivot.x, y = v.pos.y - pivot.y;
    v.pos = ImVec2(pivot.x + x * c - y * s, pivot.y + x * s + y * c);
  }
}

}  // namespace

// ==============================================================================================
// Guest side
// ==============================================================================================
void PollFrontend(PPCContext& ctx, uint8_t* base, uint32_t movie) {
  if (!movie) return;
  g_frontend_tick_ns.store(NowNs());
  // Test hook: TT_TEST_OPEN_PC_SETTINGS=1 opens the page a few seconds after the main menu.
  static const bool test_open = [] {
    const char* value = std::getenv("TT_TEST_OPEN_PC_SETTINGS");
    return value && std::strcmp(value, "pause") != 0;
  }();
  if (test_open) {
    static int frames = 0;
    if (++frames == 240) SetOpen(true);
  }
  // The Flash lookup takes a guest string; keep one copy of the flag's name in guest memory.
  static uint32_t name = 0;
  if (!name) {
    static constexpr char kFlag[] = "GLOBAL_PC_SETTINGS";
    name = rex::system::kernel_memory()->SystemHeapAlloc(sizeof(kFlag));
    if (!name) return;
    std::memcpy(base + name, kFlag, sizeof(kFlag));
  }
  // Same check-and-clear the game does for its own flags (GLOBAL_SAVE_GAME and the like).
  PPCContext call = ctx;
  call.r3.u64 = movie;
  call.r4.u64 = name;
  sub_823F9280(call, base);
  const uint32_t slot = call.r3.u32;
  if (!slot) return;
  call = ctx;
  call.r3.u64 = slot;
  sub_823FA5C8(call, base);
  if (call.r3.s32 <= 0) return;
  call = ctx;
  call.r3.u64 = movie;
  call.r4.u64 = name;
  sub_823F9318(call, base);
  if (call.r3.u32) {
    REX_STORE_U32(call.r3.u32 + 0, 0);
    REX_STORE_U32(call.r3.u32 + 4, 3);
  }
  if (!g_open.load()) {
    REXLOG_INFO("Tabletennis: PC SETTINGS chosen in the OPTIONS menu");
    SetOpen(true);
  }
}

void PollPause(PPCContext& ctx, uint8_t* base, uint32_t pause) {
  if (!pause) return;
  g_pause_tick_ns.store(NowNs());
  static uint32_t name = 0;
  if (!name) {
    static constexpr char kFlag[] = "GLOBAL_PC_SETTINGS";
    name = rex::system::kernel_memory()->SystemHeapAlloc(sizeof(kFlag));
    if (!name) return;
    std::memcpy(base + name, kFlag, sizeof(kFlag));
  }
  // Test hook: TT_TEST_OPEN_PC_SETTINGS=pause opens the page after the pause menu's OPTIONS
  // list has been showing for about two seconds.
  static const bool test_open = [] {
    const char* value = std::getenv("TT_TEST_OPEN_PC_SETTINGS");
    return value && std::strcmp(value, "pause") == 0;
  }();
  if (test_open) {
    static int frames = 0;
    static bool done = false;
    PPCContext call = ctx;
    call.r3.u64 = REX_LOAD_U32(pause + 92);
    call.r4.u64 = 0x82060730;  // Retail image's existing "STATE_CURRENT" string.
    if (!done && call.r3.u32) {
      sub_823F9280(call, base);
      constexpr uint32_t kPauseOptions = 1;  // iPAUSE_OPTIONS in pongpause_001
      if (call.r3.u32 && REX_LOAD_U32(call.r3.u32) == kPauseOptions) {
        if (++frames == 120) {
          done = true;
          SetOpen(true);
        }
      } else {
        frames = 0;
      }
    }
  }
  // The game's own check-and-clear for the pause movie's flags (sub_823246E8 takes the
  // pause object, locks its movie and resets the flag).
  PPCContext call = ctx;
  call.r3.u64 = pause;
  call.r4.u64 = name;
  sub_823246E8(call, base);
  if ((call.r3.u32 & 0xFF) && !g_open.load()) {
    REXLOG_INFO("Tabletennis: PC SETTINGS chosen in the pause menu's OPTIONS");
    SetOpen(true);
  }
}

bool IsOpen() { return g_open.load(std::memory_order_relaxed); }

bool FilterInput(uint32_t user, achievement_ui::PadSample& pad) {
  if (user >= g_users.size()) return false;
  std::lock_guard lock(g_pad_mutex);
  UserPad& state = g_users[user];
  const uint16_t buttons = Buttons(pad);
  if (g_open.load()) {
    state.ignore &= buttons;
    const uint16_t usable = buttons & uint16_t(~state.ignore);
    const uint16_t pressed = usable & uint16_t(~state.last);
    if (pressed) g_pressed.fetch_or(pressed);
    uint16_t held = 0;
    for (size_t i = 0; i < g_users.size(); ++i) {
      held |= i == user ? usable : uint16_t(g_users[i].last & ~g_users[i].ignore);
    }
    g_held.store(held);
    state.last = buttons;
    pad = achievement_ui::PadSample{};
    return true;
  }
  state.last = buttons;
  if (state.swallow_until_release) {
    if (!PadIdle(pad)) {
      pad = achievement_ui::PadSample{};
      return true;
    }
    state.swallow_until_release = false;
  }
  return false;
}

// ==============================================================================================
// Page
// ==============================================================================================
SettingsPage::SettingsPage(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, LiveHooks hooks)
    : rex::ui::ImGuiDialog(drawer), window_(window), hooks_(std::move(hooks)) {}

SettingsPage::~SettingsPage() = default;

void SettingsPage::Open() {
  if (!g_loaded) {
    // Restart rows remember what this session runs with, to flag pending changes.
    g_rows = MakeRows();
    const auto ini = ReadIni();
    for (auto& row : g_rows) {
      const auto it = ini.find(row.key);
      const int found = it == ini.end() ? -1 : FindChoice(row, it->second);
      row.current = row.running = found >= 0 ? found : row.fallback;
    }
    g_loaded = true;
  }
  // Live rows show the state in effect now (F10 and other paths can change it).
  g_rows[kShowFps].current = hooks_.get_show_fps && hooks_.get_show_fps() ? 1 : 0;
  g_rows[kAchievementSound].current = REXCVAR_GET(achievement_sound) ? 1 : 0;
  g_rows[kHaptics].current = REXCVAR_GET(haptics) ? 1 : 0;
  if (window_) g_rows[kFullscreen].current = window_->IsFullscreen() ? 1 : 0;
  selected_ = 0;
  held_direction_ = 0;
  save_error_.clear();
  notice_.clear();
  confirm_ = false;
  opened_at_ = ImGui::GetTime();
  shown_ = true;
}

void SettingsPage::Change(int row_index, int delta) {
  Row& row = g_rows[row_index];
  if (row.choices.empty()) return;
  const int count = int(row.choices.size());
  row.current = ((row.current + delta) % count + count) % count;
  ApplyLive(row_index);
  REXLOG_INFO("Tabletennis: PC setting {} = {}", row.key, row.choices[row.current].value);
  notice_.clear();
  Save();
}

// True on a Steam Deck: Steam sets SteamDeck=1, and the built-in screen is 1280x800.
bool OnSteamDeck() {
  if (const char* flag = std::getenv("SteamDeck"); flag && std::strcmp(flag, "1") == 0) return true;
  const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
  return mode && ((mode->w == 1280 && mode->h == 800) || (mode->w == 800 && mode->h == 1280));
}

// X: every row back to its default, like the game's RESTORE DEFAULTS. The defaults are the
// launcher's, except on a Steam Deck, where they match setup-deck.sh (800p, fullscreen).
void SettingsPage::RestoreDefaults() {
  for (int i = 0; i < int(g_rows.size()); ++i) {
    g_rows[i].current = g_rows[i].fallback;
  }
  // (check_updates and auto_update fall back to true and false through their Row defaults.)
  if (OnSteamDeck()) {
    g_rows[kResolution].current = std::max(0, FindChoice(g_rows[kResolution], "800p"));
    g_rows[kFullscreen].current = 1;
  }
  for (int i = 0; i < int(g_rows.size()); ++i) ApplyLive(i);
  REXLOG_INFO("Tabletennis: PC settings restored to defaults");
  notice_ = "DEFAULTS RESTORED";
  notice_until_ = ImGui::GetTime() + 2.5;
  Save();
}

// Leaves the update to the launcher: write the request, then quit the way Escape does.
void SettingsPage::RequestUpdate() {
  if (!WriteUpdateRequest(g_update->version)) {
    save_error_ = "COULD NOT REQUEST THE UPDATE";
    confirm_ = false;
    REXLOG_WARN("Tabletennis: cannot write the update request in {}", InstallDir().string());
    return;
  }
  REXLOG_INFO("Tabletennis: update to {} requested; quitting so the launcher can install it",
              g_update->version);
  confirm_ = false;
  if (window_) {
    window_->RequestClose();
  } else {
    std::_Exit(0);
  }
}

void SettingsPage::ApplyLive(int row_index) {
  const bool on = g_rows[row_index].current == 1;
  switch (row_index) {
    case kFullscreen:
      if (window_) window_->SetFullscreen(on);
      break;
    case kShowFps:
      REXCVAR_SET(show_fps, on);
      if (hooks_.set_show_fps) hooks_.set_show_fps(on);
      break;
    case kAchievementSound:
      REXCVAR_SET(achievement_sound, on);
      break;
    case kHaptics:
      REXCVAR_SET(haptics, on);
      break;
    default:
      break;
  }
}

// Rewrites only the keys this page owns, keeping every other line (iso_path, framerate,
// engine_flags, comments) as it was, and appends keys the file does not have yet.
void SettingsPage::Save() {
  const auto path = IniPath();
  std::vector<std::string> lines;
  {
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
    }
  }
  std::vector<bool> written(g_rows.size(), false);
  for (auto& line : lines) {
    const auto key_end = line.find('=');
    if (key_end == std::string::npos) continue;
    const std::string key = Trim(line.substr(0, key_end));
    for (size_t i = 0; i < g_rows.size(); ++i) {
      if (g_rows[i].choices.empty() || key != g_rows[i].key) continue;
      if (!written[i]) line = key + "=" + g_rows[i].choices[g_rows[i].current].value;
      written[i] = true;
    }
  }
  for (size_t i = 0; i < g_rows.size(); ++i) {
    if (!written[i] && !g_rows[i].choices.empty()) lines.push_back(std::string(g_rows[i].key) + "=" + g_rows[i].choices[g_rows[i].current].value);
  }
  auto temp = path;
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::trunc);
    for (const auto& line : lines) out << line << '\n';
    if (!out) {
      save_error_ = "COULD NOT SAVE " + path.filename().string();
      REXLOG_WARN("Tabletennis: cannot write {}", temp.string());
      return;
    }
  }
  std::error_code ec;
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    save_error_ = "COULD NOT SAVE " + path.filename().string();
    REXLOG_WARN("Tabletennis: cannot replace {}: {}", path.string(), ec.message());
    return;
  }
  save_error_.clear();
}

void SettingsPage::OnDraw(ImGuiIO& io) {
  PollUpdate();
  if (!g_open.load()) {
    shown_ = false;
    confirm_ = false;
    return;
  }
  if (!shown_) Open();

  const uint16_t pressed = g_pressed.exchange(0);
  const uint16_t held = g_held.load();
  const double now = ImGui::GetTime();
  const float alpha_in = std::clamp(float(now - opened_at_) / 0.18f, 0.0f, 1.0f);
  // The update confirmation owns the buttons while it is up: A updates, B goes back.
  if (confirm_) {
    if (pressed & kPadB) {
      confirm_ = false;
    } else if ((pressed & kPadA) && g_update) {
      RequestUpdate();
    }
    held_direction_ = 0;
    Draw(io, alpha_in);
    return;
  }
  if (pressed & kPadB) {
    SetOpen(false);
    shown_ = false;
    return;
  }
  const int rows = int(g_rows.size());
  auto step = [&](uint16_t button) {
    switch (button) {
      case kPadUp: selected_ = (selected_ + rows - 1) % rows; break;
      case kPadDown: selected_ = (selected_ + 1) % rows; break;
      case kPadLeft: Change(selected_, -1); break;
      case kPadRight: Change(selected_, 1); break;
      default: break;
    }
  };
  for (uint16_t button : {kPadUp, kPadDown, kPadLeft, kPadRight}) {
    if (pressed & button) {
      step(button);
      held_direction_ = button;
      next_repeat_ = now + 0.45;
    }
  }
  if (pressed & kPadA) {
    if (selected_ == kVersion) {
      if (g_update) {
        confirm_ = true;
        confirm_at_ = now;
      }
    } else {
      Change(selected_, 1);
    }
  }
  if (pressed & kPadX) RestoreDefaults();
  // Held directions repeat, like the game's lists.
  if (held_direction_ && !(held & held_direction_)) held_direction_ = 0;
  if (held_direction_ && now >= next_repeat_) {
    step(uint16_t(held_direction_));
    next_repeat_ = now + 0.13;
  }

  const float alpha = std::clamp(float(now - opened_at_) / 0.18f, 0.0f, 1.0f);
  Draw(io, alpha);
}

void SettingsPage::Draw(ImGuiIO& io, float in) {
  // The presenter letterboxes the game's 16:9 frame (or the display_aspect shape when filling
  // the screen) into the window; lay out in 720p units inside that picture.
  const double configured = REXCVAR_GET(display_aspect);
  const float aspect = configured > 0.0 ? float(configured) : 16.0f / 9.0f;
  const float pic_w = std::min(io.DisplaySize.x, io.DisplaySize.y * aspect);
  const float pic_h = pic_w / aspect;
  const float pic_x = (io.DisplaySize.x - pic_w) * 0.5f;
  const float pic_y = (io.DisplaySize.y - pic_h) * 0.5f;
  const float s = pic_h / 720.0f;
  // Horizontal positions follow the 16:9 layout, centred in wider pictures.
  const float ox = pic_x + (pic_w - 1280.0f * s) * 0.5f;
  auto X = [&](float x) { return ox + x * s; };
  auto Y = [&](float y) { return pic_y + y * s; };

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(pic_x, pic_y), ImVec2(pic_x + pic_w, pic_y + pic_h), Col(0, 0, 0, 0.30f * in));

  const int first_vertex = dl->VtxBuffer.Size;
  const float rise = (1.0f - in) * 10.0f;

  // Panel geometry, wide enough to cover the game's OPTIONS panel underneath.
  const float x0 = X(282), x1 = X(882);
  const float bx0 = X(312);
  const float header_y0 = Y(34 + rise), header_y1 = Y(96 + rise);
  const float header_cy = (header_y0 + header_y1) * 0.5f;
  const float body_y0 = header_y1 - 6.0f * s, body_y1 = Y(642 + rise);
  const float footer_y0 = body_y1, footer_y1 = Y(682 + rise);

  // Body: near-black like the game's option lists, but opaque so the OPTIONS list underneath
  // does not show through; a faint sheen runs along the top.
  dl->AddRectFilled(ImVec2(bx0, body_y0), ImVec2(x1, body_y1), Col(6, 8, 12, in), 6.0f * s,
                    ImDrawFlags_RoundCornersTop);
  dl->AddRectFilledMultiColor(ImVec2(bx0, body_y0), ImVec2(x1, body_y0 + 60.0f * s),
                              Col(255, 255, 255, 0.07f * in), Col(255, 255, 255, 0.07f * in),
                              Col(255, 255, 255, 0.0f), Col(255, 255, 255, 0.0f));

  // Footer: the light grey bar with the button legend.
  dl->AddRectFilled(ImVec2(bx0, footer_y0), ImVec2(x1, footer_y1), Col(212, 215, 220, in), 6.0f * s,
                    ImDrawFlags_RoundCornersBottom);

  // Header: dark navy pill with a pale rim and the yellow outlined title.
  const float header_r = (header_y1 - header_y0) * 0.5f;
  dl->AddRectFilled(ImVec2(x0, header_y0), ImVec2(x1, header_y1), Col(150, 166, 186, in), header_r);
  dl->AddRectFilled(ImVec2(x0 + 4.0f * s, header_y0 + 4.0f * s), ImVec2(x1 - 4.0f * s, header_y1 - 4.0f * s),
                    Col(12, 28, 52, in), header_r - 4.0f * s);
  dl->AddRectFilled(ImVec2(x0 + 4.0f * s, header_y0 + 4.0f * s), ImVec2(x1 - 4.0f * s, header_cy),
                    Col(40, 74, 118, 0.35f * in), header_r - 4.0f * s, ImDrawFlags_RoundCornersTop);
  const float title_px = 44.0f * s;
  AddOutlinedText(dl, achievement_ui::HeadlineFont(title_px), title_px,
                  ImVec2(x0 + 34.0f * s, CapsTop(header_cy, title_px)), Col(255, 204, 0, in),
                  Col(16, 18, 28, in), 2.6f * s, "PC SETTINGS", kCondensed);

  // Rows: label on the left, value on the right; the chosen row is yellow with arrows.
  const float label_px = 23.0f * s;
  const float value_px = 23.0f * s;
  const float big_px = 27.0f * s;
  ImFont* label_font = achievement_ui::HeadlineFont(label_px);
  ImFont* big_font = achievement_ui::HeadlineFont(big_px);
  const float row_h = 33.0f * s;
  const float section_h = 30.0f * s;
  const float rows_y0 = body_y0 + 16.0f * s;
  const float label_x = bx0 + 30.0f * s;
  const float value_right = x1 - 34.0f * s;
  auto row_center = [&](int i) {
    return rows_y0 + row_h * (float(i) + 0.5f) + (i >= kCheckUpdates ? section_h : 0.0f);
  };
  // UPDATES section heading.
  {
    const float cy = rows_y0 + row_h * float(kCheckUpdates) + section_h * 0.55f;
    const float px = 17.0f * s;
    AddText(dl, label_font, px, ImVec2(label_x, CapsTop(cy, px)), Col(128, 194, 238, in), "UPDATES", kCondensed);
    const float w = TextWidth(label_font, px, "UPDATES", kCondensed);
    dl->AddLine(ImVec2(label_x + w + 10.0f * s, cy), ImVec2(value_right, cy), Col(128, 194, 238, 0.35f * in),
                1.5f * s);
  }
  const std::string version_value =
      InstalledVersion().empty() ? "DEV BUILD"
      : g_update                 ? "UPDATE TO " + g_update->version
                                 : InstalledVersion();
  for (int i = 0; i < int(g_rows.size()); ++i) {
    const Row& row = g_rows[i];
    const bool selected = i == selected_;
    const float cy = row_center(i);
    if (selected) {
      dl->AddRectFilledMultiColor(ImVec2(bx0 + 10.0f * s, cy - row_h * 0.5f), ImVec2(x1 - 10.0f * s, cy + row_h * 0.5f),
                                  Col(255, 255, 255, 0.10f * in), Col(255, 255, 255, 0.02f * in),
                                  Col(255, 255, 255, 0.02f * in), Col(255, 255, 255, 0.10f * in));
    }
    std::string label = row.label;
    if (row.apply == Apply::kRestart) label += " *";
    const ImU32 label_col = selected ? Col(255, 204, 0, in) : Col(222, 224, 228, in);
    AddText(dl, label_font, label_px, ImVec2(label_x, CapsTop(cy, label_px)), label_col, label.c_str(),
            kCondensed);

    const char* value = row.choices.empty() ? version_value.c_str() : row.choices[row.current].label;
    float value_left = 0.0f;
    if (i == kVersion) {
      // No arrows: the version is not a choice. An available update is yellow, with A to start it.
      const bool update = bool(g_update);
      ImFont* font = selected ? big_font : label_font;
      const float px = selected ? big_px : value_px;
      const float w = TextWidth(font, px, value, kCondensed);
      value_left = value_right - w;
      const ImU32 col = update ? Col(255, 204, 0, in) : Col(222, 224, 228, in);
      if (selected && update) {
        AddOutlinedText(dl, font, px, ImVec2(value_left, CapsTop(cy, px)), col, Col(16, 18, 28, in), 1.8f * s,
                        value, kCondensed);
        const float r = 11.0f * s;
        FaceButton(dl, ImVec2(value_left - 10.0f * s - r, cy), r, Col(90, 186, 40, in), "A", in);
      } else {
        AddText(dl, font, px, ImVec2(value_left, CapsTop(cy, px)), col, value, kCondensed);
      }
    } else if (selected) {
      const float w = TextWidth(big_font, big_px, value, kCondensed);
      const float arrow = 13.0f * s;
      const float right_arrow_x = value_right - arrow * 0.5f;
      const float text_right = right_arrow_x - arrow - 6.0f * s;
      Arrow(dl, ImVec2(right_arrow_x, cy), arrow, true, Col(150, 156, 166, in));
      AddOutlinedText(dl, big_font, big_px, ImVec2(text_right - w, CapsTop(cy, big_px)), Col(255, 204, 0, in),
                      Col(16, 18, 28, in), 1.8f * s, value, kCondensed);
      Arrow(dl, ImVec2(text_right - w - arrow - 6.0f * s, cy), arrow, false, Col(150, 156, 166, in));
      value_left = text_right - w - arrow * 1.5f - 6.0f * s;
    } else {
      const float w = TextWidth(label_font, value_px, value, kCondensed);
      AddText(dl, label_font, value_px, ImVec2(value_right - w, CapsTop(cy, value_px)), Col(222, 224, 228, in),
              value, kCondensed);
      value_left = value_right - w;
    }
    // A changed restart setting is marked on its row until the game is restarted.
    if (row.apply == Apply::kRestart && row.current != row.running) {
      const float tag_px = 15.0f * s;
      const char* tag = "AFTER RESTART";
      const float w = TextWidth(label_font, tag_px, tag, kCondensed);
      AddText(dl, label_font, tag_px, ImVec2(value_left - 14.0f * s - w, CapsTop(cy, tag_px)),
              Col(255, 204, 0, 0.85f * in), tag, kCondensed);
    }
  }

  // Description of the chosen row, and whether its change waits for a restart.
  {
    const Row& row = g_rows[selected_];
    const float help_y = row_center(int(g_rows.size()) - 1) + row_h * 0.5f + 18.0f * s;
    dl->AddLine(ImVec2(bx0 + 24.0f * s, help_y - 8.0f * s), ImVec2(x1 - 24.0f * s, help_y - 8.0f * s),
                Col(255, 255, 255, 0.18f * in), 1.5f * s);
    const float help_px = 19.0f * s;
    ImFont* body = achievement_ui::BodyFont();
    const float status_top = body_y1 - 32.0f * s;
    if (selected_ == kVersion) {
      // The installed version, or the release notes of the available update.
      float y = help_y;
      const float line_h = help_px * 1.3f;
      auto line = [&](const std::string& text, ImU32 col) {
        if (y + line_h > status_top) return;
        const float wrap = value_right - label_x;
        const float h = body->CalcTextSizeA(help_px, FLT_MAX, wrap, text.c_str()).y;
        if (y + h > status_top) return;
        AddText(dl, body, help_px, ImVec2(label_x, y), col, text.c_str(), 1.0f, wrap);
        y += h + help_px * 0.3f;
      };
      if (InstalledVersion().empty()) {
        line("Development build: updates are not offered.", Col(176, 182, 192, in));
      } else if (!g_update) {
        line("Installed version " + InstalledVersion() + ". New versions are found when the game starts.",
             Col(176, 182, 192, in));
      } else {
        std::string heading = g_update->title.empty() ? "Version " + g_update->version
                                                      : g_update->version + ": " + g_update->title;
        if (!g_update->published.empty()) heading += "  (" + g_update->published + ")";
        line(heading, Col(235, 237, 240, in));
        for (const auto& note : g_update->notes) line("- " + note, Col(176, 182, 192, in));
      }
    } else {
      AddText(dl, body, help_px, ImVec2(label_x, help_y), Col(176, 182, 192, in), row.help, 1.0f,
              value_right - label_x);
    }
    const char* status = nullptr;
    ImU32 status_col = Col(255, 204, 0, in);
    if (!notice_.empty() && ImGui::GetTime() < notice_until_) {
      status = notice_.c_str();
      status_col = Col(128, 194, 238, in);
    } else if (!save_error_.empty()) {
      status = save_error_.c_str();
      status_col = Col(240, 90, 80, in);
    } else if (row.apply == Apply::kRestart && row.current != row.running) {
      status = "APPLIES AFTER RESTART";
    } else if (row.apply == Apply::kLive) {
      status = "APPLIES NOW";
      status_col = Col(128, 194, 238, in);
    } else if (row.apply == Apply::kSaved) {
      status = "SAVED; USED WHEN THE GAME STARTS";
      status_col = Col(128, 194, 238, in);
    } else if (row.apply == Apply::kInfo && g_update) {
      status = "PRESS A TO UPDATE";
    }
    const float status_px = 19.0f * s;
    if (status) {
      AddText(dl, label_font, status_px, ImVec2(label_x, body_y1 - 32.0f * s), status_col, status, kCondensed);
    }
    const char* legend = "* APPLIES AFTER RESTART";
    AddText(dl, label_font, status_px,
            ImVec2(value_right - TextWidth(label_font, status_px, legend, kCondensed), body_y1 - 32.0f * s),
            Col(150, 156, 166, in), legend, kCondensed);
  }

  // Footer legend.
  {
    const float cy = (footer_y0 + footer_y1) * 0.5f;
    const float px = 21.0f * s;
    ImFont* font = achievement_ui::HeadlineFont(px);
    const ImU32 text = Col(28, 30, 36, in);
    const float r = 12.0f * s;
    float x = x1 - 26.0f * s - r;
    FaceButton(dl, ImVec2(x, cy), r, Col(210, 44, 44, in), "B", in);
    x -= r + 8.0f * s;
    AddText(dl, font, px, ImVec2(x - TextWidth(font, px, "BACK", kCondensed), CapsTop(cy, px)), text, "BACK",
            kCondensed);
    x -= TextWidth(font, px, "BACK", kCondensed) + 26.0f * s;
    const float arrow = 11.0f * s;
    AddText(dl, font, px, ImVec2(x - TextWidth(font, px, "CHANGE", kCondensed), CapsTop(cy, px)), text,
            "CHANGE", kCondensed);
    x -= TextWidth(font, px, "CHANGE", kCondensed) + 10.0f * s;
    Arrow(dl, ImVec2(x - arrow * 0.5f, cy), arrow, true, text);
    Arrow(dl, ImVec2(x - arrow * 2.0f, cy), arrow, false, text);
    // Left: the game's own "(X) RESTORE DEFAULTS".
    const float lx = bx0 + 24.0f * s + r;
    FaceButton(dl, ImVec2(lx, cy), r, Col(40, 110, 210, in), "X", in);
    AddText(dl, font, px, ImVec2(lx + r + 8.0f * s, CapsTop(cy, px)), text, "RESTORE DEFAULTS", kCondensed);
  }

  if (confirm_ && g_update) {
    DrawConfirm(dl, s, X(582), (header_y0 + footer_y1) * 0.5f, in);
  }

  Rotate(dl, first_vertex, ImVec2((x0 + x1) * 0.5f, (header_y0 + footer_y1) * 0.5f), -0.03f);
}

// "Update now?" box over the page, in the game's message style.
void SettingsPage::DrawConfirm(ImDrawList* dl, float s, float cx, float cy, float in) {
  const float t = std::clamp(float(ImGui::GetTime() - confirm_at_) / 0.15f, 0.0f, 1.0f) * in;
  const float w = 520.0f * s, body_h = 150.0f * s, head_h = 54.0f * s, foot_h = 40.0f * s;
  const float x0 = cx - w * 0.5f, x1 = cx + w * 0.5f;
  const float y0 = cy - (head_h + body_h + foot_h) * 0.5f;
  const float head_y1 = y0 + head_h, body_y1 = head_y1 + body_h, foot_y1 = body_y1 + foot_h;
  // Dim the page behind the box.
  dl->AddRectFilled(ImVec2(x0 - 200.0f * s, y0 - 400.0f * s), ImVec2(x1 + 200.0f * s, foot_y1 + 400.0f * s),
                    Col(0, 0, 0, 0.55f * t));
  dl->AddRectFilled(ImVec2(x0 + 14.0f * s, head_y1 - 6.0f * s), ImVec2(x1, body_y1), Col(6, 8, 12, t));
  dl->AddRectFilled(ImVec2(x0 + 14.0f * s, body_y1), ImVec2(x1, foot_y1), Col(212, 215, 220, t), 6.0f * s,
                    ImDrawFlags_RoundCornersBottom);
  const float r = head_h * 0.5f;
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, head_y1), Col(150, 166, 186, t), r);
  dl->AddRectFilled(ImVec2(x0 + 4.0f * s, y0 + 4.0f * s), ImVec2(x1 - 4.0f * s, head_y1 - 4.0f * s),
                    Col(12, 28, 52, t), r - 4.0f * s);
  const float title_px = 36.0f * s;
  AddOutlinedText(dl, achievement_ui::HeadlineFont(title_px), title_px,
                  ImVec2(x0 + 28.0f * s, CapsTop((y0 + head_y1) * 0.5f, title_px)), Col(255, 204, 0, t),
                  Col(16, 18, 28, t), 2.2f * s, ("UPDATE TO " + g_update->version + "?").c_str(), kCondensed);
  const float text_px = 21.0f * s;
  ImFont* body = achievement_ui::BodyFont();
  AddText(dl, body, text_px, ImVec2(x0 + 40.0f * s, head_y1 + 22.0f * s), Col(235, 237, 240, t),
          "Update now? The game closes, updates (about a minute) and starts again.", 1.0f, w - 70.0f * s);
  if (!g_update->title.empty()) {
    AddText(dl, body, text_px * 0.9f, ImVec2(x0 + 40.0f * s, body_y1 - 40.0f * s), Col(176, 182, 192, t),
            g_update->title.c_str(), 1.0f, w - 70.0f * s);
  }
  // Footer: (A) UPDATE NOW on the right, (B) LATER next to it.
  const float fy = (body_y1 + foot_y1) * 0.5f;
  const float px = 20.0f * s;
  ImFont* font = achievement_ui::HeadlineFont(px);
  const ImU32 text = Col(28, 30, 36, t);
  const float br = 11.0f * s;
  float x = x1 - 22.0f * s - br;
  FaceButton(dl, ImVec2(x, fy), br, Col(210, 44, 44, t), "B", t);
  x -= br + 8.0f * s;
  AddText(dl, font, px, ImVec2(x - TextWidth(font, px, "LATER", kCondensed), CapsTop(fy, px)), text, "LATER",
          kCondensed);
  x -= TextWidth(font, px, "LATER", kCondensed) + 26.0f * s + br;
  FaceButton(dl, ImVec2(x, fy), br, Col(90, 186, 40, t), "A", t);
  x -= br + 8.0f * s;
  AddText(dl, font, px, ImVec2(x - TextWidth(font, px, "UPDATE NOW", kCondensed), CapsTop(fy, px)), text,
          "UPDATE NOW", kCondensed);
}

// ==============================================================================================
// Update popup
// ==============================================================================================
UpdateToast::UpdateToast(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

UpdateToast::~UpdateToast() = default;

void UpdateToast::OnDraw(ImGuiIO& io) {
  PollUpdate();
  constexpr double kEnter = 0.55, kHold = 8.0, kExit = 0.45;
  const double now = ImGui::GetTime();
  const bool blocked = !InFrontEnd() || IsOpen() || achievement_ui::IsPanelOpen();
  if (started_ < 0.0) {
    // Once per new version per session, only on the front end with no page open.
    if (!g_update || g_update->version == shown_version_ || blocked) return;
    shown_version_ = g_update->version;
    started_ = now;
    REXLOG_INFO("Tabletennis: update popup for {}", shown_version_);
  }
  const double t = now - started_;
  if (blocked || t > kEnter + kHold + kExit) {
    started_ = -1.0;
    return;
  }
  const float enter = float(std::clamp(t / kEnter, 0.0, 1.0));
  const float exit = float(std::clamp((t - kEnter - kHold) / kExit, 0.0, 1.0));
  const float ease_in = 1.0f - (1.0f - enter) * (1.0f - enter) * (1.0f - enter);
  const float alpha = ease_in * (1.0f - exit);

  // Bottom right of the game picture, like the achievement popup.
  const double configured = REXCVAR_GET(display_aspect);
  const float aspect = configured > 0.0 ? float(configured) : 16.0f / 9.0f;
  const float pic_w = std::min(io.DisplaySize.x, io.DisplaySize.y * aspect);
  const float pic_h = pic_w / aspect;
  const float pic_x = (io.DisplaySize.x - pic_w) * 0.5f;
  const float pic_y = (io.DisplaySize.y - pic_h) * 0.5f;
  // Never smaller than about 275 px wide, so the small lines stay readable in small windows.
  const float s = std::max(pic_h / 720.0f * 0.8f, 0.55f);
  const float w = 500.0f * s, h = 118.0f * s;
  const float margin = 30.0f * s;
  const float x1 = pic_x + pic_w - margin;
  const float x0 = x1 - w;
  const float slide = (1.0f - ease_in) * (h + margin) + exit * 20.0f * s;
  const float y0 = pic_y + pic_h - margin - h + slide;
  const float y1 = y0 + h;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float r = h * 0.5f;
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), Col(150, 166, 186, alpha), r);
  dl->AddRectFilled(ImVec2(x0 + 4.0f * s, y0 + 4.0f * s), ImVec2(x1 - 4.0f * s, y1 - 4.0f * s),
                    Col(12, 28, 52, alpha), r - 4.0f * s);
  dl->AddRectFilled(ImVec2(x0 + 4.0f * s, y0 + 4.0f * s), ImVec2(x1 - 4.0f * s, (y0 + y1) * 0.5f),
                    Col(40, 74, 118, 0.35f * alpha), r - 4.0f * s, ImDrawFlags_RoundCornersTop);
  // Badge: yellow disc with a downward arrow.
  const ImVec2 badge(x0 + r, (y0 + y1) * 0.5f);
  const float br = r - 14.0f * s;
  dl->AddCircleFilled(badge, br, Col(16, 18, 28, alpha), 32);
  dl->AddCircleFilled(badge, br * 0.88f, Col(255, 204, 0, alpha), 32);
  const ImU32 ink = Col(16, 18, 28, alpha);
  dl->AddRectFilled(ImVec2(badge.x - br * 0.14f, badge.y - br * 0.5f), ImVec2(badge.x + br * 0.14f, badge.y + br * 0.05f),
                    ink);
  dl->AddTriangleFilled(ImVec2(badge.x - br * 0.4f, badge.y), ImVec2(badge.x + br * 0.4f, badge.y),
                        ImVec2(badge.x, badge.y + br * 0.45f), ink);
  dl->AddRectFilled(ImVec2(badge.x - br * 0.45f, badge.y + br * 0.55f), ImVec2(badge.x + br * 0.45f, badge.y + br * 0.66f),
                    ink);
  // Text block.
  const float tx = badge.x + br + 18.0f * s;
  const float text_w = x1 - 24.0f * s - tx;
  const float cap_px = 18.0f * s, head_px = 34.0f * s, title_px = 19.0f * s, hint_px = 17.0f * s;
  ImFont* heavy_small = achievement_ui::HeadlineFont(cap_px);
  AddText(dl, heavy_small, cap_px, ImVec2(tx, y0 + 12.0f * s), Col(128, 194, 238, alpha), "UPDATE AVAILABLE",
          kCondensed);
  AddOutlinedText(dl, achievement_ui::HeadlineFont(head_px), head_px, ImVec2(tx, y0 + 28.0f * s),
                  Col(255, 204, 0, alpha), Col(16, 18, 28, alpha), 2.0f * s,
                  ("VERSION " + shown_version_).c_str(), kCondensed);
  if (g_update && !g_update->title.empty()) {
    std::string title = Upper(g_update->title);
    while (title.size() > 4 && TextWidth(heavy_small, title_px, title.c_str(), kCondensed) > text_w) {
      title.resize(title.size() - 4);
      title += "...";
    }
    AddText(dl, heavy_small, title_px, ImVec2(tx, y0 + 66.0f * s), Col(235, 237, 240, alpha), title.c_str(),
            kCondensed);
  }
  AddText(dl, heavy_small, hint_px, ImVec2(tx, y0 + 89.0f * s), Col(158, 164, 174, alpha),
          "OPTIONS > PC SETTINGS TO UPDATE", kCondensed);
}

}  // namespace tabletennis::pc_settings

// The in-match pause menu's per-frame update (r3 = the pause object). The page is opened from
// here when its OPTIONS list raises GLOBAL_PC_SETTINGS; the game's update then runs as usual.
extern "C" void __imp__sub_823132F0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_823132F0(PPCContext& ctx, uint8_t* base) {
  tabletennis::pc_settings::PollPause(ctx, base, ctx.r3.u32);
  __imp__sub_823132F0(ctx, base);
}
