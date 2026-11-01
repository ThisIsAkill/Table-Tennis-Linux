# Devlog

## 2026-10-31 — First Linux boot

Started the Linux port from the Windows recompilation by David Petersson. The upstream tools for reading the disc already work anywhere Python runs, so I imported them directly instead of going through the Windows-only setup script. My retail ISO passed both hash checks (the executable and the menu archive), and the menu patch that removes Xbox Live and shortens the startup applied cleanly.

ReXGlue has no prebuilt Linux SDK that matches upstream, so I built tag v0.10.0 from source with its Linux release preset and installed it inside the project. Pinning that exact tag matters: upstream's native edits are checked by hash against the generated code, and all 17 of them matched on the first try. Code generation takes about five seconds.

The Windows-specific code in the game layer turned out to be small. The Windows header, the Direct3D 12 tearing option and the Windows font lookup are now only compiled on Windows; on Linux the loading font comes from the usual system font folders, and the executable folder comes from the SDK instead of a Win32 call. Turning vsync off is enough for high-framerate presentation on Vulkan, which chooses mailbox or immediate mode by itself. The CMake project now copies the runtime library and the Vulkan GPU plugin next to the game, because the SDK loads the plugin from the executable's own folder.

In place of the Windows launcher there is a small shell launcher that reads `tt-recomp.ini` and accepts the same options as flags, then starts the game with the same engine settings the Windows launcher uses. `build.sh` runs the whole chain from ISO to a runnable `dist/` folder. FSR and CAS are not available yet because the SDK was built without FidelityFX, so the launcher defaults to bilinear scaling.

The first build compiled and linked without errors in about 36 seconds with clang 23 and ninja. On the first run, in a virtual display on an RTX 3070, the game mounted the ISO, picked up the offline menu archive, started audio, and played the copyright screen followed by the Rockstar logo intro. That is milestone 4.

Still open: the presentation counter reported about 15 FPS for the whole run, which may come from the virtual display rather than the game, and needs a run on a real display to tell. Next comes reaching the main menu and a match, then audio, controller and save checks.

## 2026-10-31 — Into a match, and a faster way to test

The game now gets all the way into a match on Linux. From the main menu it goes through the Tournament screens, the venue fly-over and the player introductions, and then plays points: serves, rallies, the rally counter and the score all work. The match music and sound effects come out of the speakers, and the game writes its save file and thumbnail to the user data folder.

Most of the session went into making test runs faster. Every check meant launching the game in a virtual display, sitting through the intro and clicking through menus, and the runs had become very slow. The reason turned out to be the window size: in the virtual display a 1080p window only reaches about 15 frames per second, while a 960x540 window runs at a steady 60. Since the game's clock follows its frames, the small window makes everything about four times faster. The old input scripts also pressed keys at fixed times, which went wrong whenever loading took longer than usual.

The new runner, `scripts/autotest.sh`, keeps the window small, keeps pressing Start until the game's own log says the main menu is ready, then plays the rest of the key presses timed from that moment and stops as soon as the scene it wants has been captured. A full trip into a match now takes about two and a half minutes, unattended, and each run writes a short summary with the time to the menu, the average frame rate and the most frequent warnings. `scripts/sweep.sh` queues runs with different engine settings, so a whole set of experiments can run in one go. The launcher also accepts engine options after `--` that replace its own defaults, which the runner uses for the window size.

One visual bug is clear now. In the National TT Auditorium the floor shows up red and lit during the venue fly-over, but during play the floor around the table is completely black, with no shadows. The menu's arena floor draws correctly. Turning off background shader compilation and switching to the other render-target method both leave the floor black, so it is not caused by either of those. A sweep over the depth, resolve and gamma options did not change it either.

Later in the night I narrowed it down further with a locally patched copy of the graphics SDK that can log every draw of one frame, skip or recolour draws by shader, and report what the resolved render targets contain. Drawing candidate shaders in solid magenta showed that the floor is drawn, as a single quad by one pixel shader, and that this shader produces black. The shadow maps it depends on are correct, and the floor's vertex colours and textures are fine. Its light comes from a half-resolution light mask that an earlier pass builds from the spotlight's cookie texture. Forcing the cookie, the light mask or the floor textures to white still leaves the floor black, so the cause sits between those inputs and the shader's output. The next step is a proper frame capture to look at that pass directly.

