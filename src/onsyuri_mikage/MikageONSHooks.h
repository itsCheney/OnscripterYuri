// Force-included into every ONScripter source by the Mikage build.
//
// ONScripter blocks in its own loop; Mikage runs that loop on a fiber and
// needs it to hand control back whenever it waits or shows a frame. These
// macros route the few calls that do so through the host, and turn exit()
// into the end of the session rather than of the app.

#ifndef MIKAGE_ONS_HOOKS_H
#define MIKAGE_ONS_HOOKS_H

#include <stdlib.h>
#ifdef __cplusplus
#include <cstdlib>
#endif
#include <SDL2/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

void MikageONSDelay(Uint32 ms);
int MikageONSWaitEvent(SDL_Event *event);
void MikageONSRenderPresent(SDL_Renderer *renderer);
__attribute__((noreturn)) void MikageONSExit(int status);

#ifdef __cplusplus
}
#endif

#define SDL_Delay(ms) MikageONSDelay(ms)
#define SDL_WaitEvent(event) MikageONSWaitEvent(event)
#define SDL_RenderPresent(renderer) MikageONSRenderPresent(renderer)
#define exit(status) MikageONSExit(status)

#endif
