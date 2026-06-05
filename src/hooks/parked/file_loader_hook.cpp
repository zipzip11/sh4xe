// PARKED: experimental loader logging/preload support. This file is kept for
// reference under src/hooks/parked and is not compiled by the active project.
#include "hooks/parked/file_loader_hook.h"

#include "core/framework.h"
#include "hooks/hook_utils.h"

#include <cstdint>

namespace sh4xe::hooks::file_loader
{
namespace
{

using LoadFileByIdFn = int(__cdecl*)(int);

// int __cdecl load_file_by_id(int fileId)
constexpr uintptr_t kLoadFileById = 0x00573470;

LoadFileByIdFn g_originalLoadFileById = nullptr;
bool g_installed = false;

int __cdecl LoadFileByIdDetour(int fileId)
{
    sh4xe::Log("load_file_by_id intercepted fileId=0x%08X (%d)",
               static_cast<unsigned int>(fileId),
               fileId);

    return g_originalLoadFileById(fileId);
}

} // namespace

bool InstallFileLoaderHook()
{
    if (g_installed)
        return true;

    if (!CreateAndEnableHook(
            "load_file_by_id", kLoadFileById, reinterpret_cast<void*>(&LoadFileByIdDetour), &g_originalLoadFileById))
        return false;

    g_installed = true;
    sh4xe::Log("load_file_by_id hooked @ %p", reinterpret_cast<void*>(kLoadFileById));
    return true;
}

int LoadFileByIdDirect(int fileId)
{
    if (g_originalLoadFileById)
        return g_originalLoadFileById(fileId);

    const auto loadFileById = reinterpret_cast<LoadFileByIdFn>(kLoadFileById);
    return loadFileById(fileId);
}

} // namespace sh4xe::hooks::file_loader
