// Crossfades and mask transitions on the GPU.
//
// ONScripter blends every frame of a transition on the CPU and uploads the
// whole frame again, which is slow at 1080p. With SDL's GPU renderer, the
// old and new frames (and the mask) are uploaded once when the transition
// starts, and each frame is one draw through MikageONSGPU's shader. The
// accumulation surface keeps the old frame until the transition ends, when
// doEffect copies the new one in as usual. Any other renderer, or a
// transition the shader doesn't cover, keeps ONScripter's CPU path.

#include "ONScripter.h"
#include "MikageONSGPU.h"
#include "MikageONSInternal.h"

namespace {

SDL_Renderer *gpuRenderer = nullptr;
SDL_Texture *beforeTexture = nullptr;
SDL_Texture *afterTexture = nullptr;
SDL_Texture *maskTexture = nullptr;
SDL_Surface *maskSource = nullptr;
MikageONSGPUEffect *effectState = nullptr;
MikageONSGPUEffectParams effectParams;
bool effectActive = false;
bool afterStale = false;
// The GPU couldn't take this transition; the rest of it stays on the CPU.
bool effectDeclined = false;

void destroyTexture(SDL_Texture *&texture)
{
    if (texture)
        SDL_DestroyTexture(texture);
    texture = nullptr;
}

// Copies `surface` into `texture`, (re)creating it at the surface's size.
bool upload(SDL_Renderer *renderer, SDL_Texture *&texture, SDL_Surface *surface)
{
    Uint32 format = 0;
    int w = 0, h = 0;
    if (texture && (SDL_QueryTexture(texture, &format, nullptr, &w, &h) != 0
                    || format != surface->format->format || w != surface->w || h != surface->h))
        destroyTexture(texture);
    if (!texture) {
        texture = SDL_CreateTexture(renderer, surface->format->format, SDL_TEXTUREACCESS_STATIC,
                                    surface->w, surface->h);
        if (!texture)
            return false;
    }
    SDL_LockSurface(surface);
    int result = SDL_UpdateTexture(texture, nullptr, surface->pixels, surface->pitch);
    SDL_UnlockSurface(surface);
    return result == 0;
}

void releaseEffect()
{
    MikageONSGPUEffectDestroy(effectState);
    effectState = nullptr;
    maskSource = nullptr;
    effectActive = false;
    afterStale = false;
}

void declineEffect()
{
    releaseEffect();
    effectDeclined = true;
}

} // namespace

bool ONScripter::mikageGpuEffectFrame(int mode, int value, SDL_Surface *mask)
{
    if (mode != ALPHA_BLEND_CONST && !mask) {
        // alphaBlend's fallback without a mask image.
        mode = ALPHA_BLEND_CONST;
    }
    if (mode == ALPHA_BLEND_CONST)
        value &= 0xff;

    if (effectDeclined)
        return false;
    if (!effectActive) {
        if (renderer != gpuRenderer) {
            mikage_ons_reset_gpu();
            gpuRenderer = renderer;
        }
        if (!MikageONSGPUAvailable(renderer)
            || !upload(renderer, beforeTexture, effect_src_surface)
            || !upload(renderer, afterTexture, effect_dst_surface)
            || (mask && !upload(renderer, maskTexture, mask))) {
            declineEffect();
            return false;
        }
        effectState = MikageONSGPUEffectCreate(renderer, afterTexture, mask ? maskTexture : afterTexture);
        if (!effectState) {
            declineEffect();
            return false;
        }
        maskSource = mask;
        effectActive = true;
        afterStale = false;
    } else if (mask != maskSource) {
        // A different transition without setEffect in between; start over.
        releaseEffect();
        return mikageGpuEffectFrame(mode, value, mask);
    } else if (afterStale) {
        if (!upload(renderer, afterTexture, effect_dst_surface)) {
            declineEffect();
            return false;
        }
        afterStale = false;
    }

    SDL_Rect &clip = dirty_rect.bounding_box;
    effectParams.clip[0] = clip.x;
    effectParams.clip[1] = clip.y;
    effectParams.clip[2] = clip.w;
    effectParams.clip[3] = clip.h;
    effectParams.sizes[0] = screen_width;
    effectParams.sizes[1] = screen_height;
    effectParams.sizes[2] = mask ? mask->w : 1;
    effectParams.sizes[3] = mask ? mask->h : 1;
    effectParams.params[0] = mode == ALPHA_BLEND_FADE_MASK ? 1 : mode == ALPHA_BLEND_CROSSFADE_MASK ? 2 : 0;
    effectParams.params[1] = value;
    // The byte alphaBlend reads from a mask pixel: the lowest one.
    effectParams.params[2] = mask && mask->format->Rmask < mask->format->Bmask ? 0 : 2;
    effectParams.params[3] = 0;
    return true;
}

bool ONScripter::mikageGpuEffectPresent()
{
    if (!effectActive)
        return false;
    if (!MikageONSGPUEffectBind(effectState, &effectParams)) {
        // This frame falls back to the old one; the next is blended on the CPU.
        declineEffect();
        return false;
    }
    if ((stretch_mode && fullscreen_mode) || (force_window_height && force_window_width)) {
        SDL_RenderCopy(renderer, beforeTexture, NULL, NULL);
    } else {
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, beforeTexture, NULL, &render_view_rect);
    }
    MikageONSGPUEffectUnbind(effectState);
    SDL_RenderPresent(renderer);
    return true;
}

void ONScripter::mikageGpuEffectDstChanged()
{
    if (effectActive)
        afterStale = true;
}

void ONScripter::mikageGpuEffectEnd()
{
    releaseEffect();
    effectDeclined = false;
}

void mikage_ons_reset_gpu(void)
{
    releaseEffect();
    effectDeclined = false;
    destroyTexture(beforeTexture);
    destroyTexture(afterTexture);
    destroyTexture(maskTexture);
    gpuRenderer = nullptr;
    MikageONSGPUReset();
}
