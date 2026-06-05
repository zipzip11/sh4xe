#include "core/framework.h"

#include "hooks/d3d8_hook.h"
#include "hooks/dinput8_hook.h"
#include "hooks/fov_hook.h"
#include "hooks/fps_unlock_hook.h"
#include "hooks/intro_skip_hook.h"
#include "hooks/minhook_manager.h"
#include "render/d3d8_console.h"
#include "scripting/lua_runtime.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace sh4xe
{
namespace
{

HMODULE g_self = nullptr;
std::wstring g_moduleDir;
std::wstring g_scriptsDir;
std::wstring g_logPath;
std::mutex g_logMutex;

std::wstring ParentDirectory(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return L".\\";
    return path.substr(0, slash + 1);
}

void InitializePaths()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_self, path, MAX_PATH);

    g_moduleDir = ParentDirectory(path);
    g_scriptsDir = g_moduleDir + L"scripts\\";
    CreateDirectoryW(g_scriptsDir.c_str(), nullptr);
    g_logPath = g_scriptsDir + L"sh4xe.log";
}

DWORD WINAPI ModThread(LPVOID)
{
    InitializePaths();

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);

    Log("============================================================");
    Log("sh4xe dsound proxy attached");
    Log("  host process : %ls", exe);
    Log("  module dir   : %ls", g_moduleDir.c_str());
    Log("  pid          : %lu", GetCurrentProcessId());
    Log("============================================================");

    if (!hooks::Initialize())
    {
        Log("MinHook initialization failed");
        return 0;
    }

    Log("MinHook initialized");
    hooks::d3d8::InstallConsoleHook();
    hooks::dinput8::InstallInputHook();
    hooks::fov::InstallFovHook();
    hooks::fps::InstallFpsUnlockHook();
    Log("parked hooks not installed: camera responsiveness, file loader, enemy spawn, god mode");
    hooks::intro::InstallIntroSkipHook();

    scripting::LuaRuntime lua;
    if (lua.Initialize())
    {
        Log("Lua initialized");
        lua.RunOptionalBootstrap();
    }
    else
    {
        Log("Lua initialization failed");
    }

    Log("bootstrap complete");
    return 0;
}

} // namespace

void Log(const char* fmt, ...)
{
    if (g_logPath.empty())
        return;

    char buf[2048] = {};
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0)
        return;

    SYSTEMTIME t = {};
    GetLocalTime(&t);

    char line[2304] = {};
    const int m = snprintf(
        line, sizeof(line), "[%02u:%02u:%02u.%03u] %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
    if (m < 0)
        return;

    render::d3d8_console::AddLog("%s", buf);

    std::lock_guard<std::mutex> lock(g_logMutex);
    HANDLE file = CreateFileW(g_logPath.c_str(),
                              FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr,
                              OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(m), &written, nullptr);
    CloseHandle(file);
}

const wchar_t* ModuleDirectory()
{
    return g_moduleDir.c_str();
}

const wchar_t* ScriptsDirectory()
{
    return g_scriptsDir.c_str();
}

void Bootstrap(HMODULE self)
{
    g_self = self;

    HANDLE thread = CreateThread(nullptr, 0, ModThread, nullptr, 0, nullptr);
    if (thread)
        CloseHandle(thread);
}

} // namespace sh4xe
