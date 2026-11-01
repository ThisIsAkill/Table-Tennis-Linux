// app.cpp - installer flow: screen changes, the chosen disc, the install options, and driving
// the backend process. See app.h.

#include "app.h"

#include <SDL3/SDL.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tt {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

std::string Lower(std::string text) {
  for (auto& c : text) {
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  }
  return text;
}

bool LooksLikeIso(const fs::path& path) { return Lower(path.extension().string()) == ".iso"; }

}  // namespace

App::App(Config c) : config(std::move(c)) {
  is_steamos = sys::IsSteamOS();
  const fs::path home = sys::HomeDir();
  home_device = sys::DeviceOf(home);
  home_free = sys::FreeBytes(home);
  if (UpdateMode()) {
    // Straight to the progress screen: the install's stored choices drive tools/tt-update.sh.
    exit_code = 10;
    StartInstall();
    return;
  }
  scanner.Start(config.extra_scan_dirs);
  if (config.iso) {
    ChooseIso(*config.iso);
  } else {
    Enter(Screen::Welcome);
  }
}

App::~App() { delete process; }

void App::Enter(Screen next) {
  screen = next;
  screen_entered = ui.f.t;
  ui.focus.Reset();
  modal.reset();
  switch (next) {
    case Screen::Welcome:
      ui.focus.Set("browse");
      break;
    case Screen::Check:
      break;
    case Screen::Options:
      BuildLocations();
      ui.focus.Set("install");
      break;
    case Screen::Install:
      ui.focus.Set("details");
      break;
    case Screen::Done:
      ui.focus.Set("play");
      break;
    case Screen::Error:
      ui.focus.Set("retry");
      break;
  }
}

void App::Notice(std::string text) {
  notice = std::move(text);
  notice_until = ui.f.t + 6.0f;
}

void App::ChooseIso(const fs::path& path) {
  std::error_code ec;
  if (fs::is_directory(path, ec)) {
    Notice("That is a folder. Drop the .iso file of your disc.");
    return;
  }
  if (!fs::is_regular_file(path, ec)) {
    Notice("That file could not be opened.");
    return;
  }
  if (!LooksLikeIso(path)) {
    Notice("That is not an .iso file. The installer needs the disc image of your Table Tennis disc.");
    return;
  }
  iso = path;
  iso_bytes = fs::file_size(path, ec);
  StartCheck();
}

void App::StartCheck() {
  check_result.reset();
  iso::Setup setup;
  setup.repo_root = config.repo_root;
  setup.backend = config.backend;
  setup.ask_backend = config.backend_overridden;
  checker.Start(iso, setup);
  Enter(Screen::Check);
}

void App::FileDialogResult(const char* path, bool folder, bool error) {
  if (folder) {
    folder_dialog_open = false;
    if (error) {
      // No portal or zenity: fall back to typing the path.
      custom_path_editing = true;
      const std::string start = (sys::HomeDir() / "Games" / "TableTennis").string();
      std::snprintf(custom_path_buffer, sizeof(custom_path_buffer), "%s", start.c_str());
      return;
    }
    if (path && *path) UseCustomFolder(path);
    return;
  }
  browse_open = false;
  if (error) {
    Notice("No file dialog is available here. Drop the .iso onto this window instead.");
    return;
  }
  if (path && *path) ChooseIso(path);
}

void App::OpenBrowse() {
  if (browse_open) return;
  browse_open = true;
  static const SDL_DialogFileFilter filters[] = {{"Disc images", "iso"}, {"All files", "*"}};
  SDL_ShowOpenFileDialog(
      [](void* userdata, const char* const* filelist, int) {
        // The callback may run on another thread: hand the result to the main loop as an event.
        SDL_Event event{};
        event.type = SDL_EVENT_USER;
        event.user.code = filelist ? 0 : -1;
        event.user.data1 = (filelist && filelist[0]) ? SDL_strdup(filelist[0]) : nullptr;
        event.user.data2 = userdata;  // non-null marks a folder dialog
        SDL_PushEvent(&event);
      },
      nullptr, window, filters, 2, sys::HomeDir().c_str(), false);
}

