#pragma once

#include <cstdint>

namespace sh4xe::hooks
{

bool CreateAndEnableHook(const char* name, void* target, void* detour, void** original);
bool CreateAndEnableHook(const char* name, uintptr_t target, void* detour, void** original);

template<typename Fn> bool CreateAndEnableHook(const char* name, void* target, void* detour, Fn* original)
{
    return CreateAndEnableHook(name, target, detour, reinterpret_cast<void**>(original));
}

template<typename Fn> bool CreateAndEnableHook(const char* name, uintptr_t target, void* detour, Fn* original)
{
    return CreateAndEnableHook(name, reinterpret_cast<void*>(target), detour, reinterpret_cast<void**>(original));
}

bool DisableHook(const char* name, void* target);
bool DisableHook(const char* name, uintptr_t target);

} // namespace sh4xe::hooks
