// SDL3 side of Mikage's GPU transitions; see MikageONSGPU.h.
//
// The engine links SDL2 (sdl2-compat), whose symbols share SDL3's names, so
// everything here is looked up in the SDL3 library sdl2-compat loaded.

#include "MikageONSGPU.h"

#include <SDL3/SDL.h>

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

// From MikageONSRuntime.mm; MikageONSInternal.h includes SDL2's headers.
void mikage_ons_log(int level, const char *message);

namespace {

// ONScripter's crossfade and mask blends (ONScripter_image.cpp alphaBlend32
// and alphaBlendConst32): the new frame's weight is value / 256 for a
// crossfade; with a mask, value - mask level, as all or nothing for a fade
// and clamped to 255 for a crossfade. The mask tiles at its own size.
const char kTransitionShader[] = R"(
#include <metal_stdlib>
using namespace metal;

struct Uniforms {
    float4 clip;
    float4 sizes;
    float4 params;
};

struct FragmentIn {
    float4 color [[user(locn0)]];
    float2 uv [[user(locn1)]];
};

fragment float4 mikage_transition(FragmentIn in [[stage_in]],
                                  texture2d<float> before [[texture(0)]],
                                  sampler beforeSampler [[sampler(0)]],
                                  texture2d<float> after [[texture(1)]],
                                  sampler afterSampler [[sampler(1)]],
                                  texture2d<float> mask [[texture(2)]],
                                  sampler maskSampler [[sampler(2)]],
                                  constant Uniforms &u [[buffer(0)]])
{
    float3 from = before.sample(beforeSampler, in.uv).rgb;
    float2 p = in.uv * u.sizes.xy;
    if (any(p < u.clip.xy) || any(p >= u.clip.xy + u.clip.zw))
        return float4(from, 1.0);
    float3 to = after.sample(afterSampler, in.uv).rgb;
    float value = u.params.y;
    float weight = value;
    if (u.params.x > 0.5) {
        uint2 texel = uint2(fmod(floor(p), u.sizes.zw));
        float4 m = mask.read(texel);
        float level = floor((u.params.z > 1.0 ? m.b : m.r) * 255.0 + 0.5);
        float d = max(value - level, 0.0);
        weight = u.params.x < 1.5 ? (d > 0.0 ? 255.0 : 0.0) : min(d, 255.0);
    }
    return float4(mix(from, to, weight / 256.0), 1.0);
}
)";

#define SDL3_FUNCTIONS(X) \
    X(SDL_GetError) \
    X(SDL_GetRendererName) \
    X(SDL_GetGPURendererDevice) \
    X(SDL_GetGPUShaderFormats) \
    X(SDL_CreateGPUShader) \
    X(SDL_ReleaseGPUShader) \
    X(SDL_CreateGPUSampler) \
    X(SDL_ReleaseGPUSampler) \
    X(SDL_GetTextureProperties) \
    X(SDL_GetPointerProperty) \
    X(SDL_CreateGPURenderState) \
    X(SDL_SetGPURenderStateFragmentUniforms) \
    X(SDL_SetGPURenderState) \
    X(SDL_DestroyGPURenderState)

struct SDL3 {
#define DECLARE(name) decltype(&::name) name = nullptr;
    SDL3_FUNCTIONS(DECLARE)
#undef DECLARE
    bool loaded = false;
    bool failed = false;
};

SDL3 sdl3;