void App::OpenFolderDialog() {
  if (folder_dialog_open) return;
  folder_dialog_open = true;
  static int folder_marker = 1;
  SDL_ShowOpenFolderDialog(
      [](void* userdata, const char* const* filelist, int) {
        SDL_Event event{};
        event.type = SDL_EVENT_USER;
        event.user.code = filelist ? 0 : -1;
        event.user.data1 = (filelist && filelist[0]) ? SDL_strdup(filelist[0]) : nullptr;
        event.user.data2 = userdata;
        SDL_PushEvent(&event);
      },
      &folder_marker, window, sys::HomeDir().c_str(), false);
}

void App::UseCustomFolder(fs::path chosen) {
  std::error_code ec;
  // A folder that already holds something else gets a TableTennis folder inside it.
  if (fs::is_directory(chosen, ec) && !fs::is_empty(chosen, ec) &&
      !fs::exists(chosen / "tt-recomp.sh", ec) && chosen.filename() != "TableTennis") {
    chosen /= "TableTennis";
  }
  for (size_t i = 0; i < locations.size(); ++i) {
    Location& loc = locations[i];
    if (!loc.custom) continue;
    loc.prefix = chosen;
    loc.free_bytes = sys::FreeBytes(chosen);
    loc.device = sys::DeviceOf(chosen);
    loc.title = "CUSTOM FOLDER";
    loc.note = "Your own choice.";
    loc.enabled = true;
    location = int(i);
  }
  custom_path_editing = false;
}

void App::BuildLocations() {
  const fs::path home = sys::HomeDir();
  home_free = sys::FreeBytes(home);
  std::optional<fs::path> keep_custom;
  if (!locations.empty() && CurrentLocation().custom && !CurrentLocation().prefix.empty()) {
    keep_custom = CurrentLocation().prefix;
  }
  locations.clear();

  Location internal;
  internal.id = "loc-internal";
  internal.prefix = home / "Games" / "TableTennis";
  internal.title = "INTERNAL STORAGE";
  internal.note = "Fastest. The game reads your disc image while playing.";
  internal.free_bytes = sys::FreeBytes(internal.prefix);
  internal.device = sys::DeviceOf(internal.prefix);
  internal.recommended = true;
  locations.push_back(internal);

  int n = 0;
  for (const sys::Mount& m : sys::UserMounts()) {
    if (n >= 3) break;
    Location loc;
    loc.id = "loc-mount-" + std::to_string(n++);
    loc.prefix = m.mountpoint / "Games" / "TableTennis";
    loc.title = m.is_sd_card ? "SD CARD" : ui::Upper(m.label);
    if (!m.is_sd_card) loc.title += " (EXTERNAL DRIVE)";
    loc.note = "Works, loads a little slower.";
    loc.free_bytes = sys::FreeBytes(m.mountpoint);
    loc.device = m.fake_device ? m.fake_device : sys::DeviceOf(m.mountpoint);
    if (access(m.mountpoint.c_str(), W_OK) != 0) {
      loc.enabled = false;
      loc.disabled_reason = "Read-only here";
    }
    locations.push_back(loc);
  }

  Location custom;
  custom.id = "loc-custom";
  custom.custom = true;
  custom.title = "CHOOSE ANOTHER FOLDER...";
  custom.note = "Pick any folder you like.";
  if (keep_custom) {
    custom.prefix = *keep_custom;
    custom.title = "CUSTOM FOLDER";
    custom.note = "Your own choice.";
    custom.free_bytes = sys::FreeBytes(*keep_custom);
    custom.device = sys::DeviceOf(*keep_custom);
  }
  locations.push_back(custom);

  // Grey out drives that cannot hold what the setup needs there.
  for (Location& loc : locations) {
    if (loc.custom || !loc.enabled) continue;
    const bool holds_cache = loc.device == home_device;
    const uint64_t needed = holds_cache ? kGameBytes + kSetupBytes : kGameBytes;
    if (loc.free_bytes < needed) {
      loc.enabled = false;
      loc.disabled_reason = "Only " + sys::FormatBytes(loc.free_bytes) + " free, needs " + sys::FormatBytes(needed);
    }
  }
  if (location < 0 || location >= int(locations.size()) || !locations[size_t(location)].enabled) {
    location = 0;
    for (size_t i = 0; i < locations.size(); ++i) {
      if (locations[i].enabled && !locations[i].custom) {
        location = int(i);
        break;
      }
    }
  }
  // When the home drive is short on space and another drive is chosen, keep the build files
  // there automatically.
  cache_on_target = home_free < kSetupBytes && CurrentLocation().device != home_device;
}

