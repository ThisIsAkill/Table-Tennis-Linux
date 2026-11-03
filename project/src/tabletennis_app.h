// tabletennis_app.h - ReXGlue application configuration for Rockstar Games presents Table Tennis

#pragma once

#include <chrono>
#include <algorithm>
#include <cstdlib>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>
#include <cstdio>
#include "offline_menu_device.h"
#include "intro_skip.h"
#include "achievement_ui.h"
#include "pc_features.h"
#include "pc_settings.h"
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/presenter.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <imgui.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

REXCVAR_DECLARE(bool, show_fps);
REXCVAR_DECLARE(int32_t, framerate);
REXCVAR_DECLARE(std::string, picture);

class FpsOverlayDialog : public rex::ui::ImGuiDialog {
 public:
  explicit FpsOverlayDialog(rex::ui::ImGuiDrawer* drawer, ImFont* loading_font)
      : rex::ui::ImGuiDialog(drawer), loading_font_(loading_font) {}

  void SetVisible(bool visible) { visible_ = visible; }
  bool IsVisible() const { return visible_; }
  void Toggle() { visible_ = !visible_; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    tabletennis::intro::PollGamepads();
    if (tabletennis::intro::ShowTransition()) {
      const auto* viewport = ImGui::GetMainViewport();
      auto* draw = ImGui::GetForegroundDrawList();
      draw->AddRectFilled(viewport->Pos,
          ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
          IM_COL32(0, 0, 0, 255));
      const char* message = "LOADING MAIN MENU...";
      auto* font = loading_font_ ? loading_font_ : ImGui::GetFont();
      // Use the baked loading face size, independent of the smaller overlay font.
      const float font_size = loading_font_ ? loading_font_->LegacySize
                                            : ImGui::GetFontSize() * 3.0f;
      const auto size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, message);
      draw->AddText(font, font_size,
                    ImVec2(viewport->Pos.x + (viewport->Size.x - size.x) * 0.5f,
                          viewport->Pos.y + (viewport->Size.y - size.y) * 0.5f),
                    IM_COL32(255, 255, 255, 255), message);
    }
    // Pace presentation thread to target framerate limit
    int32_t target_fps = REXCVAR_GET(framerate);
    if (target_fps > 0) {
      using clock = std::chrono::steady_clock;
      static auto last_draw = clock::now();
      const auto target_dur = std::chrono::nanoseconds(1'000'000'000LL / target_fps);
      auto now = clock::now();
      auto elapsed = now - last_draw;
      if (elapsed < target_dur) {
        auto sleep_dur = target_dur - elapsed;
        if (sleep_dur > std::chrono::milliseconds(2)) {
          std::this_thread::sleep_for(sleep_dur - std::chrono::milliseconds(1));
        }
        while (clock::now() - last_draw < target_dur) {
#if defined(_M_X64) || defined(__x86_64__)
          _mm_pause();
#endif
        }
      }
      last_draw = clock::now();
    }

    static int perf_log_counter = 0;
    if (++perf_log_counter % 120 == 0) {
      REXLOG_INFO("PERF TICK: Current FPS: {:.1f} ({:.2f} ms)", io.Framerate, (io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f));
    }

    if (!visible_) {
      return;
    }

    // Position overlay at top-left with slight offset
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.65f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoInputs;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 6.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.08f, 0.12f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.18f, 0.42f, 0.70f, 0.60f));

    if (ImGui::Begin("##FpsOverlay", nullptr, flags)) {
      float fps = io.Framerate;
      float ms = (fps > 0.0f) ? (1000.0f / fps) : 0.0f;

      ImVec4 color = (fps >= 115.0f) ? ImVec4(0.0f, 0.9f, 1.0f, 1.0f) :   // Cyan for 120Hz/144Hz/240Hz
                     (fps >= 55.0f)  ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f) :   // Green for 60Hz
                     (fps >= 30.0f)  ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f) :  // Yellow
                                       ImVec4(1.0f, 0.3f, 0.3f, 1.0f);    // Red

      ImGui::TextColored(color, "FPS: %.1f", fps);
      ImGui::SameLine();
      ImGui::TextDisabled("(%.2f ms)", ms);
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
  }

 private:
  ImFont* loading_font_ = nullptr;
  bool visible_ = false;
};