Two smaller findings from the same work: every test run that ended by being killed left a 4.5 GB guest-memory file behind in shared memory, which eventually filled it and crashed later runs; the test runner now removes those files. And the repeating "call to NULL" warning comes from a media-stream decoder object that was never created, not from rendering.

## 2026-10-31 — The black floor, found and fixed

The dark arena turned out to be a bug in the graphics SDK, not in the game code. With RenderDoc installed I could capture match frames and step through the floor's pixel shader on a real pixel. The vertex colour and the lighting inputs arrived intact, but every texture sample came back scaled down by 2 to the power of -13. That factor comes from the texture's exponent-adjust field, and the Vulkan shader translator was reading it from the wrong word of the texture descriptor: word 4, where the LOD bias lives, instead of word 3. Any texture with a LOD bias, which includes the arena floor and the players' skin, was effectively multiplied down to black. The Direct3D 12 path on Windows reads the right word, which is why the Windows build never showed it.

The fix is a few lines, kept as a patch in `project/sdk-patches/` that `build.sh` applies to the pinned SDK source before building it. A second small patch stops the SDK from loading RenderDoc into every run whenever it is installed. The floor is now red and lit with the table's shadow on it, the players' faces and arms are lit, and the character select screen shows the player properly instead of a dark silhouette. Two runs agree.

Getting there needed better tools than screenshots: a way to launch the game under RenderDoc from the test runner and capture a burst of frames in a match, and a few headless RenderDoc scripts that find a draw by its shader, read the vertex shader outputs, and dump a full shader debug trace for a pixel.

## 2026-10-31 — Soak test, Steam Deck package and the PC options

A 20-minute unattended session of back-to-back Exhibition games ran without a crash, with correct lighting throughout. Matches ran at about 47 frames per second in the virtual display, which looked like a problem until a smaller window brought them to a flat 60: the cost is copying frames into the virtual display, not the game. Profiling still turned up three threads that spin instead of sleeping (the audio worker, the frame pacer and a game thread waiting on the GPU), which costs nothing on a desktop but matters on the Steam Deck's smaller CPU.

The Linux build needed glibc 2.44, newer than SteamOS ships, so there is now a portable build made in an Ubuntu 24.04 container with clang 20 and the C++ runtime bundled alongside the game. It needs glibc 2.38, contains nothing from the disc, and a small setup script extracts and patches the menu files from your own ISO on the Deck and picks the new 1280x800 preset. It plays a full match on this PC; the Deck itself is next.

The PC options from the Windows version all work on Linux: 120 FPS and uncapped play at well over 100 frames per second, the 1440p and 4K internal resolutions and both FXAA modes hold 60 with no errors. The exception is AMD FidelityFX (FSR and CAS sharpening): its build runs AMD's Windows shader compiler under Wine, which rejects macro definitions with the current glslang, so Linux keeps bilinear filtering for now.

## 2026-11-01 — Running on the Steam Deck

The game now runs on a Steam Deck (SteamOS 3.8) at a steady 60 frames per second, with the Deck's own controls working as a real gamepad. Launched outside Steam, the Deck's desktop controller layout turns B into Escape, which quit the game on the spot, so on Linux Escape now has to be pressed twice within two seconds. Started as a non-Steam game, Steam presents the controls as an Xbox-style pad and everything works.

The first sessions had occasional hitches. Lining up the frame-rate log with the shader log showed why: every dip came right after a burst of new graphics pipelines being compiled, about nineteen in two seconds against almost none otherwise. The game keeps those pipelines in its cache and builds them at startup next time, and the third session started with 153 of them ready and ran without dips.

Profiling here also turned up the SDK's audio worker spinning a whole CPU core. Waits on Linux are cut into one-millisecond slices, and the remaining time was rounded down to whole milliseconds, so the last fraction became a zero-length sleep and the thread spun instead of sleeping. Sleeping for the real remaining time brought it from 99% of a core to about 5%, which on the Deck's four cores leaves room for the game. On the Deck the processor now sits about 70% idle during a match and the graphics chip at about 40%.
