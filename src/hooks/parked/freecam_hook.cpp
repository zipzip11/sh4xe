#include "hooks/parked/freecam_hook.h"

#include "core/framework.h"
#include "hooks/hook_utils.h"
#include "sdk/memory.h"
#include "sh4/addresses.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>

// ---------------------------------------------------------------------------
// PARKED / EXPERIMENTAL freecam. See docs/freecam_design.md.
//
// Key finding from the RE pass: SH4 PC is a PS2 port and does NOT drive Direct3D's
// view matrix (there is no SetTransform(D3DTS_VIEW) call site). The camera is a
// set of engine-built matrices, so the override must happen at the engine's
// camera sink, per mode -- NOT at the D3D8 layer, and NOT at EndScene (too late,
// the scene is already built).
//
// This scaffold hooks the apartment/first-person camera-decide function
// (sub_5009D0, the PC `cam3DecidePosition`) and, on the way out, overwrites the
// "decided" camera POSITION globals it just wrote. That gives a translating
// freecam in the apartment view. Free-LOOK (orientation) and the otherworld
// camera are TODOs that need the view/world-matrix sink located first.
// ---------------------------------------------------------------------------

namespace sh4xe::hooks::freecam
{
namespace
{

// PC addresses for the apartment camera (verified against the IDA DB; kept local
// to the parked module so the shipping build's addresses.h stays minimal).
constexpr uintptr_t kCamDecidePosition = 0x005009D0;  // PC cam3DecidePosition
constexpr uintptr_t kDecidedCamPosX = 0x01083500;     // decided camera position.x
constexpr uintptr_t kDecidedCamPosY = 0x01083504;     // .y  (also mirrored at 0x01083540)
constexpr uintptr_t kDecidedCamPosZ = 0x01083508;     // .z
constexpr uintptr_t kDecidedCamPosYMirror = 0x01083540;

using CamDecideFn = int(__cdecl*)();
CamDecideFn g_originalCamDecide = nullptr;
bool g_installed = false;

std::atomic<bool> g_enabled{false};
bool g_seeded = false;

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Vec3 g_pos;
float g_yaw = 0.0f;    // radians, around world up
float g_pitch = 0.0f;  // radians
float g_baseSpeed = 800.0f;   // world units/sec (SH4 uses large units; 300 ~= eye height drop)
LONGLONG g_lastTick = 0;
LONGLONG g_qpcFreq = 0;
bool g_togglePrev = false;

float ReadFloat(uintptr_t address)
{
    float value = 0.0f;
    sdk::ReadBytes(address, &value, sizeof(value));
    return value;
}

bool KeyDown(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

double DeltaSeconds()
{
    if (g_qpcFreq == 0)
    {
        LARGE_INTEGER f = {};
        QueryPerformanceFrequency(&f);
        g_qpcFreq = f.QuadPart > 0 ? f.QuadPart : 1;
    }
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    if (g_lastTick == 0)
    {
        g_lastTick = now.QuadPart;
        return 0.0;
    }
    const double dt = static_cast<double>(now.QuadPart - g_lastTick) / static_cast<double>(g_qpcFreq);
    g_lastTick = now.QuadPart;
    // Clamp to avoid a huge jump after a pause / first frame.
    return dt > 0.1 ? 0.1 : dt;
}

void SeedFromCamera()
{
    g_pos.x = ReadFloat(kDecidedCamPosX);
    g_pos.y = ReadFloat(kDecidedCamPosY);
    g_pos.z = ReadFloat(kDecidedCamPosZ);
    g_yaw = 0.0f;
    g_pitch = 0.0f;
    g_seeded = true;
    g_lastTick = 0;
}

// Samples input and advances the freecam transform. Movement is relative to the
// current yaw so "forward" follows where you are looking on the horizontal plane.
void Integrate(double dt)
{
    const float step = static_cast<float>(g_baseSpeed * dt *
                                          (KeyDown(VK_SHIFT) ? 4.0 : (KeyDown(VK_CONTROL) ? 0.25 : 1.0)));
    const float turn = static_cast<float>(1.8 * dt);  // rad/sec

    if (KeyDown(VK_LEFT)) g_yaw -= turn;
    if (KeyDown(VK_RIGHT)) g_yaw += turn;
    if (KeyDown(VK_UP)) g_pitch += turn;
    if (KeyDown(VK_DOWN)) g_pitch -= turn;
    const float kPitchLimit = 1.5533f;  // ~89 deg
    g_pitch = g_pitch > kPitchLimit ? kPitchLimit : (g_pitch < -kPitchLimit ? -kPitchLimit : g_pitch);

    const float s = std::sin(g_yaw);
    const float c = std::cos(g_yaw);
    // Forward on the XZ plane (SH4 world: X right, Y up, Z forward-ish).
    const float fwdX = s;
    const float fwdZ = c;
    const float rightX = c;
    const float rightZ = -s;

    if (KeyDown('W')) { g_pos.x += fwdX * step; g_pos.z += fwdZ * step; }
    if (KeyDown('S')) { g_pos.x -= fwdX * step; g_pos.z -= fwdZ * step; }
    if (KeyDown('D')) { g_pos.x += rightX * step; g_pos.z += rightZ * step; }
    if (KeyDown('A')) { g_pos.x -= rightX * step; g_pos.z -= rightZ * step; }
    if (KeyDown('R') || KeyDown(VK_SPACE)) g_pos.y += step;
    if (KeyDown('F')) g_pos.y -= step;
}

// Writes the freecam position over the engine's decided camera position. This is
// the verified, working part. Orientation (free-look) requires the view/world
// matrix sink -- see docs/freecam_design.md -- and is left as the open TODO so we
// do not write a half-correct matrix blind.
void ApplyOverride()
{
    sdk::Write<float>(kDecidedCamPosX, g_pos.x);
    sdk::Write<float>(kDecidedCamPosY, g_pos.y);
    sdk::Write<float>(kDecidedCamPosYMirror, g_pos.y);
    sdk::Write<float>(kDecidedCamPosZ, g_pos.z);

    // TODO(freecam): drive orientation. Locate the PC camera view/world-matrix
    // sink (PS2 GameCameraMatrixSet / sfCameraSetViewWorldMatrix) and write a
    // look matrix built from g_yaw/g_pitch at g_pos. Until then the view angle
    // stays whatever the engine computed.
}

int __cdecl CamDecideDetour()
{
    const int result = g_originalCamDecide ? g_originalCamDecide() : 0;

    // Edge-triggered toggle on F8 so the camera can be flipped without a console.
    const bool toggleNow = KeyDown(VK_F8);
    if (toggleNow && !g_togglePrev)
        Toggle();
    g_togglePrev = toggleNow;

    if (g_enabled.load(std::memory_order_relaxed))
        Tick();
    return result;
}

} // namespace

void SetEnabled(bool enabled)
{
    if (enabled && !g_seeded)
        SeedFromCamera();
    g_enabled.store(enabled, std::memory_order_relaxed);
    sh4xe::Log("freecam %s", enabled ? "enabled" : "disabled");
}

bool Enabled()
{
    return g_enabled.load(std::memory_order_relaxed);
}

void Toggle()
{
    SetEnabled(!Enabled());
}

void Tick()
{
    if (!g_seeded)
        SeedFromCamera();
    Integrate(DeltaSeconds());
    ApplyOverride();
}

bool InstallFreecamHook()
{
    if (g_installed)
        return true;
    if (!CreateAndEnableHook("Cam_DecidePosition(freecam)",
                             kCamDecidePosition,
                             reinterpret_cast<void*>(&CamDecideDetour),
                             &g_originalCamDecide))
        return false;
    g_installed = true;
    sh4xe::Log("freecam hook installed @ %p (disabled; F8 to toggle)",
               reinterpret_cast<void*>(kCamDecidePosition));
    return true;
}

} // namespace sh4xe::hooks::freecam