class TabletennisApp : public rex::ReXApp {
 public:
  TabletennisApp(rex::ui::WindowedAppContext& ctx, const std::string_view name,
                 const rex::PPCImageInfo& ppc_info)
      : rex::ReXApp(ctx, name, ppc_info) {
    rex::cvar::SetFlagByName("protect_zero", "false");
    rex::cvar::SetFlagByName("gpu_allow_invalid_fetch_constants", "true");
  }

  ~TabletennisApp() override {
    tabletennis::intro::Uninstall();
    rex::ui::UnregisterBind("bind_toggle_fps");
    rex::ui::UnregisterBind("bind_quit_game");
    if (imgui_drawer_ && fps_overlay_) {
      imgui_drawer_->RemoveDialog(fps_overlay_.get());
    }
    if (imgui_drawer_ && pc_settings_) {
      imgui_drawer_->RemoveDialog(pc_settings_.get());
    }
    if (imgui_drawer_ && update_toast_) {
      imgui_drawer_->RemoveDialog(update_toast_.get());
    }
    update_toast_.reset();
    pc_settings_.reset();
    fps_overlay_.reset();
  }

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<TabletennisApp>(new TabletennisApp(ctx, "tabletennis",
        PPCImageConfig));
  }

 protected:
  void OnConfigurePaths(rex::PathConfig& paths) override {
    rex::cvar::SetFlagByName("protect_zero", "false");
    rex::cvar::SetFlagByName("gpu_allow_invalid_fetch_constants", "true");

    auto exe_dir = rex::filesystem::GetExecutableFolder();
    auto root_dir = exe_dir.parent_path();

    // Scan settings.ini in executable folder
    std::filesystem::path settings_path = exe_dir / "settings.ini";
    if (std::filesystem::exists(settings_path)) {
      std::ifstream ini_file(settings_path);
      std::string line;
      while (std::getline(ini_file, line)) {
        if (line.rfind("iso_path=", 0) == 0 && (paths.game_data_root.empty() || !std::filesystem::exists(paths.game_data_root))) {
          std::string val = line.substr(9);
          while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || val.back() == ' ' || val.back() == '"')) val.pop_back();
          if (!val.empty() && val.front() == '"') val = val.substr(1);
          if (!val.empty() && std::filesystem::exists(val)) {
            paths.game_data_root = val;
            REXLOG_INFO("TabletennisApp: Found ISO from settings.ini: {}", paths.game_data_root.string());
          }
        } else if (line.rfind("framerate=", 0) == 0) {
          std::string val = line.substr(10);
          try {
            int target = std::stoi(val);
            if (target >= 0) {
              REXCVAR_SET(framerate, target);
              REXLOG_INFO("TabletennisApp: Target framerate from settings.ini: {} FPS", target);
            }
          } catch (...) {}
        }
      }
    }

    // Auto-detect ISO if still empty
    if (paths.game_data_root.empty() || !std::filesystem::exists(paths.game_data_root)) {
      for (const auto& dir : {exe_dir, root_dir}) {
        if (std::filesystem::exists(dir)) {
          for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".iso") {
              paths.game_data_root = entry.path();
              REXLOG_INFO("TabletennisApp: Auto-detected ISO: {}", paths.game_data_root.string());
              break;
            }
          }
        }
        if (!paths.game_data_root.empty()) break;
      }
    }

    // Set isolated user_data and cache directories
    if (paths.user_data_root.empty() || paths.user_data_root == rex::filesystem::GetUserFolder() / GetName()) {
      auto custom_user = root_dir / "user_data";
      if (std::filesystem::exists(custom_user) || std::filesystem::exists(root_dir / "reports")) {
        paths.user_data_root = custom_user;
      }
    }

    if (paths.cache_root.empty() || paths.cache_root == paths.user_data_root / "cache") {
      auto custom_cache = root_dir / "cache";
      if (std::filesystem::exists(custom_cache) || std::filesystem::exists(root_dir / "reports")) {
        paths.cache_root = custom_cache;
      }
    }

    // Validate ISO if specified
    if (!paths.game_data_root.empty() && std::filesystem::is_regular_file(paths.game_data_root)) {
      ValidateIsoSource(paths.game_data_root);
    }
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
#if defined(__linux__)
    // TT_ALLOW_PTRACE=1 lets a profiler or debugger that is not this process's parent attach
    // (for stack sampling under the default Yama ptrace_scope=1). Off unless set.
    if (const char* allow = std::getenv("TT_ALLOW_PTRACE"); allow && allow[0] == '1') {
      prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
      REXLOG_INFO("TabletennisApp: TT_ALLOW_PTRACE set; any process may attach a debugger");
    }
