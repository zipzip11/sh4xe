#pragma once

// PARKED: experimental loader logging/preload support. Not part of the active
// build unless it is intentionally moved back out of src/hooks/parked.

namespace sh4xe::hooks::file_loader
{

// Hooks the game's file-ID loader so callers can be observed before the
// original loader resolves and reads the file entry.
bool InstallFileLoaderHook();

// Calls the original loader trampoline when the hook is installed. Console
// commands use this for intentional preloads so the logging detour does not
// recurse through the console while its command mutex is held.
int LoadFileByIdDirect(int fileId);

} // namespace sh4xe::hooks::file_loader
