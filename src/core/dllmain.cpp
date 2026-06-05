#include <windows.h>

#include "core/framework.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        sh4xe::Bootstrap(module);
    }

    return TRUE;
}
