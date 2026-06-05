#pragma once

#include <windows.h>

namespace sh4xe
{

void Bootstrap(HMODULE self);
void Log(const char* fmt, ...);

const wchar_t* ModuleDirectory();
const wchar_t* ScriptsDirectory();

} // namespace sh4xe
