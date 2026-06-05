#include "hooks/intro_skip_hook.h"

#include "core/framework.h"
#include "hooks/hook_utils.h"
#include "sh4/addresses.h"

#include <atomic>

namespace sh4xe::hooks::intro
{
namespace
{

using SimpleTaskFn = int(__cdecl*)();

SimpleTaskFn g_originalRunOpeningSplash = nullptr;
SimpleTaskFn g_originalPlayBootMovie = nullptr;
SimpleTaskFn g_taskAdvanceState = reinterpret_cast<SimpleTaskFn>(sh4::addr::kTaskAdvanceState);

bool g_installed = false;
std::atomic<bool> g_enabled{true};

int __cdecl RunOpeningSplashDetour()
{
    if (!g_enabled.load(std::memory_order_relaxed))
        return g_originalRunOpeningSplash();

    sh4xe::Log("intro skip: bypassing snap_opening.bin splash sequence");
    return g_taskAdvanceState();
}

int __cdecl PlayBootMovieDetour()
{
    if (!g_enabled.load(std::memory_order_relaxed))
        return g_originalPlayBootMovie();

    sh4xe::Log("intro skip: bypassing movie\\m_00.pac boot reel");
    return g_taskAdvanceState();
}

} // namespace

bool InstallIntroSkipHook()
{
    if (g_installed)
        return true;

    const bool splashHook = CreateAndEnableHook("Startup_RunOpeningSplash",
                                                sh4::addr::kStartupRunOpeningSplash,
                                                reinterpret_cast<void*>(&RunOpeningSplashDetour),
                                                &g_originalRunOpeningSplash);
    const bool movieHook = CreateAndEnableHook("Startup_PlayBootMovie",
                                               sh4::addr::kStartupPlayBootMovie,
                                               reinterpret_cast<void*>(&PlayBootMovieDetour),
                                               &g_originalPlayBootMovie);

    if (!splashHook || !movieHook)
        return false;

    g_installed = true;
    sh4xe::Log("intro skip hooks installed splash=%p movie=%p",
               reinterpret_cast<void*>(sh4::addr::kStartupRunOpeningSplash),
               reinterpret_cast<void*>(sh4::addr::kStartupPlayBootMovie));
    return true;
}

void SetIntroSkipEnabled(bool enabled)
{
    g_enabled.store(enabled, std::memory_order_relaxed);
}

bool IntroSkipEnabled()
{
    return g_enabled.load(std::memory_order_relaxed);
}

} // namespace sh4xe::hooks::intro
