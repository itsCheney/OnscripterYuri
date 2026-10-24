#pragma once

// Transitions drawn by a fragment shader on SDL's GPU renderer. This side
// talks to SDL3 directly, which ONScripter's SDL2 code can't include, so the
// renderer and textures pass through as the SDL3 objects sdl2-compat hands
// out. Not exported from the framework.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MikageONSGPUEffect MikageONSGPUEffect;

// One frame of a transition, in the game's own pixels.
typedef struct MikageONSGPUEffectParams {
    float clip[4];   // x, y, w, h: blended inside, the old frame outside
    float sizes[4];  // screen w, h; mask w, h
    float params[4]; // mode (0 crossfade, 1 fade with mask, 2 crossfade
                     // with mask), ONScripter's blend value, mask channel
                     // (0 red, 2 blue), unused
} MikageONSGPUEffectParams;

// True when `renderer` is SDL's GPU renderer and the shader compiled.
bool MikageONSGPUAvailable(void *renderer);
// Draws `before` with `after` and `mask` (both textures of `renderer`)
// blended in while bound. NULL when that isn't possible.
MikageONSGPUEffect *MikageONSGPUEffectCreate(void *renderer, void *after, void *mask);
// Applies to the renderer's draws until MikageONSGPUEffectUnbind.
bool MikageONSGPUEffectBind(MikageONSGPUEffect *effect, const MikageONSGPUEffectParams *params);
void MikageONSGPUEffectUnbind(MikageONSGPUEffect *effect);
void MikageONSGPUEffectDestroy(MikageONSGPUEffect *effect);
// Releases the shader while the renderer still exists; when a session ends.
void MikageONSGPUReset(void);

#ifdef __cplusplus
}
#endif
