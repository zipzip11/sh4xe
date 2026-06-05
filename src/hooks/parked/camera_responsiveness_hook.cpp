// PARKED: experimental camera responsiveness/mouselook work. This file is kept
// for reference under src/hooks/parked and is not compiled by the active project.
#include "hooks/parked/camera_responsiveness_hook.h"

#include "core/framework.h"
#include "hooks/hook_utils.h"
#include "render/d3d8_console.h"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace sh4xe::hooks::camera
{
namespace
{

using GetLookInputFn = double(__cdecl*)(int);
using SyncCameraStateFn = int(__cdecl*)();

// Normal exploration uses DfltOutDoor/sub_5033C0. Its right-stick look path is
// only a temporary search-view deflection, so feeding mouse deltas through the
// stick getter either looks invisible (one-frame deltas) or springy (held fake
// stick). The stable path is the state that sub_532990 copies into the working
// camera yaw before sub_5033C0 rebuilds the camera for the frame.
constexpr uintptr_t kCameraSyncState = 0x00532990; // sub_532990
constexpr uintptr_t kCameraMasterYaw = 0x010A0FC4;
constexpr uintptr_t kCameraWorkingYaw = 0x010A1BF4;
constexpr uintptr_t kInputGetRightStickX = 0x00554350;
constexpr uintptr_t kInputGetRightStickY = 0x00554370;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = kPi * 2.0f;
constexpr float kDegToRad = kPi / 180.0f;

// Sensitivity in persistent yaw degrees per raw mouse count. 0.10 deg/count is a
// calm, precise FPS-style default (~3600 counts for a 360).
constexpr float kDefaultSensitivity = 0.10f;
constexpr float kMinSensitivity = 0.01f;
constexpr float kMaxSensitivity = 0.60f;

// Single-frame spike guard for bad focus-regain deltas.
constexpr float kMaxYawPerUpdateDeg = 45.0f;

// Return-address filter: only inject when Camera_GetLookInputH/V has called the
// right-stick getters. Other direct right-stick consumers keep the stock value.
constexpr uintptr_t kCameraGetLookInputH = 0x004F86B0;
constexpr uintptr_t kCameraGetLookInputHSize = 0x41;
constexpr uintptr_t kCameraGetLookInputV = 0x004F8700;
constexpr uintptr_t kCameraGetLookInputVSize = 0x73;

GetLookInputFn g_originalGetRightStickX = nullptr;
GetLookInputFn g_originalGetRightStickY = nullptr;
SyncCameraStateFn g_originalSyncCameraState = nullptr;
bool g_installed = false;

std::atomic<bool> g_enabled{true};
std::atomic<bool> g_invertY{false};
std::atomic<float> g_sensitivity{kDefaultSensitivity};

// Diagnostic: when on, OnFramePresented logs mouse deltas, yaw deltas, camera
// gates, and raw stick values. Rate-limited to avoid flooding the log. Toggle via
// the console: `camera debug on|off`.
std::atomic<bool> g_debug{false};
std::atomic<double> g_lastOriginalX{0.0};
std::atomic<double> g_lastOriginalY{0.0};
std::atomic<void*> g_lastCallerX{nullptr};

struct LookSnapshot
{
    long dx;
    long dy;
    float yawDeltaDeg;
    float yaw;
};

// Mouse state is protected because DirectInput, camera-state, and EndScene hooks
// are separate hook entry points even though SH4 normally runs them on one thread.
std::mutex g_stateMutex;
long g_pendingMouseX = 0;
long g_pendingMouseY = 0;
LookSnapshot g_lastApplied = {};

float LoadClampedSensitivity()
{
    const float value = g_sensitivity.load(std::memory_order_relaxed);
    if (!(value > 0.0f))
        return kDefaultSensitivity;
    return std::clamp(value, kMinSensitivity, kMaxSensitivity);
}

bool ReturnAddressIn(void* caller, uintptr_t start, uintptr_t size)
{
    const uintptr_t address = reinterpret_cast<uintptr_t>(caller);
    return address >= start && address < start + size;
}

float NormalizeAngle(float radians)
{
    while (radians > kPi)
        radians -= kTwoPi;
    while (radians < -kPi)
        radians += kTwoPi;
    return radians;
}

LookSnapshot SnapshotLook()
{
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_lastApplied;
}

void StoreLastApplied(const LookSnapshot& sample)
{
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_lastApplied = sample;
}

void ClearLastApplied()
{
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_lastApplied = {};
}

LookSnapshot ConsumePendingMouseLook()
{
    LookSnapshot sample = {};
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        sample.dx = g_pendingMouseX;
        sample.dy = g_pendingMouseY;
        g_pendingMouseX = 0;
        g_pendingMouseY = 0;
    }

    if (!g_enabled.load(std::memory_order_relaxed) || render::d3d8_console::IsOpen() || sample.dx == 0)
    {
        StoreLastApplied(sample);
        return sample;
    }

    const float sensitivity = LoadClampedSensitivity();
    sample.yawDeltaDeg =
        std::clamp(static_cast<float>(sample.dx) * sensitivity, -kMaxYawPerUpdateDeg, kMaxYawPerUpdateDeg);

    auto* masterYaw = reinterpret_cast<float*>(kCameraMasterYaw);
    auto* workingYaw = reinterpret_cast<float*>(kCameraWorkingYaw);
    const float yaw = NormalizeAngle(*masterYaw + sample.yawDeltaDeg * kDegToRad);
    *masterYaw = yaw;
    *workingYaw = yaw;
    sample.yaw = yaw;

    StoreLastApplied(sample);
    return sample;
}

double __cdecl GetRightStickXDetour(int player)
{
    void* const caller = _ReturnAddress();
    const double original = g_originalGetRightStickX(player);
    if (player != 0 || !ReturnAddressIn(caller, kCameraGetLookInputH, kCameraGetLookInputHSize))
        return original;

    if (g_debug.load(std::memory_order_relaxed))
    {
        g_lastOriginalX.store(original, std::memory_order_relaxed);
        g_lastCallerX.store(caller, std::memory_order_relaxed);
    }

    return original;
}

double __cdecl GetRightStickYDetour(int player)
{
    void* const caller = _ReturnAddress();
    const double original = g_originalGetRightStickY(player);
    if (player != 0 || !ReturnAddressIn(caller, kCameraGetLookInputV, kCameraGetLookInputVSize))
        return original;

    if (g_debug.load(std::memory_order_relaxed))
        g_lastOriginalY.store(original, std::memory_order_relaxed);

    return original;
}

int __cdecl SyncCameraStateDetour()
{
    const int result = g_originalSyncCameraState();
    ConsumePendingMouseLook();
    return result;
}

} // namespace

