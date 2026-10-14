// Mikage's backlog, save and load panels for ONScripter.
//
// ONScripter keeps a save point at the start of each text page: the state a
// save restores. The backlog records each page's text, its voice and a
// compressed copy of that save point, so the player can read back and also
// jump back to a recent line. The game's lookback, save and load screens
// become host panels; the engine waits in mikageRunPanel while one is open.

#include "ONScripter.h"
#include "MikageONSInternal.h"
#include "MikageONSRuntime.h"

#include <zlib.h>

#include <cstring>
#include <deque>
#include <string>
#include <vector>

extern Coding2UTF16 *coding2utf16;
extern ONScripter *mikage_ons_engine;

#define ONS_MIKAGE_PANEL_EVENT (SDL_USEREVENT+16)

namespace {

// Lines kept for reading, and how many of the newest can be jumped to.
constexpr size_t kMaxEntries = 500;
constexpr size_t kMaxJumpPoints = 200;
// Width of the picture saved with each save slot.
constexpr int kThumbnailWidth = 320;

struct Entry {
    // The ONScripter page holding the text until it is copied out.
    void *page = nullptr;
    bool captured = false;
    std::string name;
    std::string text;
    // As the script named it, for playSound.
    std::string voice;
    // The page's save point, zlib-compressed; empty once too old.
    std::vector<unsigned char> snapshot;
    size_t snapshotLength = 0;
};

std::deque<Entry> entries;
// entries.back() is the page on screen.
bool hasOpenEntry = false;
// Voice played before its page's save point.
std::string pendingVoice;

enum class Action { None, Cancel, Save, Load, Jump };
int openPanel = MikageONSPanelKindNone;
Action action = Action::None;
int actionArgument = 0;
int replayRequest = -1;
// The open page's text while a panel shows it.
std::string currentName;
std::string currentText;

// The screen as the game's menu opened, before any menu covered it.
SDL_Surface *menuThumbnail = nullptr;
bool inSystemCall = false;

void appendUTF16(std::string &out, uint16_t code)
{
    unsigned char bytes[4];
    int length = coding2utf16->convUTF16ToUTF8(bytes, code);
    out.append(reinterpret_cast<char *>(bytes), length);
}

// Page text is in the script's encoding, with 0x0a for line breaks and a
// Shift_JIS full-width space (0x81 0x40) for each indent, whatever the
// encoding.
std::string toUTF8(const char *text, size_t length)
{
    std::string out;
    if (!text || !coding2utf16)
        return out;
    const bool utf8 = coding2utf16->force_utf8;
    for (size_t i = 0; i < length;) {
        const unsigned char c = text[i];
        if (c == 0)
            break;
        if (c == 0x0a) {
            out += '\n';
            ++i;
        } else if (c == 0x81 && i + 1 < length && static_cast<unsigned char>(text[i + 1]) == 0x40) {
            out += "\xE3\x80\x80";
            i += 2;
        } else if (utf8) {
            out += static_cast<char>(c);
            ++i;
        } else if (IS_TWO_BYTE(c) && i + 1 < length) {
            appendUTF16(out, coding2utf16->conv2UTF16(static_cast<uint16_t>((c << 8) | static_cast<unsigned char>(text[i + 1]))));
            i += 2;
        } else {
            if (c < 0x80)
                out += static_cast<char>(c);
            ++i;
        }
    }
    return out;
}

bool isBlank(const std::string &text)
{
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r')
            continue;
        if (text.compare(i, 3, "\xE3\x80\x80") == 0) {
            i += 2;
            continue;
        }
        return false;
    }
    return true;
}

// ScriptParser's system call numbers, which are protected; checked in
// mikageExecuteSystemCall.
constexpr int kSystemNull = 0;
constexpr int kSystemSave = 3;
constexpr int kSystemLoad = 4;
constexpr int kSystemLookback = 5;

