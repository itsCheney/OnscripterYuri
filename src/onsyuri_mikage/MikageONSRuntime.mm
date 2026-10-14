// Runs ONScripter Yuri inside Mikage. See MikageONSRuntime.h.
//
// ONScripter is written as a program that owns the main loop: it blocks in
// SDL_WaitEvent and leaves through exit(). Mikage can't give up its main
// thread, and UIKit and SDL's video code must stay on it, so the engine runs
// on its own stack on the main thread. MikageONSHooks.h routes its waits,
// presents and exits here; each of them switches back to the host, and the
// next display refresh switches into the engine again.

#import <AVFoundation/AVFoundation.h>
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL_syswm.h>
#include <SDL2/SDL_ttf.h>

#include "FontInfo.h"
#include "ONScripter.h"
#include "gbk2utf16.h"
#include "sjis2utf16.h"

#include "MikageONSRuntime.h"

#include <dlfcn.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// Defined by Yuri's onscripter_main.cpp, which Mikage replaces.
Coding2UTF16 *coding2utf16 = NULL;
std::string g_stdoutpath;
std::string g_stderrpath;
// The running engine, also read by the built-in layer effects.
ONScripter *mikage_ons_engine = nullptr;

// Engine globals that must start from zero for each game.
extern SDL_TimerID timer_cdaudio_id;
extern SDL_TimerID timer_bgmfade_id;
extern Uint32 g_lastpress_time;
extern bool ext_music_play_once_flag;

extern "C" void mikage_ons_fiber_switch(void **save_sp, void *load_sp);
extern "C" void mikage_ons_fiber_trampoline(void);

// Engine-facing hooks; MikageONSHooks.h maps the engine's calls to these.
extern "C" {
void MikageONSDelay(Uint32 ms);
int MikageONSWaitEvent(SDL_Event *event);
void MikageONSRenderPresent(SDL_Renderer *renderer);
__attribute__((noreturn)) void MikageONSExit(int status);
void MikageONSLogV(int level, const char *format, va_list ap);
}

