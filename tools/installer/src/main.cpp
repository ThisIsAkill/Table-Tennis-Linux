// Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
// main.cpp - window, renderer, input and the frame loop of the Table Tennis installer.
//
//   install-table-tennis [--iso PATH] [--backend SCRIPT] [--scan-dir DIR]... [--size WxH]
//   install-table-tennis --update INSTALL_DIR
//   install-table-tennis --verify-manifest MANIFEST SIGNATURE PUBLIC_KEY
//
// The installer finds scripts/install/tt-install.sh by walking up from its own location, so it
// can live at the root of the unpacked source tree or in tools/installer/bin. --backend points
// it at another script (tools/installer/fake-backend.sh while developing).

#include "app.h"
#include "backend.h"
#include "system.h"
#include "ui.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>

#include "../third_party/monocypher/monocypher-ed25519.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" {
extern const unsigned char tt_icon_data[], tt_icon_end[];
}

namespace {

void Usage() {
  std::printf(
      "Table Tennis setup for Linux and the Steam Deck\n\n"
      "  install-table-tennis [options]\n\n"
      "  --iso PATH        start with this disc image (skips the welcome screen)\n"
      "  --backend SCRIPT  use this setup script instead of scripts/install/tt-install.sh\n"
      "  --scan-dir DIR    also look for disc images here (may be repeated)\n"
      "  --size WxH        initial window size (default 1280x800)\n"
      "  --update DIR      update the install in DIR with its stored settings (used by the\n"
      "                    game's launcher; exit code 0 updated, 10 play the current version)\n"
      "  --verify-manifest MANIFEST SIGNATURE PUBLIC_KEY\n"
      "                    check a release manifest's Ed25519 signature and exit (0 valid)\n"
      "  --help            this text\n");
}

// Checks the detached Ed25519 signature (64 raw bytes) of a release manifest against the public
// key file (64 hex digits; '#' starts a comment). Prints the verdict; exit 0 valid, 1 invalid,
// 2 unreadable input.
int VerifyManifest(const char* manifest_path, const char* sig_path, const char* key_path) {
  auto read = [](const char* path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
  };
  std::string manifest, sig, key_text;
  if (!read(manifest_path, manifest) || !read(sig_path, sig) || !read(key_path, key_text)) {
    std::printf("unreadable input\n");
    return 2;
  }
  std::string hex;
  bool comment = false;
  for (char c : key_text) {
    if (c == '#') comment = true;
    if (c == '\n') comment = false;
    if (!comment && std::isxdigit(static_cast<unsigned char>(c))) hex += c;
  }
  if (sig.size() != 64 || hex.size() != 64) {
    std::printf("malformed signature or key\n");
    return 2;
  }
  uint8_t key[32];
  for (int i = 0; i < 32; ++i) key[i] = uint8_t(std::stoi(hex.substr(size_t(i) * 2, 2), nullptr, 16));
  const bool valid = crypto_ed25519_check(reinterpret_cast<const uint8_t*>(sig.data()), key,
                                          reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size()) == 0;
  std::printf(valid ? "valid\n" : "invalid signature\n");
  return valid ? 0 : 1;
}

// Decodes the largest PNG entry of the embedded .ico into RGBA pixels.
SDL_Surface* DecodeIcon() {
  const unsigned char* data = tt_icon_data;
  const size_t size = size_t(tt_icon_end - tt_icon_data);
  if (size < 6) return nullptr;
  auto u16 = [&](size_t at) { return unsigned(data[at]) | unsigned(data[at + 1]) << 8; };
  auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
  const unsigned count = u16(4);
  unsigned best_w = 0;
  size_t best_off = 0, best_size = 0;
  for (unsigned i = 0; i < count; ++i) {
    const size_t e = 6 + 16 * i;
    if (e + 16 > size) break;
    const unsigned w = data[e] ? data[e] : 256;
    const size_t bytes = u32(e + 8), off = u32(e + 12);
    if (off + bytes > size) continue;
    if (w > best_w) {
      best_w = w;
      best_off = off;
      best_size = bytes;
    }
  }
  if (best_size == 0) return nullptr;
  int w = 0, h = 0, channels = 0;
  unsigned char* pixels = stbi_load_from_memory(data + best_off, int(best_size), &w, &h, &channels, 4);
  if (!pixels) return nullptr;
  SDL_Surface* surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
  if (surface) {
    SDL_LockSurface(surface);
    for (int y = 0; y < h; ++y) {
      std::memcpy(static_cast<unsigned char*>(surface->pixels) + y * surface->pitch, pixels + y * w * 4, size_t(w) * 4);
    }
    SDL_UnlockSurface(surface);
  }
  stbi_image_free(pixels);
  return surface;
}

struct PadState {
  tt::ui::Nav held = tt::ui::Nav::None;
  std::chrono::steady_clock::time_point next_repeat{};
  float axis_x = 0.0f, axis_y = 0.0f;
  bool dpad[4] = {false, false, false, false};  // up down left right
};

tt::ui::Nav PadDirection(const PadState& pad) {
  using tt::ui::Nav;
  if (pad.dpad[0]) return Nav::Up;
  if (pad.dpad[1]) return Nav::Down;
  if (pad.dpad[2]) return Nav::Left;
  if (pad.dpad[3]) return Nav::Right;
  const float dead = 0.55f;
  if (std::fabs(pad.axis_y) >= std::fabs(pad.axis_x)) {
    if (pad.axis_y < -dead) return Nav::Up;
    if (pad.axis_y > dead) return Nav::Down;
  } else {
    if (pad.axis_x < -dead) return Nav::Left;
    if (pad.axis_x > dead) return Nav::Right;
  }
  return Nav::None;
}

}  // namespace

