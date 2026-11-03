// pc_settings.cpp - see pc_settings.h.

#include "pc_settings.h"

#include "generated/default/tabletennis_init.h"
#include "haptics.h"
#include "intro_skip.h"
#include "system_link.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xsocket.h>
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
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

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
  kResolution, kFullscreen, kPicture, kAntiAliasing, kSharpening, kVsync, kShowFps, kHaptics,
  kAchievementSound,
  // ONLINE section
  kPlayerName, kOnlineServer, kOnlineStatus,
  // UPDATES section
  kCheckUpdates, kAutoUpdate, kCheckNow, kVersion,
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
      {"aa", "ANTI-ALIASING", "Smooths jagged edges with FXAA after each frame.", Apply::kLive,
       {{"none", "OFF"}, {"fxaa", "FXAA"}, {"fxaa_extreme", "FXAA EXTREME"}},
       0},
      {"post", "SHARPENING", "CAS sharpens the picture (best on the Steam Deck); FSR upscales "
       "it to large screens and sharpens it.", Apply::kLive,
       {{"bilinear", "OFF"}, {"cas", "CAS"}, {"fsr", "FSR"}},
       0},
      {"vsync", "VSYNC", "Waits for the display before showing each frame, so the picture does not "
       "tear. AUTO is on for macOS and off elsewhere (lowest latency).", Apply::kRestart,
       {{"auto", "AUTO"}, {"on", "ON"}, {"off", "OFF"}},
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
      {"player_name", "PLAYER NAME", "The name other players see online and in SYSTEM LINK. Press A "
       "to change it (player_name= in tt-recomp.ini).", Apply::kRestart, {}, 0},
      {"", "ONLINE SERVER", "The server ONLINE in the main menu plays through (server= in "
       "tt-recomp.ini).", Apply::kInfo, {}, 0},
      {"", "ONLINE STATUS", "", Apply::kInfo, {}, 0},
      {"check_updates", "CHECK AT START", "Look for a new version each time the game starts.",
       Apply::kSaved,
       {{"false", "OFF"}, {"true", "ON"}},
       1},
      {"auto_update", "AUTOMATIC UPDATES", "Install new versions when the game starts.",
       Apply::kSaved,
       {{"false", "OFF"}, {"true", "ON"}},
       0},
      {"", "CHECK NOW", "", Apply::kInfo, {}, 0},
      {"", "VERSION", "", Apply::kInfo, {}, 0},
  };
}

// UI thread only.
std::vector<Row> g_rows;
bool g_loaded = false;
std::string g_room_code;  // this session's private room code (ONLINE > PLAY WITH A FRIEND)

// PLAYER NAME: player_name= in the ini ("" = the launcher's automatic name), and the value the
// game started with. The game reads the name into its profile once, so a change needs a restart.
// The game tells network players apart only by name; the online server refuses a name that is
// already in the room.
std::string g_player_name;
std::string g_player_name_running;
constexpr const char* kNameChars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr int kNameMax = 15;  // the Xbox gamertag limit
bool IsNameChar(unsigned c) { return c && c < 128 && std::strchr(kNameChars, int(c)); }

// CHECK NOW: the launcher's update check (tools/tt-update.sh --check-only), run from the game.
enum class Check { kIdle, kRunning, kUpToDate, kFound, kFailed };
Check g_check = Check::kIdle;
double g_check_started = 0.0;
std::string g_check_error;
#if !defined(_WIN32)
pid_t g_check_pid = -1;
#endif

constexpr const char* kRoomAlphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";  // no 0/O, 1/I/L
constexpr int kRoomLength = 6;

bool ValidRoomCode(const std::string& code);

std::string NewRoomCode() {
  // Test hook: TT_TEST_ROOM_CODE fixes the code, so a scripted second instance can enter it.
  if (const char* fixed = std::getenv("TT_TEST_ROOM_CODE"); fixed && ValidRoomCode(fixed)) return fixed;
  std::random_device random;
  std::string code;
  const int n = int(std::strlen(kRoomAlphabet));
  for (int i = 0; i < kRoomLength; ++i) code += kRoomAlphabet[random() % n];
  return code;
}

bool ValidRoomCode(const std::string& code) {
  return code.size() == size_t(kRoomLength) &&
         std::all_of(code.begin(), code.end(), [](char c) { return std::strchr(kRoomAlphabet, c); });
}

// Value and description of the STATUS row, from the online client.
std::string OnlineStatusText(std::string* help) {
  const auto status = rex::system::net::GetOnlineStatus();
  if (status.mode != "public" && status.mode != "room") {
    *help = "Not online. ONLINE in the main menu connects; SYSTEM LINK plays on the local network.";
    return "NOT CONNECTED";
  }
  if (!status.connected) {
    *help = status.state == "NO SERVER SET"
                ? "No online server is set (server= in tt-recomp.ini)."
                : "Connecting to the online server.";
    return status.state;
  }
  const std::string where = status.mode == "room" ? "ROOM " + status.room : "PUBLIC";
  *help = "Connected. " + std::to_string(status.players_online) + " players online. Matches with " +
          std::to_string(status.direct_peers) + " player(s) run directly, " +
          std::to_string(status.relayed_peers) + " through the server.";
  if (status.server_rtt_ms >= 0) *help += " Server: " + std::to_string(status.server_rtt_ms) + " ms.";
  if (status.opponent >= 0 && status.peers[size_t(status.opponent)].rtt_ms >= 0) {
    const auto& peer = status.peers[size_t(status.opponent)];
    *help += " " + peer.name + ": " + std::to_string(peer.rtt_ms) + " ms.";
  }
  return where + ": " + std::to_string(status.room_players) +
         (status.room_players == 1 ? " PLAYER" : " PLAYERS");
}

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

std::filesystem::path UpdaterScript() { return InstallDir() / "tools" / "tt-update.sh"; }

// CHECK NOW needs an installed version (not a development build) with its updater. The Windows
// launcher has its own update path, so the page does not offer it there.
bool CanCheckForUpdates() {
#if defined(_WIN32)
  return false;
#else
  std::error_code ec;
  return !InstalledVersion().empty() && std::filesystem::exists(UpdaterScript(), ec);
#endif
}

// The last "@@error ..." line the updater printed, for the page's help text.
std::string UpdaterError(const std::filesystem::path& log) {
  std::istringstream text(ReadFile(log));
  std::string line, error;
  while (std::getline(text, line)) {
    if (line.rfind("@@error ", 0) == 0) error = line.substr(8);
  }
  return error;
}