namespace {

// ONScripter expects a desktop-sized main thread stack.
constexpr size_t kFiberStackSize = 16u << 20;
// How long RequestStop waits for the game to end itself, in steps.
constexpr int kStopGraceSteps = 180;

std::atomic<MikageONSLogCallback> logCallback{nullptr};
std::mutex errorMutex;
std::string lastEngineError;
std::string lastError;

ONScripter *&engine = mikage_ons_engine;
MikageONSMenuCallback menuCallback = nullptr;
MikageONSCompletionCallback completionCallback = nullptr;
void *callbackContext = nullptr;

bool running = false;
bool foreground = true;
bool musicPausedByHost = false;
bool stopRequested = false;
int stopSteps = 0;
bool menuGestureEnabled = false;
bool menuGestureActive = false;
std::string encodingName;
std::string savedWorkingDirectory;

void *fiberStack = nullptr;
void *hostStackPointer = nullptr;
void *fiberStackPointer = nullptr;
bool onFiber = false;
bool fiberDone = false;
int exitStatus = 0;

void log(int32_t level, const char *message)
{
    if (level >= 5) {
        std::lock_guard<std::mutex> lock(errorMutex);
        lastEngineError = message;
    }
    if (auto callback = logCallback.load(std::memory_order_acquire))
        callback(level, message);
}

bool isEngineContext()
{
    return onFiber && pthread_main_np();
}

void yieldToHost()
{
    mikage_ons_fiber_switch(&fiberStackPointer, hostStackPointer);
}

// Runs the engine until it next waits, presents or ends. Returns true when
// it has ended.
bool resumeEngine()
{
    onFiber = true;
    mikage_ons_fiber_switch(&hostStackPointer, fiberStackPointer);
    onFiber = false;
    return fiberDone;
}

void engineMain(void *)
{
    int status = -1;
    try {
        if (engine->openScript())
            log(5, "ONScripter couldn't open the game's script.");
        else if (engine->init())
            log(5, "ONScripter couldn't initialize.");
        else {
            engine->executeLabel();
            status = 0;
        }
    } catch (const std::exception &error) {
        log(5, error.what());
    } catch (...) {
        log(5, "Unknown C++ exception in ONScripter.");
    }
    MikageONSExit(status);
}

bool prepareFiber()
{
    void *stack = mmap(nullptr, kFiberStackSize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
    if (stack == MAP_FAILED)
        return false;
    // The lowest page stays unmapped so an overflow faults instead of
    // corrupting the heap.
    mprotect(stack, getpagesize(), PROT_NONE);
    fiberStack = stack;

    // The frame mikage_ons_fiber_switch restores: x19-x28, fp, lr, d8-d15.
    auto top = reinterpret_cast<uintptr_t>(stack) + kFiberStackSize;
    auto *frame = reinterpret_cast<uint64_t *>((top - 160) & ~uintptr_t(15));
    std::memset(frame, 0, 160);
    frame[0] = reinterpret_cast<uint64_t>(&engineMain);                  // x19
    frame[1] = 0;                                                         // x20
    frame[10] = 0;                                                        // fp
    frame[11] = reinterpret_cast<uint64_t>(&mikage_ons_fiber_trampoline); // lr
    fiberStackPointer = frame;
    fiberDone = false;
    exitStatus = 0;
    return true;
}

void releaseFiber()
{
    if (fiberStack)
        munmap(fiberStack, kFiberStackSize);
    fiberStack = nullptr;
    fiberStackPointer = nullptr;
}

std::string readHead(const std::string &path, size_t limit)
{
    std::string data;
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file)
        return data;
    data.resize(limit);
    data.resize(std::fread(&data[0], 1, limit, file));
    std::fclose(file);
    return data;
}

bool isUTF8WithMultibyte(const std::string &text)
{
    bool multibyte = false;
    size_t i = 0;
    while (i < text.size()) {
        unsigned char c = text[i];
        size_t length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (!length)
            return false;
        if (i + length > text.size())
            break; // The sample may end inside a character.
        for (size_t j = 1; j < length; ++j) {
            if ((static_cast<unsigned char>(text[i + j]) & 0xC0) != 0x80)
                return false;
        }
        multibyte |= length > 1;
        i += length;
    }
    return multibyte;
}

// Japanese scripts are Shift_JIS and Chinese ones GBK, and both decode as
// the other without errors. Kana separate them: Shift_JIS hiragana and
// katakana use lead bytes 0x82 and 0x83, which GBK text almost never uses,
// while GBK's common hanzi fill lead bytes 0xB0-0xF7.
std::string detectEncoding(const std::string &gamePath)
{
    static const char *const plainScripts[] = {"0.txt", "00.txt", "0.utf", "00.utf"};
    std::string sample;
    for (const char *name : plainScripts) {
        sample = readHead(gamePath + "/" + name, 64 * 1024);
        if (!sample.empty())
            break;
    }
    if (sample.empty()) {
        sample = readHead(gamePath + "/nscript.dat", 64 * 1024);
        for (auto &c : sample)
            c = static_cast<char>(static_cast<unsigned char>(c) ^ 0x84);
    }
    if (sample.empty())
        return "gbk";
    if (isUTF8WithMultibyte(sample))
        return "utf8";

    size_t kana = 0;
    size_t hanzi = 0;
    for (size_t i = 0; i + 1 < sample.size(); ++i) {
        unsigned char lead = sample[i];
        unsigned char trail = sample[i + 1];
        if (lead < 0x80)
            continue;
        if ((lead == 0x82 && trail >= 0x9F && trail <= 0xF1) ||
            (lead == 0x83 && trail >= 0x40 && trail <= 0x96))
            ++kana;
        else if (lead >= 0xB0 && lead <= 0xF7 && trail >= 0xA1 && trail <= 0xFE)
            ++hanzi;
        ++i;
    }
    return kana > hanzi / 4 ? "sjis" : "gbk";
}

std::string systemFontPath(const std::string &encoding)
{
    @autoreleasepool {
        CFStringRef name = encoding == "sjis" ? CFSTR("HiraginoSans-W3") : CFSTR("PingFangSC-Regular");
        CTFontRef font = CTFontCreateWithName(name, 24, nullptr);
        if (!font)
            return std::string();
        CFURLRef url = static_cast<CFURLRef>(CTFontCopyAttribute(font, kCTFontURLAttribute));
        CFRelease(font);
        if (!url)
            return std::string();
        char path[PATH_MAX];
        bool ok = CFURLGetFileSystemRepresentation(url, true, reinterpret_cast<UInt8 *>(path), sizeof(path));
        CFRelease(url);
        return ok ? std::string(path) : std::string();
    }
}

bool fileExists(const std::string &path)
{
    return access(path.c_str(), R_OK) == 0;
}

// A three-finger touch opens the host menu. Its touches never reach the
// game, so it doesn't also count as a click there.
int SDLCALL filterEvent(void *, SDL_Event *event)
{
    if (!menuGestureEnabled)
        return 1;
    switch (event->type) {
    case SDL_FINGERDOWN:
        if (menuGestureActive)
            return 0;
        if (SDL_GetNumTouchFingers(event->tfinger.touchId) >= 3) {
            menuGestureActive = true;
            dispatch_async(dispatch_get_main_queue(), ^{
                if (running && menuCallback)
                    menuCallback(callbackContext);
            });
            return 0;
        }
        return 1;
    case SDL_FINGERMOTION:
        return menuGestureActive ? 0 : 1;
    case SDL_FINGERUP:
        if (!menuGestureActive)
            return 1;
        if (SDL_GetNumTouchFingers(event->tfinger.touchId) <= 1)
            menuGestureActive = false;
        return 0;
    default:
        return 1;
    }
}

// MIDI music plays through the General MIDI SoundFont shipped inside this
// framework. Without it the synthesizer falls back to sine waves.
std::string soundFontPath()
{
    Dl_info info;
    if (!dladdr(reinterpret_cast<const void *>(&soundFontPath), &info) || !info.dli_fname)
        return std::string();
    std::string path(info.dli_fname);
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos)
        return std::string();
    path.resize(slash + 1);
    path += "GeneralUser-GS.sf2";
    return access(path.c_str(), R_OK) == 0 ? path : std::string();
}

