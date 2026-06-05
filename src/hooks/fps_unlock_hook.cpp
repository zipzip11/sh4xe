#include "hooks/fps_unlock_hook.h"

#include "core/framework.h"
#include "hooks/hook_utils.h"
#include "sdk/memory.h"
#include "sh4/addresses.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace sh4xe::hooks::fps
{
namespace
{

// ---------------------------------------------------------------------------
// Why this hook looks the way it does (Silent Hill 4 timing model)
//
// The main loop (WinMain @ 0x414A50) is a real-time-gated FIXED-timestep
// scheduler: it runs exactly one update+render step every kMainLoopFrameInterval
// ticks of QueryPerformanceCounter time, where that interval is seeded to
// QueryPerformanceFrequency / 30 -- i.e. 30 simulation steps per second.
//
// The simulation itself is almost entirely DELTA-TIME driven: player movement
// (sub_53C250), enemy/creature updates (sub_545160) and the camera
// (Camera_UpdateFollow) all advance by `Game_GetFrameSeconds() * speed`, and
// Game_GetFrameSeconds() returns 1.0 / kGameFrameRateValue (30 by default). Only
// a few cosmetic particle/decal spawns are genuinely fixed-step (they key off the
// per-step counter at kSimFrameCounter).
//
// Consequence: to render at 60 fps WITHOUT speeding the game up, two knobs must
// move together, atomically:
//   * the scheduler interval -> QPF / 60   (60 steps/sec, twice as many frames)
//   * the delta-time source   -> 1 / 60    (each step advances half as far)
// Then steps/sec * seconds/step == 60 * (1/60) == 1.0, so wall-clock game speed
// is unchanged. If the scheduler is bumped to 60 while the delta-time is left at
// 1/30 (even briefly), every step advances a full 30 fps worth of motion at
// 60 Hz and the WHOLE GAME runs at 2x. That decoupling is exactly the failure the
// old 250 ms background-writer version hit during startup / re-init windows.
//
// Fix: derive every timing global from a SINGLE fps value and re-assert the whole
// coherent set once per presented frame from inside the game thread (driven by
// the D3D8 EndScene detour via OnFramePresented). A value is only written when it
// has actually drifted, so the steady-state cost is a few reads per frame.
//
// One class of motion does NOT route through Game_GetFrameSeconds and so the
// re-assertion above cannot reach it: the procedural camera "head bob" / walk-sway,
// whose phase is advanced by a per-step increment with the stock frame time (1/30
// or 1/15) inlined into the camera code. We fix it by repointing those few fmul
// operands at DLL floats held at the matching per-frame step -- see addresses.h
// (kCamBobIncrement*) and EnsureBobIncrementRedirect / g_bobIncrementStep* below.
// ---------------------------------------------------------------------------

using GetFrameRateFn = int(__cdecl*)();
using GetFrameSecondsFn = double(__cdecl*)();

constexpr int kStockFps = 30;
constexpr int kDefaultUnlockedFps = 60;
constexpr int kMinFps = 15;
constexpr int kMaxFps = 240;

// 7680 == 256 anim ticks/frame * 30 fps: the engine's fixed-point animation clock
// where 7680 units == one second (SetRenderFrameRate / sub_417DF0).
constexpr double kAnimTicksPerSecond = 7680.0;

GetFrameRateFn g_originalGetFrameRate = nullptr;
GetFrameSecondsFn g_originalGetFrameSeconds = nullptr;

std::atomic<bool> g_enabled{true};
std::atomic<int> g_targetFps{kDefaultUnlockedFps};
std::atomic<int> g_measuredSimFps{0};
std::atomic<bool> g_watcherStarted{false};
bool g_installed = false;
LONGLONG g_qpcFrequency = 0;
std::mutex g_applyMutex;

// DLL-owned per-frame increments that fixed-step camera fmul sites are repointed
// to (see addresses.h: kCamBobIncrement* / kCamLookPitchIncrementSite15). The
// bob phase advances `phase += move_speed * STEP` once per simulation step, with
// STEP inlined as the stock frame time (1/30 or 1/15) instead of
// Game_GetFrameSeconds(), so it runs at 2x at 60 steps/sec. Gamepad look pitch
// has the same per-step `input * 1/15` idiom. Holding STEP at the correct
// per-frame value -- (1/30)*30/fps = 1/fps and (1/15)*30/fps = 2/fps -- keeps
// those camera motions real-time-correct. At the stock 30 these equal 1/30 and
// 1/15, so the redirect is a no-op when off. The game reads these via
// absolute-address fmul; aligned 4-byte float access is atomic, so no lock is
// needed on the engine side.
float g_bobIncrementStep30 = 1.0f / 30.0f;
float g_bobIncrementStep15 = 1.0f / 15.0f;
std::atomic<bool> g_bobRedirected{false};

int ClampedFps(int fps)
{
    return std::clamp(fps, kMinFps, kMaxFps);
}

// The frame rate the engine should actually run at right now. When the unlock is
// disabled we hold the stock 30 so toggling off fully restores original timing.
int EffectiveFps()
{
    return g_enabled.load(std::memory_order_relaxed) ? ClampedFps(g_targetFps.load(std::memory_order_relaxed))
                                                     : kStockFps;
}

double EffectiveFrameSeconds()
{
    return 1.0 / static_cast<double>(EffectiveFps());
}

LONGLONG QpcFrequency()
{
    if (g_qpcFrequency <= 0)
    {
        LARGE_INTEGER freq = {};
        g_qpcFrequency = (QueryPerformanceFrequency(&freq) && freq.QuadPart > 0) ? freq.QuadPart : 0;
    }
    return g_qpcFrequency;
}

// The scheduler stores QPF / fps; when there is no high-res timer the engine
// falls back to a millisecond interval (timeGetTime path in WinMain).
int SchedulerTicksFor(int fps)
{
    const LONGLONG freq = QpcFrequency();
    if (freq <= 0)
        return std::max(1, 1000 / fps);

    const LONGLONG ticks = std::max<LONGLONG>(1, freq / fps);
    return static_cast<int>(std::min<LONGLONG>(ticks, 0x7FFFFFFF));
}

// Reads a POD value out of the game's address space.
template <typename T>
T ReadValue(uintptr_t address)
{
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

// Writes only when the live value differs from what we want, so the steady state
// costs a handful of reads per frame and zero page-protection churn. Returns true
// when a write actually happened (i.e. the engine had reset the value behind us).
template <typename T>
bool EnsureValue(uintptr_t address, T desired)
{
    if (ReadValue<T>(address) == desired)
        return false;
    sdk::Write(address, desired);
    return true;
}

// Repoints a single `fmul dword ptr [const]` operand from the shared stock
// constant to one of our DLL floats, after verifying the live operand still equals
// the expected constant address. The verify guards against patching an unexpected
// build/byte layout. Returns true once the site points at our float.
bool RedirectBobSite(uintptr_t instr, uintptr_t expectedConst, const float* replacement)
{
    const uintptr_t dispAddr = instr + sh4::addr::kCamBobIncrementDispOffset;
    uint32_t current = 0;
    if (!sdk::ReadBytes(dispAddr, &current, sizeof(current)))
        return false;

    const uint32_t replacementAddr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(replacement));
    if (current == replacementAddr)
        return true; // already redirected
    if (current != static_cast<uint32_t>(expectedConst))
    {
        sh4xe::Log("bob fps fix: site %p operand %08X != expected %08X; skipping",
                   reinterpret_cast<void*>(instr),
                   current,
                   static_cast<uint32_t>(expectedConst));
        return false;
    }
    return sdk::Write<uint32_t>(dispAddr, replacementAddr);
}

// One-time redirect of every walk-sway bob increment site onto our fps-scaled
// floats. Driven from the game thread (EndScene/OnFramePresented) so the camera
// code is between updates when the .text operands are rewritten.
void EnsureBobIncrementRedirect()
{
    bool expected = false;
    if (!g_bobRedirected.compare_exchange_strong(expected, true, std::memory_order_relaxed))
        return;

    const bool a = RedirectBobSite(sh4::addr::kCamBobIncrementSite30A,
                                   sh4::addr::kCamBobIncrementConst30,
                                   &g_bobIncrementStep30);
    const bool b = RedirectBobSite(sh4::addr::kCamBobIncrementSite30B,
                                   sh4::addr::kCamBobIncrementConst30,
                                   &g_bobIncrementStep30);
    const bool c = RedirectBobSite(sh4::addr::kCamBobIncrementSite15,
                                   sh4::addr::kCamBobIncrementConst15,
                                   &g_bobIncrementStep15);
    const bool d = RedirectBobSite(sh4::addr::kCamLookPitchIncrementSite15,
                                   sh4::addr::kCamBobIncrementConst15,
                                   &g_bobIncrementStep15);

    // One-shot: .text is fully mapped before the first frame, so any failure here
    // is a permanent byte mismatch (unexpected build), not a transient miss --
    // retrying would only spam the log.
    sh4xe::Log("camera fps fix: fixed-step increment redirect %s (bob 1/30=%d,%d bob 1/15=%d look 1/15=%d)",
               (a && b && c && d) ? "active" : "partial",
               static_cast<int>(a),
               static_cast<int>(b),
               static_cast<int>(c),
               static_cast<int>(d));
}

// Applies the whole timing configuration as one coherent set, all derived from a
// single fps value so the scheduler cadence and the delta-time sources can never
// drift apart. This is the core of the speed fix (see the header comment above).
bool ApplyTimingState()
{
    const int fps = EffectiveFps();
    const double frameSeconds = 1.0 / static_cast<double>(fps);

    // Keep the redirected head-bob/walk-sway increments at the matching per-frame
    // step so the bob stays real-time-correct at any fps (see addresses.h). These
    // are no-ops at the stock 30 (1/30 and 1/15).
    g_bobIncrementStep30 = static_cast<float>(frameSeconds);
    g_bobIncrementStep15 = static_cast<float>(2.0 * frameSeconds);

    std::lock_guard<std::mutex> lock(g_applyMutex);

    bool changed = false;
    // Master frame-rate value: Game_GetFrameRate returns it and Game_GetFrameSeconds
    // returns 1.0/it, so this single int feeds every delta-time consumer that does
    // not route through our function detours (the engine inlines 1.0/value in spots).
    changed |= EnsureValue<int>(sh4::addr::kGameFrameRateValue, fps);

    // Main-loop scheduler cadence: how many update+render steps run per real second.
    changed |= EnsureValue<int>(sh4::addr::kMainLoopFrameIntervalTicks, SchedulerTicksFor(fps));

    // Render/animation clock (SetRenderFrameRate constants). Halving the per-frame
    // animation step at 60 keeps skeletal/animation playback real-time correct.
    changed |= EnsureValue<float>(sh4::addr::kRenderFrameRateFloat, static_cast<float>(fps));
    changed |= EnsureValue<float>(sh4::addr::kRenderFrameSecondsFloat, static_cast<float>(frameSeconds));
    changed |= EnsureValue<int>(sh4::addr::kRenderFrameStepTicks,
                                static_cast<int>(frameSeconds * kAnimTicksPerSecond));
    return changed;
}

// Derives the achieved simulation rate from the engine's own per-step frame
// counter (kSimFrameCounter, ++'d once per update in Game_UpdateWorld). This is
// the number that should read ~60 when the unlock is working; it is purely
// diagnostic (surfaced via MeasuredSimFps / the console).
void UpdateMeasuredRate()
{
    const LONGLONG freq = QpcFrequency();
    if (freq <= 0)
        return;

    static LONGLONG sampleQpc = 0;
    static int sampleCounter = 0;

    LARGE_INTEGER now = {};
    if (!QueryPerformanceCounter(&now))
        return;

    const int counter = ReadValue<int>(sh4::addr::kSimFrameCounter);
    if (sampleQpc == 0)
    {
        sampleQpc = now.QuadPart;
        sampleCounter = counter;
        return;
    }

    const LONGLONG elapsed = now.QuadPart - sampleQpc;
    if (elapsed < freq)  // sample roughly once per second
        return;

    const int frames = counter - sampleCounter;
    sampleQpc = now.QuadPart;
    sampleCounter = counter;

    // The engine resets the counter on some state transitions; drop those
    // (negative or implausibly large) samples rather than report garbage.
    if (frames <= 0 || frames > kMaxFps * 4)
        return;

    const double seconds = static_cast<double>(elapsed) / static_cast<double>(freq);
    g_measuredSimFps.store(static_cast<int>(frames / seconds + 0.5), std::memory_order_relaxed);
}

int __cdecl GetFrameRateDetour()
{
    if (g_enabled.load(std::memory_order_relaxed))
        return EffectiveFps();
    return g_originalGetFrameRate ? g_originalGetFrameRate() : kStockFps;
}

double __cdecl GetFrameSecondsDetour()
{
    if (g_enabled.load(std::memory_order_relaxed))
        return EffectiveFrameSeconds();
    return g_originalGetFrameSeconds ? g_originalGetFrameSeconds() : (1.0 / static_cast<double>(kStockFps));
}

// Safety net for the window before the D3D device (and therefore the EndScene
// tick) exists, and for the unlikely case the tick never fires. Runs at a lazy
// cadence because OnFramePresented does the real per-frame work.
DWORD WINAPI WatcherThread(LPVOID)
{
    for (;;)
    {
        ApplyTimingState();
        Sleep(500);
    }
}

void StartWatcher()
{
    bool expected = false;
    if (!g_watcherStarted.compare_exchange_strong(expected, true, std::memory_order_relaxed))
        return;
    if (HANDLE thread = CreateThread(nullptr, 0, WatcherThread, nullptr, 0, nullptr))
        CloseHandle(thread);
}

} // namespace

void OnFramePresented()
{
    if (!g_installed)
        return;
    ApplyTimingState();
    EnsureBobIncrementRedirect();
    UpdateMeasuredRate();
}

bool InstallFpsUnlockHook()
{
    if (g_installed)
        return true;

    QpcFrequency();

    const bool hookRate = CreateAndEnableHook("Game_GetFrameRate",
                                              sh4::addr::kGameGetFrameRate,
                                              reinterpret_cast<void*>(&GetFrameRateDetour),
                                              &g_originalGetFrameRate);
    const bool hookSeconds = CreateAndEnableHook("Game_GetFrameSeconds",
                                                 sh4::addr::kGameGetFrameSeconds,
                                                 reinterpret_cast<void*>(&GetFrameSecondsDetour),
                                                 &g_originalGetFrameSeconds);

    if (!hookRate || !hookSeconds)
        return false;

    g_installed = true;
    ApplyTimingState();
    StartWatcher();

    sh4xe::Log("fps unlock installed rate=%p seconds=%p target=%d (per-frame coupled timing)",
               reinterpret_cast<void*>(sh4::addr::kGameGetFrameRate),
               reinterpret_cast<void*>(sh4::addr::kGameGetFrameSeconds),
               EffectiveFps());
    return true;
}

void SetFpsUnlockEnabled(bool enabled)
{
    g_enabled.store(enabled, std::memory_order_relaxed);
    ApplyTimingState();
    sh4xe::Log("fps unlock %s target=%d", enabled ? "enabled" : "disabled", EffectiveFps());
}

bool FpsUnlockEnabled()
{
    return g_enabled.load(std::memory_order_relaxed);
}

void SetTargetFps(int fps)
{
    g_targetFps.store(ClampedFps(fps), std::memory_order_relaxed);
    ApplyTimingState();
    sh4xe::Log("fps unlock target set to %d", EffectiveFps());
}

int TargetFps()
{
    return EffectiveFps();
}

double TargetFrameSeconds()
{
    return EffectiveFrameSeconds();
}

int MeasuredSimFps()
{
    return g_measuredSimFps.load(std::memory_order_relaxed);
}

} // namespace sh4xe::hooks::fps
