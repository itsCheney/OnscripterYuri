#pragma once

// What ONScripter_mikage.cpp needs from MikageONSRuntime.mm. Not exported
// from the framework.

#include <SDL2/SDL.h>

// Returns to the host until the next step, when called from the engine.
void mikage_ons_yield(void);
// The session is ending; an open panel should close.
bool mikage_ons_stopping(void);
// The host shows Mikage's panels instead of ONScripter's own screens.
bool mikage_ons_panels_enabled(void);
// Tells the host a panel opened (MikageONSPanelKind) or closed (0).
void mikage_ons_notify_panel(int panel);
// The save slot to load once the define section ends, then 0.
int mikage_ons_take_startup_slot(void);
// Writes the picture and line shown with save slot `no`. The thumbnail
// may be NULL.
void mikage_ons_write_save_details(int no, SDL_Surface *thumbnail,
                                   const char *name, const char *text);
void mikage_ons_log(int level, const char *message);
// Forgets the backlog when a session ends. In ONScripter_mikage.cpp.
void mikage_ons_reset_panels(void);
