#include "render/d3d8_lighting.h"

#include "core/framework.h"
#include "render/render_tweaks.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace sh4xe::render::d3d8_lighting
{
namespace
{

constexpr DWORD kD3DFVF_XYZRHW = 0x000004;
constexpr DWORD kD3DFVF_DIFFUSE = 0x000040;
constexpr DWORD kOverlayFvf = kD3DFVF_XYZRHW | kD3DFVF_DIFFUSE;

constexpr DWORD kD3DPT_TRIANGLELIST = 4;
constexpr DWORD kD3DSBT_ALL = 1;

constexpr DWORD kD3DRS_SHADEMODE = 9;
constexpr DWORD kD3DRS_ZENABLE = 7;
constexpr DWORD kD3DRS_ZWRITEENABLE = 14;
constexpr DWORD kD3DRS_SRCBLEND = 19;
constexpr DWORD kD3DRS_DESTBLEND = 20;
constexpr DWORD kD3DRS_CULLMODE = 22;
constexpr DWORD kD3DRS_ALPHABLENDENABLE = 27;
constexpr DWORD kD3DRS_FOGENABLE = 28;
constexpr DWORD kD3DRS_LIGHTING = 137;

constexpr DWORD kD3DSHADE_GOURAUD = 2;
constexpr DWORD kD3DBLEND_ONE = 2;
constexpr DWORD kD3DBLEND_SRCALPHA = 5;
constexpr DWORD kD3DBLEND_INVSRCALPHA = 6;
constexpr DWORD kD3DCULL_NONE = 1;

constexpr DWORD kD3DTSS_COLOROP = 1;
constexpr DWORD kD3DTSS_COLORARG1 = 2;
constexpr DWORD kD3DTSS_ALPHAOP = 4;
constexpr DWORD kD3DTSS_ALPHAARG1 = 5;
constexpr DWORD kD3DTOP_SELECTARG1 = 2;
constexpr DWORD kD3DTA_DIFFUSE = 0;

struct D3DVIEWPORT8
{
    DWORD X;
    DWORD Y;
    DWORD Width;
    DWORD Height;
    float MinZ;
    float MaxZ;
};

struct Vertex
{
    float x;
    float y;
    float z;
    float rhw;
    DWORD color;
};

using CreateStateBlockFn = HRESULT(__stdcall*)(void*, DWORD, DWORD*);
using ApplyStateBlockFn = HRESULT(__stdcall*)(void*, DWORD);
using DeleteStateBlockFn = HRESULT(__stdcall*)(void*, DWORD);
using GetViewportFn = HRESULT(__stdcall*)(void*, D3DVIEWPORT8*);
using SetRenderStateFn = HRESULT(__stdcall*)(void*, DWORD, DWORD);
using SetTextureFn = HRESULT(__stdcall*)(void*, DWORD, void*);
using SetTextureStageStateFn = HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD);
using SetVertexShaderFn = HRESULT(__stdcall*)(void*, DWORD);
using DrawPrimitiveUPFn = HRESULT(__stdcall*)(void*, DWORD, UINT, const void*, UINT);

DWORD Argb(int a, int r, int g, int b)
{
    const auto clampByte = [](int value) -> DWORD
    {
        return static_cast<DWORD>(std::clamp(value, 0, 255));
    };

    return (clampByte(a) << 24) | (clampByte(r) << 16) | (clampByte(g) << 8) | clampByte(b);
}

void AddGradientFan(std::vector<Vertex>& out, float width, float height, DWORD centerColor, DWORD cornerColor)
{
    const float z = 0.0f;
    const float rhw = 1.0f;
    const float cx = width * 0.5f;
    const float cy = height * 0.48f;

    const Vertex center = {cx, cy, z, rhw, centerColor};
    const Vertex tl = {0.0f, 0.0f, z, rhw, cornerColor};
    const Vertex tr = {width, 0.0f, z, rhw, cornerColor};
    const Vertex br = {width, height, z, rhw, cornerColor};
    const Vertex bl = {0.0f, height, z, rhw, cornerColor};

    out.push_back(center);
    out.push_back(tl);
    out.push_back(tr);

    out.push_back(center);
    out.push_back(tr);
    out.push_back(br);

    out.push_back(center);
    out.push_back(br);
    out.push_back(bl);

    out.push_back(center);
    out.push_back(bl);
    out.push_back(tl);
}

void SetupGradeState(void* device)
{
    void** vtbl = *reinterpret_cast<void***>(device);
    const auto setRenderState = reinterpret_cast<SetRenderStateFn>(vtbl[50]);
    const auto setTexture = reinterpret_cast<SetTextureFn>(vtbl[61]);
    const auto setTextureStageState = reinterpret_cast<SetTextureStageStateFn>(vtbl[63]);
    const auto setVertexShader = reinterpret_cast<SetVertexShaderFn>(vtbl[76]);

    setTexture(device, 0, nullptr);
    setVertexShader(device, kOverlayFvf);
    setRenderState(device, kD3DRS_SHADEMODE, kD3DSHADE_GOURAUD);
    setRenderState(device, kD3DRS_ZENABLE, FALSE);
    setRenderState(device, kD3DRS_ZWRITEENABLE, FALSE);
    setRenderState(device, kD3DRS_CULLMODE, kD3DCULL_NONE);
    setRenderState(device, kD3DRS_FOGENABLE, FALSE);
    setRenderState(device, kD3DRS_LIGHTING, FALSE);
    setRenderState(device, kD3DRS_ALPHABLENDENABLE, TRUE);
    setTextureStageState(device, 0, kD3DTSS_COLOROP, kD3DTOP_SELECTARG1);
    setTextureStageState(device, 0, kD3DTSS_COLORARG1, kD3DTA_DIFFUSE);
    setTextureStageState(device, 0, kD3DTSS_ALPHAOP, kD3DTOP_SELECTARG1);
    setTextureStageState(device, 0, kD3DTSS_ALPHAARG1, kD3DTA_DIFFUSE);
}

void DrawVertices(void* device, const std::vector<Vertex>& vertices)
{
    if (vertices.empty())
        return;

    void** vtbl = *reinterpret_cast<void***>(device);
    const auto draw = reinterpret_cast<DrawPrimitiveUPFn>(vtbl[72]);
    draw(device, kD3DPT_TRIANGLELIST, static_cast<UINT>(vertices.size() / 3), vertices.data(), sizeof(Vertex));
}

} // namespace