std::optional<fs::path> App::CacheDir() const {
  if (!cache_on_target || CurrentLocation().device == home_device) return std::nullopt;
  return Prefix() / ".build-cache";
}

bool App::EnoughSpace(std::string* why) const {
  const Location& loc = CurrentLocation();
  if (loc.prefix.empty()) {
    if (why) *why = "Choose a folder first.";
    return false;
  }
  const bool cache_here = CacheDir().has_value() || loc.device == home_device;
  const uint64_t needed_here = cache_here ? kGameBytes + kSetupBytes : kGameBytes;
  if (loc.free_bytes < needed_here) {
    if (why) *why = "Only " + sys::FormatBytes(loc.free_bytes) + " free here; the setup needs " + sys::FormatBytes(needed_here) + ".";
    return false;
  }
  if (!cache_here && home_free < kSetupBytes) {
    if (why) *why = "Your home drive has " + sys::FormatBytes(home_free) + " free; the build files need " + sys::FormatBytes(kSetupBytes) + ". Keep them on the install drive or free some space.";
    return false;
  }
  return true;
}

void App::StartInstall() {
  delete process;
  process = new backend::Process();
  inst = InstallState{};
  inst.started = Clock::now();
  notes.clear();
  steam_result.clear();
  show_error_details = false;
  for (const auto& known : backend::KnownSteps()) {
    InstallStep step;
    step.id = known.id;
    step.title = known.title;
    step.known_index = backend::KnownStepIndex(known.id);
    step.weight = known.weight;
    inst.steps.push_back(step);
  }
  backend::Options options;
  options.backend = config.backend;
  if (UpdateMode()) options.args = {config.update_dir->string()};
  options.iso = iso;
  if (UpdateMode()) {
    // No location was chosen (the options screen is skipped); the install updates itself.
    options.prefix = *config.update_dir;
  } else {
    options.prefix = Prefix();
    options.cache = CacheDir();
  }
  options.steam = steam;
  options.desktop = desktop;
  std::string error;
  if (config.backend.empty()) {
    inst.error = "The setup script (scripts/install/tt-install.sh) was not found next to the installer. Run it from the unpacked source folder or pass --backend.";
    Enter(Screen::Error);
    return;
  }
  if (!process->Start(options, error)) {
    inst.error = error;
    Enter(Screen::Error);
    return;
  }
  Enter(Screen::Install);
}