// The SDL3 library sdl2-compat already loaded, from the paths it tries.
bool loadSDL3()
{
    if (sdl3.loaded)
        return true;
    if (sdl3.failed)
        return false;
    static const char *const paths[] = {
        "@rpath/SDL3.framework/SDL3",
        "@executable_path/Frameworks/SDL3.framework/SDL3",
        "@loader_path/../SDL3.framework/SDL3",
    };
    void *library = nullptr;
    for (const char *path : paths) {
        library = dlopen(path, RTLD_NOLOAD | RTLD_LAZY | RTLD_LOCAL);
        if (library)
            break;
    }
    bool complete = library != nullptr;
#define LOOKUP(name) \
    if (complete) { \
        sdl3.name = reinterpret_cast<decltype(&::name)>(dlsym(library, #name)); \
        complete = sdl3.name != nullptr; \
    }
    SDL3_FUNCTIONS(LOOKUP)
#undef LOOKUP
    if (!complete) {
        mikage_ons_log(4, "gpu.transitions: SDL3 functions not found; transitions stay on the CPU");
        sdl3.failed = true;
        return false;
    }
    sdl3.loaded = true;
    return true;
}

// Built once per GPU device, released when the session ends.
SDL_GPUDevice *shaderDevice = nullptr;
SDL_GPUShader *shader = nullptr;
SDL_GPUSampler *sampler = nullptr;
SDL_GPUDevice *failedDevice = nullptr;

SDL_GPUDevice *deviceFor(SDL_Renderer *renderer)
{
    if (!renderer || !loadSDL3())
        return nullptr;
    const char *name = sdl3.SDL_GetRendererName(renderer);
    if (!name || strcmp(name, SDL_GPU_RENDERER) != 0)
        return nullptr;
    SDL_GPUDevice *device = sdl3.SDL_GetGPURendererDevice(renderer);
    if (!device || device == failedDevice)
        return nullptr;
    if (device == shaderDevice)
        return device;

    if (!(sdl3.SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_MSL)) {
        mikage_ons_log(4, "gpu.transitions: the GPU device takes no MSL shaders");
        failedDevice = device;
        return nullptr;
    }
    SDL_GPUShaderCreateInfo info;
    SDL_zero(info);
    info.code = reinterpret_cast<const Uint8 *>(kTransitionShader);
    info.code_size = sizeof(kTransitionShader) - 1;
    info.entrypoint = "mikage_transition";
    info.format = SDL_GPU_SHADERFORMAT_MSL;
    info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_samplers = 3;
    info.num_uniform_buffers = 1;
    SDL_GPUShader *newShader = sdl3.SDL_CreateGPUShader(device, &info);

    SDL_GPUSamplerCreateInfo samplerInfo;
    SDL_zero(samplerInfo);
    samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    SDL_GPUSampler *newSampler = newShader ? sdl3.SDL_CreateGPUSampler(device, &samplerInfo) : nullptr;

    if (!newShader || !newSampler) {
        char message[512];
        snprintf(message, sizeof(message), "gpu.transitions: shader setup failed: %s", sdl3.SDL_GetError());
        mikage_ons_log(5, message);
        if (newShader)
            sdl3.SDL_ReleaseGPUShader(device, newShader);
        failedDevice = device;
        return nullptr;
    }
    MikageONSGPUReset();
    shaderDevice = device;
    shader = newShader;
    sampler = newSampler;
    mikage_ons_log(3, "gpu.transitions: on");
    return device;
}

SDL_GPUTexture *gpuTexture(SDL_Texture *texture)
{
    if (!texture)
        return nullptr;
    SDL_PropertiesID props = sdl3.SDL_GetTextureProperties(texture);
    return static_cast<SDL_GPUTexture *>(
        sdl3.SDL_GetPointerProperty(props, SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, nullptr));
}

} // namespace

struct MikageONSGPUEffect {
    SDL_Renderer *renderer;
    SDL_GPURenderState *state;
};

extern "C" bool MikageONSGPUAvailable(void *renderer)
{
    return deviceFor(static_cast<SDL_Renderer *>(renderer)) != nullptr;
}

extern "C" MikageONSGPUEffect *MikageONSGPUEffectCreate(void *renderer, void *after, void *mask)
{
    SDL_Renderer *sdlRenderer = static_cast<SDL_Renderer *>(renderer);
    if (!deviceFor(sdlRenderer))
        return nullptr;
    SDL_GPUTexture *afterTexture = gpuTexture(static_cast<SDL_Texture *>(after));
    SDL_GPUTexture *maskTexture = gpuTexture(static_cast<SDL_Texture *>(mask));
    if (!afterTexture || !maskTexture)
        return nullptr;

    SDL_GPUTextureSamplerBinding bindings[2];
    bindings[0].texture = afterTexture;
    bindings[0].sampler = sampler;
    bindings[1].texture = maskTexture;
    bindings[1].sampler = sampler;
    SDL_GPURenderStateCreateInfo info;
    SDL_zero(info);
    info.fragment_shader = shader;
    info.num_sampler_bindings = 2;
    info.sampler_bindings = bindings;
    SDL_GPURenderState *state = sdl3.SDL_CreateGPURenderState(sdlRenderer, &info);
    if (!state) {
        char message[512];
        snprintf(message, sizeof(message), "gpu.transitions: render state failed: %s", sdl3.SDL_GetError());
        mikage_ons_log(4, message);
        return nullptr;
    }
    return new MikageONSGPUEffect{sdlRenderer, state};
}

extern "C" bool MikageONSGPUEffectBind(MikageONSGPUEffect *effect, const MikageONSGPUEffectParams *params)
{
    if (!effect)
        return false;
    return sdl3.SDL_SetGPURenderStateFragmentUniforms(effect->state, 0, params, sizeof(*params))
        && sdl3.SDL_SetGPURenderState(effect->renderer, effect->state);
}

extern "C" void MikageONSGPUEffectUnbind(MikageONSGPUEffect *effect)
{
    if (effect)
        sdl3.SDL_SetGPURenderState(effect->renderer, nullptr);
}

extern "C" void MikageONSGPUEffectDestroy(MikageONSGPUEffect *effect)
{
    if (!effect)
        return;
    sdl3.SDL_DestroyGPURenderState(effect->state);
    delete effect;
}

extern "C" void MikageONSGPUReset(void)
{
    if (shaderDevice) {
        sdl3.SDL_ReleaseGPUSampler(shaderDevice, sampler);
        sdl3.SDL_ReleaseGPUShader(shaderDevice, shader);
    }
    shaderDevice = nullptr;
    shader = nullptr;
    sampler = nullptr;
    failedDevice = nullptr;
}
