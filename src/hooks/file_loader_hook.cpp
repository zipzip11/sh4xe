#include "hooks/file_loader_hook.h"

#include "core/framework.h"
#include "hooks/hook_utils.h"
#include "sh4/addresses.h"

namespace sh4xe::hooks::file_loader
{
namespace
{

using LoadFileByIdFn = int(__cdecl*)(int);

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
            "load_file_by_id", sh4::addr::kLoadFileById, reinterpret_cast<void*>(&LoadFileByIdDetour), &g_originalLoadFileById))
        return false;

    g_installed = true;
    sh4xe::Log("load_file_by_id hooked @ %p", reinterpret_cast<void*>(sh4::addr::kLoadFileById));
    return true;
}

int LoadFileByIdDirect(int fileId)
{
    if (g_originalLoadFileById)
        return g_originalLoadFileById(fileId);

    const auto loadFileById = reinterpret_cast<LoadFileByIdFn>(sh4::addr::kLoadFileById);
    return loadFileById(fileId);
}

} // namespace sh4xe::hooks::file_loader