void setAudioSessionActive(bool active)
{
    NSError *error = nil;
    if (active) {
        [[AVAudioSession sharedInstance] setActive:YES error:&error];
    } else {
        [[AVAudioSession sharedInstance]
            setActive:NO
            withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                  error:&error];
    }
    if (error) {
        lastError = error.localizedDescription.UTF8String;
        log(5, lastError.c_str());
    }
}

// Shuts the engine's libraries down so the next game starts clean. The
// ONScripter object itself is left alone: once a game has quit, its own
// cleanup has already freed what it tracks, and its destructor would free
// those again.
void tearDownEngine()
{
    SDL_SetEventFilter(nullptr, nullptr);
    if (SDL_WasInit(SDL_INIT_AUDIO)) {
        Mix_HaltChannel(-1);
        Mix_HaltMusic();
        int frequency = 0, channels = 0;
        Uint16 format = 0;
        while (Mix_QuerySpec(&frequency, &format, &channels))
            Mix_CloseAudio();
    }
    Mix_Quit();
    FontInfo::forgetOpenFonts();
    while (TTF_WasInit())
        TTF_Quit();
    IMG_Quit();
    SDL_Quit();

    engine = nullptr;
    delete coding2utf16;
    coding2utf16 = nullptr;
    timer_cdaudio_id = 0;
    timer_bgmfade_id = 0;
    g_lastpress_time = 0;
    ext_music_play_once_flag = false;
    menuGestureActive = false;

    releaseFiber();
    if (!savedWorkingDirectory.empty())
        chdir(savedWorkingDirectory.c_str());
    savedWorkingDirectory.clear();
    setAudioSessionActive(false);
}

std::string endMessage()
{
    std::lock_guard<std::mutex> lock(errorMutex);
    if (!lastEngineError.empty())
        return lastEngineError;
    return "ONScripter stopped with status " + std::to_string(exitStatus) + ".";
}

void finishSession()
{
    const bool success = exitStatus == 0;
    const std::string message = success ? std::string() : endMessage();
    tearDownEngine();

    running = false;
    foreground = true;
    stopRequested = false;
    encodingName.clear();
    lastError = message;
    auto callback = completionCallback;
    auto context = callbackContext;
    menuCallback = nullptr;
    completionCallback = nullptr;
    callbackContext = nullptr;
    log(success ? 3 : 5, success ? "session.finished" : message.c_str());
    if (callback)
        callback(success, success ? nullptr : lastError.c_str(), context);
}

} // namespace

extern "C" void MikageONSLogV(int level, const char *format, va_list ap)
{
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, ap);
    size_t length = std::strlen(buffer);
    while (length && (buffer[length - 1] == '\n' || buffer[length - 1] == '\r'))
        buffer[--length] = '\0';
    if (length)
        log(level, buffer);
}

extern "C" void MikageONSDelay(Uint32 ms)
{
    if (!isEngineContext() || ms == 0) {
        SDL_Delay(ms);
        return;
    }
    const Uint64 deadline = SDL_GetTicks64() + ms;
    do {
        yieldToHost();
    } while (SDL_GetTicks64() < deadline);
}

extern "C" int MikageONSWaitEvent(SDL_Event *event)
{
    if (!isEngineContext())
        return SDL_WaitEvent(event);
    for (;;) {
        if (SDL_PollEvent(event))
            return 1;
        yieldToHost();
    }
}

