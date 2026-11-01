# Graphical installer

`install-table-tennis` is the setup program for the Linux / Steam Deck release. A player drops
their Table Tennis ISO onto it, confirms a few options and watches the game being built; the
actual work is done by `scripts/install/tt-install.sh`, which the installer runs as a child
process and reads progress from. It is one static binary (SDL3 + Dear ImGui, software or
OpenGL rendering, no Vulkan needed) that runs on SteamOS and ordinary desktops, and it is drawn
in the game's own menu style: navy pills, blue halftone swooshes, heavy condensed yellow
headings, the light "ACCEPT (A)" footer bar.

## Flow

1. **Welcome** – a sentence on what happens, the drop zone (`SDL_EVENT_DROP_FILE`), `Browse...`
   (`SDL_ShowOpenFileDialog`, `.iso` filter) and, on the right, disc images found by a background
   scan of Downloads, Desktop, Games, ROMs, Documents, the home folder and mounted media
   (`/run/media/*`, `/media/*`), two levels deep, `.iso` files over 1 GB, best match first
   ("Found TableTennis.iso on your SD card · 7.3 GB · USE IT").
2. **Check** – the image is verified on a worker thread (see *Verification* below). The result is a
   green tick with "This is the right disc", which continues by itself after 2.5 s, or a red cross
   with a plain explanation: not an Xbox 360 image, a different game, an unsupported version or
   region, a modified image, a damaged or unreadable file.
3. **Options** – one screen, sensible defaults:
   * *Install location* as cards: internal storage `~/Games/TableTennis` (recommended), each SD card
     or external drive as `<mount>/Games/TableTennis` ("Works, loads a little slower"), and
     "Choose another folder..." (`SDL_ShowOpenFolderDialog`, with a typed-path fallback when no
     portal or zenity is available). Each card shows its free space; cards without enough space
     are greyed out with the reason. A note explains that the ISO stays where it is.
   * *Add to Steam as a non-Steam game* (default on; the subtitle mentions Game Mode on SteamOS),
     *Desktop shortcut* (default on), and, when another drive is chosen, *Keep build files on the
     same drive* (automatically on when the home drive has less than 10 GB free).
   * *What to expect*: 20–40 minutes the first time, about 10 GB temporarily for build tools and
     caches (the installed game itself is small), missing tools are downloaded once, nothing from
     the disc is uploaded.
4. **Install** – the step list on the left (ticks, the current step highlighted; steps the backend
   skips disappear, unknown ones are added), a large percentage and progress bar with a sheen,
   elapsed and estimated remaining time, the latest log line, `Show details` for the full log
   (scrollable, follows the end), warnings as a yellow strip that never blocks, and `Cancel`
   (confirmed first; SIGINT to the process group, SIGTERM after 5 s, SIGKILL after 6 more).
5. **Done** – a big `PLAY` (starts `<prefix>/tt-recomp.sh` detached and closes), `Open install
   folder`, where to find the game in Steam ("In Game Mode: Library > Non-Steam" on SteamOS) and
   any warnings the backend raised (for example that Steam needs a restart).
6. **Error** – the backend's `@@error` text, which step it was in, `Try again`, `Show details`,
   `Copy log` (clipboard) and the path of the saved log
   (`$XDG_STATE_HOME/tt-recomp-installer/install.log`, normally `~/.local/state/...`).

Escape / Backspace / the B button goes back on every screen (with a confirmation where it matters).

## Controls

Mouse, touch (SDL turns taps into mouse events), keyboard (arrows or Tab move the focus, Enter or
Space activate, Escape goes back) and SDL gamepads (D-pad or left stick move, A activates, B goes
back) all drive the same widgets. The focus ring only appears after keyboard or gamepad input.
The window is resizable (minimum 800×520); everything scales from the 1280×800 design size.

## Backend

Command line (built from the options screen):

    scripts/install/tt-install.sh --iso PATH --prefix DIR --steam yes|no --desktop yes|no [--cache DIR]

`--cache` is only passed when *Keep build files on the same drive* is on; it is then
`<prefix>/.build-cache`. Output protocol (stdout, one message per line, anything else is log text):

    @@step <id> <title>    check tools image extract sdk codegen build install integrate
    @@progress <0-100>     overall percent
    @@log <text>
    @@warn <text>          shown without interrupting
    @@error <message>      then a non-zero exit
    @@done <install dir>

The step list starts from the known ids in that order (`src/backend.cpp`, `KnownSteps()`) with
default titles and time weights; an announced step replaces its default title, skipped known steps
are removed, unknown ids are inserted after the current step. The bar shows the larger of the
reported percentage and the weight of the finished steps, creeping slowly through the active step
while nothing is reported, so it always moves and never goes backwards. Remaining time comes from
the weights until the backend's own rate has settled (15 s and 4 %), then from that rate, smoothed.