bool InstallCameraResponsivenessHook()
{
    if (g_installed)
        return true;

    const bool hookX = CreateAndEnableHook("Input_GetRightStickX",
                                           kInputGetRightStickX,
                                           reinterpret_cast<void*>(&GetRightStickXDetour),
                                           &g_originalGetRightStickX);
    const bool hookY = CreateAndEnableHook("Input_GetRightStickY",
                                           kInputGetRightStickY,
                                           reinterpret_cast<void*>(&GetRightStickYDetour),
                                           &g_originalGetRightStickY);
    const bool hookSync = CreateAndEnableHook("Camera_SyncState",
                                              kCameraSyncState,
                                              reinterpret_cast<void*>(&SyncCameraStateDetour),
                                              &g_originalSyncCameraState);
    if (!hookX || !hookY || !hookSync)
        return false;

    g_installed = true;
    sh4xe::Log("camera responsive mouse hook active (persistent yaw model) @ %p/%p sync=%p",
               reinterpret_cast<void*>(kInputGetRightStickX),
               reinterpret_cast<void*>(kInputGetRightStickY),
               reinterpret_cast<void*>(kCameraSyncState));
    return true;
}

void OnFramePresented()
{
    const LookSnapshot snapshot = SnapshotLook();
    if (g_debug.load(std::memory_order_relaxed))
    {
        if (snapshot.dx != 0 || snapshot.dy != 0 || snapshot.yawDeltaDeg != 0.0f)
        {
            static unsigned int s_tick = 0;
            if ((s_tick++ % 6u) == 0u) // ~every 6th active frame
            {
                // Read the engine's look-gate globals so we can see whether
                // Camera_GetLookInputH would actually USE our value this frame.
                // Camera_GetLookInputH returns 0 if any of these are set (and
                // returns the forced value instead if forcedH != 0), so if the
                // camera "doesn't respond", one of these tells us why.
                const float forcedH = *reinterpret_cast<volatile float*>(0x0107BE54);
                const float forcedV = *reinterpret_cast<volatile float*>(0x0107BE58);
                const int lookDisabled = *reinterpret_cast<volatile int*>(0x0107BE5C);
                const int gate1 = *reinterpret_cast<volatile int*>(0x010A1774);
                const int gate2 = *reinterpret_cast<volatile int*>(0x010A1804);

                sh4xe::Log("camera dbg dx=%ld dy=%ld sens=%.3f -> yaw+=%.2f yaw=%.3f | gate dis=%d g1=%d g2=%d forced=%.3f/%.3f stock=%.3f/%.3f caller=%p",
                           snapshot.dx,
                           snapshot.dy,
                           static_cast<double>(MouseSensitivity()),
                           static_cast<double>(snapshot.yawDeltaDeg),
                           static_cast<double>(snapshot.yaw),
                           lookDisabled,
                           gate1,
                           gate2,
                           static_cast<double>(forcedH),
                           static_cast<double>(forcedV),
                           g_lastOriginalX.load(std::memory_order_relaxed),
                           g_lastOriginalY.load(std::memory_order_relaxed),
                           g_lastCallerX.load(std::memory_order_relaxed));
            }
        }
    }

    ClearLastApplied();
}