extern "C" void MikageONSRenderPresent(SDL_Renderer *renderer)
{
    SDL_RenderPresent(renderer);
    // Core Animation shows the frame once the host's run loop turn ends.
    if (isEngineContext())
        yieldToHost();
}

extern "C" void MikageONSExit(int status)
{
    if (!isEngineContext()) {
        log(5, "ONScripter called exit() outside its main loop.");
        abort();
    }
    exitStatus = status;
    fiberDone = true;
    yieldToHost();
    __builtin_unreachable();
}

extern "C" void MikageONSSetLogCallback(MikageONSLogCallback callback)
{
    logCallback.store(callback, std::memory_order_release);
}

extern "C" bool MikageONSStart(const MikageONSLaunchOptions *options,
                               MikageONSMenuCallback menu,
                               MikageONSCompletionCallback completion,
                               void *context)
{
    if (running) {
        lastError = "An ONScripter game is already running.";
        return false;
    }
    if (!options || !options->gamePath || !*options->gamePath ||
        !options->saveDirectory || !*options->saveDirectory) {
        lastError = "The ONScripter game or save folder is missing.";
        return false;
    }

    @autoreleasepool {
        std::string gamePath(options->gamePath);
        while (gamePath.size() > 1 && gamePath.back() == '/')
            gamePath.pop_back();
        std::string saveDirectory(options->saveDirectory);
        while (saveDirectory.size() > 1 && saveDirectory.back() == '/')
            saveDirectory.pop_back();

        BOOL isDirectory = NO;
        if (![[NSFileManager defaultManager] fileExistsAtPath:@(gamePath.c_str()) isDirectory:&isDirectory] || !isDirectory) {
            lastError = "The ONScripter game folder does not exist.";
            return false;
        }
        [[NSFileManager defaultManager] createDirectoryAtPath:@(saveDirectory.c_str())
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil];

        encodingName = options->encoding && *options->encoding ? options->encoding : detectEncoding(gamePath);
        std::string font = gamePath + "/default.ttf";
        if (!fileExists(font))
            font = options->fontPath && *options->fontPath ? options->fontPath : systemFontPath(encodingName);
        if (font.empty() || !fileExists(font)) {
            lastError = "No font is available for this game. Put a default.ttf in its folder.";
            encodingName.clear();
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(errorMutex);
            lastEngineError.clear();
        }
        lastError.clear();
        std::string message = "session.start encoding=" + encodingName + " font=" + font;
        log(3, message.c_str());

        if (!prepareFiber()) {
            lastError = "Couldn't allocate the ONScripter stack.";
            encodingName.clear();
            return false;
        }

        // Some files, like the default font, are opened relative to the
        // working directory, as on the desktop.
        char cwd[PATH_MAX];
        savedWorkingDirectory = getcwd(cwd, sizeof(cwd)) ? cwd : "";
        chdir(gamePath.c_str());

        SDL_SetHint("SDL_AUDIO_CATEGORY", options->respectSilentMode ? "ambient" : "playback");
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
        // Touches reach SDL through the host's run loop between steps; the
        // engine must not spin a nested run loop on its own stack.
        SDL_iPhoneSetEventPump(SDL_FALSE);
        SDL_SetMainReady();
        const std::string soundFont = soundFontPath();
        if (soundFont.empty())
            log(4, "session.midi.soundfont.missing");
        Mix_SetSoundFonts(soundFont.empty() ? nullptr : soundFont.c_str());

        coding2utf16 = encodingName == "sjis" ? static_cast<Coding2UTF16 *>(new SJIS2UTF16())
                                              : static_cast<Coding2UTF16 *>(new GBK2UTF16());
        if (encodingName == "utf8")
            coding2utf16->force_utf8 = true;

        engine = new ONScripter();
        engine->setArchivePath(gamePath.c_str());
        engine->setSaveDir(saveDirectory.c_str());
        engine->setFontFile(font.c_str());
        // As on Android: touch players can't rely on keyboard shortcuts.
        engine->enableButtonShortCut();
        // A desktop-fullscreen window that keeps the game's aspect ratio
        // with black bars, or widens a 4:3 game to 16:9 when asked.
        engine->setFullscreenMode(1);
        engine->setStretchTo16x9(options->stretchToScreen);

        menuCallback = menu;
        completionCallback = completion;
        callbackContext = context;
        menuGestureEnabled = options->menuGestureEnabled;
        menuGestureActive = false;
        SDL_SetEventFilter(filterEvent, nullptr);
        running = true;
        foreground = true;
        stopRequested = false;
        musicPausedByHost = false;
        stopSteps = 0;
        setAudioSessionActive(true);

        // Runs openScript and init, which create the window, then the
        // script up to its first frame.
        if (resumeEngine()) {
            const std::string failure = exitStatus == 0 ? "The game ended during startup." : endMessage();
            completionCallback = nullptr;
            finishSession();
            lastError = failure;
            return false;
        }
    }
    return true;
}