**Finding the script.** The installer walks up from its own directory (up to six levels) looking
for `scripts/install/tt-install.sh`, so the shipped binary may sit at the root of the unpacked
source tree (`./install-table-tennis`) or stay in `tools/installer/bin/`. `--backend PATH` overrides
the lookup. The checkout root (two levels above `scripts/install/`) also provides
`project/tools/disc_image.py` for verification.

**Verification** lives in one place, `src/iso_check.cpp`. By default it runs the project's own
read-only check (`project/tools/disc_image.py: read_inputs`) through `python3 -I` and maps its
messages to the friendly verdicts above. When `--backend` is given, it instead asks that script
`--verify PATH` and expects `@@verify ok <description>` or `@@verify fail <reason>` on stdout; the
real backend can adopt the same answer and the switch is the `ask_backend` flag in
`App::StartCheck`.

Child-process details: the script runs in its own session (`setsid`) with stdout and stderr merged
into a pipe read by a thread, `PYTHONUNBUFFERED=1` and `TT_INSTALLER_GUI=1` set; the installer
`waitpid`s with `WNOHANG` each frame, so the window never blocks. Everything the script prints is
mirrored to the log file above.

## Building

    tools/installer/build.sh            # in the Ubuntu 24.04 container -> portable release binary
    tools/installer/build.sh --host     # with this machine's clang, for development

Both write `tools/installer/bin/install-table-tennis`. SDL3 (static, audio/camera/sensor/GPU/Vulkan
off) and Dear ImGui with its SDL3 and SDL_Renderer backends are compiled from the SDK checkout's
`.tools/rexglue-src/thirdparty/`, so `./build.sh` must have run once; `-DTT_THIRDPARTY_DIR` points
elsewhere. The fonts (`project/assets/fonts/NotoSans-*.ttf`, SIL OFL, `licenses/NotoSans/`) and the
project icon (`project/assets/tabletennis.ico`, credit in `licenses/Icon/CREDITS.txt`) are embedded
with `.incbin` (`src/assets.S`), and libstdc++/libgcc are linked statically, so the binary needs only
libc and libm (glibc 2.38 from the container; X11/Wayland/OpenGL are loaded at run time if present).
Nothing from the game is bundled; every shape is drawn procedurally. The container build adds
`-march=x86-64-v3` like the game itself.

`SDL_RENDER_DRIVER=software` forces the CPU renderer (used for the virtual-display screenshots);
by default SDL picks OpenGL where available.

## Developing without the backend

`fake-backend.sh` speaks the protocol with short sleeps and only writes a stand-in
`tt-recomp.sh` under `--prefix`:

    tools/installer/bin/install-table-tennis --backend tools/installer/fake-backend.sh --scan-dir /some/dir

Environment knobs: `FAKE_SPEED` (seconds per tick, default 0.4), `FAKE_FAIL=<step id>` (that step
ends with an `@@error`), `FAKE_SKIP_TOOLS=1` (emits `image` instead of `tools`),
`FAKE_STEAM_RUNNING=1` (the Steam restart warning), `FAKE_VERIFY_DELAY=<s>` (slow `--verify`).
Names containing `bad`, `wrong` or `other` fail verification.

Installer options for testing: `--scan-dir DIR` adds a folder to the ISO scan, `--iso PATH` starts
at the check screen, `--size WxH` sets the window size, and `TT_INSTALLER_FAKE_MOUNTS="dir|label|sd;
dir2|label2|usb"` pretends those folders are mounted media (for the location cards and the scan).

`screenshots.sh OUT_DIR` runs the whole flow on Xvfb displays :95 (1280×800) and :94 (1920×1080)
with xdotool and ImageMagick's `import`, with `HOME` pointed into `OUT_DIR/work` so nothing real is
touched, and captures every screen.

## Sources

| file | role |
|---|---|
| `src/main.cpp` | SDL window, renderer, ImGui setup, input mapping (keyboard, gamepad with repeat), frame loop, icon decoding (stb_image) |
| `src/app.h/.cpp` | state machine: screens, chosen disc, locations and options, driving the backend process, progress smoothing |
| `src/screens.cpp` | the six screens, the confirmation modal and the typed-path fallback |
| `src/ui.h/.cpp` | palette, fonts, text and shape helpers, focus/navigation, widgets (button, card, toggle, checkbox, field), background, header, footer |
| `src/backend.h/.cpp` | protocol parser, known steps, script lookup, child process with cancel |
| `src/iso_check.h/.cpp` | the verification adapter |
| `src/system.h/.cpp` | home, mounts, free space, SteamOS detection, ISO scan thread, detached launch |
| `src/assets.S` | embedded fonts and icon |