int systemCallFor(int panel)
{
    switch (panel) {
    case MikageONSPanelKindBacklog: return kSystemLookback;
    case MikageONSPanelKindSave: return kSystemSave;
    case MikageONSPanelKindLoad: return kSystemLoad;
    default: return kSystemNull;
    }
}

int panelFor(int systemCall)
{
    switch (systemCall) {
    case kSystemLookback: return MikageONSPanelKindBacklog;
    case kSystemSave: return MikageONSPanelKindSave;
    case kSystemLoad: return MikageONSPanelKindLoad;
    default: return MikageONSPanelKindNone;
    }
}

void freeMenuThumbnail()
{
    if (menuThumbnail)
        SDL_FreeSurface(menuThumbnail);
    menuThumbnail = nullptr;
}

bool hostOwnsEngine()
{
    return mikage_ons_engine && openPanel != MikageONSPanelKindNone;
}

} // namespace

// A small copy of the frame, in the frame's own pixel format.
SDL_Surface *ONScripter::mikageThumbnail(SDL_Surface *frame)
{
    if (!frame || frame->w <= 0 || frame->h <= 0 || frame->format->BytesPerPixel != 4)
        return nullptr;
    const int width = frame->w < kThumbnailWidth ? frame->w : kThumbnailWidth;
    const int height = frame->h * width / frame->w;
    if (height <= 0)
        return nullptr;
    SDL_Surface *thumbnail = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, frame->format->format);
    if (!thumbnail)
        return nullptr;
    if (thumbnail->pitch != width * 4 || frame->pitch != frame->w * 4) {
        // resizeSurface assumes tightly packed rows.
        SDL_BlitScaled(frame, nullptr, thumbnail, nullptr);
    } else {
        resizeSurface(frame, thumbnail);
    }
    return thumbnail;
}

void ONScripter::mikageCaptureEntryText(void *raw)
{
    Entry &entry = *static_cast<Entry *>(raw);
    if (entry.captured)
        return;
    Page *page = static_cast<Page *>(entry.page);
    if (page) {
        entry.text = toUTF8(page->text, page->text_count);
        entry.name = page->tag ? toUTF8(page->tag, strlen(page->tag)) : std::string();
    }
    entry.page = nullptr;
    entry.captured = true;
}

void ONScripter::mikageNoteSavePoint()
{
    if (hasOpenEntry) {
        mikageCaptureEntryText(&entries.back());
        if (isBlank(entries.back().text))
            entries.pop_back();
    }

    Entry entry;
    entry.page = current_page;
    entry.voice.swap(pendingVoice);
    if (save_data_buf && save_data_len) {
        uLongf length = compressBound(save_data_len);
        entry.snapshot.resize(length);
        if (compress2(entry.snapshot.data(), &length, save_data_buf, save_data_len, 1) == Z_OK) {
            entry.snapshot.resize(length);
            entry.snapshotLength = save_data_len;
        } else {
            entry.snapshot.clear();
        }
    }
    entries.push_back(std::move(entry));
    hasOpenEntry = true;

    while (entries.size() > kMaxEntries)
        entries.pop_front();
    if (entries.size() > kMaxJumpPoints) {
        Entry &old = entries[entries.size() - kMaxJumpPoints - 1];
        std::vector<unsigned char>().swap(old.snapshot);
        old.snapshotLength = 0;
    }
}

void ONScripter::mikageNotePageClear()
{
    // Text erased without turning the page (textclear, reset): keep it.
    if (!hasOpenEntry || !current_page)
        return;
    Entry &entry = entries.back();
    if (!entry.captured && entry.page == current_page && current_page->text_count > 0)
        mikageCaptureEntryText(&entry);
}

void ONScripter::mikageNoteLoad()
{
    if (hasOpenEntry) {
        mikageCaptureEntryText(&entries.back());
        if (isBlank(entries.back().text))
            entries.pop_back();
    }
    hasOpenEntry = false;
    pendingVoice.clear();
}

