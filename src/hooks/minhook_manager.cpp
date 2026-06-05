#include "hooks/minhook_manager.h"

#include "MinHook.h"
#include "core/framework.h"

namespace sh4xe::hooks
{
namespace
{
bool g_initialized = false;
}

bool Initialize()
{
    if (g_initialized)
        return true;

    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    {
        sh4xe::Log("MH_Initialize failed: %s", MH_StatusToString(status));
        return false;
    }

    g_initialized = true;
    return true;
}

void Shutdown()
{
    if (!g_initialized)
        return;

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_initialized = false;
}

} // namespace sh4xe::hooks
