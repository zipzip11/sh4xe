#include "hooks/hook_utils.h"

#include "MinHook.h"
#include "core/framework.h"

namespace sh4xe::hooks
{

bool CreateAndEnableHook(const char* name, void* target, void* detour, void** original)
{
    if (!target || !detour || !original)
    {
        sh4xe::Log("%s hook has invalid target/detour/original", name ? name : "unnamed");
        return false;
    }

    const MH_STATUS createStatus = MH_CreateHook(target, detour, original);
    if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED)
    {
        sh4xe::Log("%s MH_CreateHook(%p) failed: %s",
                   name ? name : "unnamed",
                   target,
                   MH_StatusToString(createStatus));
        return false;
    }

    const MH_STATUS enableStatus = MH_EnableHook(target);
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED)
    {
        sh4xe::Log("%s MH_EnableHook(%p) failed: %s",
                   name ? name : "unnamed",
                   target,
                   MH_StatusToString(enableStatus));
        return false;
    }

    return true;
}

bool CreateAndEnableHook(const char* name, uintptr_t target, void* detour, void** original)
{
    return CreateAndEnableHook(name, reinterpret_cast<void*>(target), detour, original);
}

bool DisableHook(const char* name, void* target)
{
    if (!target)
    {
        sh4xe::Log("%s disable hook has invalid target", name ? name : "unnamed");
        return false;
    }

    const MH_STATUS status = MH_DisableHook(target);
    if (status != MH_OK && status != MH_ERROR_DISABLED)
    {
        sh4xe::Log("%s MH_DisableHook(%p) failed: %s", name ? name : "unnamed", target, MH_StatusToString(status));
        return false;
    }

    return true;
}

bool DisableHook(const char* name, uintptr_t target)
{
    return DisableHook(name, reinterpret_cast<void*>(target));
}

} // namespace sh4xe::hooks