extern "C" void MikageONSStep(void)
{
    if (!running || onFiber)
        return;
    if (stopRequested && ++stopSteps > kStopGraceSteps) {
        log(3, "session.stop.forced");
        exitStatus = 0;
        finishSession();
        return;
    }
    if (!foreground && !stopRequested)
        return;
    if (resumeEngine())
        finishSession();
}

extern "C" bool MikageONSRequestExit(void)
{
    if (!running)
        return false;
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_QUIT;
    return SDL_PushEvent(&event) == 1;
}

extern "C" bool MikageONSSendBack(void)
{
    if (!running || !foreground || !engine || !engine->getWindow())
        return false;
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(engine->getWindow());
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_ESCAPE;
    event.key.keysym.sym = SDLK_ESCAPE;
    if (SDL_PushEvent(&event) != 1)
        return false;
    event.type = SDL_KEYUP;
    event.key.state = SDL_RELEASED;
    return SDL_PushEvent(&event) == 1;
}

extern "C" void MikageONSRequestStop(void)
{
    if (!running)
        return;
    stopRequested = true;
    MikageONSRequestExit();
}

extern "C" bool MikageONSSetForeground(bool value)
{
    if (!running || foreground == value)
        return true;
    foreground = value;
    if (!value) {
        // MIDI music doesn't go through SDL's audio device, so it is paused
        // on its own; only music that was playing resumes.
        musicPausedByHost = Mix_PlayingMusic() && !Mix_PausedMusic();
        if (musicPausedByHost)
            Mix_PauseMusic();
        Mix_PauseAudio(1);
        // The audio session stays active: deactivating it stops SDL's running
        // audio queue, which SDL doesn't restart, and the game came back
        // silent.
        lastError.clear();
        return true;
    }
    lastError.clear();
    setAudioSessionActive(true);
    Mix_PauseAudio(0);
    if (musicPausedByHost)
        Mix_ResumeMusic();
    musicPausedByHost = false;
    return lastError.empty();
}

extern "C" bool MikageONSIsRunning(void)
{
    return running;
}

extern "C" const char *MikageONSLastError(void)
{
    return lastError.c_str();
}

extern "C" const char *MikageONSEncoding(void)
{
    return encodingName.c_str();
}

extern "C" void *MikageONSNativeWindow(void)
{
    if (!running || !engine || !engine->getWindow())
        return nullptr;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(engine->getWindow(), &info) || info.subsystem != SDL_SYSWM_UIKIT)
        return nullptr;
    return (__bridge void *)info.info.uikit.window;
}

extern "C" bool MikageONSCaptureFrame(MikageONSCapturedFrame *frame)
{
    if (!frame || frame->pixels || !running || onFiber || !engine)
        return false;
    SDL_Surface *source = engine->getFrameSurface();
    if (!source)
        return false;
    SDL_Surface *rgba = SDL_ConvertSurfaceFormat(source, SDL_PIXELFORMAT_ABGR8888, 0);
    if (!rgba)
        return false;
    const int pitch = rgba->w * 4;
    auto *pixels = static_cast<uint8_t *>(std::malloc(static_cast<size_t>(pitch) * rgba->h));
    if (!pixels) {
        SDL_FreeSurface(rgba);
        return false;
    }
    SDL_LockSurface(rgba);
    for (int y = 0; y < rgba->h; ++y) {
        uint8_t *row = pixels + static_cast<size_t>(y) * pitch;
        std::memcpy(row, static_cast<uint8_t *>(rgba->pixels) + static_cast<size_t>(y) * rgba->pitch, pitch);
        // The engine's frame has no meaningful alpha; it is always opaque.
        for (int x = 0; x < rgba->w; ++x)
            row[x * 4 + 3] = 0xFF;
    }
    SDL_UnlockSurface(rgba);
    frame->pixels = pixels;
    frame->width = rgba->w;
    frame->height = rgba->h;
    frame->pitch = pitch;
    SDL_FreeSurface(rgba);
    return true;
}

extern "C" void MikageONSFreeCapturedFrame(MikageONSCapturedFrame *frame)
{
    if (!frame)
        return;
    std::free(frame->pixels);
    *frame = {};
}
