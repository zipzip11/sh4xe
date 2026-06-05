#include "render/d3d8_console.h"

#include "core/framework.h"
#include "hooks/enemy_spawn_hook.h"
#include "hooks/fov_hook.h"
#include "hooks/fps_unlock_hook.h"
#include "hooks/god_mode_hook.h"
#include "render/console_font.h"
#include "render/display_config.h"
#include "render/render_tweaks.h"
#include "sh4/addresses.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace sh4xe::render::d3d8_console
{
namespace
{

constexpr DWORD kD3DFVF_XYZRHW = 0x000004;
constexpr DWORD kD3DFVF_DIFFUSE = 0x000040;
constexpr DWORD kD3DFVF_TEX1 = 0x000100;
constexpr DWORD kOverlayFvf = kD3DFVF_XYZRHW | kD3DFVF_DIFFUSE;
constexpr DWORD kGlyphFvf = kD3DFVF_XYZRHW | kD3DFVF_DIFFUSE | kD3DFVF_TEX1;

constexpr DWORD kD3DPT_LINELIST = 2;
constexpr DWORD kD3DPT_TRIANGLELIST = 4;
constexpr DWORD kD3DSBT_ALL = 1;

constexpr DWORD kD3DFMT_A8R8G8B8 = 21;
constexpr DWORD kD3DPOOL_MANAGED = 1;

constexpr DWORD kD3DRS_ZENABLE = 7;
constexpr DWORD kD3DRS_ZWRITEENABLE = 14;
constexpr DWORD kD3DRS_SRCBLEND = 19;
constexpr DWORD kD3DRS_DESTBLEND = 20;
constexpr DWORD kD3DRS_CULLMODE = 22;
constexpr DWORD kD3DRS_ALPHABLENDENABLE = 27;
constexpr DWORD kD3DRS_FOGENABLE = 28;
constexpr DWORD kD3DRS_LIGHTING = 137;

constexpr DWORD kD3DBLEND_SRCALPHA = 5;
constexpr DWORD kD3DBLEND_INVSRCALPHA = 6;
constexpr DWORD kD3DCULL_NONE = 1;

constexpr DWORD kD3DTSS_COLOROP = 1;
constexpr DWORD kD3DTSS_COLORARG1 = 2;
constexpr DWORD kD3DTSS_ALPHAOP = 4;
constexpr DWORD kD3DTSS_ALPHAARG1 = 5;
constexpr DWORD kD3DTSS_ALPHAARG2 = 6;
constexpr DWORD kD3DTSS_MAGFILTER = 16;
constexpr DWORD kD3DTSS_MINFILTER = 17;
constexpr DWORD kD3DTOP_SELECTARG1 = 2;
constexpr DWORD kD3DTOP_MODULATE = 4;
constexpr DWORD kD3DTA_DIFFUSE = 0;
constexpr DWORD kD3DTA_TEXTURE = 2;
constexpr DWORD kD3DTEXF_LINEAR = 2;

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

struct TexVertex
{
    float x;
    float y;
    float z;
    float rhw;
    DWORD color;
    float u;
    float v;
};

struct D3DLockedRect
{
    INT Pitch;
    void* pBits;
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
using CreateTextureFn = HRESULT(__stdcall*)(void*, UINT, UINT, UINT, DWORD, DWORD, DWORD, void**);
using TexLockRectFn = HRESULT(__stdcall*)(void*, UINT, D3DLockedRect*, const RECT*, DWORD);
using TexUnlockRectFn = HRESULT(__stdcall*)(void*, UINT);

// IDirect3DDevice8 / IDirect3DTexture8 vtable indices used here.
constexpr size_t kVtblCreateTexture = 20;
constexpr size_t kVtblTexLockRect = 16;
constexpr size_t kVtblTexUnlockRect = 17;

// Lazily-built Consolas glyph texture. Created in D3DPOOL_MANAGED so it survives
// device resets without us tracking them.
console_font::Atlas g_atlas;
void* g_fontTexture = nullptr;
bool g_fontInitTried = false;

// Ensures the font texture exists. Runs on the render thread (from EndScene).
// Returns true once a usable textured font is available; false means callers
// should fall back to the stroke font.
bool EnsureFontTexture(void* device)
{
    if (g_fontInitTried)
        return g_fontTexture != nullptr;

    g_fontInitTried = true;

    if (!console_font::Build(g_atlas, L"Consolas", 22))
    {
        sh4xe::Log("console font: Consolas rasterization failed; using stroke font");
        return false;
    }

    void** vtbl = *reinterpret_cast<void***>(device);
    const auto createTexture = reinterpret_cast<CreateTextureFn>(vtbl[kVtblCreateTexture]);

    void* texture = nullptr;
    const HRESULT hr = createTexture(device,
                                     static_cast<UINT>(g_atlas.width),
                                     static_cast<UINT>(g_atlas.height),
                                     1,
                                     0,
                                     kD3DFMT_A8R8G8B8,
                                     kD3DPOOL_MANAGED,
                                     &texture);
    if (FAILED(hr) || !texture)
    {
        sh4xe::Log("console font: CreateTexture failed hr=0x%08lX; using stroke font",
                   static_cast<unsigned long>(hr));
        return false;
    }

    void** texVtbl = *reinterpret_cast<void***>(texture);
    const auto lockRect = reinterpret_cast<TexLockRectFn>(texVtbl[kVtblTexLockRect]);
    const auto unlockRect = reinterpret_cast<TexUnlockRectFn>(texVtbl[kVtblTexUnlockRect]);

    D3DLockedRect locked = {};
    if (FAILED(lockRect(texture, 0, &locked, nullptr, 0)) || !locked.pBits)
    {
        sh4xe::Log("console font: LockRect failed; using stroke font");
        return false;
    }

    auto* dst = static_cast<uint8_t*>(locked.pBits);
    for (int y = 0; y < g_atlas.height; ++y)
    {
        std::memcpy(dst + static_cast<size_t>(y) * static_cast<size_t>(locked.Pitch),
                    g_atlas.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(g_atlas.width),
                    static_cast<size_t>(g_atlas.width) * sizeof(uint32_t));
    }
    unlockRect(texture, 0);

    g_fontTexture = texture;
    sh4xe::Log("console font: Consolas atlas %dx%d uploaded", g_atlas.width, g_atlas.height);
    return true;
}

// Recursive so a command handler running under the lock (see ProcessInput ->
// ExecuteCommandLocked) can safely call sh4xe::Log, which re-enters AddLog and
// re-locks this mutex on the same thread. A plain std::mutex deadlocks/crashes
// there -- that was the `fps 30` console crash.
std::recursive_mutex g_mutex;
bool g_open = false;
std::string g_input;
std::vector<std::string> g_history;
size_t g_historyCursor = 0;
std::string g_historyDraft;
std::vector<std::string> g_lines = {
    "ok sh4xe d3d8 console ready",
    "press ` to toggle",
    "type help for commands",
};

constexpr size_t kMaxInputChars = 120;
constexpr size_t kMaxCommandHistory = 64;

DWORD Argb(BYTE a, BYTE r, BYTE g, BYTE b)
{
    return (static_cast<DWORD>(a) << 24) | (static_cast<DWORD>(r) << 16) | (static_cast<DWORD>(g) << 8) |
           static_cast<DWORD>(b);
}

bool Pressed(int vk)
{
    static bool previous[256] = {};
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool pressed = down && !previous[vk & 0xFF];
    previous[vk & 0xFF] = down;
    return pressed;
}

bool ShiftDown()
{
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
}

bool CtrlDown()
{
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
}

void PushLineLocked(std::string line)
{
    constexpr size_t kMaxLines = 96;
    if (line.empty())
        return;

    if (line.size() > 160)
        line.resize(160);

    g_lines.push_back(line);
    if (g_lines.size() > kMaxLines)
        g_lines.erase(g_lines.begin(), g_lines.begin() + (g_lines.size() - kMaxLines));
}

void ResetHistoryCursorLocked()
{
    g_historyCursor = g_history.size();
    g_historyDraft.clear();
}

void StoreHistoryLocked(const std::string& command)
{
    if (!command.empty() && (g_history.empty() || g_history.back() != command))
    {
        g_history.push_back(command);
        if (g_history.size() > kMaxCommandHistory)
            g_history.erase(g_history.begin(), g_history.begin() + (g_history.size() - kMaxCommandHistory));
    }

    ResetHistoryCursorLocked();
}

void RecallPreviousCommandLocked()
{
    if (g_history.empty())
        return;

    if (g_historyCursor > g_history.size())
        g_historyCursor = g_history.size();

    if (g_historyCursor == g_history.size())
        g_historyDraft = g_input;

    if (g_historyCursor > 0)
        --g_historyCursor;

    g_input = g_history[g_historyCursor];
}

void RecallNextCommandLocked()
{
    if (g_history.empty() || g_historyCursor >= g_history.size())
        return;

    ++g_historyCursor;
    if (g_historyCursor == g_history.size())
    {
        g_input = g_historyDraft;
        g_historyDraft.clear();
        return;
    }

    g_input = g_history[g_historyCursor];
}

void AppendInputCharLocked(char value)
{
    if (g_input.size() >= kMaxInputChars)
        return;

    g_input.push_back(value);
    ResetHistoryCursorLocked();
}

bool ParseDisplayMode(const std::string& value, display::Mode& mode)
{
    if (value == "fullscreen" || value == "full")
    {
        mode = display::Mode::Fullscreen;
        return true;
    }
    if (value == "windowed" || value == "window")
    {
        mode = display::Mode::Windowed;
        return true;
    }
    if (value == "borderless" || value == "borderless-windowed")
    {
        mode = display::Mode::Borderless;
        return true;
    }
    return false;
}

bool ParseResolution(const std::string& value, UINT& width, UINT& height)
{
    const size_t sep = value.find('x');
    if (sep == std::string::npos || sep == 0 || sep + 1 >= value.size())
        return false;

    char* end = nullptr;
    const unsigned long parsedWidth = std::strtoul(value.c_str(), &end, 10);
    if (!end || *end != 'x')
        return false;

    const unsigned long parsedHeight = std::strtoul(end + 1, &end, 10);
    if (!end || *end != '\0')
        return false;

    width = static_cast<UINT>(parsedWidth);
    height = static_cast<UINT>(parsedHeight);
    return width > 0 && height > 0;
}

void ExecuteDisplayCommandLocked(const std::string& command)
{
    char message[160] = {};
    const std::string args = command.size() > 7 ? command.substr(8) : std::string();

    if (args.empty())
    {
        display::Describe(message, sizeof(message));
        PushLineLocked(message);
        PushLineLocked("ok usage: display <fullscreen|windowed|borderless>");
        PushLineLocked("ok        display resolution <width>x<height>");
        return;
    }

    if (args == "resolution" || args == "res")
    {
        display::Describe(message, sizeof(message));
        PushLineLocked(message);
        return;
    }

    if (args.rfind("resolution ", 0) == 0 || args.rfind("res ", 0) == 0)
    {
        const size_t offset = args[3] == ' ' ? 4 : 11;
        UINT width = 0;
        UINT height = 0;
        if (!ParseResolution(args.substr(offset), width, height))
        {
            PushLineLocked("err display resolution: use <width>x<height>");
            return;
        }

        display::RequestResolution(width, height, message, sizeof(message));
        PushLineLocked(message);
        return;
    }

    const size_t space = args.find(' ');
    const std::string first = space == std::string::npos ? args : args.substr(0, space);
    const std::string second = space == std::string::npos ? std::string() : args.substr(space + 1);

    display::Mode mode = display::Mode::Windowed;
    if (ParseDisplayMode(first, mode))
    {
        if (second.empty())
        {
            display::RequestMode(mode, message, sizeof(message));
            PushLineLocked(message);
            return;
        }

        UINT width = 0;
        UINT height = 0;
        if (!ParseResolution(second, width, height))
        {
            PushLineLocked("err display: use mode plus optional <width>x<height>");
            return;
        }

        display::RequestModeAndResolution(mode, width, height, message, sizeof(message));
        PushLineLocked(message);
        return;
    }

    UINT width = 0;
    UINT height = 0;
    if (ParseResolution(args, width, height))
    {
        display::RequestResolution(width, height, message, sizeof(message));
        PushLineLocked(message);
        return;
    }

    PushLineLocked("err display: use fullscreen|windowed|borderless|resolution");
}

constexpr double kRadToDeg = 57.295779513082323; // 180 / pi
constexpr double kDegToRad = 0.017453292519943295; // pi / 180

// FOV is expressed to the user in vertical degrees, anchored to the game's
// stock 35deg default. We translate that into the persistent multiplier the
// hook applies, so cutscenes that request a different base FOV stay
// proportional instead of being clamped to one fixed angle.
void ExecuteFovCommandLocked(const std::string& command)
{
    const std::string arg = command.size() > 3 ? command.substr(4) : std::string();

    const float defaultRad = sh4::addr::kDefaultVerticalFovRadians;
    const double defaultDeg = static_cast<double>(defaultRad) * kRadToDeg;

    const auto report = [&]()
    {
        const float scale = hooks::fov::GetFovScale();
        const double anchorDeg = defaultDeg * static_cast<double>(scale);
        char message[160] = {};

        const float lastBase = hooks::fov::LastBaseFovRadians();
        if (lastBase > 0.0f)
        {
            const double effDeg = static_cast<double>(lastBase) * static_cast<double>(scale) * kRadToDeg;
            snprintf(message,
                     sizeof(message),
                     "ok fov = %.1f deg (x%.2f, default %.1f); current scene %.1f deg",
                     anchorDeg,
                     static_cast<double>(scale),
                     defaultDeg,
                     effDeg);
        }
        else
        {
            snprintf(message,
                     sizeof(message),
                     "ok fov = %.1f deg (x%.2f, default %.1f)",
                     anchorDeg,
                     static_cast<double>(scale),
                     defaultDeg);
        }
        PushLineLocked(message);
    };

    if (arg.empty())
    {
        report();
        PushLineLocked("ok usage: fov <degrees> | fov reset  (default 35)");
        return;
    }

    if (arg == "reset" || arg == "default")
    {
        hooks::fov::SetFovScale(1.0f);
        report();
        return;
    }

    char* end = nullptr;
    const double degrees = std::strtod(arg.c_str(), &end);
    if (!end || end == arg.c_str() || *end != '\0' || !(degrees > 0.0))
    {
        PushLineLocked("err fov: use a vertical angle in degrees, e.g. fov 60");
        return;
    }

    const double clampedDeg = std::clamp(degrees, 10.0, 170.0);
    const float scale = static_cast<float>((clampedDeg * kDegToRad) / static_cast<double>(defaultRad));
    hooks::fov::SetFovScale(scale);
    report();
}

bool ParseToggle(const std::string& value, bool& enabled);

void ReportFpsCommandLocked()
{
    char message[192] = {};
    const int measured = hooks::fps::MeasuredSimFps();
    if (measured > 0)
    {
        snprintf(message,
                 sizeof(message),
                 "ok fps = %d (%s, frame %.4fs, measured sim %d/s)",
                 hooks::fps::TargetFps(),
                 hooks::fps::FpsUnlockEnabled() ? "unlocked" : "stock",
                 hooks::fps::TargetFrameSeconds(),
                 measured);
    }
    else
    {
        snprintf(message,
                 sizeof(message),
                 "ok fps = %d (%s, frame %.4fs)",
                 hooks::fps::TargetFps(),
                 hooks::fps::FpsUnlockEnabled() ? "unlocked" : "stock",
                 hooks::fps::TargetFrameSeconds());
    }
    PushLineLocked(message);
}

void ExecuteFpsCommandLocked(const std::string& command)
{
    const std::string arg = command.size() > 3 ? command.substr(4) : std::string();

    if (arg.empty())
    {
        ReportFpsCommandLocked();
        PushLineLocked("ok usage: fps <30|60|120> | fps on|off|reset");
        return;
    }

    bool enabled = false;
    if (ParseToggle(arg, enabled))
    {
        hooks::fps::SetFpsUnlockEnabled(enabled);
        if (enabled && hooks::fps::TargetFps() <= 30)
            hooks::fps::SetTargetFps(60);
        ReportFpsCommandLocked();
        return;
    }

    if (arg == "reset" || arg == "default")
    {
        hooks::fps::SetTargetFps(60);
        hooks::fps::SetFpsUnlockEnabled(true);
        ReportFpsCommandLocked();
        return;
    }

    char* end = nullptr;
    const long parsed = std::strtol(arg.c_str(), &end, 10);
    if (!end || end == arg.c_str() || *end != '\0' || parsed < 15 || parsed > 240)
    {
        PushLineLocked("err fps: use 15..240, usually 30 or 60");
        return;
    }

    hooks::fps::SetTargetFps(static_cast<int>(parsed));
    hooks::fps::SetFpsUnlockEnabled(parsed > 30);
    ReportFpsCommandLocked();
}

std::string GodCommandArgs(const std::string& command)
{
    if (command == "god" || command == "godmode" || command == "immortal" || command == "invincible")
        return std::string();
    if (command.rfind("god ", 0) == 0)
        return command.substr(4);
    if (command.rfind("godmode ", 0) == 0)
        return command.substr(8);
    if (command.rfind("immortal ", 0) == 0)
        return command.substr(9);
    return command.substr(11);
}

void ReportGodCommandLocked()
{
    char message[160] = {};
    hooks::god_mode::DescribeGodMode(message, sizeof(message));
    PushLineLocked(message);
}

void ExecuteGodCommandLocked(const std::string& command)
{
    const std::string args = GodCommandArgs(command);

    if (args.empty() || args == "status")
    {
        ReportGodCommandLocked();
        PushLineLocked("ok usage: god <on|off|status>");
        return;
    }

    if (args == "toggle")
    {
        hooks::god_mode::SetGodModeEnabled(!hooks::god_mode::GodModeEnabled());
        ReportGodCommandLocked();
        return;
    }

    bool enabled = false;
    if (ParseToggle(args, enabled))
    {
        hooks::god_mode::SetGodModeEnabled(enabled);
        ReportGodCommandLocked();
        return;
    }

    PushLineLocked("err god: use on|off|status");
}

bool ParseDoubleExact(const std::string& value, double& parsed)
{
    char* end = nullptr;
    parsed = std::strtod(value.c_str(), &end);
    return end && end != value.c_str() && *end == '\0';
}

bool ParseToggle(const std::string& value, bool& enabled)
{
    if (value == "on" || value == "true" || value == "1")
    {
        enabled = true;
        return true;
    }
    if (value == "off" || value == "false" || value == "0")
    {
        enabled = false;
        return true;
    }
    return false;
}

bool ParseIntExact(const std::string& value, int& parsed)
{
    char* end = nullptr;
    const long result = std::strtol(value.c_str(), &end, 10);
    if (!end || end == value.c_str() || *end != '\0' || result < -32768 || result > 32767)
        return false;

    parsed = static_cast<int>(result);
    return true;
}

std::string ToLowerAscii(std::string value)
{
    for (char& ch : value)
    {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch + ('a' - 'A'));
    }
    return value;
}

std::vector<std::string> SplitWords(const std::string& value)
{
    std::vector<std::string> words;
    size_t pos = 0;
    while (pos < value.size())
    {
        while (pos < value.size() && (value[pos] == ' ' || value[pos] == '\t'))
            ++pos;

        const size_t start = pos;
        while (pos < value.size() && value[pos] != ' ' && value[pos] != '\t')
            ++pos;

        if (start < pos)
            words.push_back(value.substr(start, pos - start));
    }

    return words;
}

void ReportFilterCommandLocked()
{
    char message[160] = {};
    snprintf(message,
             sizeof(message),
             "ok filter = %s, lod bias = %.2f",
             render::tweaks::FilterModeName(render::tweaks::GetFilterMode()),
             static_cast<double>(render::tweaks::TextureLodBias()));
    PushLineLocked(message);
}

void ExecuteFilterCommandLocked(const std::string& command)
{
    const std::string args = command.size() > 6 ? command.substr(7) : std::string();
    using render::tweaks::FilterMode;

    if (args.empty())
    {
        ReportFilterCommandLocked();
        PushLineLocked("ok usage: filter <game|point|linear|aniso>");
        PushLineLocked("ok        filter bias <value>  (default -0.35, use 0 for neutral)");
        return;
    }

    if (args == "game")
        render::tweaks::SetFilterMode(FilterMode::Game);
    else if (args == "point")
        render::tweaks::SetFilterMode(FilterMode::Point);
    else if (args == "linear")
        render::tweaks::SetFilterMode(FilterMode::Linear);
    else if (args == "aniso")
        render::tweaks::SetFilterMode(FilterMode::Anisotropic);
    else if (args == "reset" || args == "default" || args == "modern")
    {
        render::tweaks::SetFilterMode(FilterMode::Anisotropic);
        render::tweaks::SetTextureLodBias(-0.35f);
    }
    else if (args.rfind("bias ", 0) == 0 || args.rfind("lod ", 0) == 0)
    {
        const size_t offset = args[0] == 'l' ? 4 : 5;
        double bias = 0.0;
        if (!ParseDoubleExact(args.substr(offset), bias) || bias < -2.0 || bias > 2.0)
        {
            PushLineLocked("err filter bias: use -2..2, usually -0.5..0");
            return;
        }

        render::tweaks::SetTextureLodBias(static_cast<float>(bias));
    }
    else
    {
        PushLineLocked("err filter: use game|point|linear|aniso|bias");
        return;
    }

    ReportFilterCommandLocked();
}

void ReportFogCommandLocked()
{
    char message[160] = {};
    snprintf(message,
             sizeof(message),
             "ok fog = %s, distance x%.2f, density x%.2f",
             render::tweaks::FogDisabled() ? "off" : "on",
             static_cast<double>(render::tweaks::FogDistanceScale()),
             static_cast<double>(render::tweaks::FogDensityScale()));
    PushLineLocked(message);
}

void ExecuteFogCommandLocked(const std::string& command)
{
    const std::string args = command.size() > 3 ? command.substr(4) : std::string();

    if (args.empty())
    {
        ReportFogCommandLocked();
        PushLineLocked("ok usage: fog <on|off|soft|game>");
        PushLineLocked("ok        fog distance <scale> | fog density <scale>");
        return;
    }

    if (args == "soft" || args == "reset" || args == "default" || args == "modern")
    {
        render::tweaks::SetFogDisabled(false);
        render::tweaks::SetFogDistanceScale(1.30f);
        render::tweaks::SetFogDensityScale(0.72f);
    }
    else if (args == "game" || args == "stock")
    {
        render::tweaks::SetFogDisabled(false);
        render::tweaks::SetFogDistanceScale(1.0f);
        render::tweaks::SetFogDensityScale(1.0f);
    }
    else if (args.rfind("distance ", 0) == 0 || args.rfind("dist ", 0) == 0)
    {
        const size_t offset = args.rfind("dist ", 0) == 0 ? 5 : 9;
        double scale = 0.0;
        if (!ParseDoubleExact(args.substr(offset), scale) || scale < 0.25 || scale > 4.0)
        {
            PushLineLocked("err fog distance: use 0.25..4");
            return;
        }

        render::tweaks::SetFogDistanceScale(static_cast<float>(scale));
    }
    else if (args.rfind("density ", 0) == 0)
    {
        double scale = 0.0;
        if (!ParseDoubleExact(args.substr(8), scale) || scale < 0.0 || scale > 4.0)
        {
            PushLineLocked("err fog density: use 0..4");
            return;
        }

        render::tweaks::SetFogDensityScale(static_cast<float>(scale));
    }
    else
    {
        bool enabled = false;
        if (!ParseToggle(args, enabled))
        {
            PushLineLocked("err fog: use on|off|soft|game|distance|density");
            return;
        }

        render::tweaks::SetFogDisabled(!enabled);
    }

    ReportFogCommandLocked();
}

void ReportGraphicsCommandLocked()
{
    char message[224] = {};
    snprintf(message,
             sizeof(message),
             "ok graphics filter=%s lod=%.2f fog=%s dist=%.2f dens=%.2f dither=%s",
             render::tweaks::FilterModeName(render::tweaks::GetFilterMode()),
             static_cast<double>(render::tweaks::TextureLodBias()),
             render::tweaks::FogDisabled() ? "off" : "on",
             static_cast<double>(render::tweaks::FogDistanceScale()),
             static_cast<double>(render::tweaks::FogDensityScale()),
             render::tweaks::DitherEnabled() ? "on" : "off");
    PushLineLocked(message);
}

void ExecuteGraphicsCommandLocked(const std::string& command)
{
    const std::string args = command.size() > 8 ? command.substr(9) : std::string();

    if (args.empty())
    {
        ReportGraphicsCommandLocked();
        PushLineLocked("ok usage: graphics <modern|game|reset>");
        PushLineLocked("ok        graphics dither <on|off>");
        return;
    }

    if (args == "modern" || args == "reset" || args == "default")
    {
        render::tweaks::ResetGraphicsQualityTweak();
        ReportGraphicsCommandLocked();
        return;
    }

    if (args == "game" || args == "stock")
    {
        render::tweaks::SetFilterMode(render::tweaks::FilterMode::Game);
        render::tweaks::SetTextureLodBias(0.0f);
        render::tweaks::SetFogDisabled(false);
        render::tweaks::SetFogDistanceScale(1.0f);
        render::tweaks::SetFogDensityScale(1.0f);
        render::tweaks::SetDitherEnabled(false);
        ReportGraphicsCommandLocked();
        return;
    }

    if (args.rfind("dither ", 0) == 0)
    {
        bool enabled = false;
        if (!ParseToggle(args.substr(7), enabled))
        {
            PushLineLocked("err graphics dither: use on|off");
            return;
        }

        render::tweaks::SetDitherEnabled(enabled);
        ReportGraphicsCommandLocked();
        return;
    }

    PushLineLocked("err graphics: use modern|game|reset|dither");
}

std::string SpawnCommandArgs(const std::string& command)
{
    if (command == "spawn" || command == "enemy" || command == "summon")
        return std::string();
    if (command.rfind("spawn ", 0) == 0)
        return command.substr(6);
    if (command.rfind("summon ", 0) == 0)
        return command.substr(7);
    return command.substr(6);
}

void PrintSpawnUsageLocked()
{
    PushLineLocked("ok usage: spawn [random|list|<enemy|type> [count] [variant] [distance]]");
    PushLineLocked(std::string("ok enemies: ") + hooks::enemy_spawn::EnemyListSummary());
    PushLineLocked("ok examples: spawn | spawn buzz 2 | spawn twins 1 0 800");
}

bool ParseSpawnIntTokenLocked(const std::string& token, const char* usage, int& value)
{
    if (!ParseIntExact(token, value))
    {
        PushLineLocked(usage);
        return false;
    }

    return true;
}

bool ParseSpawnDistanceTokenLocked(const std::string& token, float& distance)
{
    double parsed = 0.0;
    if (!ParseDoubleExact(token, parsed))
    {
        PushLineLocked("err spawn distance: use 150..2500");
        return false;
    }

    distance = static_cast<float>(parsed);
    return true;
}

void ExecuteSpawnCommandLocked(const std::string& command)
{
    constexpr float kDefaultDistance = 600.0f;

    const std::string args = ToLowerAscii(SpawnCommandArgs(command));
    const std::vector<std::string> words = SplitWords(args);
    char message[192] = {};

    if (words.empty() || words[0] == "random" || words[0] == "surprise" || words[0] == "me")
    {
        hooks::enemy_spawn::SpawnSurprise(message, sizeof(message));
        PushLineLocked(message);
        return;
    }

    if (words[0] == "help" || words[0] == "?")
    {
        PrintSpawnUsageLocked();
        return;
    }

    if (words[0] == "list" || words[0] == "enemies")
    {
        PushLineLocked(std::string("ok enemies: ") + hooks::enemy_spawn::EnemyListSummary());
        PushLineLocked("ok aliases: dog=hyena bat=buzz baby=bab walter=killer det=detective");
        return;
    }

    int type = 0;
    int variant = 0;
    size_t index = 1;

    const hooks::enemy_spawn::EnemyChoice* choice = hooks::enemy_spawn::FindEnemyChoice(words[0].c_str());
    if (choice)
    {
        type = choice->type;
        variant = choice->variant;
    }
    else if (words[0] == "type")
    {
        if (words.size() < 2 || !ParseSpawnIntTokenLocked(words[1], "err spawn type: use 2..17", type))
            return;
        index = 2;
    }
    else if (!ParseSpawnIntTokenLocked(words[0], "err spawn: unknown enemy; use spawn list", type))
    {
        return;
    }

    int count = 1;
    float distance = kDefaultDistance;
    bool countSet = false;
    bool variantSet = false;
    bool distanceSet = false;

    while (index < words.size())
    {
        const std::string& token = words[index++];

        if (token == "count" || token == "x")
        {
            if (index >= words.size() ||
                !ParseSpawnIntTokenLocked(words[index++], "err spawn count: use 1..8", count))
                return;
            countSet = true;
            continue;
        }

        if (token == "variant" || token == "var" || token == "v")
        {
            if (index >= words.size() ||
                !ParseSpawnIntTokenLocked(words[index++], "err spawn variant: use 0..99", variant))
                return;
            variantSet = true;
            continue;
        }

        if (token == "distance" || token == "dist" || token == "d")
        {
            if (index >= words.size() || !ParseSpawnDistanceTokenLocked(words[index++], distance))
                return;
            distanceSet = true;
            continue;
        }

        if (!countSet)
        {
            if (!ParseSpawnIntTokenLocked(token, "err spawn count: use 1..8", count))
                return;
            countSet = true;
            continue;
        }

        if (!variantSet)
        {
            if (!ParseSpawnIntTokenLocked(token, "err spawn variant: use 0..99", variant))
                return;
            variantSet = true;
            continue;
        }

        if (!distanceSet)
        {
            if (!ParseSpawnDistanceTokenLocked(token, distance))
                return;
            distanceSet = true;
            continue;
        }

        PushLineLocked("err spawn: too many arguments");
        PrintSpawnUsageLocked();
        return;
    }

    if (variant < 0 || variant > 99)
    {
        PushLineLocked("err spawn variant: use 0..99");
        return;
    }

    hooks::enemy_spawn::SpawnEnemy(type, variant, count, distance, message, sizeof(message));
    PushLineLocked(message);
}

std::string LightingCommandArgs(const std::string& command)
{
    if (command == "lighting" || command == "lights")
        return std::string();
    if (command.rfind("lighting ", 0) == 0)
        return command.substr(9);
    return command.substr(7);
}

void ReportLightingCommandLocked()
{
    char message[192] = {};
    snprintf(message,
             sizeof(message),
             "ok lighting=%s boost=%.2f ambient=%.2f lift=%.3f spec=%.2f range=%.2f grade=%.2f vignette=%.2f",
             render::tweaks::LightingEnabled() ? "on" : "off",
             static_cast<double>(render::tweaks::LightingLightBoost()),
             static_cast<double>(render::tweaks::LightingAmbientBoost()),
             static_cast<double>(render::tweaks::LightingAmbientLift()),
             static_cast<double>(render::tweaks::LightingSpecularBoost()),
             static_cast<double>(render::tweaks::LightingRangeBoost()),
             static_cast<double>(render::tweaks::LightingScreenWarmth()),
             static_cast<double>(render::tweaks::LightingVignette()));
    PushLineLocked(message);
}

bool ParseLightingNumberLocked(const std::string& value, double minValue, double maxValue, const char* usage, double& out)
{
    if (!ParseDoubleExact(value, out) || out < minValue || out > maxValue)
    {
        PushLineLocked(usage);
        return false;
    }

    return true;
}

void ExecuteLightingCommandLocked(const std::string& command)
{
    const std::string args = LightingCommandArgs(command);

    if (args.empty())
    {
        ReportLightingCommandLocked();
        PushLineLocked("ok usage: lighting <on|off|reset>");
        PushLineLocked("ok        lighting boost|ambient|spec|range <value>");
        PushLineLocked("ok        lighting lift <0..0.25>  grade|vignette <0..1>");
        return;
    }

    if (args == "reset" || args == "default")
    {
        render::tweaks::ResetLightingTweak();
        ReportLightingCommandLocked();
        return;
    }

    bool enabled = false;
    if (ParseToggle(args, enabled))
    {
        render::tweaks::SetLightingEnabled(enabled);
        ReportLightingCommandLocked();
        return;
    }

    const size_t space = args.find(' ');
    const std::string key = space == std::string::npos ? args : args.substr(0, space);
    const std::string value = space == std::string::npos ? std::string() : args.substr(space + 1);
    double number = 0.0;

    if (key == "boost" || key == "light" || key == "diffuse")
    {
        if (!ParseLightingNumberLocked(value, 0.0, 3.0, "err lighting boost: use 0..3", number))
            return;
        render::tweaks::SetLightingLightBoost(static_cast<float>(number));
        ReportLightingCommandLocked();
        return;
    }

    if (key == "ambient")
    {
        if (!ParseLightingNumberLocked(value, 0.0, 3.0, "err lighting ambient: use 0..3", number))
            return;
        render::tweaks::SetLightingAmbientBoost(static_cast<float>(number));
        ReportLightingCommandLocked();
        return;
    }

    if (key == "lift")
    {
        if (!ParseLightingNumberLocked(value, 0.0, 0.25, "err lighting lift: use 0..0.25", number))
            return;
        render::tweaks::SetLightingAmbientLift(static_cast<float>(number));
        ReportLightingCommandLocked();
        return;
    }

    if (key == "spec" || key == "specular")
    {
        if (!ParseLightingNumberLocked(value, 0.0, 4.0, "err lighting spec: use 0..4", number))
            return;
        render::tweaks::SetLightingSpecularBoost(static_cast<float>(number));
        ReportLightingCommandLocked();
        return;
    }

    if (key == "range")
    {
        if (!ParseLightingNumberLocked(value, 0.25, 3.0, "err lighting range: use 0.25..3", number))
            return;
        render::tweaks::SetLightingRangeBoost(static_cast<float>(number));
        ReportLightingCommandLocked();
        return;
    }

    if (key == "grade" || key == "warmth" || key == "screen")
    {
        if (!ParseLightingNumberLocked(value, 0.0, 1.0, "err lighting grade: use 0..1", number))
            return;
        render::tweaks::SetLightingScreenWarmth(static_cast<float>(number));
        ReportLightingCommandLocked();
        return;
    }

    if (key == "vignette")
    {
        if (!ParseLightingNumberLocked(value, 0.0, 1.0, "err lighting vignette: use 0..1", number))
            return;
        render::tweaks::SetLightingVignette(static_cast<float>(number));
        ReportLightingCommandLocked();
        return;
    }

    PushLineLocked("err lighting: use on|off|reset|boost|ambient|lift|spec|range|grade|vignette");
}

void ExecuteCameraCommandLocked(const std::string&)
{
    PushLineLocked("err camera responsiveness hook is parked in src/hooks/parked");
}

void ExecuteCommandLocked(const std::string& command)
{
    PushLineLocked("> " + command);

    if (command == "clear")
    {
        g_lines.clear();
    }
    else if (command == "help")
    {
        PushLineLocked("ok commands: help, clear, echo <text>, display, fps, god, graphics, lighting, spawn");
        PushLineLocked("ok   graphics <modern|game|reset> | graphics dither <on|off>");
        PushLineLocked("ok   filter <game|point|linear|aniso> | filter bias <value>");
        PushLineLocked("ok   fog <on|off|soft|game> | fog distance|density <scale>");
        PushLineLocked("ok   fov <degrees> | fov reset  (default 35)");
        PushLineLocked("ok   fps <30|60|120> | fps on|off|reset");
        PushLineLocked("ok   god <on|off|status>");
        PushLineLocked("ok   lighting <on|off|reset|boost|ambient|lift|spec|range|grade|vignette>");
        PushLineLocked("ok   spawn [random|list|<enemy|type> [count] [variant] [distance]]");
        PushLineLocked("ok   display <fullscreen|windowed|borderless> [widthxheight]");
        PushLineLocked("ok   display resolution <width>x<height>");
        PushLineLocked("ok   history: up/ctrl-p previous, down/ctrl-n next");
    }
    else if (command.rfind("echo ", 0) == 0)
    {
        PushLineLocked("ok " + command.substr(5));
    }
    else if (command == "filter" || command.rfind("filter ", 0) == 0)
    {
        ExecuteFilterCommandLocked(command);
    }
    else if (command == "fog" || command.rfind("fog ", 0) == 0)
    {
        ExecuteFogCommandLocked(command);
    }
    else if (command == "fov" || command.rfind("fov ", 0) == 0)
    {
        ExecuteFovCommandLocked(command);
    }
    else if (command == "fps" || command.rfind("fps ", 0) == 0 || command == "framerate" ||
             command.rfind("framerate ", 0) == 0)
    {
        const std::string normalized = command == "framerate"
                                           ? std::string("fps")
                                           : (command.rfind("framerate ", 0) == 0 ? "fps " + command.substr(10)
                                                                                  : command);
        ExecuteFpsCommandLocked(normalized);
    }
    else if (command == "camera" || command.rfind("camera ", 0) == 0 || command == "mouselook" ||
             command.rfind("mouselook ", 0) == 0)
    {
        ExecuteCameraCommandLocked(command);
    }
    else if (command == "god" || command.rfind("god ", 0) == 0 || command == "godmode" ||
             command.rfind("godmode ", 0) == 0 || command == "immortal" || command.rfind("immortal ", 0) == 0 ||
             command == "invincible" || command.rfind("invincible ", 0) == 0)
    {
        ExecuteGodCommandLocked(command);
    }
    else if (command == "graphics" || command.rfind("graphics ", 0) == 0 || command == "gfx" ||
             command.rfind("gfx ", 0) == 0)
    {
        ExecuteGraphicsCommandLocked(command == "gfx" ? std::string("graphics")
                                                      : (command.rfind("gfx ", 0) == 0
                                                             ? "graphics " + command.substr(4)
                                                             : command));
    }
    else if (command == "spawn" || command.rfind("spawn ", 0) == 0 || command == "enemy" ||
             command.rfind("enemy ", 0) == 0 || command == "summon" || command.rfind("summon ", 0) == 0)
    {
        ExecuteSpawnCommandLocked(command);
    }
    else if (command == "lighting" || command.rfind("lighting ", 0) == 0 || command == "lights" ||
             command.rfind("lights ", 0) == 0)
    {
        ExecuteLightingCommandLocked(command);
    }
    else if (command == "display" || command.rfind("display ", 0) == 0)
    {
        ExecuteDisplayCommandLocked(command);
    }
    else if (!command.empty())
    {
        PushLineLocked("err unknown command");
    }
}

void ProcessInput()
{
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    if (Pressed(VK_OEM_3))
    {
        g_open = !g_open;
        return;
    }

    if (!g_open)
        return;

    if (Pressed(VK_ESCAPE))
    {
        g_open = false;
        return;
    }

    const bool ctrl = CtrlDown();
    if (Pressed(VK_UP) || (ctrl && Pressed('P')))
    {
        RecallPreviousCommandLocked();
        return;
    }

    if (Pressed(VK_DOWN) || (ctrl && Pressed('N')))
    {
        RecallNextCommandLocked();
        return;
    }

    if (Pressed(VK_BACK))
    {
        if (!g_input.empty())
        {
            g_input.pop_back();
            ResetHistoryCursorLocked();
        }
    }

    if (Pressed(VK_RETURN))
    {
        StoreHistoryLocked(g_input);
        ExecuteCommandLocked(g_input);
        g_input.clear();
        return;
    }

    if (g_input.size() >= kMaxInputChars)
        return;

    const bool shift = ShiftDown();
    for (int vk = 'A'; vk <= 'Z'; ++vk)
    {
        if (Pressed(vk))
            AppendInputCharLocked(static_cast<char>(shift ? vk : (vk + ('a' - 'A'))));
    }
    for (int vk = '0'; vk <= '9'; ++vk)
    {
        if (Pressed(vk))
        {
            const char shifted[] = {')', '!', '@', '#', '$', '%', '^', '&', '*', '('};
            AppendInputCharLocked(shift ? shifted[vk - '0'] : static_cast<char>(vk));
        }
    }

    struct KeyChar
    {
        int vk;
        char plain;
        char shifted;
    };
    constexpr KeyChar keys[] = {
        {VK_SPACE, ' ', ' '},
        {VK_OEM_MINUS, '-', '_'},
        {VK_OEM_PLUS, '=', '+'},
        {VK_OEM_COMMA, ',', '<'},
        {VK_OEM_PERIOD, '.', '>'},
        {VK_OEM_2, '/', '?'},
        {VK_OEM_1, ';', ':'},
        {VK_OEM_7, '\'', '"'},
        {VK_OEM_4, '[', '{'},
        {VK_OEM_6, ']', '}'},
        {VK_OEM_5, '\\', '|'},
    };

    for (const auto& key : keys)
    {
        if (Pressed(key.vk))
            AppendInputCharLocked(shift ? key.shifted : key.plain);
    }
}

void AddRect(std::vector<Vertex>& vertices, float x, float y, float w, float h, DWORD color)
{
    const float z = 0.0f;
    const float rhw = 1.0f;
    vertices.push_back({x, y, z, rhw, color});
    vertices.push_back({x + w, y, z, rhw, color});
    vertices.push_back({x + w, y + h, z, rhw, color});
    vertices.push_back({x, y, z, rhw, color});
    vertices.push_back({x + w, y + h, z, rhw, color});
    vertices.push_back({x, y + h, z, rhw, color});
}

void AddLine(std::vector<Vertex>& vertices, float x1, float y1, float x2, float y2, DWORD color)
{
    vertices.push_back({x1, y1, 0.0f, 1.0f, color});
    vertices.push_back({x2, y2, 0.0f, 1.0f, color});
}

void AddStrokeChar(std::vector<Vertex>& lines, char c, float ox, float oy, float scale, DWORD color)
{
    if (c >= 'a' && c <= 'z')
        c = static_cast<char>(c - ('a' - 'A'));

    auto line = [&](float x1, float y1, float x2, float y2)
    {
        AddLine(lines, ox + x1 * scale, oy + y1 * scale, ox + x2 * scale, oy + y2 * scale, color);
    };

    switch (c)
    {
        case 'A':
            line(0, 6, 2, 0);
            line(2, 0, 4, 6);
            line(0.8f, 3.4f, 3.2f, 3.4f);
            break;
        case 'B':
            line(0, 0, 0, 6);
            line(0, 0, 3, 0);
            line(3, 0, 4, 1);
            line(4, 1, 3, 3);
            line(3, 3, 0, 3);
            line(3, 3, 4, 5);
            line(4, 5, 3, 6);
            line(3, 6, 0, 6);
            break;
        case 'C':
            line(4, 0, 0, 0);
            line(0, 0, 0, 6);
            line(0, 6, 4, 6);
            break;
        case 'D':
            line(0, 0, 0, 6);
            line(0, 0, 3, 0);
            line(3, 0, 4, 3);
            line(4, 3, 3, 6);
            line(3, 6, 0, 6);
            break;
        case 'E':
            line(4, 0, 0, 0);
            line(0, 0, 0, 6);
            line(0, 3, 3, 3);
            line(0, 6, 4, 6);
            break;
        case 'F':
            line(4, 0, 0, 0);
            line(0, 0, 0, 6);
            line(0, 3, 3, 3);
            break;
        case 'G':
            line(4, 0, 0, 0);
            line(0, 0, 0, 6);
            line(0, 6, 4, 6);
            line(4, 6, 4, 3);
            line(4, 3, 2, 3);
            break;
        case 'H':
            line(0, 0, 0, 6);
            line(4, 0, 4, 6);
            line(0, 3, 4, 3);
            break;
        case 'I':
            line(2, 0, 2, 6);
            line(0.5f, 0, 3.5f, 0);
            line(0.5f, 6, 3.5f, 6);
            break;
        case 'J':
            line(3, 0, 3, 5);
            line(3, 5, 1.5f, 6);
            line(1.5f, 6, 0, 5);
            break;
        case 'K':
            line(0, 0, 0, 6);
            line(0, 3, 3.5f, 0);
            line(0, 3, 3.5f, 6);
            break;
        case 'L':
            line(0, 0, 0, 6);
            line(0, 6, 4, 6);
            break;
        case 'M':
            line(0, 0, 0, 6);
            line(4, 0, 4, 6);
            line(0, 0, 2, 3);
            line(2, 3, 4, 0);
            break;
        case 'N':
            line(0, 0, 0, 6);
            line(4, 0, 4, 6);
            line(0, 0, 4, 6);
            break;
        case 'O':
            line(0, 0, 4, 0);
            line(4, 0, 4, 6);
            line(4, 6, 0, 6);
            line(0, 6, 0, 0);
            break;
        case 'P':
            line(0, 0, 0, 6);
            line(0, 0, 3, 0);
            line(3, 0, 4, 1.5f);
            line(4, 1.5f, 3, 3);
            line(3, 3, 0, 3);
            break;
        case 'Q':
            line(0, 0, 4, 0);
            line(4, 0, 4, 6);
            line(4, 6, 0, 6);
            line(0, 6, 0, 0);
            line(2.5f, 4.5f, 4, 6);
            break;
        case 'R':
            line(0, 0, 0, 6);
            line(0, 0, 3, 0);
            line(3, 0, 4, 1.5f);
            line(4, 1.5f, 3, 3);
            line(3, 3, 0, 3);
            line(1.5f, 3, 4, 6);
            break;
        case 'S':
            line(4, 0, 0, 0);
            line(0, 0, 0, 3);
            line(0, 3, 4, 3);
            line(4, 3, 4, 6);
            line(4, 6, 0, 6);
            break;
        case 'T':
            line(0, 0, 4, 0);
            line(2, 0, 2, 6);
            break;
        case 'U':
            line(0, 0, 0, 6);
            line(4, 0, 4, 6);
            line(0, 6, 4, 6);
            break;
        case 'V':
            line(0, 0, 2, 6);
            line(2, 6, 4, 0);
            break;
        case 'W':
            line(0, 0, 0, 6);
            line(4, 0, 4, 6);
            line(0, 6, 2, 3);
            line(2, 3, 4, 6);
            break;
        case 'X':
            line(0, 0, 4, 6);
            line(4, 0, 0, 6);
            break;
        case 'Y':
            line(0, 0, 2, 3);
            line(4, 0, 2, 3);
            line(2, 3, 2, 6);
            break;
        case 'Z':
            line(0, 0, 4, 0);
            line(4, 0, 0, 6);
            line(0, 6, 4, 6);
            break;
        case '0':
            line(0, 0, 4, 0);
            line(4, 0, 4, 6);
            line(4, 6, 0, 6);
            line(0, 6, 0, 0);
            line(4, 0, 0, 6);
            break;
        case '1':
            line(2, 0, 2, 6);
            line(1, 1, 2, 0);
            line(1, 6, 3, 6);
            break;
        case '2':
            line(0, 0, 4, 0);
            line(4, 0, 4, 3);
            line(4, 3, 0, 6);
            line(0, 6, 4, 6);
            break;
        case '3':
            line(0, 0, 4, 0);
            line(4, 0, 4, 6);
            line(4, 6, 0, 6);
            line(0, 3, 4, 3);
            break;
        case '4':
            line(0, 0, 0, 3);
            line(0, 3, 4, 3);
            line(3, 0, 3, 6);
            break;
        case '5':
            line(4, 0, 0, 0);
            line(0, 0, 0, 3);
            line(0, 3, 4, 3);
            line(4, 3, 4, 6);
            line(4, 6, 0, 6);
            break;
        case '6':
            line(4, 0, 0, 0);
            line(0, 0, 0, 6);
            line(0, 6, 4, 6);
            line(4, 6, 4, 3);
            line(4, 3, 0, 3);
            break;
        case '7':
            line(0, 0, 4, 0);
            line(4, 0, 1.5f, 6);
            break;
        case '8':
            line(0, 0, 4, 0);
            line(4, 0, 4, 6);
            line(4, 6, 0, 6);
            line(0, 6, 0, 0);
            line(0, 3, 4, 3);
            break;
        case '9':
            line(0, 0, 4, 0);
            line(4, 0, 4, 6);
            line(0, 0, 0, 3);
            line(0, 3, 4, 3);
            break;
        case '.':
            line(1.5f, 6, 2.5f, 6);
            break;
        case ':':
            line(2, 1.5f, 2, 2);
            line(2, 4.5f, 2, 5);
            break;
        case '-':
            line(0.5f, 3, 3.5f, 3);
            break;
        case '_':
            line(0, 6, 4, 6);
            break;
        case '/':
            line(4, 0, 0, 6);
            break;
        case '\\':
            line(0, 0, 4, 6);
            break;
        case '>':
            line(0.5f, 1, 3.5f, 3);
            line(3.5f, 3, 0.5f, 5);
            break;
        case '<':
            line(3.5f, 1, 0.5f, 3);
            line(0.5f, 3, 3.5f, 5);
            break;
        case '+':
            line(2, 1, 2, 5);
            line(0.5f, 3, 3.5f, 3);
            break;
        case '=':
            line(0.5f, 2, 3.5f, 2);
            line(0.5f, 4, 3.5f, 4);
            break;
        case '!':
            line(2, 0, 2, 4.5f);
            line(2, 5.5f, 2, 6);
            break;
        case '?':
            line(0, 1.5f, 0.5f, 0);
            line(0.5f, 0, 3.5f, 0);
            line(3.5f, 0, 4, 1.5f);
            line(4, 1.5f, 2, 3);
            line(2, 3, 2, 4.5f);
            line(2, 5.5f, 2, 6);
            break;
        default:
            break;
    }
}

void AddString(std::vector<Vertex>& lines, const std::string& text, float x, float y, float scale, DWORD color)
{
    float cx = x;
    for (char c : text)
    {
        AddStrokeChar(lines, c, cx, y, scale, color);
        cx += 5.6f * scale;
    }
}

std::string FitLeft(const std::string& text, size_t maxChars)
{
    if (text.size() <= maxChars)
        return text;
    if (maxChars <= 2)
        return text.substr(0, maxChars);
    return text.substr(0, maxChars - 2) + "..";
}

// Appends textured quads for one string using the Consolas atlas. (x, top) is
// the upper-left of the first glyph cell. Returns the pen x after the string.
float AddTexString(std::vector<TexVertex>& out, const std::string& text, float x, float top, float drawScale, DWORD color)
{
    const float cw = static_cast<float>(g_atlas.cellW) * drawScale;
    const float ch = static_cast<float>(g_atlas.cellH) * drawScale;
    const float advance = static_cast<float>(g_atlas.advance) * drawScale;
    const float du = 1.0f / static_cast<float>(g_atlas.width);
    const float dv = 1.0f / static_cast<float>(g_atlas.height);

    float cx = x;
    for (unsigned char uc : text)
    {
        if (uc != ' ' && uc >= g_atlas.firstChar && uc <= g_atlas.lastChar)
        {
            const int idx = uc - g_atlas.firstChar;
            const int col = idx % g_atlas.columns;
            const int row = idx / g_atlas.columns;
            const float u0 = static_cast<float>(col * g_atlas.cellW) * du;
            const float v0 = static_cast<float>(row * g_atlas.cellH) * dv;
            const float u1 = u0 + static_cast<float>(g_atlas.cellW) * du;
            const float v1 = v0 + static_cast<float>(g_atlas.cellH) * dv;

            const float x0 = cx;
            const float y0 = top;
            const float x1 = cx + cw;
            const float y1 = top + ch;

            out.push_back({x0, y0, 0.0f, 1.0f, color, u0, v0});
            out.push_back({x1, y0, 0.0f, 1.0f, color, u1, v0});
            out.push_back({x1, y1, 0.0f, 1.0f, color, u1, v1});
            out.push_back({x0, y0, 0.0f, 1.0f, color, u0, v0});
            out.push_back({x1, y1, 0.0f, 1.0f, color, u1, v1});
            out.push_back({x0, y1, 0.0f, 1.0f, color, u0, v1});
        }
        cx += advance;
    }
    return cx;
}

void DrawBatches(void* device, const std::vector<Vertex>& triangles, const std::vector<Vertex>& lines)
{
    void** vtbl = *reinterpret_cast<void***>(device);
    const auto draw = reinterpret_cast<DrawPrimitiveUPFn>(vtbl[72]);

    if (!triangles.empty())
        draw(device, kD3DPT_TRIANGLELIST, static_cast<UINT>(triangles.size() / 3), triangles.data(), sizeof(Vertex));

    if (!lines.empty())
        draw(device, kD3DPT_LINELIST, static_cast<UINT>(lines.size() / 2), lines.data(), sizeof(Vertex));
}

void DrawGlyphBatch(void* device, const std::vector<TexVertex>& glyphs)
{
    if (glyphs.empty())
        return;

    void** vtbl = *reinterpret_cast<void***>(device);
    const auto draw = reinterpret_cast<DrawPrimitiveUPFn>(vtbl[72]);
    draw(device, kD3DPT_TRIANGLELIST, static_cast<UINT>(glyphs.size() / 3), glyphs.data(), sizeof(TexVertex));
}

void SetupOverlayState(void* device)
{
    void** vtbl = *reinterpret_cast<void***>(device);
    const auto setRenderState = reinterpret_cast<SetRenderStateFn>(vtbl[50]);
    const auto setTexture = reinterpret_cast<SetTextureFn>(vtbl[61]);
    const auto setTextureStageState = reinterpret_cast<SetTextureStageStateFn>(vtbl[63]);
    const auto setVertexShader = reinterpret_cast<SetVertexShaderFn>(vtbl[76]);

    setTexture(device, 0, nullptr);
    setVertexShader(device, kOverlayFvf);
    setRenderState(device, kD3DRS_ZENABLE, FALSE);
    setRenderState(device, kD3DRS_ZWRITEENABLE, FALSE);
    setRenderState(device, kD3DRS_CULLMODE, kD3DCULL_NONE);
    setRenderState(device, kD3DRS_FOGENABLE, FALSE);
    setRenderState(device, kD3DRS_LIGHTING, FALSE);
    setRenderState(device, kD3DRS_ALPHABLENDENABLE, TRUE);
    setRenderState(device, kD3DRS_SRCBLEND, kD3DBLEND_SRCALPHA);
    setRenderState(device, kD3DRS_DESTBLEND, kD3DBLEND_INVSRCALPHA);
    setTextureStageState(device, 0, kD3DTSS_COLOROP, kD3DTOP_SELECTARG1);
    setTextureStageState(device, 0, kD3DTSS_COLORARG1, kD3DTA_DIFFUSE);
    setTextureStageState(device, 0, kD3DTSS_ALPHAOP, kD3DTOP_SELECTARG1);
    setTextureStageState(device, 0, kD3DTSS_ALPHAARG1, kD3DTA_DIFFUSE);
}

// Binds the font atlas and a colour=diffuse / alpha=(texture*diffuse) blend so
// glyphs are tinted by the vertex colour and anti-aliased via the atlas alpha.
void SetupGlyphState(void* device)
{
    void** vtbl = *reinterpret_cast<void***>(device);
    const auto setTexture = reinterpret_cast<SetTextureFn>(vtbl[61]);
    const auto setTextureStageState = reinterpret_cast<SetTextureStageStateFn>(vtbl[63]);
    const auto setVertexShader = reinterpret_cast<SetVertexShaderFn>(vtbl[76]);

    setVertexShader(device, kGlyphFvf);
    setTexture(device, 0, g_fontTexture);
    setTextureStageState(device, 0, kD3DTSS_COLOROP, kD3DTOP_SELECTARG1);
    setTextureStageState(device, 0, kD3DTSS_COLORARG1, kD3DTA_DIFFUSE);
    setTextureStageState(device, 0, kD3DTSS_ALPHAOP, kD3DTOP_MODULATE);
    setTextureStageState(device, 0, kD3DTSS_ALPHAARG1, kD3DTA_TEXTURE);
    setTextureStageState(device, 0, kD3DTSS_ALPHAARG2, kD3DTA_DIFFUSE);
    setTextureStageState(device, 0, kD3DTSS_MAGFILTER, kD3DTEXF_LINEAR);
    setTextureStageState(device, 0, kD3DTSS_MINFILTER, kD3DTEXF_LINEAR);
}

void DrawConsole(void* device)
{
    D3DVIEWPORT8 viewport = {0, 0, 640, 480, 0.0f, 1.0f};
    void** vtbl = *reinterpret_cast<void***>(device);
    reinterpret_cast<GetViewportFn>(vtbl[41])(device, &viewport);

    std::vector<std::string> lines;
    std::string input;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (!g_open)
            return;
        lines = g_lines;
        input = g_input;
    }

    const bool texFont = EnsureFontTexture(device);

    const float width = static_cast<float>(viewport.Width);
    const float height = static_cast<float>(viewport.Height);
    const float padX = std::clamp(width * 0.018f, 12.0f, 24.0f);
    const float padY = std::clamp(height * 0.018f, 10.0f, 20.0f);

    // Per-font layout: the textured atlas measures in texels (scaled), the stroke
    // fallback in its own 0..6 unit box.
    float strokeScale = 0.0f;
    float drawScale = 0.0f;
    float charW = 0.0f;
    float lineStep = 0.0f;
    float glyphH = 0.0f;
    if (texFont)
    {
        const float textPx = std::clamp(std::min(width, height) / 30.0f, 13.0f, 26.0f);
        drawScale = textPx / static_cast<float>(g_atlas.lineHeight);
        charW = static_cast<float>(g_atlas.advance) * drawScale;
        lineStep = static_cast<float>(g_atlas.lineHeight) * drawScale + 3.0f;
        glyphH = static_cast<float>(g_atlas.cellH) * drawScale;
    }
    else
    {
        strokeScale = std::clamp(std::min(width, height) / 360.0f, 1.35f, 2.3f);
        charW = 5.6f * strokeScale;
        lineStep = 9.0f * strokeScale;
        glyphH = 6.5f * strokeScale;
    }

    const float consoleH = std::clamp(height * 0.42f, 170.0f, height - 12.0f);
    const float promptH = lineStep + padY * 1.5f;

    std::vector<Vertex> triangles;
    std::vector<Vertex> lineVerts;
    std::vector<TexVertex> glyphs;
    triangles.reserve(24);
    lineVerts.reserve(4096);
    glyphs.reserve(4096);

    AddRect(triangles, 0.0f, 0.0f, width, consoleH, Argb(232, 3, 4, 6));
    AddRect(triangles, 0.0f, consoleH - promptH, width, promptH, Argb(238, 8, 11, 16));
    AddLine(lineVerts, 0.0f, consoleH - 1.0f, width, consoleH - 1.0f, Argb(245, 42, 180, 220));

    const DWORD promptColor = Argb(245, 45, 188, 225);
    const DWORD inputColor = Argb(250, 230, 235, 240);
    const DWORD normalColor = Argb(226, 174, 184, 198);
    const DWORD okColor = Argb(236, 90, 230, 125);
    const DWORD errorColor = Argb(240, 255, 92, 88);

    // Emit a string at its upper-left in whichever font is active.
    const auto emit = [&](const std::string& text, float x, float top, DWORD color)
    {
        if (texFont)
            AddTexString(glyphs, text, x, top, drawScale, color);
        else
            AddString(lineVerts, text, x, top, strokeScale, color);
    };

    const std::string prompt = "sh4xe> ";
    const float inputTop = consoleH - padY - glyphH;
    emit(prompt, padX, inputTop, promptColor);

    const size_t inputChars = static_cast<size_t>((width - padX * 2.0f - prompt.size() * charW) / charW);
    emit(FitLeft(input, inputChars), padX + prompt.size() * charW, inputTop, inputColor);

    if ((GetTickCount() / 420) % 2 == 0)
    {
        const float cursorX = padX + (prompt.size() + std::min(input.size(), inputChars)) * charW;
        AddLine(lineVerts, cursorX, inputTop, cursorX, inputTop + glyphH, inputColor);
    }

    const size_t maxChars = static_cast<size_t>((width - padX * 2.0f) / charW);
    float y = inputTop - lineStep;
    for (int i = static_cast<int>(lines.size()) - 1; i >= 0 && y > padY; --i)
    {
        const std::string& line = lines[static_cast<size_t>(i)];
        DWORD color = normalColor;
        if (line.rfind("err", 0) == 0)
            color = errorColor;
        else if (line.rfind("ok", 0) == 0)
            color = okColor;

        emit(FitLeft(line, maxChars), padX, y, color);
        y -= lineStep;
    }

    DWORD token = 0;
    const auto createStateBlock = reinterpret_cast<CreateStateBlockFn>(vtbl[57]);
    const auto applyStateBlock = reinterpret_cast<ApplyStateBlockFn>(vtbl[54]);
    const auto deleteStateBlock = reinterpret_cast<DeleteStateBlockFn>(vtbl[56]);
    const bool captured = SUCCEEDED(createStateBlock(device, kD3DSBT_ALL, &token)) && token != 0;

    SetupOverlayState(device);
    DrawBatches(device, triangles, lineVerts);

    if (texFont && !glyphs.empty())
    {
        // Keep our font filtering bilinear regardless of the user's filter tweak.
        render::tweaks::SetSuppressFilterOverride(true);
        SetupGlyphState(device);
        DrawGlyphBatch(device, glyphs);
        render::tweaks::SetSuppressFilterOverride(false);
    }

    if (captured)
    {
        applyStateBlock(device, token);
        deleteStateBlock(device, token);
    }
}

} // namespace

void AddLog(const char* fmt, ...)
{
    char buffer[512] = {};
    va_list args;
    va_start(args, fmt);
    const int written = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (written < 0)
        return;

    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    PushLineLocked(buffer);
}

void Render(void* device)
{
    if (!device)
        return;

    ProcessInput();
    DrawConsole(device);
}

bool IsOpen()
{
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_open;
}

} // namespace sh4xe::render::d3d8_console
