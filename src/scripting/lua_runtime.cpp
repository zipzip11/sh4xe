#include "scripting/lua_runtime.h"

#include "core/framework.h"
#include "sdk/memory.h"

extern "C"
{
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <windows.h>

#include <cstdint>
#include <cstring>
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

// ---------------------------------------------------------------------------
// sh4.* runtime memory bridge
//
// Scripts inevitably pass bad addresses, so reads are validated against the
// page map before dereferencing -- a typo prints a Lua error instead of taking
// the whole game down. Writes go through sdk::WriteBytes (which flips page
// protection as needed). Addresses come in as Lua integers; SH4 is a 32-bit,
// fixed-base (0x00400000) image, so a uintptr_t round-trips through lua_Integer.
// ---------------------------------------------------------------------------

bool RegionReadable(uintptr_t address, size_t size)
{
    if (address == 0 || size == 0)
        return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & readable) == 0 || (mbi.Protect & PAGE_GUARD) != 0)
        return false;
    // Ensure the whole span sits inside this committed region.
    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return address + size <= regionEnd;
}

uintptr_t CheckAddress(lua_State* L, int index)
{
    return static_cast<uintptr_t>(luaL_checkinteger(L, index));
}

template <typename T>
int ReadScalar(lua_State* L, bool asFloat = false)
{
    const uintptr_t address = CheckAddress(L, 1);
    if (!RegionReadable(address, sizeof(T)))
    {
        lua_pushnil(L);
        lua_pushfstring(L, "unreadable address 0x%I64x", static_cast<lua_Integer>(address));
        return 2;
    }
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    if (asFloat)
        lua_pushnumber(L, static_cast<lua_Number>(value));
    else
        lua_pushinteger(L, static_cast<lua_Integer>(value));
    return 1;
}

template <typename T>
int WriteScalar(lua_State* L, bool asFloat = false)
{
    const uintptr_t address = CheckAddress(L, 1);
    T value = asFloat ? static_cast<T>(luaL_checknumber(L, 2))
                      : static_cast<T>(luaL_checkinteger(L, 2));
    lua_pushboolean(L, sdk::WriteBytes(address, &value, sizeof(T)) ? 1 : 0);
    return 1;
}

int Sh4ReadU8(lua_State* L) { return ReadScalar<uint8_t>(L); }
int Sh4ReadU16(lua_State* L) { return ReadScalar<uint16_t>(L); }
int Sh4ReadU32(lua_State* L) { return ReadScalar<uint32_t>(L); }
int Sh4ReadI32(lua_State* L) { return ReadScalar<int32_t>(L); }
int Sh4ReadF32(lua_State* L) { return ReadScalar<float>(L, /*asFloat=*/true); }
int Sh4ReadF64(lua_State* L) { return ReadScalar<double>(L, /*asFloat=*/true); }

int Sh4WriteU8(lua_State* L) { return WriteScalar<uint8_t>(L); }
int Sh4WriteU16(lua_State* L) { return WriteScalar<uint16_t>(L); }
int Sh4WriteU32(lua_State* L) { return WriteScalar<uint32_t>(L); }
int Sh4WriteI32(lua_State* L) { return WriteScalar<int32_t>(L); }
int Sh4WriteF32(lua_State* L) { return WriteScalar<float>(L, /*asFloat=*/true); }

int Sh4ReadBytes(lua_State* L)
{
    const uintptr_t address = CheckAddress(L, 1);
    const lua_Integer size = luaL_checkinteger(L, 2);
    if (size <= 0 || size > 0x10000 || !RegionReadable(address, static_cast<size_t>(size)))
    {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, reinterpret_cast<const char*>(address), static_cast<size_t>(size));
    return 1;
}

