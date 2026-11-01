// app.h - the installer's state machine: which screen is shown, what the user chose, and the
// running install. Drawing lives in screens.cpp; this header is shared by app.cpp, screens.cpp
// and main.cpp.
#pragma once

#include "backend.h"
#include "iso_check.h"
#include "system.h"
#include "ui.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct SDL_Window;

namespace tt {

enum class Screen { Welcome, Check, Options, Install, Done, Error };

struct Config {
  std::filesystem::path backend;    // scripts/install/tt-install.sh or the --backend override
  std::filesystem::path repo_root;  // checkout root; holds project/tools/disc_image.py
  bool backend_overridden = false;  // --backend was given (verification asks the script)
  std::vector<std::filesystem::path> extra_scan_dirs;  // --scan-dir, for testing
  std::optional<std::filesystem::path> iso;            // --iso skips the welcome screen
  std::optional<std::filesystem::path> update_dir;     // --update: update this install
  int width = 1280, height = 800;
};

// A place the game can be installed.
struct Location {
  std::string id;
  std::filesystem::path prefix;
  std::string title;      // "INTERNAL STORAGE"
  std::string note;       // "Fastest. The game reads the disc image while playing."
  uint64_t free_bytes = 0;
  uint64_t device = 0;
  bool recommended = false;
  bool enabled = true;
  std::string disabled_reason;
  bool custom = false;    // "Choose another folder..."
};

struct InstallStep {
  enum class State { Pending, Active, Done };
  std::string id;
  std::string title;
  State state = State::Pending;
  int known_index = -1;
  float weight = 1.0f;
  std::chrono::steady_clock::time_point started{};
};

struct InstallState {
  std::vector<InstallStep> steps;
  int current = -1;
  float reported = 0.0f;          // last @@progress, 0..1
  float shown = 0.0f;             // smoothed value drawn
  std::vector<std::string> log;   // everything, for the details view
  std::vector<std::string> warnings;
  std::string last_line;          // most recent log text, shown under the bar
  std::chrono::steady_clock::time_point started{};
  float remaining_seconds = -1.0f;  // smoothed estimate; < 0 = unknown yet
  std::string error;
  std::string done_dir;
  bool details_open = false;
  float log_scroll = 0.0f;        // in lines from the top
  bool log_follow = true;
  bool cancelled = false;
};

struct Modal {
  std::string title;
  std::string body;
  std::string confirm_label;
  std::string cancel_label;
  bool danger = false;
  std::function<void()> on_confirm;
};

struct App {
  explicit App(Config config);
  ~App();

  Config config;
  SDL_Window* window = nullptr;
  ui::Ui ui;
  Screen screen = Screen::Welcome;
  float screen_entered = 0.0f;
  bool quit = false;
  // Process exit code. In update mode the launcher reads it: 0 updated (start the new version),
  // 10 start the current version, 1 do not start the game.
  int exit_code = 0;
  bool UpdateMode() const { return config.update_dir.has_value(); }
  void FinishUpdate(int code) {
    exit_code = code;
    quit = true;
  }
  std::optional<Modal> modal;

  // Welcome.
  sys::IsoScanner scanner;
  std::vector<sys::FoundIso> found;
  bool scan_finished = false;
  bool browse_open = false;
  std::string notice;        // transient line under the drop zone ("That is not an .iso file")
  float notice_until = 0.0f;

  // Chosen disc.
  std::filesystem::path iso;
  uint64_t iso_bytes = 0;
  iso::Checker checker;
  std::optional<iso::Result> check_result;
  float check_done_at = 0.0f;

  // Options.
  std::vector<Location> locations;
  int location = 0;
  bool steam = true;
  bool desktop = true;
  bool cache_on_target = false;   // keep build files on the install drive
  bool is_steamos = false;
  uint64_t home_device = 0;
  uint64_t home_free = 0;
  bool folder_dialog_open = false;
  bool custom_path_editing = false;  // text field fallback when no folder dialog is available
  char custom_path_buffer[1024] = {};

  // Install.
  backend::Process* process = nullptr;  // recreated for every attempt
  InstallState inst;
  std::vector<std::string> notes;       // warnings carried to the done screen
  std::string steam_result;             // backend's "@@result steam": added, requested or none
  bool show_error_details = false;

  // --- flow ---
  void Enter(Screen next);
  void Frame();   // one full frame: logic then drawing
  void ChooseIso(const std::filesystem::path& path);
  void FileDialogResult(const char* path, bool folder, bool error);
  void StartCheck();
  void BuildLocations();
  const Location& CurrentLocation() const { return locations[size_t(location)]; }
  // The install folder: the one being updated in update mode, else the chosen location.
  std::filesystem::path Prefix() const {
    return config.update_dir ? *config.update_dir : CurrentLocation().prefix;
  }
  std::optional<std::filesystem::path> CacheDir() const;
  bool EnoughSpace(std::string* why = nullptr) const;
  void StartInstall();
  void PumpBackend(float dt);
  void CancelInstall();
  void Play();
  void OpenInstallFolder();
  void CopyLog();
  void Ask(Modal m) {
    modal = std::move(m);
    ui.focus.Set("modal-cancel");
  }
  void Notice(std::string text);
  void OpenBrowse();
  void OpenFolderDialog();
  void UseCustomFolder(std::filesystem::path chosen);

  // --- drawing (screens.cpp) ---
  void DrawWelcome();
  void DrawCheck();
  void DrawOptions();
  void DrawInstall();
  void DrawDone();
  void DrawError();
  void DrawModal();
  void DrawLog(const ui::Rect& r, float alpha);
  void DrawCustomPathEditor();

  float SinceEnter() const { return ui.f.t - screen_entered; }
};

// Bytes the setup needs: the finished game is small, the build caches are not.
constexpr uint64_t kGameBytes = 1ull * 1000 * 1000 * 1000;      // headroom for the install itself
constexpr uint64_t kSetupBytes = 10ull * 1000 * 1000 * 1000;    // build tools and caches, temporary

}  // namespace tt
