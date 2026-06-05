#pragma once

struct lua_State;

namespace sh4xe::scripting
{

class LuaRuntime
{
public:
    LuaRuntime() = default;
    ~LuaRuntime();

    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    bool Initialize();
    bool RunOptionalBootstrap();

private:
    lua_State* L_ = nullptr;
};

} // namespace sh4xe::scripting