void App::PumpBackend(float dt) {
  if (!process) return;
  process->Poll();
  for (const std::string& raw : process->TakeLines()) {
    const backend::Line line = backend::ParseLine(raw);
    switch (line.kind) {
      case backend::LineKind::Step: {
        // Finish the running step, drop known steps the backend skipped, add unknown ones.
        if (inst.current >= 0) inst.steps[size_t(inst.current)].state = InstallStep::State::Done;
        int index = -1;
        for (size_t i = 0; i < inst.steps.size(); ++i) {
          if (inst.steps[i].id == line.a) index = int(i);
        }
        if (index < 0) {
          InstallStep step;
          step.id = line.a;
          step.title = line.b;
          step.weight = 4.0f;
          const size_t at = inst.current >= 0 ? size_t(inst.current) + 1 : inst.steps.size();
          inst.steps.insert(inst.steps.begin() + long(at), step);
          index = int(at);
        } else {
          if (!line.b.empty()) inst.steps[size_t(index)].title = line.b;
          // Steps between the previous and this one were skipped: remove them.
          const int from = inst.current + 1;
          if (index > from) {
            inst.steps.erase(inst.steps.begin() + from, inst.steps.begin() + index);
            index = from;
          }
        }
        inst.current = index;
        inst.steps[size_t(index)].state = InstallStep::State::Active;
        inst.steps[size_t(index)].started = Clock::now();
        inst.log.push_back("== " + inst.steps[size_t(index)].title);
        inst.last_line.clear();
        break;
      }
      case backend::LineKind::Progress:
        inst.reported = std::max(inst.reported, float(line.value) / 100.0f);
        break;
      case backend::LineKind::Warn:
        inst.warnings.push_back(line.a);
        notes.push_back(line.a);
        inst.log.push_back("Warning: " + line.a);
        break;
      case backend::LineKind::Error:
        inst.error = line.a;
        inst.log.push_back("Error: " + line.a);
        break;
      case backend::LineKind::Result:
        if (line.a == "steam") steam_result = line.b;
        inst.log.push_back("Result: " + line.a + " " + line.b);
        break;
      case backend::LineKind::Done:
        inst.done_dir = line.a;
        inst.log.push_back("Done: " + line.a);
        break;
      case backend::LineKind::Log:
      case backend::LineKind::Plain:
        if (!line.a.empty()) {
          inst.log.push_back(line.a);
          inst.last_line = line.a;
        }
        break;
    }
  }

  // Progress shown: never below the finished steps' share, creeping through the active step
  // while nothing is reported, and never above what the backend says plus that creep.
  float total_weight = 0.0f, done_weight = 0.0f, active_weight = 0.0f;
  float active_seconds = 0.0f;
  for (const InstallStep& step : inst.steps) {
    total_weight += step.weight;
    if (step.state == InstallStep::State::Done) done_weight += step.weight;
    if (step.state == InstallStep::State::Active) {
      active_weight = step.weight;
      active_seconds = std::chrono::duration<float>(Clock::now() - step.started).count();
    }
  }
  float target = inst.reported;
  if (total_weight > 0.0f) {
    const float floor_value = done_weight / total_weight;
    // A step's expected duration is its share of a 30 minute install.
    const float expected = std::max(20.0f, active_weight / total_weight * 1800.0f);
    const float creep = active_weight / total_weight * (1.0f - std::exp(-active_seconds / expected)) * 0.9f;
    target = std::max(target, floor_value + creep);
  }
  if (!inst.done_dir.empty()) target = 1.0f;
  target = std::clamp(target, 0.0f, 1.0f);
  inst.shown += (target - inst.shown) * (1.0f - std::exp(-dt * 2.5f));
  if (std::fabs(target - inst.shown) < 0.0005f) inst.shown = target;

  // Remaining time: the backend's progress rate once it has settled, the weights before that.
  const float elapsed = std::chrono::duration<float>(Clock::now() - inst.started).count();
  float estimate;
  if (inst.shown > 0.04f && elapsed > 15.0f) {
    estimate = elapsed / inst.shown * (1.0f - inst.shown);
  } else {
    estimate = 1800.0f * (1.0f - inst.shown);
  }
  if (inst.remaining_seconds < 0.0f) {
    inst.remaining_seconds = estimate;
  } else {
    inst.remaining_seconds += (estimate - inst.remaining_seconds) * (1.0f - std::exp(-dt * 0.3f));
  }

  if (!process->Running()) {
    const std::optional<int> code = process->ExitCode();
    if (inst.cancelled && UpdateMode()) {
      FinishUpdate(10);  // the installed version is untouched
      return;
    }
    if (inst.cancelled) {
      Notice("Setup cancelled. Nothing was changed that cannot be redone.");
      Enter(Screen::Options);
      return;
    }
    if (!inst.done_dir.empty() && code && *code == 0) {
      inst.shown = 1.0f;
      Enter(Screen::Done);
      return;
    }
    if (inst.error.empty()) {
      if (code && *code == 127) {
        inst.error = "The setup script could not be started. Is it marked executable?";
      } else if (process->KilledBySignal()) {
        inst.error = "The setup script was stopped by the system (signal " + std::to_string(code.value_or(0) - 128) + ").";
      } else {
        inst.error = "The setup script stopped unexpectedly (exit code " + std::to_string(code.value_or(-1)) + ").";
      }
    }
    Enter(Screen::Error);
  }
}