// Calls a __cdecl function with integer/pointer arguments and returns its int
// result. EXPERIMENTAL and unguarded by design: the wrong address or arg shape
// will crash the game. Float arguments are not supported -- write float globals
// with sh4.write_f32 instead. Up to six integer args (covers the common getters
// and pointer-taking helpers in the engine).
int Sh4CallCdecl(lua_State* L)
{
    const uintptr_t address = CheckAddress(L, 1);
    const int argc = lua_gettop(L) - 1;
    if (argc < 0 || argc > 6)
        return luaL_error(L, "sh4.call_cdecl supports 0..6 integer args, got %d", argc);

    uintptr_t a[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < argc; ++i)
        a[i] = static_cast<uintptr_t>(luaL_checkinteger(L, i + 2));

    using Fn0 = int(__cdecl*)();
    using Fn1 = int(__cdecl*)(uintptr_t);
    using Fn2 = int(__cdecl*)(uintptr_t, uintptr_t);
    using Fn3 = int(__cdecl*)(uintptr_t, uintptr_t, uintptr_t);
    using Fn4 = int(__cdecl*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
    using Fn5 = int(__cdecl*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
    using Fn6 = int(__cdecl*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

    int result = 0;
    switch (argc)
    {
    case 0: result = reinterpret_cast<Fn0>(address)(); break;
    case 1: result = reinterpret_cast<Fn1>(address)(a[0]); break;
    case 2: result = reinterpret_cast<Fn2>(address)(a[0], a[1]); break;
    case 3: result = reinterpret_cast<Fn3>(address)(a[0], a[1], a[2]); break;
    case 4: result = reinterpret_cast<Fn4>(address)(a[0], a[1], a[2], a[3]); break;
    case 5: result = reinterpret_cast<Fn5>(address)(a[0], a[1], a[2], a[3], a[4]); break;
    case 6: result = reinterpret_cast<Fn6>(address)(a[0], a[1], a[2], a[3], a[4], a[5]); break;
    }
    lua_pushinteger(L, result);
    return 1;
}

const luaL_Reg kSh4Functions[] = {
    {"read_u8", Sh4ReadU8},   {"read_u16", Sh4ReadU16}, {"read_u32", Sh4ReadU32},
    {"read_i32", Sh4ReadI32}, {"read_f32", Sh4ReadF32}, {"read_f64", Sh4ReadF64},
    {"read_bytes", Sh4ReadBytes},
    {"write_u8", Sh4WriteU8}, {"write_u16", Sh4WriteU16}, {"write_u32", Sh4WriteU32},
    {"write_i32", Sh4WriteI32}, {"write_f32", Sh4WriteF32},
    {"call_cdecl", Sh4CallCdecl},
    {nullptr, nullptr},
};

std::wstring ScriptPath(const wchar_t* leaf)
{
    return std::wstring(sh4xe::ScriptsDirectory()) + leaf;
}

bool FileExists(const std::wstring& path)
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Loads scripts/sh4_symbols.lua (generated by tools/sdk/generate_lua_symbols.py
// from the validated SDK addresses) and binds the table it returns to sh4.sym,
// so scripts can write `sh4.write_f32(sh4.sym.kRenderFrameSecondsFloat, ...)`
// instead of hardcoding addresses.
void LoadSymbolTable(lua_State* L)
{
    const std::wstring path = ScriptPath(L"sh4_symbols.lua");
    if (!FileExists(path))
        return;

    const std::string narrow(path.begin(), path.end());
    if (luaL_dofile(L, narrow.c_str()) != LUA_OK)
    {
        sh4xe::Log("Lua symbols load failed: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        return;
    }
    lua_getglobal(L, "sh4");      // [symtab, sh4]
    lua_pushvalue(L, -2);         // [symtab, sh4, symtab]
    lua_setfield(L, -2, "sym");   // sh4.sym = symtab
    lua_pop(L, 2);
    sh4xe::Log("Lua symbol table bound to sh4.sym (%ls)", path.c_str());
}

void RegisterSh4Table(lua_State* L)
{
    lua_newtable(L);
    luaL_setfuncs(L, kSh4Functions, 0);
    lua_setglobal(L, "sh4");
    LoadSymbolTable(L);
}

std::wstring BootstrapScriptPath()
{
    return ScriptPath(L"sh4xe.lua");
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
    RegisterSh4Table(L_);
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