std::filesystem::path UpdateCheckLog() { return InstallDir() / "logs" / "update-check.log"; }

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
std::atomic<bool> g_prompt_open{false};  // ONLINE prompt (OnlinePrompt)
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

// The ONLINE prompt captures the pad like the page. Closing with B can leave the B to the game
// (it then backs out of the online menu to the main menu).
void SetPromptOpen(bool open, bool pass_b_to_game = false) {
  std::lock_guard lock(g_pad_mutex);
  for (auto& user : g_users) {
    user.ignore = open ? user.last : 0;
    user.swallow_until_release = !open && !pass_b_to_game;
    if (!open && pass_b_to_game) user.last &= uint16_t(~kPadB);
  }
  g_pressed.store(0);
  g_held.store(0);
  g_prompt_open.store(open);
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
  lan::PollFrontend(ctx, base, movie);
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
  if (g_open.load() || g_prompt_open.load()) {
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
    const auto name = ini.find("player_name");
    g_player_name = g_player_name_running = name == ini.end() ? std::string() : name->second;
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
    case kAntiAliasing:
      if (hooks_.set_anti_aliasing) hooks_.set_anti_aliasing(g_rows[row_index].choices[g_rows[row_index].current].value);
      break;
    case kSharpening:
      if (hooks_.set_sharpening) hooks_.set_sharpening(g_rows[row_index].choices[g_rows[row_index].current].value);
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
  // player_name is only written once it was changed here, or when the file already has it.
  bool name_written = false;
  for (auto& line : lines) {
    const auto key_end = line.find('=');
    if (key_end == std::string::npos) continue;
    const std::string key = Trim(line.substr(0, key_end));
    if (key == "player_name") {
      if (!name_written) line = "player_name=" + g_player_name;
      name_written = true;
      continue;
    }
    for (size_t i = 0; i < g_rows.size(); ++i) {
      if (g_rows[i].choices.empty() || key != g_rows[i].key) continue;
      if (!written[i]) line = key + "=" + g_rows[i].choices[g_rows[i].current].value;
      written[i] = true;
    }
  }
  for (size_t i = 0; i < g_rows.size(); ++i) {
    if (!written[i] && !g_rows[i].choices.empty()) lines.push_back(std::string(g_rows[i].key) + "=" + g_rows[i].choices[g_rows[i].current].value);
  }
  if (!name_written && g_player_name != g_player_name_running) lines.push_back("player_name=" + g_player_name);

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
  PollUpdateCheck();  // also while the page is closed, so a started check always finishes
  if (!g_open.load()) {
    shown_ = false;
    confirm_ = false;
    if (naming_ && window_) window_->SetTextInputActive(false);
    naming_ = false;
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
  // So does the name editor.
  if (naming_) {
    EditName(io, pressed, held, now);
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
    if (selected_ == kOnlineServer || selected_ == kOnlineStatus) {
    } else if (selected_ == kPlayerName) {
      OpenNameEditor();
    } else if (selected_ == kCheckNow) {
      StartUpdateCheck();
    } else if (selected_ == kVersion) {
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
  const float body_y0 = header_y1 - 6.0f * s, body_y1 = Y(660 + rise);
  const float footer_y0 = body_y1, footer_y1 = Y(700 + rise);

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
  const float row_h = 26.0f * s;
  const float section_h = 22.0f * s;
  const float rows_y0 = body_y0 + 16.0f * s;
  const float label_x = bx0 + 30.0f * s;
  const float value_right = x1 - 34.0f * s;
  auto row_center = [&](int i) {
    return rows_y0 + row_h * (float(i) + 0.5f) + (i >= kPlayerName ? section_h : 0.0f) +
           (i >= kCheckUpdates ? section_h : 0.0f);
  };
  // Section headings.
  for (const auto& [first, title] : {std::pair<int, const char*>{kPlayerName, "ONLINE"},
                                     std::pair<int, const char*>{kCheckUpdates, "UPDATES"}}) {
    const float cy = row_center(first) - row_h * 0.5f - section_h * 0.45f;
    const float px = 17.0f * s;
    AddText(dl, label_font, px, ImVec2(label_x, CapsTop(cy, px)), Col(128, 194, 238, in), title, kCondensed);
    const float w = TextWidth(label_font, px, title, kCondensed);
    dl->AddLine(ImVec2(label_x + w + 10.0f * s, cy), ImVec2(value_right, cy), Col(128, 194, 238, 0.35f * in),
                1.5f * s);
  }
  std::string status_help;
  const std::string status_value = OnlineStatusText(&status_help);
  std::string server_value = rex::cvar::GetFlagByName("online_server");
  for (auto& c : server_value) c = char(std::toupper(static_cast<unsigned char>(c)));
  if (server_value.empty()) server_value = "NOT SET";
  const std::string version_value =
      InstalledVersion().empty() ? "DEV BUILD"
      : g_update                 ? "UPDATE TO " + g_update->version
                                 : InstalledVersion();
  // The name in use; after it was cleared, the launcher picks the automatic name at the next start.
  const std::string auto_name = rex::cvar::GetFlagByName("user_name");
  const std::string name_value = !g_player_name.empty() ? g_player_name
                                 : g_player_name_running.empty() && !auto_name.empty() ? auto_name
                                                                                       : "AUTOMATIC";
  const bool can_check = CanCheckForUpdates();
  const std::string check_value = !can_check                   ? "NOT AVAILABLE"
                                  : g_check == Check::kRunning  ? "CHECKING..."
                                  : g_check == Check::kUpToDate ? "UP TO DATE"
                                  : g_check == Check::kFound && g_update ? "FOUND " + g_update->version
                                  : g_check == Check::kFailed   ? "FAILED"
                                                                : "CHECK";
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

    const char* value = !row.choices.empty() ? row.choices[row.current].label
                        : i == kPlayerName   ? name_value.c_str()
                        : i == kOnlineServer ? server_value.c_str()
                        : i == kOnlineStatus ? status_value.c_str()
                        : i == kCheckNow     ? check_value.c_str()
                                             : version_value.c_str();
    const bool pending = row.apply == Apply::kRestart &&
                         (i == kPlayerName ? g_player_name != g_player_name_running : row.current != row.running);
    float value_left = 0.0f;
    if (row.choices.empty()) {
      // No arrows: these are not choices. Rows that A acts on (an available update, the name,
      // CHECK NOW) show the A button when chosen; an available update is yellow.
      const bool update = i == kVersion && bool(g_update);
      const bool action = update || i == kPlayerName ||
                          (i == kCheckNow && can_check && g_check != Check::kRunning);
      ImFont* font = selected ? big_font : label_font;
      const float px = selected ? big_px : value_px;
      const float w = TextWidth(font, px, value, kCondensed);
      value_left = value_right - w;
      const ImU32 col = update || (selected && action) ? Col(255, 204, 0, in) : Col(222, 224, 228, in);
      if (selected && action) {
        AddOutlinedText(dl, font, px, ImVec2(value_left, CapsTop(cy, px)), col, Col(16, 18, 28, in), 1.8f * s,
                        value, kCondensed);
        const float r = 11.0f * s;
        FaceButton(dl, ImVec2(value_left - 10.0f * s - r, cy), r, Col(90, 186, 40, in), "A", in);
        value_left -= 10.0f * s + 2.0f * r;  // keep the AFTER RESTART tag clear of the button
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
    if (pending) {
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
        line("Installed version " + InstalledVersion() + ". New versions are found when the game starts, "
             "or with CHECK NOW.", Col(176, 182, 192, in));
      } else {
        std::string heading = g_update->title.empty() ? "Version " + g_update->version
                                                      : g_update->version + ": " + g_update->title;
        if (!g_update->published.empty()) heading += "  (" + g_update->published + ")";
        line(heading, Col(235, 237, 240, in));
        for (const auto& note : g_update->notes) line("- " + note, Col(176, 182, 192, in));
      }
    } else {
      std::string extra;
      if (selected_ == kPlayerName) {
        extra = std::string(row.help) + " Two players in one room cannot use the same name.";
      } else if (selected_ == kCheckNow) {
        extra = !can_check                    ? (InstalledVersion().empty()
                                                     ? "Development build: updates are not offered."
                                                     : "Checking from the game is not available here.")
                : g_check == Check::kRunning  ? "Looking for a new version..."
                : g_check == Check::kUpToDate ? "Version " + InstalledVersion() + " is the latest."
                : g_check == Check::kFound && g_update ? "Version " + g_update->version +
                                                             " is available: see VERSION below, or press A on it."
                : g_check == Check::kFailed   ? g_check_error
                                              : "Look for a new version now.";
      }
      const char* help = selected_ == kOnlineStatus ? status_help.c_str()
                         : !extra.empty()           ? extra.c_str()
                                                    : row.help;
      AddText(dl, body, help_px, ImVec2(label_x, help_y), Col(176, 182, 192, in), help, 1.0f,
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
    } else if (row.apply == Apply::kRestart &&
               (selected_ == kPlayerName ? g_player_name != g_player_name_running : row.current != row.running)) {
      status = "APPLIES AFTER RESTART";
    } else if (row.apply == Apply::kLive) {
      status = "APPLIES NOW";
      status_col = Col(128, 194, 238, in);
    } else if (row.apply == Apply::kSaved) {
      status = "SAVED; USED WHEN THE GAME STARTS";
      status_col = Col(128, 194, 238, in);
    } else if (selected_ == kVersion && g_update) {
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
  if (naming_) {
    DrawNameEditor(dl, s, X(582), (header_y0 + footer_y1) * 0.5f, in);
  }

  Rotate(dl, first_vertex, ImVec2((x0 + x1) * 0.5f, (header_y0 + footer_y1) * 0.5f), -0.03f);
}

// --- CHECK NOW ----------------------------------------------------------------------------------
// The check runs as its own process (network timeouts of a few seconds) while the game keeps
// drawing; PollUpdateCheck collects it. It writes .tt-update.json exactly like the launcher's
// check at start, and the result is read the same way.
void SettingsPage::StartUpdateCheck() {
#if !defined(_WIN32)
  if (!CanCheckForUpdates() || g_check == Check::kRunning) return;
  std::error_code ec;
  std::filesystem::create_directories(UpdateCheckLog().parent_path(), ec);
  posix_spawn_file_actions_t files;
  posix_spawn_file_actions_init(&files);
  posix_spawn_file_actions_addopen(&files, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&files, 1, UpdateCheckLog().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_adddup2(&files, 1, 2);
  const std::string script = UpdaterScript().string(), dir = InstallDir().string();
  char* argv[] = {const_cast<char*>("bash"), const_cast<char*>(script.c_str()),
                  const_cast<char*>(dir.c_str()), const_cast<char*>("--check-only"), nullptr};
  // Without LD_PRELOAD: Steam's overlay library would otherwise be loaded into every tool the
  // script runs (the launcher clears it for the updater the same way).
  std::vector<char*> env;
  for (char** e = environ; *e; ++e) {
    if (std::strncmp(*e, "LD_PRELOAD=", 11) != 0) env.push_back(*e);
  }
  env.push_back(nullptr);
  pid_t pid = -1;
  const int err = posix_spawnp(&pid, "bash", &files, nullptr, argv, env.data());
  posix_spawn_file_actions_destroy(&files);
  if (err != 0) {
    g_check = Check::kFailed;
    g_check_error = "The update check could not be started.";
    REXLOG_WARN("Tabletennis: cannot start {}: {}", script, std::strerror(err));
    return;
  }
  g_check_pid = pid;
  g_check = Check::kRunning;
  g_check_started = ImGui::GetTime();
  REXLOG_INFO("Tabletennis: checking for updates (CHECK NOW)");
#endif
}

void SettingsPage::PollUpdateCheck() {
#if !defined(_WIN32)
  if (g_check != Check::kRunning) return;
  int status = 0;
  const pid_t done = waitpid(g_check_pid, &status, WNOHANG);
  if (done == 0) {
    if (ImGui::GetTime() - g_check_started < 45.0) return;
    kill(g_check_pid, SIGTERM);
    waitpid(g_check_pid, &status, 0);
    g_check_pid = -1;
    g_check = Check::kFailed;
    g_check_error = "The check took too long. Check your internet connection and try again.";
    REXLOG_WARN("Tabletennis: update check timed out");
    return;
  }
  g_check_pid = -1;
  const std::string error = UpdaterError(UpdateCheckLog());
  // An exit status we cannot collect (ECHILD) is judged by the log alone.
  const bool failed = done > 0 ? !WIFEXITED(status) || WEXITSTATUS(status) != 0 : !error.empty();
  if (failed) {
    g_check = Check::kFailed;
    g_check_error = !error.empty() ? error : "The update check failed (see logs/update-check.log).";
    REXLOG_WARN("Tabletennis: update check failed: {}", g_check_error);
    return;
  }
  auto info = ParseUpdate(ReadFile(InstallDir() / ".tt-update.json"));
  if (info && IsNewer(info->version, InstalledVersion())) {
    REXLOG_INFO("Tabletennis: update check found {} (installed {})", info->version, InstalledVersion());
    g_update = std::move(info);
    g_check = Check::kFound;
    if (g_open.load() && !naming_) {  // offer it right away, as at start
      confirm_ = true;
      confirm_at_ = ImGui::GetTime();
    }
  } else {
    REXLOG_INFO("Tabletennis: update check: {} is up to date", InstalledVersion());
    g_check = Check::kUpToDate;
  }
#endif
}

// --- PLAYER NAME --------------------------------------------------------------------------------
// D-pad: up/down change the letter under the cursor, left/right move (right past the end adds
// a letter), X deletes. A keyboard types and deletes (Backspace) directly. A saves, B cancels.
// Empty means the launcher's automatic name.
void SettingsPage::OpenNameEditor() {
  std::string start = g_player_name;
  if (start.empty() && g_player_name_running.empty()) start = rex::cvar::GetFlagByName("user_name");
  name_edit_.clear();
  for (char c : start) {
    if (IsNameChar(uint8_t(c)) && int(name_edit_.size()) < kNameMax) name_edit_ += c;
  }
  name_pos_ = std::max(0, int(name_edit_.size()) - 1);
  naming_ = true;
  naming_at_ = ImGui::GetTime();
  held_direction_ = 0;
  ImGui::GetIO().InputQueueCharacters.resize(0);
}

void SettingsPage::EditName(ImGuiIO& io, uint16_t pressed, uint16_t held, double now) {
  // SDL only delivers typed characters while text input is on; the engine turns it on for its
  // own text fields only, so the editor does it while it is open.
  if (window_ && !window_->IsTextInputActive()) window_->SetTextInputActive(true);
  // With keyboard controls on (--mnk_mode) letters are also pad inputs (W A S D the stick, J K L I
  // the face buttons, Backspace B): shortly after typing, the pad is ignored so a key only types.
  static double last_typing = -1.0;
  const bool typing = !io.InputQueueCharacters.empty() || ImGui::IsKeyPressed(ImGuiKey_Backspace) ||
                      ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
                      ImGui::IsKeyPressed(ImGuiKey_Escape);
  if (typing) last_typing = now;
  if (now - last_typing < 0.35) {
    pressed = 0;
    held = 0;
    held_direction_ = 0;
  }
  const int n = int(std::strlen(kNameChars));
  auto direction = [&](uint16_t button) {
    const int size = int(name_edit_.size());
    switch (button) {
      case kPadUp:
      case kPadDown: {
        if (size == 0) {
          name_edit_ = "A";
          name_pos_ = 0;
          break;
        }
        char& c = name_edit_[size_t(name_pos_)];
        const char* at = std::strchr(kNameChars, c);
        const int index = at ? int(at - kNameChars) : 0;
        c = kNameChars[(index + (button == kPadUp ? 1 : n - 1)) % n];
        break;
      }
      case kPadLeft: name_pos_ = std::max(0, name_pos_ - 1); break;
      case kPadRight:
        if (name_pos_ < size - 1) {
          ++name_pos_;
        } else if (size > 0 && size < kNameMax) {
          name_edit_ += 'A';
          name_pos_ = size;
        }
        break;
      default: break;
    }
  };
  for (uint16_t button : {kPadUp, kPadDown, kPadLeft, kPadRight}) {
    if (pressed & button) {
      direction(button);
      held_direction_ = button;
      next_repeat_ = now + 0.45;
    }
  }
  if (held_direction_ && !(held & held_direction_)) held_direction_ = 0;
  if (held_direction_ && now >= next_repeat_) {
    direction(uint16_t(held_direction_));
    next_repeat_ = now + 0.13;
  }
  auto erase = [&] {
    if (name_edit_.empty()) return;
    name_edit_.erase(size_t(name_pos_), 1);
    name_pos_ = std::clamp(name_pos_ - (name_pos_ >= int(name_edit_.size()) ? 1 : 0), 0,
                           std::max(0, int(name_edit_.size()) - 1));
  };
  // Keyboard: typed letters go after the cursor; Backspace deletes the letter under it.
  for (ImWchar c : io.InputQueueCharacters) {
    if (!IsNameChar(c) || int(name_edit_.size()) >= kNameMax) continue;
    const size_t at = name_edit_.empty() ? 0 : size_t(name_pos_) + 1;
    name_edit_.insert(at, 1, char(c));
    name_pos_ = int(at);
  }
  io.InputQueueCharacters.resize(0);
  if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) erase();
  if (pressed & kPadX) erase();
  if ((pressed & kPadA) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
    SaveName();
    naming_ = false;
  } else if ((pressed & kPadB) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    naming_ = false;
  }
  if (!naming_) {
    held_direction_ = 0;
    if (window_) window_->SetTextInputActive(false);
  }
}

void SettingsPage::SaveName() {
  if (name_edit_ == g_player_name) return;
  g_player_name = name_edit_;
  REXLOG_INFO("Tabletennis: player name set to '{}' (used from the next start)",
              g_player_name.empty() ? "automatic" : g_player_name);
  notice_ = g_player_name == g_player_name_running ? "NAME UNCHANGED" : "NAME SAVED - USED AFTER RESTART";
  notice_until_ = ImGui::GetTime() + 3.0;
  Save();
}

// The name editor box over the page, in the same style as the update confirmation.
void SettingsPage::DrawNameEditor(ImDrawList* dl, float s, float cx, float cy, float in) {
  const float t = std::clamp(float(ImGui::GetTime() - naming_at_) / 0.15f, 0.0f, 1.0f) * in;
  const float w = 560.0f * s, body_h = 190.0f * s, head_h = 54.0f * s, foot_h = 40.0f * s;
  const float x0 = cx - w * 0.5f, x1 = cx + w * 0.5f;
  const float y0 = cy - (head_h + body_h + foot_h) * 0.5f;
  const float head_y1 = y0 + head_h, body_y1 = head_y1 + body_h, foot_y1 = body_y1 + foot_h;
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
                  Col(16, 18, 28, t), 2.2f * s, "PLAYER NAME", kCondensed);
  ImFont* body = achievement_ui::BodyFont();
  const float text_px = 18.0f * s;
  AddText(dl, body, text_px, ImVec2(x0 + 40.0f * s, head_y1 + 16.0f * s), Col(176, 182, 192, t),
          "Up/down: letter, left/right: position, X: delete. A keyboard works too.", 1.0f, w - 70.0f * s);
  // One cell per letter, plus a faint empty cell where the next letter would go.
  const float cell = 31.0f * s;
  const int cells = std::min(kNameMax, int(name_edit_.size()) + 1);
  float x = cx + 7.0f * s - cell * float(cells) * 0.5f;
  const float row_cy = head_y1 + 92.0f * s;
  const float px = 30.0f * s;
  ImFont* font = achievement_ui::HeadlineFont(px);
  for (int k = 0; k < cells; ++k) {
    const bool letter = k < int(name_edit_.size());
    const bool hot = letter && k == name_pos_;
    dl->AddRectFilled(ImVec2(x + 2.0f * s, row_cy - 22.0f * s), ImVec2(x + cell - 2.0f * s, row_cy + 22.0f * s),
                      hot      ? Col(255, 204, 0, 0.28f * t)
                      : letter ? Col(255, 255, 255, 0.09f * t)
                               : Col(255, 255, 255, 0.04f * t),
                      4.0f * s);
    if (letter) {
      const char ch[2] = {name_edit_[size_t(k)], 0};
      const float cw = TextWidth(font, px, ch);
      AddOutlinedText(dl, font, px, ImVec2(x + (cell - cw) * 0.5f, CapsTop(row_cy, px)),
                      hot ? Col(255, 204, 0, t) : Col(235, 237, 240, t), Col(16, 18, 28, t), 1.6f * s, ch);
    }
    if (hot) Arrow(dl, ImVec2(x + cell * 0.5f, row_cy - 32.0f * s), 9.0f * s, true, Col(150, 156, 166, t));
    x += cell;
  }
  const std::string note = name_edit_.empty()
                               ? "Empty: the automatic name (this computer's name)."
                               : std::to_string(name_edit_.size()) + " of " + std::to_string(kNameMax) +
                                     " letters. Used from the next start.";
  AddText(dl, body, text_px, ImVec2(x0 + 40.0f * s, body_y1 - 40.0f * s), Col(176, 182, 192, t), note.c_str(),
          1.0f, w - 70.0f * s);
  // Footer: (A) SAVE, (B) CANCEL.
  const float fy = (body_y1 + foot_y1) * 0.5f;
  const float fpx = 20.0f * s;
  ImFont* ffont = achievement_ui::HeadlineFont(fpx);
  const ImU32 text = Col(28, 30, 36, t);
  const float br = 11.0f * s;
  float fx = x1 - 22.0f * s - br;
  FaceButton(dl, ImVec2(fx, fy), br, Col(210, 44, 44, t), "B", t);
  fx -= br + 8.0f * s;
  AddText(dl, ffont, fpx, ImVec2(fx - TextWidth(ffont, fpx, "CANCEL", kCondensed), CapsTop(fy, fpx)), text,
          "CANCEL", kCondensed);
  fx -= TextWidth(ffont, fpx, "CANCEL", kCondensed) + 26.0f * s + br;
  FaceButton(dl, ImVec2(fx, fy), br, Col(90, 186, 40, t), "A", t);
  fx -= br + 8.0f * s;
  AddText(dl, ffont, fpx, ImVec2(fx - TextWidth(ffont, fpx, "SAVE", kCondensed), CapsTop(fy, fpx)), text, "SAVE",
          kCondensed);
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


// ==============================================================================================
// Connection quality
// ==============================================================================================
namespace {
constexpr int kHighLatencyMs = 150;
constexpr float kHighLoss = 0.02f;

// 1-4 bars and their colour (green, yellow, red) for a round trip and packet loss.
int QualityBars(int rtt_ms, float loss) {
  int bars = rtt_ms < 60 ? 4 : rtt_ms < 100 ? 3 : rtt_ms < kHighLatencyMs ? 2 : 1;
  if (loss > 0.05f) bars = 1;
  else if (loss > kHighLoss) bars = std::min(bars, 2);
  return bars;
}

ImU32 QualityColour(int bars, float alpha) {
  return bars >= 3 ? Col(90, 200, 80, alpha) : bars == 2 ? Col(240, 200, 40, alpha) : Col(235, 70, 60, alpha);
}

const char* QualityWarning(int rtt_ms, float loss) {
  if (loss > kHighLoss) return "PACKET LOSS: THE GAME WILL STUTTER AND FEEL DELAYED";
  if (rtt_ms > kHighLatencyMs) return "HIGH LATENCY: THE GAME WILL FEEL DELAYED";
  return nullptr;
}

void DrawBars(ImDrawList* dl, ImVec2 left_bottom, float s, int bars, float alpha) {
  const ImU32 on = QualityColour(bars, alpha);
  const ImU32 off = Col(255, 255, 255, 0.18f * alpha);
  for (int i = 0; i < 4; ++i) {
    const float x = left_bottom.x + float(i) * 7.0f * s;
    const float h = (6.0f + 4.0f * float(i)) * s;
    dl->AddRectFilled(ImVec2(x, left_bottom.y - h), ImVec2(x + 5.0f * s, left_bottom.y), i < bars ? on : off,
                      1.0f * s);
  }
}

std::string QualityText(const rex::system::net::OnlinePeerQuality& peer) {
  std::string text = std::to_string(peer.rtt_ms) + " MS";
  if (peer.loss > 0.005f) text += ", " + std::to_string(int(peer.loss * 100.0f + 0.5f)) + "% LOSS";
  text += peer.direct ? ", DIRECT" : ", VIA SERVER";
  if (peer.redundant) text += ", REDUNDANT";  // every datagram sent twice (online_redundancy)
  return text;
}
}  // namespace

ConnectionIndicator::ConnectionIndicator(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}
ConnectionIndicator::~ConnectionIndicator() = default;

// The connection to the other player, in the bottom right corner of the picture while online
// outside a match (lobby, menus, results): bars, round trip, loss and path, and a warning when
// the match will feel delayed.
void ConnectionIndicator::OnDraw(ImGuiIO& io) {
  using clock = std::chrono::steady_clock;
  static clock::time_point last_match{};
  if (haptics::MatchInPlay()) last_match = clock::now();
  const bool hidden = g_open.load() || g_prompt_open.load() || clock::now() - last_match < std::chrono::seconds(3);
  const auto status = rex::system::net::GetOnlineStatus();
  if (hidden || !status.connected || status.opponent < 0) return;
  const auto& peer = status.peers[size_t(status.opponent)];
  if (peer.rtt_ms < 0) return;

  const double configured = REXCVAR_GET(display_aspect);
  const float aspect = configured > 0.0 ? float(configured) : 16.0f / 9.0f;
  const float pic_w = std::min(io.DisplaySize.x, io.DisplaySize.y * aspect);
  const float pic_h = pic_w / aspect;
  const float pic_x = (io.DisplaySize.x - pic_w) * 0.5f;
  const float pic_y = (io.DisplaySize.y - pic_h) * 0.5f;
  const float s = pic_h / 720.0f;
  ImDrawList* dl = ImGui::GetForegroundDrawList();

  const int bars = QualityBars(peer.rtt_ms, peer.loss);
  const std::string name = peer.name.empty() ? "OPPONENT" : peer.name;
  std::string upper_name = name;
  for (auto& c : upper_name) c = char(std::toupper(static_cast<unsigned char>(c)));
  const std::string text = QualityText(peer);
  const float px = 19.0f * s;
  ImFont* font = achievement_ui::HeadlineFont(px);
  const float w = std::max(TextWidth(font, px, upper_name.c_str(), kCondensed),
                           TextWidth(font, px, text.c_str(), kCondensed)) + 64.0f * s;
  const float right = pic_x + pic_w - 36.0f * s, bottom = pic_y + pic_h - 30.0f * s;
  const float left = right - w, top = bottom - 52.0f * s;
  dl->AddRectFilled(ImVec2(left, top), ImVec2(right, bottom), Col(6, 8, 12, 0.82f), 6.0f * s);
  dl->AddRect(ImVec2(left, top), ImVec2(right, bottom), Col(150, 166, 186, 0.6f), 6.0f * s, 0, 1.5f * s);
  DrawBars(dl, ImVec2(left + 14.0f * s, bottom - 14.0f * s), s, bars, 1.0f);
  AddText(dl, font, px, ImVec2(left + 50.0f * s, top + 7.0f * s), Col(222, 224, 228, 1.0f), upper_name.c_str(),
          kCondensed);
  AddText(dl, font, px, ImVec2(left + 50.0f * s, top + 27.0f * s), QualityColour(bars, 1.0f), text.c_str(),
          kCondensed);
  if (const char* warning = QualityWarning(peer.rtt_ms, peer.loss)) {
    // Not condensed: ImGui culls glyphs against the screen before the condensing, which would
    // cut the end of a right-aligned line.
    // Right-aligned to the box. Measured at the drawn size plus the outline and a small margin,
    // and placed on two lines when it is wider than the room left of the picture's edge.
    const float wpx = 15.0f * s;
    const float pad = 4.0f * s;
    const float room = right - pic_x - 20.0f * s;
    const float ww = TextWidth(font, wpx, warning) + pad;
    if (ww <= room) {
      AddOutlinedText(dl, font, wpx, ImVec2(right - ww, top - 22.0f * s), Col(255, 204, 0, 1.0f),
                      Col(16, 18, 28, 1.0f), 1.5f * s, warning);
    } else {
      const std::string text(warning);
      const size_t cut = text.find(": ");
      const std::string first = cut == std::string::npos ? text : text.substr(0, cut + 1);
      const std::string second = cut == std::string::npos ? std::string() : text.substr(cut + 2);
      const float w1 = TextWidth(font, wpx, first.c_str()) + pad;
      const float w2 = TextWidth(font, wpx, second.c_str()) + pad;
      AddOutlinedText(dl, font, wpx, ImVec2(right - w1, top - 40.0f * s), Col(255, 204, 0, 1.0f),
                      Col(16, 18, 28, 1.0f), 1.5f * s, first.c_str());
      AddOutlinedText(dl, font, wpx, ImVec2(right - w2, top - 22.0f * s), Col(255, 204, 0, 1.0f),
                      Col(16, 18, 28, 1.0f), 1.5f * s, second.c_str());
    }
  }
}

// ==============================================================================================
// ONLINE prompt
// ==============================================================================================
void OpenOnlinePrompt() {
  if (g_prompt_open.load()) return;
  REXLOG_INFO("Tabletennis: ONLINE chosen in the main menu");
  SetPromptOpen(true);
}

OnlinePrompt::OnlinePrompt(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}
OnlinePrompt::~OnlinePrompt() = default;

void OnlinePrompt::Close(bool back_to_game) {
  if (back_to_game) rex::system::net::SetOnline("off", "");
  SetPromptOpen(false, back_to_game);
  shown_ = false;
}

void OnlinePrompt::OnDraw(ImGuiIO& io) {
  if (!g_prompt_open.load()) {
    shown_ = false;
    return;
  }
  const double now = ImGui::GetTime();
  if (!shown_) {
    shown_ = true;
    opened_at_ = now;
    selected_ = 0;
    held_direction_ = 0;
    step_ = rex::cvar::GetFlagByName("online_server").empty() ? Step::kUnavailable : Step::kChoose;
    // Join the public room right away, so the first screen can already show how many players
    // are online and near you; PLAY WITH A FRIEND moves to a private room, B leaves.
    if (step_ == Step::kChoose) rex::system::net::SetOnline("public", "");
  }
  const uint16_t pressed = g_pressed.exchange(0);
  const uint16_t held = g_held.load();
  const int n = int(std::strlen(kRoomAlphabet));

  auto direction = [&](uint16_t button) {
    if (step_ == Step::kEnterCode) {
      char& c = code_edit_[size_t(code_pos_)];
      const int index = int(std::strchr(kRoomAlphabet, c) - kRoomAlphabet);
      switch (button) {
        case kPadUp: c = kRoomAlphabet[(index + 1) % n]; break;
        case kPadDown: c = kRoomAlphabet[(index + n - 1) % n]; break;
        case kPadLeft: code_pos_ = (code_pos_ + kRoomLength - 1) % kRoomLength; break;
        case kPadRight: code_pos_ = (code_pos_ + 1) % kRoomLength; break;
        default: break;
      }
    } else if (step_ == Step::kChoose || step_ == Step::kFriend) {
      if (button == kPadUp || button == kPadDown) selected_ ^= 1;
    }
  };
  for (uint16_t button : {kPadUp, kPadDown, kPadLeft, kPadRight}) {
    if (pressed & button) {
      direction(button);
      held_direction_ = button;
      next_repeat_ = now + 0.45;
    }
  }
  if (held_direction_ && !(held & held_direction_)) held_direction_ = 0;
  if (held_direction_ && now >= next_repeat_) {
    direction(uint16_t(held_direction_));
    next_repeat_ = now + 0.13;
  }

  if (pressed & kPadA) {
    switch (step_) {
      case Step::kUnavailable:
        Close(true);
        return;
      case Step::kChoose:
        if (selected_ == 0) {
          rex::system::net::SetOnline("public", "");
          REXLOG_INFO("Tabletennis: online, public room");
          Close(false);
          return;
        }
        step_ = Step::kFriend;
        selected_ = 0;
        break;
      case Step::kFriend:
        if (selected_ == 0) {
          if (!ValidRoomCode(g_room_code)) g_room_code = NewRoomCode();
          rex::system::net::SetOnline("room", g_room_code);
          REXLOG_INFO("Tabletennis: online, hosting private room {}", g_room_code);
          step_ = Step::kShowCode;
        } else {
          code_edit_ = std::string(size_t(kRoomLength), kRoomAlphabet[0]);
          code_pos_ = 0;
          step_ = Step::kEnterCode;
        }
        break;
      case Step::kShowCode:
        Close(false);
        return;
      case Step::kEnterCode:
        g_room_code = code_edit_;
        rex::system::net::SetOnline("room", g_room_code);
        REXLOG_INFO("Tabletennis: online, joining private room {}", g_room_code);
        Close(false);
        return;
    }
  } else if (pressed & kPadB) {
    switch (step_) {
      case Step::kUnavailable:
      case Step::kChoose:
        Close(true);
        return;
      case Step::kFriend:
        step_ = Step::kChoose;
        selected_ = 1;
        rex::system::net::SetOnline("public", "");
        break;
      case Step::kShowCode:
        rex::system::net::SetOnline("public", "");
        step_ = Step::kFriend;
        selected_ = 0;
        break;
      case Step::kEnterCode:
        step_ = Step::kFriend;
        selected_ = 1;
        break;
    }
  }
  Draw(io, std::clamp(float(now - opened_at_) / 0.18f, 0.0f, 1.0f));
}

void OnlinePrompt::Draw(ImGuiIO& io, float in) {
  const double configured = REXCVAR_GET(display_aspect);
  const float aspect = configured > 0.0 ? float(configured) : 16.0f / 9.0f;
  const float pic_w = std::min(io.DisplaySize.x, io.DisplaySize.y * aspect);
  const float pic_h = pic_w / aspect;
  const float pic_x = (io.DisplaySize.x - pic_w) * 0.5f;
  const float pic_y = (io.DisplaySize.y - pic_h) * 0.5f;
  const float s = pic_h / 720.0f;
  const float ox = pic_x + (pic_w - 1280.0f * s) * 0.5f;
  auto X = [&](float x) { return ox + x * s; };
  auto Y = [&](float y) { return pic_y + y * s; };

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(pic_x, pic_y), ImVec2(pic_x + pic_w, pic_y + pic_h), Col(0, 0, 0, 0.45f * in));
  const int first_vertex = dl->VtxBuffer.Size;
  const float rise = (1.0f - in) * 10.0f;

  // The PC SETTINGS panel's look, smaller: navy pill header, near-black body, grey footer.
  const float x0 = X(370), x1 = X(910), bx0 = X(400);
  const float header_y0 = Y(168 + rise), header_y1 = Y(230 + rise);
  const float header_cy = (header_y0 + header_y1) * 0.5f;
  const float body_y0 = header_y1 - 6.0f * s, body_y1 = Y(508 + rise);
  const float footer_y0 = body_y1, footer_y1 = Y(548 + rise);
  dl->AddRectFilled(ImVec2(bx0, body_y0), ImVec2(x1, body_y1), Col(6, 8, 12, in), 6.0f * s,
                    ImDrawFlags_RoundCornersTop);
  dl->AddRectFilledMultiColor(ImVec2(bx0, body_y0), ImVec2(x1, body_y0 + 60.0f * s),
                              Col(255, 255, 255, 0.07f * in), Col(255, 255, 255, 0.07f * in),
                              Col(255, 255, 255, 0.0f), Col(255, 255, 255, 0.0f));
  dl->AddRectFilled(ImVec2(bx0, footer_y0), ImVec2(x1, footer_y1), Col(212, 215, 220, in), 6.0f * s,
                    ImDrawFlags_RoundCornersBottom);
  const float header_r = (header_y1 - header_y0) * 0.5f;
  dl->AddRectFilled(ImVec2(x0, header_y0), ImVec2(x1, header_y1), Col(150, 166, 186, in), header_r);
  dl->AddRectFilled(ImVec2(x0 + 4.0f * s, header_y0 + 4.0f * s), ImVec2(x1 - 4.0f * s, header_y1 - 4.0f * s),
                    Col(12, 28, 52, in), header_r - 4.0f * s);
  const float title_px = 44.0f * s;
  const char* title = step_ == Step::kChoose || step_ == Step::kUnavailable ? "ONLINE" : "PLAY WITH A FRIEND";
  AddOutlinedText(dl, achievement_ui::HeadlineFont(title_px), title_px,
                  ImVec2(x0 + 34.0f * s, CapsTop(header_cy, title_px)), Col(255, 204, 0, in),
                  Col(16, 18, 28, in), 2.6f * s, title, kCondensed);

  const float item_px = 30.0f * s, big_px = 34.0f * s;
  ImFont* item_font = achievement_ui::HeadlineFont(item_px);
  ImFont* big_font = achievement_ui::HeadlineFont(big_px);
  ImFont* body = achievement_ui::BodyFont();
  const float left = bx0 + 34.0f * s, right = x1 - 34.0f * s;
  const float help_px = 19.0f * s;
  float y = body_y0 + 34.0f * s;
  auto text_block = [&](const std::string& text, ImU32 col) {
    const float wrap = right - left;
    AddText(dl, body, help_px, ImVec2(left, y), col, text.c_str(), 1.0f, wrap);
    y += body->CalcTextSizeA(help_px, FLT_MAX, wrap, text.c_str()).y + 10.0f * s;
  };
  auto menu = [&](const char* a, const char* b) {
    const char* items[2] = {a, b};
    for (int i = 0; i < 2; ++i) {
      const float cy = y + 22.0f * s;
      const bool selected = i == selected_;
      if (selected) {
        dl->AddRectFilledMultiColor(ImVec2(bx0 + 10.0f * s, cy - 20.0f * s), ImVec2(x1 - 10.0f * s, cy + 20.0f * s),
                                    Col(255, 255, 255, 0.12f * in), Col(255, 255, 255, 0.02f * in),
                                    Col(255, 255, 255, 0.02f * in), Col(255, 255, 255, 0.12f * in));
        AddOutlinedText(dl, big_font, big_px, ImVec2(left, CapsTop(cy, big_px)), Col(255, 204, 0, in),
                        Col(16, 18, 28, in), 1.8f * s, items[i], kCondensed);
      } else {
        AddText(dl, item_font, item_px, ImVec2(left, CapsTop(cy, item_px)), Col(222, 224, 228, in), items[i],
                kCondensed);
      }
      y += 46.0f * s;
    }
    y += 8.0f * s;
  };
  auto code_boxes = [&](const std::string& code, int current) {
    const float cell = 52.0f * s;
    float x = (bx0 + x1) * 0.5f - cell * kRoomLength * 0.5f;
    const float cy = y + 32.0f * s;
    for (int k = 0; k < kRoomLength; ++k) {
      const char ch[2] = {code[size_t(k)], 0};
      const bool hot = k == current;
      dl->AddRectFilled(ImVec2(x + 4.0f * s, cy - 28.0f * s), ImVec2(x + cell - 4.0f * s, cy + 28.0f * s),
                        hot ? Col(255, 204, 0, 0.28f * in) : Col(255, 255, 255, 0.09f * in), 4.0f * s);
      const float px = 44.0f * s;
      ImFont* font = achievement_ui::HeadlineFont(px);
      const float w = TextWidth(font, px, ch);
      AddOutlinedText(dl, font, px, ImVec2(x + (cell - w) * 0.5f, CapsTop(cy, px)),
                      hot || current < 0 ? Col(255, 204, 0, in) : Col(235, 237, 240, in), Col(16, 18, 28, in),
                      1.8f * s, ch);
      if (hot) {
        Arrow(dl, ImVec2(x + cell * 0.5f, cy - 40.0f * s), 10.0f * s, true, Col(150, 156, 166, in));
      }
      x += cell;
    }
    y += 76.0f * s;
  };

  const char* a_label = "SELECT";
  switch (step_) {
    case Step::kUnavailable:
      text_block("Online play is not available.", Col(235, 237, 240, in));
      text_block("No online server is set (server= in tt-recomp.ini). SYSTEM LINK in the main menu "
                 "plays against other machines on your local network.", Col(176, 182, 192, in));
      a_label = "BACK";
      break;
    case Step::kChoose:
      menu("PLAY ANYONE", "PLAY WITH A FRIEND");
      text_block(selected_ == 0 ? "Find or create a match with anyone playing online."
                                : "Play only with a friend: one of you shares a room code.",
                 Col(176, 182, 192, in));
      break;
    case Step::kFriend:
      menu("SHOW MY CODE", "ENTER CODE");
      text_block(selected_ == 0 ? "Get a code to give your friend, then create a match."
                                : "Type the code your friend gives you, then use QUICK MATCH.",
                 Col(176, 182, 192, in));
      break;
    case Step::kShowCode:
      text_block("Give this code to your friend:", Col(235, 237, 240, in));
      code_boxes(g_room_code, -1);
      text_block("Then choose PLAYER MATCH > CREATE MATCH. Your friend enters the code and uses "
                 "QUICK MATCH.", Col(176, 182, 192, in));
      a_label = "CONTINUE";
      break;
    case Step::kEnterCode:
      text_block("Enter your friend's code (up/down: letter, left/right: position):",
                 Col(235, 237, 240, in));
      code_boxes(code_edit_, code_pos_);
      a_label = "JOIN";
      break;
  }

  // Connection status, once a room is chosen (or the server is unreachable).
  if (step_ != Step::kUnavailable) {
    const auto status = rex::system::net::GetOnlineStatus();
    std::string line;
    ImU32 col = Col(128, 194, 238, in);
    if (status.mode == "public" || status.mode == "room") {
      if (status.connected) {
        line = "CONNECTED - " + std::to_string(status.room_players) + " IN THIS ROOM, " +
               std::to_string(status.players_online) + " ONLINE";
        if (status.mode == "public") line += " (" + std::to_string(status.nearby_players) + " NEAR YOU)";
        if (status.server_rtt_ms >= 0) line += " - SERVER " + std::to_string(status.server_rtt_ms) + " MS";
      } else {
        line = status.state;
        if (line != "CONNECTING" && line != "RECONNECTING") col = Col(240, 90, 80, in);
      }
    }
    if (!line.empty()) {
      const float px = 19.0f * s;
      AddText(dl, item_font, px, ImVec2(left, body_y1 - 32.0f * s), col, line.c_str(), kCondensed);
    }
  }

  // Footer: (A) action, (B) back.
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
    if (step_ != Step::kUnavailable) {
      FaceButton(dl, ImVec2(x - r, cy), r, Col(90, 186, 40, in), "A", in);
      x -= 2.0f * r + 8.0f * s;
      AddText(dl, font, px, ImVec2(x - TextWidth(font, px, a_label, kCondensed), CapsTop(cy, px)), text,
              a_label, kCondensed);
    }
  }
  Rotate(dl, first_vertex, ImVec2((x0 + x1) * 0.5f, (header_y0 + footer_y1) * 0.5f), -0.03f);
}

}  // namespace tabletennis::pc_settings

// The in-match pause menu's per-frame update (r3 = the pause object). The page is opened from
// here when its OPTIONS list raises GLOBAL_PC_SETTINGS; the game's update then runs as usual.
extern "C" void __imp__sub_823132F0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_823132F0(PPCContext& ctx, uint8_t* base) {
  tabletennis::pc_settings::PollPause(ctx, base, ctx.r3.u32);
  __imp__sub_823132F0(ctx, base);
}
