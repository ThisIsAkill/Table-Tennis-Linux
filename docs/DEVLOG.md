# Devlog

## 2026-10-31 — First Linux boot

Started the Linux port from the Windows recompilation by David Petersson. The upstream tools for reading the disc already work anywhere Python runs, so I imported them directly instead of going through the Windows-only setup script. My retail ISO passed both hash checks (the executable and the menu archive), and the menu patch that removes Xbox Live and shortens the startup applied cleanly.

ReXGlue has no prebuilt Linux SDK that matches upstream, so I built tag v0.10.0 from source with its Linux release preset and installed it inside the project. Pinning that exact tag matters: upstream's native edits are checked by hash against the generated code, and all 17 of them matched on the first try. Code generation takes about five seconds.

The Windows-specific code in the game layer turned out to be small. The Windows header, the Direct3D 12 tearing option and the Windows font lookup are now only compiled on Windows; on Linux the loading font comes from the usual system font folders, and the executable folder comes from the SDK instead of a Win32 call. Turning vsync off is enough for high-framerate presentation on Vulkan, which chooses mailbox or immediate mode by itself. The CMake project now copies the runtime library and the Vulkan GPU plugin next to the game, because the SDK loads the plugin from the executable's own folder.

In place of the Windows launcher there is a small shell launcher that reads `tt-recomp.ini` and accepts the same options as flags, then starts the game with the same engine settings the Windows launcher uses. `build.sh` runs the whole chain from ISO to a runnable `dist/` folder. FSR and CAS are not available yet because the SDK was built without FidelityFX, so the launcher defaults to bilinear scaling.

The first build compiled and linked without errors in about 36 seconds with clang 23 and ninja. On the first run, in a virtual display on an RTX 3070, the game mounted the ISO, picked up the offline menu archive, started audio, and played the copyright screen followed by the Rockstar logo intro. That is milestone 4.

Still open: the presentation counter reported about 15 FPS for the whole run, which may come from the virtual display rather than the game, and needs a run on a real display to tell. Next comes reaching the main menu and a match, then audio, controller and save checks.
