#include "hooks/d3d8_hook.h"

#include "core/framework.h"
#include "hooks/fps_unlock_hook.h"
#include "hooks/god_mode_hook.h"
#include "hooks/hook_utils.h"
#include "render/d3d8_console.h"
#include "render/d3d8_lighting.h"
#include "render/display_config.h"
#include "render/render_tweaks.h"
#include "sh4/addresses.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace sh4xe::hooks::d3d8
{
namespace
{

using Direct3DCreate8Fn = void*(__stdcall*)(UINT);
using CreateDeviceFn = HRESULT(__stdcall*)(void*, UINT, DWORD, HWND, DWORD, void*, void**);
using EndSceneFn = HRESULT(__stdcall*)(void*);
using ResetFn = HRESULT(__stdcall*)(void*, render::display::PresentParameters*);
using SetRenderStateFn = HRESULT(__stdcall*)(void*, DWORD, DWORD);
using SetTextureStageStateFn = HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD);
using SetMaterialFn = HRESULT(__stdcall*)(void*, const void*);
using SetLightFn = HRESULT(__stdcall*)(void*, DWORD, const void*);
using ReleaseFn = ULONG(__stdcall*)(void*);

constexpr UINT kD3d8SdkVersion = 120;

// IDirect3DDevice8 vtable indices (canonical D3D8 layout).
constexpr size_t kVtblReset = 14;
constexpr size_t kVtblEndScene = 35;
constexpr size_t kVtblSetMaterial = 42;
constexpr size_t kVtblSetLight = 44;
constexpr size_t kVtblSetRenderState = 50;
constexpr size_t kVtblSetTextureStageState = 63;

// D3DRENDERSTATETYPE / D3DTEXTURESTAGESTATETYPE / D3DTEXTUREFILTERTYPE values.
constexpr DWORD kD3DRS_SPECULARENABLE = 29;
constexpr DWORD kD3DRS_FOGENABLE = 28;
constexpr DWORD kD3DRS_AMBIENT = 139;
constexpr DWORD kD3DRS_DITHERENABLE = 26;
constexpr DWORD kD3DRS_FOGSTART = 36;
constexpr DWORD kD3DRS_FOGEND = 37;
constexpr DWORD kD3DRS_FOGDENSITY = 38;
constexpr DWORD kD3DRS_NORMALIZENORMALS = 143;
constexpr DWORD kD3DTSS_MAGFILTER = 16;
constexpr DWORD kD3DTSS_MINFILTER = 17;
constexpr DWORD kD3DTSS_MIPFILTER = 18;
constexpr DWORD kD3DTSS_MIPMAPLODBIAS = 19;
constexpr DWORD kD3DTSS_MAXANISOTROPY = 21;
constexpr DWORD kD3DTEXF_POINT = 1;
constexpr DWORD kD3DTEXF_LINEAR = 2;
constexpr DWORD kD3DTEXF_ANISOTROPIC = 3;
constexpr DWORD kForcedMaxAnisotropy = 16;
constexpr DWORD kQualityTextureStages = 4;

constexpr DWORD kD3DLIGHT_POINT = 1;
constexpr DWORD kD3DLIGHT_SPOT = 2;

struct D3DCOLORVALUE
{
    float r;
    float g;
    float b;
    float a;
};

struct D3DVECTOR
{
    float x;
    float y;
    float z;
};

struct D3DMATERIAL8
{
    D3DCOLORVALUE Diffuse;
    D3DCOLORVALUE Ambient;
    D3DCOLORVALUE Specular;
    D3DCOLORVALUE Emissive;
    float Power;
};

struct D3DLIGHT8
{
    DWORD Type;
    D3DCOLORVALUE Diffuse;
    D3DCOLORVALUE Specular;
    D3DCOLORVALUE Ambient;
    D3DVECTOR Position;
    D3DVECTOR Direction;
    float Range;
    float Falloff;
    float Attenuation0;
    float Attenuation1;
    float Attenuation2;
    float Theta;
    float Phi;
};

Direct3DCreate8Fn g_originalDirect3DCreate8 = nullptr;
CreateDeviceFn g_originalCreateDevice = nullptr;
EndSceneFn g_originalEndScene = nullptr;
ResetFn g_originalReset = nullptr;
SetRenderStateFn g_originalSetRenderState = nullptr;
SetTextureStageStateFn g_originalSetTextureStageState = nullptr;
SetMaterialFn g_originalSetMaterial = nullptr;
SetLightFn g_originalSetLight = nullptr;
std::mutex g_hookMutex;
bool g_createDeviceHooked = false;
bool g_endSceneHooked = false;
unsigned int g_appliedGraphicsQualityRevision = 0;

HRESULT __stdcall CreateDeviceDetour(void* self,
                                     UINT adapter,
                                     DWORD deviceType,
                                     HWND focusWindow,
                                     DWORD behaviorFlags,
                                     void* presentationParameters,
                                     void** returnedDevice);
HRESULT __stdcall EndSceneDetour(void* device);
HRESULT __stdcall ResetDetour(void* device, render::display::PresentParameters* presentationParameters);
HRESULT __stdcall SetMaterialDetour(void* device, const void* material);
HRESULT __stdcall SetLightDetour(void* device, DWORD index, const void* light);
bool HookDevice(void* device);

DWORD FloatToDword(float value)
{
    DWORD bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float DwordToFloat(DWORD value)
{
    float result = 0.0f;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

DWORD ScaleFloatState(DWORD value, float scale)
{
    if (scale == 1.0f)
        return value;

    const float decoded = DwordToFloat(value);
    if (!std::isfinite(decoded))
        return value;

    return FloatToDword(decoded * scale);
}

float ClampLight(float value)
{
    return std::clamp(value, 0.0f, 2.0f);
}

void ScaleColorValue(D3DCOLORVALUE& color, float boost, float lift)
{
    color.r = ClampLight(color.r * boost + lift);
    color.g = ClampLight(color.g * boost + lift);
    color.b = ClampLight(color.b * boost + lift);
}

void SeedSpecular(D3DCOLORVALUE& specular, const D3DCOLORVALUE& diffuse, float boost, float seed)
{
    if (boost <= 0.0f)
        return;

    specular.r = std::max(ClampLight(specular.r * boost), ClampLight(diffuse.r * seed));
    specular.g = std::max(ClampLight(specular.g * boost), ClampLight(diffuse.g * seed));
    specular.b = std::max(ClampLight(specular.b * boost), ClampLight(diffuse.b * seed));
}

BYTE BoostAmbientChannel(BYTE value)
{
    const float boosted = static_cast<float>(value) * render::tweaks::LightingAmbientBoost() +
                          render::tweaks::LightingAmbientLift() * 255.0f;
    return static_cast<BYTE>(std::clamp(boosted, 0.0f, 255.0f));
}

DWORD BoostAmbientColor(DWORD value)
{
    const DWORD a = value & 0xFF000000u;
    const DWORD r = static_cast<DWORD>(BoostAmbientChannel(static_cast<BYTE>((value >> 16) & 0xFFu))) << 16;
    const DWORD g = static_cast<DWORD>(BoostAmbientChannel(static_cast<BYTE>((value >> 8) & 0xFFu))) << 8;
    const DWORD b = static_cast<DWORD>(BoostAmbientChannel(static_cast<BYTE>(value & 0xFFu)));
    return a | r | g | b;
}

void ApplyTextureStageQuality(void* device, DWORD stage)
{
    if (!g_originalSetTextureStageState)
        return;

    using render::tweaks::FilterMode;
    const FilterMode mode = render::tweaks::GetFilterMode();
    if (mode == FilterMode::Game || render::tweaks::SuppressFilterOverride())
        return;

    DWORD textureFilter = kD3DTEXF_LINEAR;
    DWORD mipFilter = kD3DTEXF_LINEAR;
    if (mode == FilterMode::Point)
    {
        textureFilter = kD3DTEXF_POINT;
        mipFilter = kD3DTEXF_POINT;
    }
    else if (mode == FilterMode::Anisotropic)
    {
        textureFilter = kD3DTEXF_ANISOTROPIC;
        g_originalSetTextureStageState(device, stage, kD3DTSS_MAXANISOTROPY, kForcedMaxAnisotropy);
    }

    g_originalSetTextureStageState(device, stage, kD3DTSS_MAGFILTER, textureFilter);
    g_originalSetTextureStageState(device, stage, kD3DTSS_MINFILTER, textureFilter);
    g_originalSetTextureStageState(device, stage, kD3DTSS_MIPFILTER, mipFilter);

    const float lodBias = mode == FilterMode::Point ? 0.0f : render::tweaks::TextureLodBias();
    g_originalSetTextureStageState(device, stage, kD3DTSS_MIPMAPLODBIAS, FloatToDword(lodBias));
}

void ApplyGraphicsQualityStates(void* device, bool force)
{
    if (!device)
        return;

    const unsigned int revision = render::tweaks::GraphicsQualityRevision();
    if (!force && revision == g_appliedGraphicsQualityRevision)
        return;

    g_appliedGraphicsQualityRevision = revision;

    if (g_originalSetRenderState)
        g_originalSetRenderState(device, kD3DRS_DITHERENABLE, render::tweaks::DitherEnabled() ? TRUE : FALSE);

    for (DWORD stage = 0; stage < kQualityTextureStages; ++stage)
        ApplyTextureStageQuality(device, stage);
}

void ApplyLightTweak(D3DLIGHT8& light)
{
    ScaleColorValue(light.Diffuse, render::tweaks::LightingLightBoost(), 0.0f);
    ScaleColorValue(light.Ambient,
                    render::tweaks::LightingAmbientBoost(),
                    render::tweaks::LightingAmbientLift() * 0.35f);
    SeedSpecular(light.Specular, light.Diffuse, render::tweaks::LightingSpecularBoost(), 0.16f);

    if (light.Type == kD3DLIGHT_POINT || light.Type == kD3DLIGHT_SPOT)
    {
        const float rangeBoost = render::tweaks::LightingRangeBoost();
        light.Range = std::max(1.0f, light.Range * rangeBoost);
        if (rangeBoost > 0.01f)
        {
            light.Attenuation1 /= rangeBoost;
            light.Attenuation2 /= rangeBoost;
        }
    }
}

void ApplyMaterialTweak(D3DMATERIAL8& material)
{
    ScaleColorValue(material.Ambient,
                    render::tweaks::LightingAmbientBoost(),
                    render::tweaks::LightingAmbientLift() * 0.25f);
    SeedSpecular(material.Specular,
                 material.Diffuse,
                 render::tweaks::LightingSpecularBoost(),
                 0.10f);

    if (render::tweaks::LightingSpecularBoost() > 0.0f && material.Power < 8.0f)
        material.Power = 8.0f;
}

bool HookDirect3D8Interface(void* direct3d)
{
    if (!direct3d)
        return false;

    std::lock_guard<std::mutex> lock(g_hookMutex);
    if (g_createDeviceHooked)
        return true;

    void** vtbl = *reinterpret_cast<void***>(direct3d);
    void* createDevice = vtbl[15];
    if (!CreateAndEnableHook("IDirect3D8::CreateDevice",
                             createDevice,
                             reinterpret_cast<void*>(&CreateDeviceDetour),
                             &g_originalCreateDevice))
        return false;

    g_createDeviceHooked = true;
    sh4xe::Log("IDirect3D8::CreateDevice hooked @ %p", createDevice);
    render::d3d8_console::AddLog("ok IDirect3D8::CreateDevice hooked");
    return true;
}

HRESULT __stdcall EndSceneDetour(void* device)
{
    // Per-frame, game-thread ticks. FPS: race-free re-assertion of the timing
    // globals (cheap, reads only unless something drifted). God mode clears any
    // lingering Henry impact state. Both are safe if EndScene fires more than
    // once per frame.
    hooks::fps::OnFramePresented();
    hooks::god_mode::OnFramePresented();

    ApplyGraphicsQualityStates(device, false);
    render::d3d8_lighting::Render(device);
    render::d3d8_console::Render(device);
    const HRESULT hr = g_originalEndScene(device);
    render::display::ApplyPending(device, g_originalReset);
    return hr;
}

HRESULT __stdcall ResetDetour(void* device, render::display::PresentParameters* presentationParameters)
{
    if (presentationParameters)
    {
        sh4xe::Log("IDirect3DDevice8::Reset called windowed=%lu resolution=%ux%u",
                   static_cast<unsigned long>(presentationParameters->Windowed),
                   presentationParameters->BackBufferWidth,
                   presentationParameters->BackBufferHeight);
    }
    else
    {
        sh4xe::Log("IDirect3DDevice8::Reset called params=null");
    }

    const HRESULT hr = g_originalReset(device, presentationParameters);
    sh4xe::Log("IDirect3DDevice8::Reset returned hr=0x%08lX", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr))
    {
        render::display::CaptureReset(presentationParameters);
        ApplyGraphicsQualityStates(device, true);
    }
    return hr;
}

HRESULT __stdcall SetRenderStateDetour(void* device, DWORD state, DWORD value)
{
    if (state == kD3DRS_FOGENABLE && render::tweaks::FogDisabled())
        value = FALSE;
    else if (state == kD3DRS_FOGSTART || state == kD3DRS_FOGEND)
        value = ScaleFloatState(value, render::tweaks::FogDistanceScale());
    else if (state == kD3DRS_FOGDENSITY)
        value = ScaleFloatState(value, render::tweaks::FogDensityScale());
    else if (state == kD3DRS_DITHERENABLE && render::tweaks::DitherEnabled())
        value = TRUE;

    if (render::tweaks::LightingEnabled())
    {
        if (state == kD3DRS_AMBIENT)
            value = BoostAmbientColor(value);
        else if (state == kD3DRS_NORMALIZENORMALS)
            value = TRUE;
        else if (state == kD3DRS_SPECULARENABLE && render::tweaks::LightingSpecularBoost() > 0.0f)
            value = TRUE;
    }

    return g_originalSetRenderState(device, state, value);
}

HRESULT __stdcall SetMaterialDetour(void* device, const void* material)
{
    if (material && render::tweaks::LightingEnabled())
    {
        D3DMATERIAL8 adjusted = *static_cast<const D3DMATERIAL8*>(material);
        ApplyMaterialTweak(adjusted);
        return g_originalSetMaterial(device, &adjusted);
    }

    return g_originalSetMaterial(device, material);
}

HRESULT __stdcall SetLightDetour(void* device, DWORD index, const void* light)
{
    if (light && render::tweaks::LightingEnabled())
    {
        D3DLIGHT8 adjusted = *static_cast<const D3DLIGHT8*>(light);
        ApplyLightTweak(adjusted);

        if (g_originalSetRenderState)
        {
            g_originalSetRenderState(device, kD3DRS_NORMALIZENORMALS, TRUE);
            if (render::tweaks::LightingSpecularBoost() > 0.0f)
                g_originalSetRenderState(device, kD3DRS_SPECULARENABLE, TRUE);
        }

        return g_originalSetLight(device, index, &adjusted);
    }

    return g_originalSetLight(device, index, light);
}

HRESULT __stdcall SetTextureStageStateDetour(void* device, DWORD stage, DWORD type, DWORD value)
{
    const render::tweaks::FilterMode mode = render::tweaks::GetFilterMode();
    if (mode != render::tweaks::FilterMode::Game && !render::tweaks::SuppressFilterOverride())
    {
        switch (type)
        {
            case kD3DTSS_MAGFILTER:
            case kD3DTSS_MINFILTER:
                switch (mode)
                {
                    case render::tweaks::FilterMode::Point:
                        value = kD3DTEXF_POINT;
                        break;
                    case render::tweaks::FilterMode::Linear:
                        value = kD3DTEXF_LINEAR;
                        break;
                    case render::tweaks::FilterMode::Anisotropic:
                        value = kD3DTEXF_ANISOTROPIC;
                        g_originalSetTextureStageState(device, stage, kD3DTSS_MAXANISOTROPY, kForcedMaxAnisotropy);
                        break;
                    default:
                        break;
                }
                break;
            case kD3DTSS_MIPFILTER:
                // Keep mip sampling coherent with the chosen mode: trilinear for
                // smooth modes, nearest mips for point.
                value = (mode == render::tweaks::FilterMode::Point) ? kD3DTEXF_POINT : kD3DTEXF_LINEAR;
                break;
            case kD3DTSS_MIPMAPLODBIAS:
                value = FloatToDword(mode == render::tweaks::FilterMode::Point ? 0.0f
                                                                                : render::tweaks::TextureLodBias());
                break;
            case kD3DTSS_MAXANISOTROPY:
                if (mode == render::tweaks::FilterMode::Anisotropic)
                    value = kForcedMaxAnisotropy;
                break;
            default:
                break;
        }
    }

    return g_originalSetTextureStageState(device, stage, type, value);
}

bool HookDevice(void* device)
{
    if (!device)
        return false;

    std::lock_guard<std::mutex> lock(g_hookMutex);
    if (g_endSceneHooked)
        return true;

    void** vtbl = *reinterpret_cast<void***>(device);

    void* reset = vtbl[kVtblReset];
    if (!g_originalReset)
        g_originalReset = reinterpret_cast<ResetFn>(reset);
    if (CreateAndEnableHook("IDirect3DDevice8::Reset",
                            reset,
                            reinterpret_cast<void*>(&ResetDetour),
                            &g_originalReset))
    {
        sh4xe::Log("IDirect3DDevice8::Reset hooked @ %p", reset);
        render::d3d8_console::AddLog("ok IDirect3DDevice8::Reset hooked");
    }

    void* endScene = vtbl[kVtblEndScene];
    if (!CreateAndEnableHook("IDirect3DDevice8::EndScene",
                             endScene,
                             reinterpret_cast<void*>(&EndSceneDetour),
                             &g_originalEndScene))
        return false;

    g_endSceneHooked = true;
    sh4xe::Log("IDirect3DDevice8::EndScene hooked @ %p", endScene);
    render::d3d8_console::AddLog("ok IDirect3DDevice8::EndScene hooked");

    // Render tweaks: optional, do not fail the device hook if they cannot be
    // installed -- the console must still come up.
    void* setMaterial = vtbl[kVtblSetMaterial];
    if (CreateAndEnableHook("IDirect3DDevice8::SetMaterial",
                            setMaterial,
                            reinterpret_cast<void*>(&SetMaterialDetour),
                            &g_originalSetMaterial))
    {
        sh4xe::Log("IDirect3DDevice8::SetMaterial hooked @ %p", setMaterial);
        render::d3d8_console::AddLog("ok IDirect3DDevice8::SetMaterial hooked");
    }

    void* setLight = vtbl[kVtblSetLight];
    if (CreateAndEnableHook("IDirect3DDevice8::SetLight",
                            setLight,
                            reinterpret_cast<void*>(&SetLightDetour),
                            &g_originalSetLight))
    {
        sh4xe::Log("IDirect3DDevice8::SetLight hooked @ %p", setLight);
        render::d3d8_console::AddLog("ok IDirect3DDevice8::SetLight hooked");
    }

    void* setRenderState = vtbl[kVtblSetRenderState];
    if (CreateAndEnableHook("IDirect3DDevice8::SetRenderState",
                            setRenderState,
                            reinterpret_cast<void*>(&SetRenderStateDetour),
                            &g_originalSetRenderState))
    {
        sh4xe::Log("IDirect3DDevice8::SetRenderState hooked @ %p", setRenderState);
        render::d3d8_console::AddLog("ok IDirect3DDevice8::SetRenderState hooked");
    }

    void* setTextureStageState = vtbl[kVtblSetTextureStageState];
    if (CreateAndEnableHook("IDirect3DDevice8::SetTextureStageState",
                            setTextureStageState,
                            reinterpret_cast<void*>(&SetTextureStageStateDetour),
                            &g_originalSetTextureStageState))
    {
        sh4xe::Log("IDirect3DDevice8::SetTextureStageState hooked @ %p", setTextureStageState);
        render::d3d8_console::AddLog("ok IDirect3DDevice8::SetTextureStageState hooked");
    }

    ApplyGraphicsQualityStates(device, true);

    return true;
}

HRESULT __stdcall CreateDeviceDetour(void* self,
                                     UINT adapter,
                                     DWORD deviceType,
                                     HWND focusWindow,
                                     DWORD behaviorFlags,
                                     void* presentationParameters,
                                     void** returnedDevice)
{
    sh4xe::Log("IDirect3D8::CreateDevice called");
    const HRESULT hr = g_originalCreateDevice(
        self, adapter, deviceType, focusWindow, behaviorFlags, presentationParameters, returnedDevice);

    sh4xe::Log("IDirect3D8::CreateDevice returned hr=0x%08lX device=%p",
               static_cast<unsigned long>(hr),
               returnedDevice ? *returnedDevice : nullptr);

    if (SUCCEEDED(hr) && returnedDevice && *returnedDevice)
    {
        render::display::CaptureCreateDevice(
            focusWindow, static_cast<const render::display::PresentParameters*>(presentationParameters));
        HookDevice(*returnedDevice);
    }

    return hr;
}

void* __stdcall Direct3DCreate8Detour(UINT sdkVersion)
{
    sh4xe::Log("Direct3DCreate8 called sdk=%u", sdkVersion);
    void* direct3d = g_originalDirect3DCreate8(sdkVersion);
    HookDirect3D8Interface(direct3d);
    return direct3d;
}

// Fallback that does not race startup: the game stores its device pointer in a
// fixed global once CreateDevice succeeds. We poll it and hook EndScene (and the
// render-tweak entry points) directly off the live device vtable. This is the
// reliable path; the Direct3DCreate8/CreateDevice export hooks above only win if
// we happen to install them before the game initializes graphics.
DWORD WINAPI DevicePollThread(LPVOID)
{
    auto** devicePtr = reinterpret_cast<void**>(sh4::addr::kDirect3DDevice8Ptr);

    // Poll for up to ~60s; the device is normally created within the first
    // second or two of process startup.
    for (int i = 0; i < 600; ++i)
    {
        if (g_endSceneHooked)
            return 0;

        void* device = *devicePtr;
        if (device)
        {
            sh4xe::Log("device global @ %p populated -> %p", devicePtr, device);
            HookDevice(device);
            return 0;
        }

        Sleep(100);
    }

    sh4xe::Log("device global @ %p never populated; console/tweaks inactive", devicePtr);
    return 0;
}

DWORD WINAPI HookThread(LPVOID)
{
    // Start the race-free device poll first; it works regardless of whether the
    // export hooks below install successfully.
    if (HANDLE poll = CreateThread(nullptr, 0, DevicePollThread, nullptr, 0, nullptr))
        CloseHandle(poll);

    HMODULE d3d8 = LoadLibraryW(L"d3d8.dll");
    if (!d3d8)
    {
        sh4xe::Log("d3d8.dll could not be loaded; console hook disabled");
        return 0;
    }

    void* direct3DCreate8 = reinterpret_cast<void*>(GetProcAddress(d3d8, "Direct3DCreate8"));
    if (!direct3DCreate8)
    {
        sh4xe::Log("Direct3DCreate8 export missing; console hook disabled");
        return 0;
    }

    if (CreateAndEnableHook("Direct3DCreate8",
                            direct3DCreate8,
                            reinterpret_cast<void*>(&Direct3DCreate8Detour),
                            &g_originalDirect3DCreate8))
    {
        sh4xe::Log("Direct3DCreate8 hooked @ %p", direct3DCreate8);

        // Create a throwaway IDirect3D8 only to install the CreateDevice hook on
        // the (shared) interface vtable. EndScene is hooked later from
        // CreateDeviceDetour, on the game's real device and its own thread --
        // creating/releasing a real device here (off-thread, during startup)
        // crashes the GOG d3d8->d3d9 wrapper.
        void* probe = g_originalDirect3DCreate8(kD3d8SdkVersion);
        if (probe)
        {
            HookDirect3D8Interface(probe);
            void** vtbl = *reinterpret_cast<void***>(probe);
            reinterpret_cast<ReleaseFn>(vtbl[2])(probe);
        }
    }

    return 0;
}

} // namespace

bool InstallConsoleHook()
{
    HookThread(nullptr);
    return true;
}

} // namespace sh4xe::hooks::d3d8