#endif
    rex::cvar::SetFlagByName("protect_zero", "false");
    rex::cvar::SetFlagByName("gpu_allow_invalid_fetch_constants", "true");
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }

    // Always keep guest video mode refresh rate at 60.0 Hz.
    // This ensures guest VBlank interrupts fire at 60Hz so video player,
    // audio clocks, and menu background matches run at normal 1.0x real-time speed.
    rex::cvar::SetFlagByName("video_mode_refresh_rate", "60.0");

    int32_t target_hz = REXCVAR_GET(framerate);
    if (target_hz != 60) {
      // High refresh rate / uncapped presentation. D3D12 needs tearing enabled
      // explicitly; Vulkan picks mailbox or immediate when vsync is off.
      rex::cvar::SetFlagByName("vsync", "false");
#if defined(_WIN32)
      rex::cvar::SetFlagByName("d3d12_allow_variable_refresh_rate_and_tearing", "true");
#endif
      REXLOG_INFO("TabletennisApp::OnPreSetup - High Framerate Mode: target {} FPS (vsync=false, guest_vblank=60Hz)", target_hz);
    } else {
      rex::cvar::SetFlagByName("vsync", "true");
      REXLOG_INFO("TabletennisApp::OnPreSetup - Standard 60 Hz VSync enabled");
    }

    REXLOG_INFO("TabletennisApp::OnPreSetup - GPU plugin: {}, protect_zero: false", config.gpu_plugin);
  }

  void OnPostLoadXexImage() override {
    // TT_DUMP_IMAGE=path writes the loaded (decompressed) executable image to a file, for
    // locating constants when developing patches. Off unless set; the file is game data and
    // stays local.
    if (const char* dump = std::getenv("TT_DUMP_IMAGE"); dump && dump[0]) {
      if (auto module = runtime()->kernel_state()->GetExecutableModule()) {
        auto* xex = module->xex_module();
        const uint32_t base = xex->base_address();
        const uint32_t size = xex->image_size();
        auto* bytes = runtime()->memory()->TranslateVirtual<const uint8_t*>(base);
        if (FILE* f = std::fopen(dump, "wb")) {
          std::fwrite(bytes, 1, size, f);
          std::fclose(f);
          REXLOG_INFO("Tabletennis: dumped image {:08X}+{:X} to {}", base, size, dump);
        }
      }
    }
    // PICTURE (pc_features.h): the 3D camera and 2D layers follow the window's shape.
    tabletennis::pc::SetPicture(REXCVAR_GET(picture));
    if (window()) {
      tabletennis::pc::SetSurfaceSize(window()->GetActualPhysicalWidth(), window()->GetActualPhysicalHeight());
    }
    auto vfs = runtime()->kernel_state()->file_system();
    if (vfs) {
      vfs->RegisterSymbolicLink("cache:", "\\Device\\Harddisk0\\Cache0");

      std::filesystem::path exe_dir = rex::filesystem::GetExecutableFolder();
      std::filesystem::path flash_override = exe_dir / "assets" / "resource" / "flash";
      if (std::filesystem::exists(flash_override / "flash.rpf")) {
        if (auto* root = vfs->ResolvePath("game:")) {
          auto dev = std::make_unique<OfflineMenuDevice>(flash_override, root->device());
          if (dev->Initialize() && vfs->RegisterDevice(std::move(dev))) {
            vfs->UnregisterSymbolicLink("game:");
            vfs->UnregisterSymbolicLink("d:");
            vfs->RegisterSymbolicLink("game:", "\\Device\\TabletennisOfflineMenu");
            vfs->RegisterSymbolicLink("d:", "\\Device\\TabletennisOfflineMenu");
            REXLOG_INFO("Tabletennis: Offline main-menu override mounted from {}", flash_override.string());
          }
        }
      }
      if (auto entry = vfs->ResolvePath("game:\\assets\\resource\\flash\\flash.rpf")) {
        REXLOG_INFO("Tabletennis: flash.rpf resolves to device {} ({} bytes)",
                    entry->device()->mount_path(), entry->size());
      }
    }
  }

  void OnConfigureFonts(ImFontAtlas* atlas) override {
    // Keep the regular overlay font first. The loading message gets its own
    // TrueType face baked at 3x size, with extra raster resolution for DPI.
    if (atlas->Fonts.empty()) atlas->AddFontDefault();
    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 2;
    config.PixelSnapH = false;
    config.RasterizerDensity = 2.0f;
    static const ImWchar ranges[] = {0x20, 0x7E, 0};
    for (const auto& path : LoadingFontCandidates()) {
      if (!std::filesystem::exists(path)) continue;
      loading_font_ = atlas->AddFontFromFileTTF(path.string().c_str(), 39.0f, &config, ranges);
      if (loading_font_) {
        REXLOG_INFO("Tabletennis: Loading text TrueType font: {} (39px, 2x raster density)", path.string());
        break;
      }
    }
    // Achievement popup and list faces, shipped next to the executable.
    tabletennis::achievement_ui::ConfigureFonts(
        atlas, rex::filesystem::GetExecutableFolder() / "fonts");
  }

  // Game-styled replacements for the SDK's achievement overlays (achievement_ui.h).
  std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override {
    if (!imgui_drawer() || !runtime() || !runtime()->kernel_state()) return nullptr;
    return std::make_unique<tabletennis::achievement_ui::AchievementsPanel>(
        imgui_drawer(), immediate_drawer(), runtime(), &achievements());
  }

  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog()
      override {
    if (!imgui_drawer() || !runtime() || !runtime()->kernel_state()) return nullptr;
    return std::make_unique<tabletennis::achievement_ui::AchievementToast>(
        imgui_drawer(), immediate_drawer(), runtime(), &achievements(), window());
  }

  // The window's drawable size changed (resize, fullscreen): the picture follows its shape.
  void OnWindowPixelSizeChanged(uint32_t width, uint32_t height) override {
    tabletennis::pc::SetSurfaceSize(width, height);
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    imgui_drawer_ = drawer;
    if (drawer) {
      fps_overlay_ = std::make_unique<FpsOverlayDialog>(drawer, loading_font_);
      bool initial_visible = REXCVAR_GET(show_fps);
      fps_overlay_->SetVisible(initial_visible);
      drawer->AddDialog(fps_overlay_.get());
      REXLOG_INFO("TabletennisApp: FPS overlay created (initial state: {})", initial_visible ? "ON" : "OFF");

      // OPTIONS > PC SETTINGS (pc_settings.h).
      tabletennis::pc_settings::LiveHooks hooks;
      hooks.get_show_fps = [this] { return fps_overlay_ && fps_overlay_->IsVisible(); };
      hooks.set_show_fps = [this](bool on) {
        if (fps_overlay_) fps_overlay_->SetVisible(on);
      };
      hooks.set_anti_aliasing = [this](const std::string& name) {
        if (auto* graphics = runtime() ? runtime()->graphics_system() : nullptr) {
          graphics->SetSwapPostEffectByName(name);
        }
      };
      hooks.set_vsync = [this](bool on) {
        // The swapchain picks its present mode when it is created: change the setting, then
        // rebuild the swapchain after this UI frame (not from inside a drawer).
        rex::cvar::SetFlagByName("vulkan_allow_present_mode_immediate", on ? "false" : "true");
        auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
        auto* presenter = graphics ? graphics->presenter() : nullptr;
        if (presenter && window()) {
          window()->app_context().CallInUIThreadDeferred(
              [presenter] { presenter->OnSurfaceResizeFromUIThread(); });
        }
      };
      hooks.set_sharpening = [this](const std::string& name) {
        auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
        if (auto* presenter = graphics ? graphics->presenter() : nullptr) {
          presenter->SetGuestOutputEffectFromUIThread(name);
        }
      };
      pc_settings_ = std::make_unique<tabletennis::pc_settings::SettingsPage>(drawer, window(),
                                                                             std::move(hooks));
      drawer->AddDialog(pc_settings_.get());
      update_toast_ = std::make_unique<tabletennis::pc_settings::UpdateToast>(drawer);
      drawer->AddDialog(update_toast_.get());
      online_prompt_ = std::make_unique<tabletennis::pc_settings::OnlinePrompt>(drawer);
      drawer->AddDialog(online_prompt_.get());
      connection_indicator_ = std::make_unique<tabletennis::pc_settings::ConnectionIndicator>(drawer);
      drawer->AddDialog(connection_indicator_.get());
    }

    // Hotkey F10 toggles FPS overlay during gameplay
    rex::ui::RegisterBind("bind_toggle_fps", "F10", "Toggle In-Game FPS Counter", [this] {
      if (fps_overlay_) {
        fps_overlay_->Toggle();
        REXLOG_INFO("FPS counter toggled via F10: {}", fps_overlay_->IsVisible() ? "ON" : "OFF");
      }
    });

    // Hotkey ESC quits game cleanly to Windows
    rex::ui::RegisterBind("bind_quit_game", "Escape", "Quit Game", [this] {
#if !defined(_WIN32)
      // On Linux a stray Escape is easy to send (the Steam Deck's desktop layout maps B to
      // it), so quitting needs a second press within two seconds.
      static std::chrono::steady_clock::time_point last_escape{};
      const auto now = std::chrono::steady_clock::now();
      if (now - last_escape > std::chrono::seconds(2)) {
        last_escape = now;
        REXLOG_INFO("ESC pressed. Press ESC again within 2 seconds to quit.");
        return;
      }
#endif
      REXLOG_INFO("ESC pressed. Exiting cleanly...");
      if (window()) {
        window()->RequestClose();
      } else {
        std::_Exit(0);
      }
    });

    REXLOG_INFO("TabletennisApp: Binds for ESC (Quit; twice on Linux) and F10 (FPS Toggle) are active");
  }

  void OnPostSetup() override {
    tabletennis::intro::Install();
    REXLOG_INFO("TabletennisApp::OnPostSetup - Runtime and VFS fully initialized");
  }

 private:
  static std::vector<std::filesystem::path> LoadingFontCandidates() {
#if defined(_WIN32)
    wchar_t windows_dir[MAX_PATH]{};
    if (!GetWindowsDirectoryW(windows_dir, MAX_PATH)) return {};
    const auto fonts = std::filesystem::path(windows_dir) / L"Fonts";
    return {fonts / L"seguisb.ttf", fonts / L"segoeui.ttf", fonts / L"arial.ttf"};
#else
    // Common locations on Arch/CachyOS, Debian/Ubuntu, Fedora and SteamOS.
    return {"/usr/share/fonts/noto/NotoSans-SemiBold.ttf",
            "/usr/share/fonts/truetype/noto/NotoSans-SemiBold.ttf",
            "/usr/share/fonts/google-noto/NotoSans-SemiBold.ttf",
            "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
            "/usr/share/fonts/TTF/LiberationSans-Bold.ttf"};
#endif
  }

  void ValidateIsoSource(const std::filesystem::path& iso_path) {
    std::ifstream f(iso_path, std::ios::binary);
    if (!f.is_open()) {
      REXLOG_ERROR("Could not open ISO file: {}", iso_path.string());
      return;
    }

    // Check GDFX magic at sector 32 of game partition (offset 0xFDA0000 in XGD2)
    f.seekg(0xFDA0000);
    char magic[20] = {0};
    f.read(magic, 20);
    if (std::memcmp(magic, "MICROSOFT*XBOX*MEDIA", 20) == 0) {
      REXLOG_INFO("ISO validation PASSED: GDFX filesystem identified at sector 32 (offset 0xFDA0000)");
    } else {
      REXLOG_WARN("ISO validation WARNING: GDFX magic not found at 0xFDA0000. Image may be corrupt or non-standard.");
    }
  }

  ImFont* loading_font_ = nullptr;
  std::unique_ptr<FpsOverlayDialog> fps_overlay_;
  std::unique_ptr<tabletennis::pc_settings::SettingsPage> pc_settings_;
  std::unique_ptr<tabletennis::pc_settings::UpdateToast> update_toast_;
  std::unique_ptr<tabletennis::pc_settings::OnlinePrompt> online_prompt_;
  std::unique_ptr<tabletennis::pc_settings::ConnectionIndicator> connection_indicator_;
  rex::ui::ImGuiDrawer* imgui_drawer_ = nullptr;
};
