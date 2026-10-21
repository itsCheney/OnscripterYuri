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
    // Save slot (saveN.dat) to load once the script's define section ends,
    // instead of starting from the title. 0 starts normally.
    int32_t loadSlot;
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

// Frame statistics since the previous call, for the host's performance
// display. Call about once a second; false when no game is running.
typedef struct MikageONSStats {
    // Frames the game presented per second, and the average gap between them.
    double framesPerSecond;
    double frameTimeMilliseconds;
    // Time the engine ran per step (one display refresh), average and worst.
    double cpuFrameTimeMilliseconds;
    double maxCpuFrameTimeMilliseconds;
    // Size of the window's drawable in pixels.
    int32_t drawableWidth;
    int32_t drawableHeight;
    // SDL's renderer, e.g. "metal".
    char renderer[32];
    // Textures the engine drew per presented frame. SDL may batch them
    // into fewer GPU draw calls.
    double drawCallsPerFrame;
} MikageONSStats;
bool MikageONSGetStats(MikageONSStats *stats);

// Mikage's backlog, save and load panels.
//
// With a panel callback set, the game's lookback, save and load screens
// (from its right-click menu or systemcall) open as host panels instead of
// ONScripter's own. The engine waits while a panel is open; the host ends
// it with exactly one of Cancel, Save, Load or Jump. Strings stay valid
// until then.
//
// Save slots are the game's own saveN.dat in the save directory. For each
// save, the engine also writes Mikage/saveN.jpg (the screen) and
// Mikage/saveN.json ({"name": speaker, "text": line on screen}) there.
typedef enum MikageONSPanelKind {
    MikageONSPanelKindNone = 0,
    MikageONSPanelKindBacklog = 1,
    MikageONSPanelKindSave = 2,
    MikageONSPanelKindLoad = 3,
} MikageONSPanelKind;

// Called with the panel that opened, and with MikageONSPanelKindNone when the
// engine closed it itself (the game is ending). Called on the main thread.
typedef void (*MikageONSPanelCallback)(int32_t panel, void *context);
// Set before MikageONSStart; NULL keeps ONScripter's own screens.
void MikageONSSetPanelCallback(MikageONSPanelCallback callback);

// Opens a panel over the game, as its menu would. Only while the game
// waits for a click on its text; see MikageONSCanOpenPanel.
bool MikageONSOpenPanel(int32_t panel);
bool MikageONSCanOpenPanel(void);
// The open panel, or MikageONSPanelKindNone.
int32_t MikageONSOpenPanelKind(void);
// The game's number of save slots (savenumber).
int32_t MikageONSSaveSlotCount(void);

// Lines shown this session, oldest first; the last is the one on screen.
typedef struct MikageONSBacklogEntry {
    // Speaker from the line's [name] tag, or empty. UTF-8.
    const char *name;
    // The text as the game showed it, with line breaks. UTF-8.
    const char *text;
    bool hasVoice;
    // The game can go back to where this line began.
    bool canJump;
    bool isCurrent;
} MikageONSBacklogEntry;
int32_t MikageONSBacklogCount(void);
bool MikageONSBacklogGetEntry(int32_t index, MikageONSBacklogEntry *entry);
// Plays the line's voice again; the panel stays open.
bool MikageONSBacklogReplayVoice(int32_t index);

bool MikageONSPanelCancel(void);
// Saves to slot 1...MikageONSSaveSlotCount() and closes the panel.
bool MikageONSPanelSave(int32_t slot);
// Loads the slot; the game continues from there.
bool MikageONSPanelLoad(int32_t slot);
// Goes back to where the line began; later lines leave the backlog.
bool MikageONSPanelJump(int32_t index);

// Quick actions and the game menu's playback controls.
typedef enum MikageONSCommand {
    // Toggle auto mode or skip mode, as the a and s keys do. Skip follows
    // the game's kidokuskip setting.
    MikageONSCommandAuto = 1,
    MikageONSCommandSkip = 2,
    // Save to or load from MikageONSQuickSaveSlot, while the game waits on
    // its text (MikageONSCanOpenPanel).
    MikageONSCommandQuickSave = 3,
    MikageONSCommandQuickLoad = 4,
} MikageONSCommand;

// The quick save's own slot, outside the game's save menu.
#define MikageONSQuickSaveSlot 1000

typedef enum MikageONSControl {
    MikageONSControlAuto = 1 << 0,
    MikageONSControlSkip = 1 << 1,
    MikageONSControlCanAuto = 1 << 8,
    MikageONSControlCanSkip = 1 << 9,
    MikageONSControlCanQuickSave = 1 << 10,
    MikageONSControlCanQuickLoad = 1 << 11,
} MikageONSControl;

// Queues the command for the engine; it runs at the next event check.
bool MikageONSRunCommand(int32_t command);
uint32_t MikageONSGameControls(void);
// For the diagnostics log: event_mode in the low 16 bits, then
// current_mode, system_menu_mode and the open panel, 8, 4 and 4 bits.
uint32_t MikageONSWaitState(void);
// Holds or releases Ctrl, which skips every line (read or not) while held.
bool MikageONSSetControlSkip(bool held);

// The host's trackpad cursor: a mouse at x, y in window points.
typedef enum MikageONSPointerPhase {
    MikageONSPointerMove = 0,
    MikageONSPointerDown = 1,
    MikageONSPointerUp = 2,
    MikageONSPointerSecondaryDown = 3,
    MikageONSPointerSecondaryUp = 4,
} MikageONSPointerPhase;
bool MikageONSSendPointer(int32_t phase, float x, float y);

#ifdef __cplusplus
}
#endif