void ONScripter::mikageNoteVoice(const char *filename)
{
    if (!filename || !*filename)
        return;
    // A voice on a page that already shows text belongs to its next
    // sentence (several per page); otherwise to the page about to start.
    if (hasOpenEntry && current_page && entries.back().page == current_page && current_page->text_count > 0) {
        if (entries.back().voice.empty())
            entries.back().voice = filename;
        return;
    }
    pendingVoice = filename;
}

void ONScripter::mikageNoteSaveWritten(int no)
{
    SDL_Surface *thumbnail = nullptr;
    bool owned = false;
    if (inSystemCall && menuThumbnail) {
        thumbnail = menuThumbnail;
    } else {
        thumbnail = mikageThumbnail(accumulation_surface);
        owned = true;
    }
    std::string name, text;
    if (cached_page) {
        text = toUTF8(cached_page->text, cached_page->text_count);
        if (cached_page->tag)
            name = toUTF8(cached_page->tag, strlen(cached_page->tag));
    }
    if (isBlank(text) && !entries.empty()) {
        // The game's own save screen may have cleared the text window.
        const Entry &last = entries.back();
        if (last.captured) {
            text = last.text;
            name = last.name;
        }
    }
    mikage_ons_write_save_details(no, thumbnail, name.c_str(), text.c_str());
    if (owned && thumbnail)
        SDL_FreeSurface(thumbnail);
}

void ONScripter::mikageNoteSystemCallStart()
{
    inSystemCall = true;
    freeMenuThumbnail();
    if (mikage_ons_panels_enabled())
        menuThumbnail = mikageThumbnail(accumulation_surface);
}

void ONScripter::mikageNoteSystemCallEnd()
{
    inSystemCall = false;
    freeMenuThumbnail();
}

int ONScripter::mikageExecuteSystemCall()
{
    static_assert(SYSTEM_NULL == kSystemNull && SYSTEM_SAVE == kSystemSave &&
                  SYSTEM_LOAD == kSystemLoad && SYSTEM_LOOKBACK == kSystemLookback,
                  "ONScripter's system call numbers changed");
    const int panel = panelFor(system_menu_mode);
    if (panel == MikageONSPanelKindNone || !mikage_ons_panels_enabled())
        return -1;
    return mikageRunPanel(panel);
}

bool ONScripter::mikageCanOpenPanel()
{
    return mikage_ons_panels_enabled() && openPanel == MikageONSPanelKindNone &&
           current_mode == NORMAL_MODE && system_menu_mode == SYSTEM_NULL &&
           (event_mode & WAIT_TEXT_MODE);
}

bool ONScripter::mikageOpenPanelFromHost(int panel)
{
    const int mode = systemCallFor(panel);
    if (mode == kSystemNull || !mikageCanOpenPanel())
        return false;
    // As a right click opens the game's menu at a click wait.
    system_menu_mode = mode;
    current_button_state.button = -1;
    sprintf(current_button_state.str, "RCLICK");
    if (event_mode & (WAIT_INPUT_MODE | WAIT_BUTTON_MODE))
        stopAnimation(clickstr_state);
    return true;
}

void ONScripter::mikageReplayVoice(const char *filename)
{
    if (filename && *filename)
        playSound(filename, SOUND_CHUNK, false, 0);
}