void AddMouseDelta(long dx, long dy)
{
    if (!dx && !dy)
        return;
    if (!g_enabled.load(std::memory_order_relaxed))
        return;

    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_pendingMouseX += dx;
    g_pendingMouseY += dy;
}

void SetResponsiveMouseEnabled(bool enabled)
{
    g_enabled.store(enabled, std::memory_order_relaxed);
    if (!enabled)
        ResetMouseLook();
}

bool ResponsiveMouseEnabled()
{
    return g_enabled.load(std::memory_order_relaxed);
}

void SetMouseSensitivity(float sensitivity)
{
    g_sensitivity.store(std::clamp(sensitivity, kMinSensitivity, kMaxSensitivity), std::memory_order_relaxed);
}

float MouseSensitivity()
{
    return LoadClampedSensitivity();
}

void SetMouseInvertY(bool invert)
{
    g_invertY.store(invert, std::memory_order_relaxed);
}

void SetMouseDebug(bool enabled)
{
    g_debug.store(enabled, std::memory_order_relaxed);
    sh4xe::Log("camera mouse debug logging %s", enabled ? "on" : "off");
}

bool MouseDebug()
{
    return g_debug.load(std::memory_order_relaxed);
}

bool MouseInvertY()
{
    return g_invertY.load(std::memory_order_relaxed);
}

void ResetMouseLook()
{
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_pendingMouseX = 0;
    g_pendingMouseY = 0;
    g_lastApplied = {};
}

float CurrentMouseLookX()
{
    return SnapshotLook().yawDeltaDeg;
}

float CurrentMouseLookY()
{
    return 0.0f;
}

bool MouseLookActive()
{
    const LookSnapshot snapshot = SnapshotLook();
    return snapshot.dx != 0 || snapshot.dy != 0 || snapshot.yawDeltaDeg != 0.0f;
}

} // namespace sh4xe::hooks::camera
