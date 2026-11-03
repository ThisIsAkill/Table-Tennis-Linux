# Rockstar Games presents Table Tennis — Linux and Steam Deck

A native Linux port of the Xbox 360 classic, built on [davidptrssvea's Windows recompilation](https://github.com/davidptrssvea/Table-Tennis-Rockstar-PC). The game's code is recompiled to native code with ReXGlue SDK v0.10.0 and rendered with Vulkan. It plays at a steady, evenly paced 60 FPS on Linux PCs and the Steam Deck, from **your own Xbox 360 disc image**, with:

- **Online and System Link play** through the game's own menus, with nothing to set up
- **PC SETTINGS** in the game's OPTIONS menu: resolution, widescreen, VSync, brightness, anti-aliasing, sharpening and more, almost all applied at once
- **Widescreen and ultrawide** pictures with a wider 3D view and menus kept in proportion
- **PlayStation button icons** in every menu while a PlayStation controller is in use
- **Achievements** with an unlock popup and a full list, **controller haptics** on every hit, and **in-game updates**

<p align="center">
  <img src="docs/images/linux-main-menu.jpg" width="49%" alt="Main menu running natively on Linux">
  <img src="docs/images/linux-match.jpg" width="49%" alt="A match in the National TT Auditorium on Linux">
</p>

## Install

1. Download **[install-table-tennis-linux-x86_64.tar.gz](https://github.com/ThisIsAkill/Table-Tennis-Linux/releases/latest/download/install-table-tennis-linux-x86_64.tar.gz)** from the [latest release](https://github.com/ThisIsAkill/Table-Tennis-Linux/releases/latest) and unpack it (on a Steam Deck, in Desktop Mode).
2. Open the `TableTennis-Installer` folder and double-click **install-table-tennis** (or run `./install-table-tennis` in a terminal).
3. Drop your Table Tennis ISO on the window, press **BROWSE...**, or pick one of the ISOs it found in your Downloads, Desktop, Games folder or on an SD card. It checks that it is the right disc.
4. Choose where to install (internal storage is recommended; SD cards and other folders work too), whether to **add the game to Steam** (so it shows up in your library and in Game Mode on a Steam Deck) and whether to add a desktop shortcut, then press **INSTALL**.
5. When it says **Ready to play**, press **PLAY**. On a Steam Deck you can go back to Game Mode and start it from Library > Non-Steam.

The mouse, touch screen, keyboard and controller all work in the installer.

<p align="center">
  <img src="docs/images/installer-welcome.jpg" width="49%" alt="Installer: drop your ISO">
  <img src="docs/images/installer-building.jpg" width="49%" alt="Installer: building the game">
</p>

### What you need

- A 64-bit x86 PC with an AVX2 CPU (most PCs since 2015, and the Steam Deck) and a Vulkan GPU.
- Your own unmodified **USA/Europe retail Xbox 360 Table Tennis ISO** (Title ID `545407DF`). It is only read, never changed, and nothing from it leaves your computer. Keep it where it is after installing: the game reads it while you play.
- An internet connection for the first install, about 1 GB free in the install folder and about 3 GB of temporary space for build files.

### What the installer does

The game cannot be downloaded ready-made, because it is built from the code on your own disc. The installer does that on your machine: it downloads the build tools and the prebuilt engine from the release once (about 125 MB), turns your disc's Xbox code into native code, compiles it, and installs the game with its settings, desktop entry and Steam shortcut. A first install takes about a minute on a desktop PC and a few minutes on a Steam Deck; later installs and updates reuse everything.

If the prebuilt files cannot be downloaded, it builds the engine too and fetches whatever tools are missing: podman or docker when available, otherwise your system's compilers, otherwise an official Ubuntu base system unpacked into your cache folder, without admin rights. That takes a few more minutes the first time.

| What | Where |
|---|---|
| The game | the install folder (default `~/Games/TableTennis`) |
| Settings | `tt-recomp.ini` in the install folder |
| Saves, achievements, shader cache | `~/.local/share/tabletennis` (kept when you reinstall or move the game) |
| Build tools and caches | `~/.cache/tt-recomp` (can be deleted after installing; updates are then slower once) |

To uninstall, delete the install folder (and the cache folder), remove the shortcut from Steam, and delete `~/.local/share/tabletennis` if you also want to remove your saves.

The same install runs without a window with `scripts/install/tt-install.sh --iso /path/to/TableTennis.iso` from the unpacked folder; see [scripts/install/README.md](scripts/install/README.md).

## Updates

The game looks for a new release in the background when it starts, at most every six hours. When there is one, the main menu shows an **UPDATE AVAILABLE** popup. Open **OPTIONS > PC SETTINGS > UPDATES** and choose **UPDATE TO ...** to see what is new and update: the game closes, updates in about a minute and starts again. Saves and settings are kept, and if an update fails the current version keeps working.

**CHECK NOW** on the same tab looks for a new version right away. Releases are signed, and the game only installs updates signed with this project's key. The check only downloads the public release information from GitHub; nothing about you or your game is sent.

## Playing

### Controls

Any controller SDL supports works, including the Steam Deck's own controls. The game's **OPTIONS > CONTROLS** screen shows the layout: the left stick moves, the face buttons (or the right stick) play the four spins, the left trigger plays soft shots, the right trigger focus shots, the bumpers full focus, and Back changes the camera.

- **QUIT GAME**, the last item in the main menu, closes the game. With a keyboard, Escape pressed twice within two seconds does the same.
- **F7** opens the achievements list, **F10** toggles the FPS counter.
- **Keyboard controls** for playing without a controller are off by default; set `keyboard=on` in `tt-recomp.ini`:

| Action | Keys |
|---|---|
| Move | W A S D or the arrow keys |
| Spins (same diamond as the controller's buttons) | I backspin, J leftspin, L rightspin, K topspin |
| Soft shot / focus shot / full focus | Q / E / F |
| Change camera | C |
| Menus: accept / back / pause | Space or Enter / Backspace / P |
| Shoulder buttons (PC SETTINGS tabs) | 1 / 3 |

### PC SETTINGS

The game's **OPTIONS** menu, in the main menu and in the pause menu during a match, has a **PC SETTINGS** entry. It opens a page in the game's own style with four tabs, switched with **LB / RB**. Use the D-pad to choose and change, **X** to restore the defaults (1280x800 fullscreen on a Steam Deck) and **B** to go back. Changes are saved to `tt-recomp.ini`, and everything applies at once except the two settings marked below.

<p align="center">
  <img src="docs/images/linux-pc-settings.jpg" width="70%" alt="PC SETTINGS page with its DISPLAY tab">
</p>

**DISPLAY**

| Setting | Values |
|---|---|
| Resolution | 1280x720, 1280x800 (Steam Deck), 1920x1080, 2560x1440, 3840x2160. 1440p and 4K also raise the internal 3D resolution and apply after a restart |
| Fullscreen | on / off |
| Picture | for screens that are not 16:9 (see below): auto / letterbox / fill screen |
| VSync | auto (on for macOS, off elsewhere) / on / off: waits for the display, so the picture does not tear |
| Brightness | -2 to +5; NORMAL is the picture as the game draws it. Lightens dark and middle tones, white stays white |
| Anti-aliasing | off / FXAA / FXAA extreme |
| Sharpening | off / CAS (sharpens; best on the Steam Deck) / FSR (upscales to large screens and sharpens) |
| Show FPS | on / off; also shows the game's own frame rate with its worst frame times |

**GAME**: **button prompts** (see below), haptics (a pulse on every ball hit and a light tick in menus) and the achievement unlock sound, on or off.

**ONLINE**: **player name**, the name other players see (press A, then the D-pad or a keyboard; up to 15 letters, digits, `-` and `_`; applies after a restart), and the online server and connection status.

**UPDATES**: check now, check at start, automatic updates, and the installed version.

### PlayStation button icons

With a PlayStation controller (DualShock 3 or 4, DualSense), the game's menus show cross, circle, square, triangle, L1 and R1 instead of the Xbox A, B, X, Y, LB and RB, and so do the PC SETTINGS page and the achievements list. **BUTTON PROMPTS** on the GAME tab chooses: **AUTO** (default) follows the controller you used last, **XBOX** and **PLAYSTATION** always show those icons. A change shows at once, also in a menu that is open. The icons are this project's own artwork (`project/assets/button_prompts/`), swapped in when the game loads its icon textures.

### Widescreen and ultrawide

The original game is 16:9. **PICTURE** decides what happens on other screen shapes, and it follows the window as you resize it:

- **AUTO** (default): on wider screens (21:9 and so on) the 3D view widens instead of stretching, and the menus and HUD keep their proportions in the middle, clipped so off-screen artwork stays hidden. Narrower screens such as the Steam Deck's 16:10 get thin bars.
- **LETTERBOX**: the original 16:9 picture with bars on any screen.
- **FILL SCREEN**: always fills. On narrower screens the 3D view shows more at the top and bottom; the menus stretch slightly.

The widescreen camera and menu handling come from davidptrssvea's v1.1.0.

### Achievements

All 29 achievements (1000 gamerscore) come from your copy of the game and unlock as you play.

- **Unlock popup:** slides into the bottom right corner with the achievement's icon, name and gamerscore, and a short ping-pong sound made by the game build itself.
- **Achievements list:** open it from **EXTRAS > ACHIEVEMENTS**, by holding **View/Back** on a controller for a moment, or with F7. Scroll with the D-pad or stick and close with B.
- The three online achievements unlock through ONLINE and SYSTEM LINK matches: The Noob for finishing a match, The Victor for winning one, The Spectator for watching one through TABLE TENNIS NETWORK.

<p align="center">
  <img src="docs/images/linux-achievement-popup.jpg" width="60%" alt="Achievement unlocked popup"><br>
  <img src="docs/images/linux-achievements.jpg" width="80%" alt="Achievements list">
</p>

### Online and System Link

Two players can play each other on the same network or over the internet. Both main menu entries open the game's own online menus:

- **ONLINE** plays over the internet through the project's online server, with no ports or addresses to set up. It first asks how: **PLAY ANYONE** joins the public room; **PLAY WITH A FRIEND** shows a six-character code to give your friend (**SHOW MY CODE**) or lets you type theirs (**ENTER CODE**). The match runs directly between the two machines when the networks allow it, and through the server when they do not.
- **SYSTEM LINK** plays on the local network, for example a Steam Deck and a PC on the same Wi-Fi.

Then one player chooses **PLAYER MATCH > CREATE MATCH**, sets up the match and waits in the lobby; the other chooses **PLAYER MATCH > QUICK MATCH**, which finds it and joins. The host starts the match.

**Watching a match:** others can watch a match in progress. Choose **TABLE TENNIS NETWORK > EXHIBITION**, wait for the search, select the match with A and choose **SPECTATE MATCH**. The match plays live with the score, and the spectator controls (camera views, HUD) are shown on screen.

- The game was made for local networks: every action waits for the other machine, so online matches play best between players in the same region. Outside matches, a small box in the bottom right corner shows the connection: bars, the round trip in milliseconds, packet loss, and whether it runs directly or through the server. In the public room the server pairs players who are close to each other first; the ONLINE prompt shows how many players are near you.
- A short connection drop (Wi-Fi hiccup, switching networks) no longer ends a match: the game waits up to about 15 seconds for the other player to come back, where it gave up after 3. A player who really leaves is noticed after those 15 seconds.
- On lossy connections the game sends every match packet twice (`online_redundancy=` in `tt-recomp.ini`: `auto`, the default, only while packets get lost; `on`; `off`).
- Every player is named after their computer with a short code (for example `steamdeck-4f2`) until they choose a name. The game tells players apart by name, so the server does not let two players with the same name into one room.
- System Link uses UDP ports 11000, 11001 and 13075 on the local network. A PC firewall must allow them, for example `sudo ufw allow 11000:11001/udp` and `sudo ufw allow 13075/udp`; SteamOS has no firewall by default. Online play needs no open ports.
- If machines on the same network do not find each other (some access points and VPNs drop broadcasts), add `lan_peers=` with the other machine's IP address to `tt-recomp.ini` on both.
- To use another server (for example your own, see [server/README.md](server/README.md)), set `server=` in `tt-recomp.ini`. RANKED MATCH, TABLE TENNIS NETWORK and LEADERBOARDS were Xbox LIVE services and are not available. `system_link=false` turns both entries off.

### Steam Deck

- Add the game to Steam during installation and play it in Game Mode with the Deck's controls; outside Steam, the Deck's desktop layout turns B into Escape and the right stick into a mouse.
- It runs at 60 FPS, and sleep and resume in the middle of a match work. The launcher uses Feral GameMode when it is installed.
- The picture is the original 16:9 with thin bars; **FILL SCREEN** uses the whole 16:10 screen.
- The first time a scene appears its shaders are compiled in the background, so an effect can be missing for a moment; after that they come from the shader cache.

### Tips

- Keep the frame rate at 60. The game advances its simulation once per frame and was built for 60 frames per second: higher settings only repeat frames, and uncapped presentation slows gameplay down.
- On a PC with a strong GPU, `render_scale=2` (or `3`) in `tt-recomp.ini` renders the 3D view above the screen's resolution and scales it down for a sharper picture. The Steam Deck cannot hold 60 FPS with it.

## macOS (experimental)

`build.sh` also builds the game on Macs with Apple silicon, rendering through MoltenVK (Vulkan on Metal). It starts and plays, but it is not part of the releases yet: on an M2 it runs at about 25 to 35 game frames per second, because the GPU emulation starts far more render passes per frame than Apple's tile-based GPUs handle well, and Low Power Mode holds the GPU at its lowest clock. Resizing the window can turn the picture black. Faster rendering on Apple silicon is being worked on.

## What this port changes

<p align="center">
  <img src="docs/images/linux-match-before-fix.jpg" width="49%" alt="Before: the arena floor and players render almost black">
  <img src="docs/images/linux-match.jpg" width="49%" alt="After: correct lighting">
</p>

**Engine fixes** (small patches in `project/sdk-patches/`, applied to the pinned SDK source):

1. **Lighting (above).** The Vulkan shader translator read each texture's exponent adjustment from the wrong word of the texture descriptor, so textures with a LOD bias came out far too dark: a black arena floor, dark skin, a dark character select. (Upstream PR rexglue/rexglue-sdk#490.)
2. **Full-speed gameplay.** Every game write to memory the GPU watches looked up page permissions by parsing `/proc/self/maps` as text, which held the Steam Deck to 24-27 game frames per second (slow motion). It now uses the kernel's `PROCMAP_QUERY` call (Linux 6.11+), with the old method as a fallback.
3. **Correct picture shape.** The SDK reported the window size to the game as its TV mode, which stretched the picture on 16:10 screens and switched small windows to the 4:3 SD layout.
4. **CPU and battery.** The audio thread spun a whole CPU core waiting for work, a debug-only lookup ran on every GPU register write, and the game's wait for the GPU spun a core at full speed (it now sleeps between polls).
5. **Controller rumble.** Gamepads report whether they can rumble, and failed rumble requests are logged.
6. **RenderDoc.** An installed RenderDoc was injected into every run. (Upstream PR rexglue/rexglue-sdk#522.)
7. **FSR and CAS** work on Linux (the FidelityFX runtime for temporal FSR 2/3, whose shader build needs a Windows compiler, is left out).
8. **System Link and online play**: networking for the game's own system link sessions, an online client for the matchmaking server, connection quality and packet redundancy.
9. **Stability**: the 4.5 GB guest memory file no longer outlives a crashed game, and a fault nothing handles is reported instead of hanging silently.
10. **Settings that apply at once**: anti-aliasing and sharpening switch while the game runs, and brightness is applied through the game's gamma ramp.
11. **Texture hook**: the game can look at a texture just before it reaches the GPU, which the PlayStation button icons use.

**Game-side changes** (in `project/src/` and as verified edits to the generated code):

- **Frame pacing** from davidptrssvea's Windows version: each frame is held to a fixed schedule before it goes to the GPU, so every frame takes the same 16.67 ms instead of an occasional 24 ms hitch.
- **Widescreen** from the same version, extended to narrower screens and to window resizing.
- The PC SETTINGS page, achievements UI, haptics, PlayStation button icons, update system, the ONLINE prompt, QUIT GAME and the player name editor.

The [devlog](docs/DEVLOG.md) tells the story of the port.

## For developers

Building from a checkout needs `python3`, `git`, `cmake`, `ninja` and `clang` 19 or newer on x86_64 Linux, or Xcode 16 or newer (its command line tools are enough) on a Mac with Apple silicon:

```sh
./build.sh --iso /path/to/TableTennis.iso
./dist/tt-recomp.sh
```

`build.sh` verifies and extracts the ISO read-only, builds ReXGlue SDK v0.10.0 from source into `.tools/rexglue-sdk` (`--sdk PATH` uses an existing one) with the patches in `project/sdk-patches/`, runs code generation, applies the verified native edits and compiles the game into `dist/`. A development build has no version file, shows DEV BUILD in PC SETTINGS and never offers updates.

`dist/tt-recomp.sh` is the launcher. It reads `tt-recomp.ini` next to it (keys: `iso_path`, `resolution`, `framerate`, `fullscreen`, `show_fps`, `aa`, `post`, `aspect`, `vsync`, `brightness`, `button_prompts`, `achievement_sound`, `haptics`, `render_scale`, `check_updates`, `auto_update`, `gamemode`, `player_name`, `lan_peers`, `server`, `online_redundancy`, `system_link`, `keyboard`, `engine_flags`). Command-line flags override it, for example `./dist/tt-recomp.sh --resolution=1440p --fullscreen=true`; `--save` writes them back, and anything after `--` goes straight to the engine.

| Path | What it is |
|---|---|
| `project/src/` | game-side code: PC SETTINGS, achievements UI, haptics, button icons, online prompt, widescreen, frame pacing, update popup, intro skip |
| `project/assets/button_prompts/` | the PlayStation button icons (SVG and PNG); `project/tools/make_button_art.py` turns them into `project/src/button_prompt_art.inc` |
| `project/sdk-patches/` | the engine fixes listed above |
| `project/tools/` | ISO reader, menu patch (PC SETTINGS, ONLINE, SYSTEM LINK, QUIT GAME), native edits |
| `server/` | the online matchmaking and relay server ([README](server/README.md)) |
| `tools/installer/` | the graphical installer (SDL3 + Dear ImGui); `tools/installer/build.sh` builds it |
| `scripts/install/` | the install and update engine used by the installer ([README](scripts/install/README.md)) |
| `scripts/release/` | building and signing releases ([README](scripts/release/README.md)) |
| `scripts/autotest.sh`, `scripts/lan-test.sh`, `scripts/scenarios/` | unattended test runs under Xvfb, one or two instances |
| `scripts/rdc/` | RenderDoc analysis scripts used to find the rendering bugs |
| `scripts/deck/` | a private all-in-one Steam Deck package built in a container (for your own use only, since it contains the compiled game) |

## What is in a release

Releases contain only open-source, game-free files: the installer and the source code of this project, the prebuilt ReXGlue SDK (an open-source Xbox 360 runtime, with the patches above), slim build tools (Ubuntu 24.04 packages and LLVM's clang, with the source packages of the GPL parts attached to the release), a manifest with every file's SHA256, and its Ed25519 signature. No game files, no game code and nothing from the disc are distributed; the game is always built on the player's machine from their own disc. The source code refers to the game only by function addresses and checksums, and the screenshots show Rockstar's game to document this port.

## Windows

This project is the Linux port of [davidptrssvea/Table-Tennis-Rockstar-PC](https://github.com/davidptrssvea/Table-Tennis-Rockstar-PC), the Windows recompilation it is built on. For Windows, use their installer and instructions. Thanks to davidptrssvea for the recompilation, the local-build installer approach, and the widescreen, frame pacing and quit-to-desktop work adopted here.

## License and rights

Desktop icon artwork by **Gary aka POOTERMAN :)** — [visit POOTERMAN on DeviantArt](https://www.deviantart.com/pooterman). The artwork is credited separately and is not covered by this project's MIT license.

Project-authored integration, installer and tools use the [MIT license](LICENSE). Adapted ReXGlue SDK headers, the ISO application adapter and SDK-derived CMake integration retain their BSD 3-Clause notices; see [ReXGlue license](licenses/ReXGlue/LICENSE.txt). [SDL's notice](licenses/SDL3/LICENSE.txt) is retained, and the Noto Sans font used by the achievements and settings pages is under the SIL Open Font License (`licenses/NotoSans/`).

Rockstar Games and Take-Two retain rights to their original game and material, including the game imagery in the screenshots, which are included only to document this port. This project is independent and unaffiliated. No project license grants rights to the original game. Local generation reduces what this project distributes; it is **not a guarantee of legal compliance**. You must have the necessary rights to use and process your copy.