// Returns what executeSystemCall returns: 1 after a load, else 0.
int ONScripter::mikageRunPanel(int panel)
{
    currentName.clear();
    currentText.clear();
    if (hasOpenEntry) {
        Entry &entry = entries.back();
        if (entry.captured) {
            currentName = entry.name;
            currentText = entry.text;
        } else if (Page *page = static_cast<Page *>(entry.page)) {
            currentText = toUTF8(page->text, page->text_count);
            currentName = page->tag ? toUTF8(page->tag, strlen(page->tag)) : std::string();
        }
    }

    openPanel = panel;
    action = Action::None;
    replayRequest = -1;
    mikage_ons_notify_panel(panel);
    while (action == Action::None) {
        if (replayRequest >= 0) {
            const int index = replayRequest;
            replayRequest = -1;
            if (index < (int)entries.size())
                mikageReplayVoice(entries[index].voice.c_str());
        }
        SDL_Event quit;
        if (mikage_ons_stopping() || SDL_PeepEvents(&quit, 1, SDL_PEEKEVENT, SDL_QUIT, SDL_QUIT) > 0) {
            // The quit event stays queued for the game to handle.
            action = Action::Cancel;
            mikage_ons_notify_panel(MikageONSPanelKindNone);
            break;
        }
        mikage_ons_yield();
    }
    openPanel = MikageONSPanelKindNone;
    const Action chosen = action;
    action = Action::None;

    switch (chosen) {
    case Action::Save: {
        if (saveon_flag && internal_saveon_flag)
            storeSaveFile();
        writeSaveFile(actionArgument);
        leaveSystemCall();
        return 0;
    }
    case Action::Load:
        return mikageLoad(actionArgument, nullptr, 0) ? 1 : 0;
    case Action::Jump: {
        const int index = actionArgument;
        if (index < 0 || index >= (int)entries.size() || entries[index].snapshot.empty()) {
            leaveSystemCall();
            return 0;
        }
        std::vector<unsigned char> data(entries[index].snapshotLength);
        uLongf length = data.size();
        if (uncompress(data.data(), &length, entries[index].snapshot.data(), entries[index].snapshot.size()) != Z_OK ||
            length != data.size()) {
            mikage_ons_log(5, "backlog.jump.corrupt");
            leaveSystemCall();
            return 0;
        }
        const std::string voice = entries[index].voice;
        // The line itself comes back as the game shows it again.
        entries.erase(entries.begin() + index, entries.end());
        hasOpenEntry = false;
        if (!mikageLoad(0, data.data(), data.size()))
            return 0;
        pendingVoice = voice;
        mikageReplayVoice(voice.c_str());
        return 1;
    }
    default:
        leaveSystemCall();
        return 0;
    }
}

// Loads a save slot, or the save point in data, from inside a system call,
// as the game's load screen does.
bool ONScripter::mikageLoad(int no, const unsigned char *data, size_t length)
{
    current_font = &sentence_font;
    system_menu_mode = SYSTEM_NULL; // for fadeout in mp3stopCommand()
    int failed = -1;
    if (data) {
        file_io_buf_ptr = length;
        allocFileIOBuf();
        memcpy(file_io_buf, data, length);
        file_io_buf_ptr = 0;
        // storeSaveFile always writes the current version.
        if (length > 5 && memcmp(file_io_buf, "ONS", 3) == 0) {
            file_io_buf_ptr = 3;
            int version = readChar() * 100;
            version += readChar();
            failed = loadSaveFile2(version);
        }
    } else {
        failed = loadSaveFile(no);
    }
    if (failed) {
        mikage_ons_log(5, "panel.load.failed");
        leaveSystemCall();
        return false;
    }
    leaveSystemCall(false);
    mikageAfterLoad(no);
    return true;
}

void ONScripter::mikageAfterLoad(int no)
{
    saveon_flag = true;
    internal_saveon_flag = true;
    skip_mode &= ~SKIP_NORMAL;
    automode_flag = false;
    text_on_flag = false;
    indent_offset = 0;
    line_enter_status = 0;
    page_enter_status = 0;
    string_buffer_offset = 0;
    break_flag = false;

    flushEvent();

#ifdef USE_LUA
    if (lua_handler.isCallbackEnabled(LUAHandler::LUA_LOAD)){
        if (lua_handler.callFunction(true, "load", &no))
            errorAndExit( lua_handler.error_str );
    }
#endif

    if (loadgosub_label)
        gosubReal( loadgosub_label, script_h.getCurrent() );
}