void Render(void* device)
{
    if (!device || !tweaks::LightingEnabled())
        return;

    const float warmth = tweaks::LightingScreenWarmth();
    const float vignette = tweaks::LightingVignette();
    if (warmth <= 0.0f && vignette <= 0.0f)
        return;

    void** vtbl = *reinterpret_cast<void***>(device);

    D3DVIEWPORT8 viewport = {0, 0, 640, 480, 0.0f, 1.0f};
    const auto getViewport = reinterpret_cast<GetViewportFn>(vtbl[41]);
    if (FAILED(getViewport(device, &viewport)) || viewport.Width == 0 || viewport.Height == 0)
        return;

    DWORD token = 0;
    const auto createStateBlock = reinterpret_cast<CreateStateBlockFn>(vtbl[57]);
    const auto applyStateBlock = reinterpret_cast<ApplyStateBlockFn>(vtbl[54]);
    const auto deleteStateBlock = reinterpret_cast<DeleteStateBlockFn>(vtbl[56]);
    const bool captured = SUCCEEDED(createStateBlock(device, kD3DSBT_ALL, &token)) && token != 0;

    SetupGradeState(device);

    const float width = static_cast<float>(viewport.Width);
    const float height = static_cast<float>(viewport.Height);

    const auto setRenderState = reinterpret_cast<SetRenderStateFn>(vtbl[50]);
    if (warmth > 0.0f)
    {
        std::vector<Vertex> glow;
        glow.reserve(12);
        const int centerAlpha = static_cast<int>(std::clamp(warmth, 0.0f, 1.0f) * 72.0f);
        const int cornerAlpha = static_cast<int>(std::clamp(warmth, 0.0f, 1.0f) * 12.0f);
        AddGradientFan(glow, width, height, Argb(centerAlpha, 255, 228, 178), Argb(cornerAlpha, 78, 96, 128));

        setRenderState(device, kD3DRS_SRCBLEND, kD3DBLEND_SRCALPHA);
        setRenderState(device, kD3DRS_DESTBLEND, kD3DBLEND_ONE);
        DrawVertices(device, glow);
    }

    if (vignette > 0.0f)
    {
        std::vector<Vertex> edges;
        edges.reserve(12);
        const int edgeAlpha = static_cast<int>(std::clamp(vignette, 0.0f, 1.0f) * 150.0f);
        AddGradientFan(edges, width, height, Argb(0, 0, 0, 0), Argb(edgeAlpha, 0, 0, 0));

        setRenderState(device, kD3DRS_SRCBLEND, kD3DBLEND_SRCALPHA);
        setRenderState(device, kD3DRS_DESTBLEND, kD3DBLEND_INVSRCALPHA);
        DrawVertices(device, edges);
    }

    if (captured)
    {
        applyStateBlock(device, token);
        deleteStateBlock(device, token);
    }
}

} // namespace sh4xe::render::d3d8_lighting