void App::CancelInstall() {
  if (!process || !process->Running()) return;
  inst.cancelled = true;
  process->Cancel();
}

void App::Play() {
  if (UpdateMode()) {
    FinishUpdate(0);  // the launcher starts the new version
    return;
  }
  const fs::path dir = inst.done_dir.empty() ? Prefix() : fs::path(inst.done_dir);
  const fs::path launcher = dir / "tt-recomp.sh";
  if (sys::LaunchDetached({launcher.string()}, dir)) {
    quit = true;
  } else {
    Notice("The launcher " + launcher.string() + " could not be started.");
  }
}

void App::OpenInstallFolder() {
  const fs::path dir = inst.done_dir.empty() ? Prefix() : fs::path(inst.done_dir);
  sys::OpenInFileManager(dir);
}

void App::CopyLog() {
  std::string text;
  if (process) text = process->FullLog();
  if (text.empty()) {
    for (const auto& line : inst.log) text += line + "\n";
  }
  if (!inst.error.empty()) text += "\nError: " + inst.error + "\n";
  SDL_SetClipboardText(text.c_str());
  Notice("Log copied to the clipboard.");
}

void App::Frame() {
  // Logic that runs regardless of the screen.
  if (!scan_finished && scanner.Done()) {
    found = scanner.Results();
    scan_finished = true;
  }
  if (screen == Screen::Check && checker.Done() && !check_result) {
    check_result = checker.Take();
    check_done_at = ui.f.t;
  }
  if (screen == Screen::Install) PumpBackend(ui.f.dt);
  // An update hands back to the launcher shortly after it finished.
  if (UpdateMode() && screen == Screen::Done && SinceEnter() > 2.0f) FinishUpdate(0);

  // Back (B / Escape) per screen; the modal takes it first.
  if (ui.f.back && !custom_path_editing) {
    if (modal) {
      modal.reset();
    } else {
      switch (screen) {
        case Screen::Welcome:
          Ask({"Quit setup?", "Nothing has been installed yet. You can start again any time.", "Quit",
               "Stay", false, [this] { quit = true; }});
          break;
        case Screen::Check:
          checker.Reset();
          check_result.reset();
          Enter(Screen::Welcome);
          break;
        case Screen::Options:
          Enter(Screen::Welcome);
          break;
        case Screen::Install:
          Ask({"Stop the setup?", "The game is being built. Stopping now keeps what was downloaded, so a later attempt is faster, but nothing will be playable yet.",
               "Stop setup", "Keep going", true, [this] { CancelInstall(); }});
          break;
        case Screen::Done:
          if (UpdateMode()) {
            FinishUpdate(0);
            break;
          }
          Ask({"Close setup?", "The game is installed. You can start it from Steam or the install folder.", "Close",
               "Stay", false, [this] { quit = true; }});
          break;
        case Screen::Error:
          if (UpdateMode()) {
            FinishUpdate(10);
            break;
          }
          Enter(Screen::Options);
          break;
      }
    }
    ui.f.back = false;
  }

  ui.Background();
  ui.inert = modal.has_value() || custom_path_editing;
  switch (screen) {
    case Screen::Welcome: DrawWelcome(); break;
    case Screen::Check: DrawCheck(); break;
    case Screen::Options: DrawOptions(); break;
    case Screen::Install: DrawInstall(); break;
    case Screen::Done: DrawDone(); break;
    case Screen::Error: DrawError(); break;
  }
  ui.inert = false;
  if (modal) DrawModal();
  if (custom_path_editing) DrawCustomPathEditor();
}

}  // namespace tt