int main(int argc, char** argv) {
  tt::Config config;
  std::string backend_arg;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", arg.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--help" || arg == "-h") {
      Usage();
      return 0;
    } else if (arg == "--verify-manifest") {
      if (i + 3 >= argc) {
        std::fprintf(stderr, "--verify-manifest needs MANIFEST SIGNATURE PUBLIC_KEY\n");
        return 2;
      }
      return VerifyManifest(argv[i + 1], argv[i + 2], argv[i + 3]);
    } else if (arg == "--update") {
      config.update_dir = std::filesystem::absolute(value());
    } else if (arg == "--iso") {
      config.iso = value();
    } else if (arg == "--backend") {
      backend_arg = value();
    } else if (arg == "--scan-dir") {
      config.extra_scan_dirs.push_back(value());
    } else if (arg == "--size") {
      if (std::sscanf(value(), "%dx%d", &config.width, &config.height) != 2) {
        std::fprintf(stderr, "--size wants WxH\n");
        return 2;
      }
    } else {
      std::fprintf(stderr, "unknown option %s\n", arg.c_str());
      Usage();
      return 2;
    }
  }

  const std::filesystem::path exe_dir = tt::sys::ExecutableDir();
  if (config.update_dir && backend_arg.empty()) {
    // The install keeps its own updater, so updates work after the download folder is gone.
    config.backend = *config.update_dir / "tools" / "tt-update.sh";
    config.backend_overridden = true;
  } else if (!backend_arg.empty()) {
    config.backend = std::filesystem::absolute(backend_arg);
    config.backend_overridden = true;
  } else {
    config.backend = tt::backend::FindBackend(exe_dir);
  }
  config.repo_root = tt::backend::RepoRootOf(config.backend.empty() ? exe_dir / "x" : config.backend);
  if (config.repo_root.empty()) config.repo_root = tt::backend::RepoRootOf(exe_dir / "x");

  SDL_SetHint(SDL_HINT_APP_NAME, "Table Tennis Setup");
  SDL_SetAppMetadata("Table Tennis Setup", "1.0", "tt-recomp.installer");
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return config.update_dir ? 20 : 1;  // 20: no window possible; the launcher updates without one
  }
  SDL_Window* window = SDL_CreateWindow("Table Tennis Setup", config.width, config.height,
                                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
  if (!window) {
    std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    return config.update_dir ? 20 : 1;
  }
  SDL_SetWindowMinimumSize(window, 800, 520);
  SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
  if (!renderer) {
    std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
    return config.update_dir ? 20 : 1;
  }
  SDL_SetRenderVSync(renderer, 1);
  SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  SDL_ShowWindow(window);

  SDL_Texture* icon_texture = nullptr;
  int icon_w = 0, icon_h = 0;
  if (SDL_Surface* icon = DecodeIcon()) {
    SDL_SetWindowIcon(window, icon);
    icon_texture = SDL_CreateTextureFromSurface(renderer, icon);
    if (icon_texture) SDL_SetTextureScaleMode(icon_texture, SDL_SCALEMODE_LINEAR);
    icon_w = icon->w;
    icon_h = icon->h;
    SDL_DestroySurface(icon);
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  tt::ui::LoadFonts(io.Fonts);
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  tt::App app(config);
  app.window = window;
  app.ui.f.icon = icon_texture;
  app.ui.f.icon_w = icon_w;
  app.ui.f.icon_h = icon_h;

  PadState pad;
  const auto start = std::chrono::steady_clock::now();
  auto last_frame = start;
  bool pointer_mode = true;
  bool window_hidden = false;
  auto last_input = start;
  SDL_Texture* background = nullptr;
  int background_w = 0, background_h = 0;
  const char* renderer_name = SDL_GetRendererName(renderer);
  const bool software_renderer = renderer_name && std::strcmp(renderer_name, "software") == 0;
  while (!app.quit) {
    tt::ui::Frame& f = app.ui.f;
    f.nav = tt::ui::Nav::None;
    f.activate = false;
    f.back = false;
    f.mouse_moved = false;
    const bool typing = app.custom_path_editing;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type >= SDL_EVENT_KEY_DOWN && event.type <= SDL_EVENT_PEN_AXIS) {
        last_input = std::chrono::steady_clock::now();
      }
      switch (event.type) {
        case SDL_EVENT_QUIT:
          app.quit = true;
          break;
        case SDL_EVENT_WINDOW_HIDDEN:
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_OCCLUDED:
          window_hidden = true;
          break;
        case SDL_EVENT_WINDOW_SHOWN:
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_EXPOSED:
          window_hidden = false;
          break;
        case SDL_EVENT_DROP_FILE:
          if (event.drop.data && app.screen != tt::Screen::Install && app.screen != tt::Screen::Done) {
            app.ChooseIso(event.drop.data);
          }
          break;
        case SDL_EVENT_USER:
          app.FileDialogResult(static_cast<const char*>(event.user.data1), event.user.data2 != nullptr,
                               event.user.code != 0);
          if (event.user.data1) SDL_free(event.user.data1);
          break;
        case SDL_EVENT_MOUSE_MOTION:
          pointer_mode = true;
          f.mouse_moved = true;
          break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_FINGER_DOWN:
          pointer_mode = true;
          f.mouse_moved = true;  // a tap focuses what is under it
          break;
        case SDL_EVENT_KEY_DOWN: {
          if (typing) break;
          pointer_mode = false;
          const SDL_Keycode key = event.key.key;
          const bool shift = event.key.mod & SDL_KMOD_SHIFT;
          using tt::ui::Nav;
          if (key == SDLK_UP) f.nav = Nav::Up;
          else if (key == SDLK_DOWN) f.nav = Nav::Down;
          else if (key == SDLK_LEFT) f.nav = Nav::Left;
          else if (key == SDLK_RIGHT) f.nav = Nav::Right;
          else if (key == SDLK_TAB) f.nav = shift ? Nav::Up : Nav::Down;
          else if (!event.key.repeat && (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE)) f.activate = true;
          else if (!event.key.repeat && (key == SDLK_ESCAPE || key == SDLK_BACKSPACE)) f.back = true;
          else pointer_mode = true;  // an unrelated key should not pop the focus ring up
          break;
        }
        case SDL_EVENT_GAMEPAD_ADDED:
          SDL_OpenGamepad(event.gdevice.which);
          break;
        case SDL_EVENT_GAMEPAD_REMOVED:
          if (SDL_Gamepad* g = SDL_GetGamepadFromID(event.gdevice.which)) SDL_CloseGamepad(g);
          break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: {
          const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
          if (down) pointer_mode = false;
          switch (event.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP: pad.dpad[0] = down; break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: pad.dpad[1] = down; break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: pad.dpad[2] = down; break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: pad.dpad[3] = down; break;
            case SDL_GAMEPAD_BUTTON_SOUTH: if (down) f.activate = true; break;
            case SDL_GAMEPAD_BUTTON_EAST: if (down) f.back = true; break;
            default: break;
          }
          break;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
          if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) pad.axis_x = event.gaxis.value / 32767.0f;
          if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) pad.axis_y = event.gaxis.value / 32767.0f;
          break;
        default:
          break;
      }
    }

    // Gamepad direction with auto-repeat.
    {
      const auto now = std::chrono::steady_clock::now();
      const tt::ui::Nav dir = PadDirection(pad);
      if (dir != pad.held) {
        pad.held = dir;
        if (dir != tt::ui::Nav::None) {
          f.nav = dir;
          pointer_mode = false;
          pad.next_repeat = now + std::chrono::milliseconds(380);
        }
      } else if (dir != tt::ui::Nav::None && now >= pad.next_repeat) {
        f.nav = dir;
        pad.next_repeat = now + std::chrono::milliseconds(110);
      }
    }

    // The backdrop (gradients, swooshes, halftone fields, the table) is the most expensive
    // thing to draw and never changes, so it is rendered once per window size into a texture.
    {
      int pw = 0, ph = 0;
      SDL_GetCurrentRenderOutputSize(renderer, &pw, &ph);
      if (!background || background_w != pw || background_h != ph) {
        if (background) SDL_DestroyTexture(background);
        background = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, pw, ph);
        background_w = pw;
        background_h = ph;
        if (background) {
          SDL_SetTextureScaleMode(background, SDL_SCALEMODE_NEAREST);
          ImGui_ImplSDLRenderer3_NewFrame();
          ImGui_ImplSDL3_NewFrame();
          ImGui::NewFrame();
          tt::ui::Frame& bf = app.ui.f;
          bf.dl = ImGui::GetBackgroundDrawList();
          bf.W = io.DisplaySize.x;
          bf.H = io.DisplaySize.y;
          bf.s = std::max(0.5f, std::min(bf.W / 1280.0f, bf.H / 800.0f));
          bf.t = 0.0f;
          app.ui.BackgroundShapes();
          ImGui::Render();
          SDL_SetRenderTarget(renderer, background);
          SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
          SDL_SetRenderDrawColor(renderer, 8, 18, 34, 255);
          SDL_RenderClear(renderer);
          ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
          SDL_SetRenderTarget(renderer, nullptr);
        }
      }
      app.ui.f.background = background;
    }

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    const auto now = std::chrono::steady_clock::now();
    f.t = std::chrono::duration<float>(now - start).count();
    f.dt = std::clamp(std::chrono::duration<float>(now - last_frame).count(), 0.0f, 0.1f);
    last_frame = now;
    f.dl = ImGui::GetBackgroundDrawList();
    f.W = io.DisplaySize.x;
    f.H = io.DisplaySize.y;
    f.s = std::max(0.5f, std::min(f.W / 1280.0f, f.H / 800.0f));
    f.mouse = io.MousePos;
    f.mouse_clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    f.mouse_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    f.mouse_released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    f.wheel = io.MouseWheel;
    f.pointer_mode = pointer_mode;
    app.ui.activated_this_frame.clear();

    app.ui.focus.BeginFrame(f);
    app.Frame();
    app.ui.focus.EndFrame(f);

    ImGui::Render();
    SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(renderer, 8, 18, 34, 255);
    SDL_RenderClear(renderer);
    if (background) {
      SDL_SetRenderScale(renderer, 1.0f, 1.0f);
      SDL_RenderTexture(renderer, background, nullptr, nullptr);
      SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    }
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);

    // An installer does not need 60 frames per second, and the CPU is busy compiling the game
    // while this runs: 30 is plenty, less on the CPU renderer, and almost nothing when hidden.
    const bool recently_used = std::chrono::duration<float>(now - last_input).count() < 2.0f;
    float target_fps = recently_used ? 30.0f : 15.0f;
    if (software_renderer) target_fps = std::min(target_fps, 20.0f);
    if (window_hidden) target_fps = 4.0f;
    const auto frame_end = std::chrono::steady_clock::now();
    const float spent = std::chrono::duration<float>(frame_end - now).count();
    if (spent < 1.0f / target_fps) SDL_Delay(Uint32((1.0f / target_fps - spent) * 1000.0f));
  }

  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  if (background) SDL_DestroyTexture(background);
  if (icon_texture) SDL_DestroyTexture(icon_texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return app.exit_code;
}