void ONScripter::mikageStartupLoad()
{
    const int no = mikage_ons_take_startup_slot();
    if (no <= 0)
        return;
    // As loadgame does.
    const Uint32 fadeout = mp3fadeout_duration;
    mp3fadeout_duration = 0;
    if (loadSaveFile(no) == 0) {
        dirty_rect.fill( screen_width, screen_height );
        flush( refreshMode() );
        deleteButtonLink();
        deleteSelectLink();
        mikageAfterLoad(no);
        mikage_ons_log(3, "startup.load.done");
    } else {
        mikage_ons_log(5, "startup.load.failed");
    }
    mp3fadeout_duration = fadeout;
}

// Host API. The engine waits in mikageRunPanel while these are called.

extern "C" bool MikageONSOpenPanel(int32_t panel)
{
    if (!mikage_ons_engine || systemCallFor(panel) == kSystemNull ||
        !mikage_ons_engine->mikageCanOpenPanel())
        return false;
    SDL_Event event;
    SDL_zero(event);
    event.type = ONS_MIKAGE_PANEL_EVENT;
    event.user.code = panel;
    return SDL_PushEvent(&event) == 1;
}

extern "C" bool MikageONSCanOpenPanel(void)
{
    return mikage_ons_engine && mikage_ons_engine->mikageCanOpenPanel();
}

extern "C" int32_t MikageONSOpenPanelKind(void)
{
    return mikage_ons_engine ? openPanel : MikageONSPanelKindNone;
}

extern "C" int32_t MikageONSSaveSlotCount(void)
{
    return mikage_ons_engine ? mikage_ons_engine->mikageSaveSlotCount() : 0;
}

extern "C" int32_t MikageONSBacklogCount(void)
{
    return hostOwnsEngine() ? (int32_t)entries.size() : 0;
}

extern "C" bool MikageONSBacklogGetEntry(int32_t index, MikageONSBacklogEntry *out)
{
    if (!out || !hostOwnsEngine() || index < 0 || index >= (int32_t)entries.size())
        return false;
    const Entry &entry = entries[index];
    const bool current = hasOpenEntry && index == (int32_t)entries.size() - 1;
    out->name = current ? currentName.c_str() : entry.name.c_str();
    out->text = current ? currentText.c_str() : entry.text.c_str();
    out->hasVoice = !entry.voice.empty();
    out->canJump = !current && !entry.snapshot.empty();
    out->isCurrent = current;
    return true;
}

extern "C" bool MikageONSBacklogReplayVoice(int32_t index)
{
    if (!hostOwnsEngine() || index < 0 || index >= (int32_t)entries.size() || entries[index].voice.empty())
        return false;
    replayRequest = index;
    return true;
}

extern "C" bool MikageONSPanelCancel(void)
{
    if (!hostOwnsEngine() || action != Action::None)
        return false;
    action = Action::Cancel;
    return true;
}

extern "C" bool MikageONSPanelSave(int32_t slot)
{
    if (!hostOwnsEngine() || action != Action::None || slot < 1 || slot > MikageONSSaveSlotCount())
        return false;
    actionArgument = slot;
    action = Action::Save;
    return true;
}

extern "C" bool MikageONSPanelLoad(int32_t slot)
{
    if (!hostOwnsEngine() || action != Action::None || slot < 1 || slot > MikageONSSaveSlotCount())
        return false;
    actionArgument = slot;
    action = Action::Load;
    return true;
}

extern "C" bool MikageONSPanelJump(int32_t index)
{
    MikageONSBacklogEntry entry;
    if (action != Action::None || !MikageONSBacklogGetEntry(index, &entry) || !entry.canJump)
        return false;
    actionArgument = index;
    action = Action::Jump;
    return true;
}

// Called by the runtime when a session ends.
void mikage_ons_reset_panels(void)
{
    entries.clear();
    hasOpenEntry = false;
    pendingVoice.clear();
    openPanel = MikageONSPanelKindNone;
    action = Action::None;
    replayRequest = -1;
    currentName.clear();
    currentText.clear();
    freeMenuThumbnail();
    inSystemCall = false;
}
