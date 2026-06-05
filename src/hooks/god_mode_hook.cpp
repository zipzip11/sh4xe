#include "hooks/god_mode_hook.h"

#include "core/framework.h"
#include "hooks/hook_utils.h"
#include "sdk/memory.h"
#include "sh4/addresses.h"
#include "sh4/player.h"

#include <atomic>
#include <cstdio>

namespace sh4xe::hooks::god_mode
{
namespace
{

using AccumulateContactImpactFn = double(__cdecl*)(int);
using GetPendingImpactFn = double(__cdecl*)();

AccumulateContactImpactFn g_originalAccumulateContactImpact = nullptr;
GetPendingImpactFn g_originalGetPendingImpact = nullptr;

std::atomic<bool> g_enabled{false};
std::atomic<unsigned int> g_suppressedImpacts{0};
bool g_installed = false;
bool g_pendingImpactGetterHooked = false;

void WriteFloat(uintptr_t address, float value)
{
    sdk::Write(address, value);
}

void ClearHenryImpactState()
{
    constexpr float kZero = 0.0f;
    WriteFloat(sh4::addr::kPlayerPendingImpact, kZero);
    WriteFloat(sh4::addr::kPlayerPendingContactImpact + sh4::player::kHenryPlayerIndex * sizeof(float), kZero);
    WriteFloat(sh4::addr::kPlayerImpactTimer + sh4::player::kHenryPlayerIndex * sizeof(float), kZero);
}

double __cdecl AccumulateContactImpactDetour(int player)
{
    if (g_enabled.load(std::memory_order_relaxed) && player == sh4::player::kHenryPlayerIndex)
    {
        ClearHenryImpactState();
        g_suppressedImpacts.fetch_add(1, std::memory_order_relaxed);
        return 0.0;
    }

    return g_originalAccumulateContactImpact ? g_originalAccumulateContactImpact(player) : 0.0;
}

double __cdecl GetPendingImpactDetour()
{
    if (g_enabled.load(std::memory_order_relaxed))
    {
        ClearHenryImpactState();
        return 0.0;
    }

    return g_originalGetPendingImpact ? g_originalGetPendingImpact() : 0.0;
}

} // namespace

bool InstallGodModeHook()
{
    if (g_installed)
        return true;

    if (!CreateAndEnableHook("Player_AccumulateContactImpact",
                             sh4::addr::kPlayerAccumulateContactImpact,
                             reinterpret_cast<void*>(&AccumulateContactImpactDetour),
                             &g_originalAccumulateContactImpact))
    {
        return false;
    }

    g_pendingImpactGetterHooked = CreateAndEnableHook("Player_GetPendingImpact",
                                                      sh4::addr::kPlayerGetPendingImpact,
                                                      reinterpret_cast<void*>(&GetPendingImpactDetour),
                                                      &g_originalGetPendingImpact);

    g_installed = true;
    sh4xe::Log("god mode hook installed contact=%p pending=%p pending_hook=%s",
               reinterpret_cast<void*>(sh4::addr::kPlayerAccumulateContactImpact),
               reinterpret_cast<void*>(sh4::addr::kPlayerGetPendingImpact),
               g_pendingImpactGetterHooked ? "yes" : "no");
    return true;
}

void OnFramePresented()
{
    if (g_enabled.load(std::memory_order_relaxed))
        ClearHenryImpactState();
}

void SetGodModeEnabled(bool enabled)
{
    g_enabled.store(enabled, std::memory_order_relaxed);
    if (enabled)
        ClearHenryImpactState();
    sh4xe::Log("god mode %s", enabled ? "enabled" : "disabled");
}

bool GodModeEnabled()
{
    return g_enabled.load(std::memory_order_relaxed);
}

unsigned int SuppressedImpactCount()
{
    return g_suppressedImpacts.load(std::memory_order_relaxed);
}

void DescribeGodMode(char* out, std::size_t outSize)
{
    if (!out || outSize == 0)
        return;

    snprintf(out,
             outSize,
             "ok god = %s (henry contact impact suppressed, hits=%u, getter=%s)",
             GodModeEnabled() ? "on" : "off",
             SuppressedImpactCount(),
             g_pendingImpactGetterHooked ? "hooked" : "fallback");
}

} // namespace sh4xe::hooks::god_mode
