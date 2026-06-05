#include "scripting/lua_runtime.h"

#include "core/framework.h"

extern "C"
{
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <string>

namespace sh4xe::scripting
{
namespace
{

int LuaLog(lua_State* L)
{
    const int argc = lua_gettop(L);
    std::string message;

    for (int i = 1; i <= argc; ++i)
    {
        size_t len = 0;
        const char* text = luaL_tolstring(L, i, &len);
        if (text)
        {
            if (!message.empty())
                message += "\t";
            message.append(text, len);
        }
        lua_pop(L, 1);
    }

    sh4xe::Log("[lua] %s", message.c_str());
    return 0;
}

std::wstring BootstrapScriptPath()
{
    return std::wstring(sh4xe::ScriptsDirectory()) + L"sh4xe.lua";
}

} // namespace

LuaRuntime::~LuaRuntime()
{
    if (L_)
    {
        lua_close(L_);
        L_ = nullptr;
    }
}

bool LuaRuntime::Initialize()
{
    if (L_)
        return true;

    L_ = luaL_newstate();
    if (!L_)
        return false;

    luaL_openlibs(L_);
    lua_pushcfunction(L_, LuaLog);
    lua_setglobal(L_, "sh4xe_log");
    return true;
}

bool LuaRuntime::RunOptionalBootstrap()
{
    if (!L_)
        return false;

    const std::wstring path = BootstrapScriptPath();
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
    {
        sh4xe::Log("Lua bootstrap not present: %ls", path.c_str());
        return true;
    }

    const int status = luaL_dofile(L_, std::string(path.begin(), path.end()).c_str());
    if (status != LUA_OK)
    {
        const char* error = lua_tostring(L_, -1);
        sh4xe::Log("Lua bootstrap failed: %s", error ? error : "unknown error");
        lua_pop(L_, 1);
        return false;
    }

    sh4xe::Log("Lua bootstrap executed: %ls", path.c_str());
    return true;
}

} // namespace sh4xe::scripting
