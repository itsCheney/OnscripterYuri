#pragma once

// Host API that lets Mikage run ONScripter Yuri inside its own app.
//
// The engine keeps its blocking main loop, but runs on a separate stack on
// the main thread. Each MikageONSStep() resumes it until it presents a frame
// or waits for input, then returns to the host. Every call here must be made
// on the main thread.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*MikageONSCompletionCallback)(bool success, const char *message, void *context);
typedef void (*MikageONSMenuCallback)(void *context);
// level follows the KRKR host: 3 info, 5 error. The message is borrowed for
// the duration of the call. May be called on engine worker threads.
typedef void (*MikageONSLogCallback)(int32_t level, const char *message);

void MikageONSSetLogCallback(MikageONSLogCallback callback);

typedef struct MikageONSLaunchOptions {
    // Folder holding the script (0.txt, 00.txt, nscript.dat...) and archives.
    const char *gamePath;
    // Where save files and envdata go. Created if missing.
    const char *saveDirectory;
    // TTF/TTC used when the game folder has no default.ttf. NULL picks a
    // system font that matches the script encoding.
    const char *fontPath;
    // "sjis", "gbk", "utf8" or NULL to detect it from the script.
    const char *encoding;
    // Follow the Ring/Silent switch (ambient) instead of always playing.
    bool respectSilentMode;
    // A three-finger tap opens the host menu instead of ONScripter's own
    // right-click menu.
    bool menuGestureEnabled;
    // Widen a landscape game narrower than 16:9 (such as 4:3) to 16:9
    // instead of keeping its aspect ratio. Either way it is letterboxed.
    bool stretchToScreen;
} MikageONSLaunchOptions;

// Starts the game and runs it until its first frame. Returns false with
// MikageONSLastError() set when it can't start; completion is then not
// called. Otherwise completion is called once, from MikageONSStep(), when the
// game ends.
bool MikageONSStart(const MikageONSLaunchOptions *options,
                    MikageONSMenuCallback menu,
                    MikageONSCompletionCallback completion,
                    void *context);

// Call once per display refresh while running.
void MikageONSStep(void);

// Asks the game to quit the way closing its window would. ONScripter saves
// its settings and ends; completion follows on a later step.
bool MikageONSRequestExit(void);

// Sends the game an Escape key press, which ONScripter treats like a right
// click: it closes menus and steps back. For games without an on-screen way
// back. Delivered on the next step.
bool MikageONSSendBack(void);

// Like RequestExit, but ends the session even if the game doesn't respond.
void MikageONSRequestStop(void);

// Pauses the game and its audio in the background, and resumes them.
bool MikageONSSetForeground(bool foreground);

bool MikageONSIsRunning(void);
const char *MikageONSLastError(void);
// The engine's UIWindow, or NULL before it exists.
void *MikageONSNativeWindow(void);
// Script encoding in use: "sjis", "gbk" or "utf8"; empty when not running.
const char *MikageONSEncoding(void);

// Copy of the last composited game frame, top-down RGBA8 with straight
// alpha, at the game's own resolution. Free pixels with
// MikageONSFreeCapturedFrame.
typedef struct MikageONSCapturedFrame {
    uint8_t *pixels;
    int32_t width;
    int32_t height;
    int32_t pitch;
} MikageONSCapturedFrame;
bool MikageONSCaptureFrame(MikageONSCapturedFrame *frame);
void MikageONSFreeCapturedFrame(MikageONSCapturedFrame *frame);

#ifdef __cplusplus
}
#endif
