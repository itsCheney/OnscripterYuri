# onsyuri_mikage

Builds ONScripter Yuri as `ONSRuntime.framework`, the ONScripter engine of [Mikage Next](https://github.com/itsCheney/Mikage-Next), an iOS visual novel player that runs several engines in one app.

## How it differs from the other ports

- **One shared SDL.** Mikage ships a single `SDL3.framework` for all its engines. This build doesn't include SDL2: it links [sdl2-compat](https://github.com/itsCheney/sdl2-compat/tree/shared-sdl3/ons) statically, which translates ONScripter's SDL2 calls to that SDL3. SDL2_image, SDL2_mixer and SDL2_ttf are built against sdl2-compat with their bundled codecs only (stb_image, stb_vorbis, minimp3, dr_flac, FreeType).
- **The app keeps its main loop.** ONScripter blocks in `SDL_WaitEvent` and ends with `exit()`. Here it runs on its own stack on the main thread (`MikageONSFiber.S`). `MikageONSHooks.h` is force-included into the engine sources and sends `SDL_WaitEvent`, `SDL_Delay`, `SDL_RenderPresent` and `exit` to the host. Each of these hands control back to Mikage, and the next display refresh (`MikageONSStep`) resumes the engine.
- **MIDI and movies.** SDL_mixer's native MIDI backend is macOS-only, so `native_midi_ios.c` replaces it: each song plays through Apple's MIDI synthesizer with the SoundFont the host passes to `Mix_SetSoundFonts` (Mikage ships GeneralUser GS inside the framework). Movies (`mpegplay`, `movie`, `avi`) are decoded with FFmpeg in `MikageONSMovie.cpp` and drawn into the game's window, with their sound mixed through SDL_mixer. The build links FFmpeg's static libraries from `MIKAGE_FFMPEG_ROOT`.
- **Host API.** `MikageONSRuntime.h` replaces `onscripter_main.cpp`. It covers starting a game, stepping, quitting, background/foreground, the engine window and screenshots. It detects Shift_JIS, GBK or UTF-8 scripts. When the game folder has no `default.ttf`, it uses a system font (Hiragino Sans for Japanese, PingFang for Chinese).
- **Backlog and saves.** With a panel callback, the game's lookback, save and load screens open as the host's own panels (`ONScripter_mikage.cpp`). The backlog keeps each text page of the session with its voice and a compressed copy of the page's save point, so the newest 200 lines can be jumped back to. Saves stay the game's own `saveN.dat`; each also gets `Mikage/saveN.jpg` and `Mikage/saveN.json` (the screen and the line) for the host's save lists. A launch can also start straight from a save slot.

## Engine changes

They are small and inactive outside this build:

- `Utils.h` sends log messages to the host when `MIKAGE_ONS_HOST` is defined.
- `FontInfo::forgetOpenFonts()` lets the host run another game after `TTF_Quit()`.
- `ONScripter::getWindow()` and `getFrameSurface()` give the host the window and the last frame; `getRenderViewRect()` and `readGameFile()` let the movie player draw over the game and read files from archives.
- A window size change recomputes where the game is drawn, as a resize does.
- The iOS `playAVI` path no longer uses an undefined `loop_flag`.
- Text save points, page clears, loads, channel 0 voices, written saves and system calls are reported to `ONScripter_mikage.cpp`, which can take over the lookback, save and load system calls. The `game` command loads the launch's save slot, if any.

## Building

Mikage's `scripts/build-ons-ios.sh` builds the device and simulator slices and packs them into `ONSRuntime.xcframework`. To build one slice by hand:

```sh
cmake -S src/onsyuri_mikage -B build/ons-ios -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
  -DMIKAGE_SDL3_FRAMEWORK=/path/to/ios-arm64/SDL3.framework \
  -DMIKAGE_FFMPEG_ROOT=/path/to/ffmpeg/arm64-ios
cmake --build build/ons-ios --config Release --target ONSRuntime
```

The license is the same as the rest of ONScripter Yuri (GPL-2.0-or-later).
