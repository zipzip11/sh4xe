#include "sdk/memory.h"

#include <cstring>
#include <vector>
#include <windows.h>

namespace sh4xe::sdk
{

bool ReadBytes(const void* address, void* out, size_t size)
{
    if (!address || !out || size == 0)
        return false;

    std::memcpy(out, address, size);
    return true;
}

bool WriteBytes(void* address, const void* bytes, size_t size)
{
    if (!address || !bytes || size == 0)
        return false;

    DWORD oldProtect = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    std::memcpy(address, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);

    DWORD ignored = 0;
    VirtualProtect(address, size, oldProtect, &ignored);
    return true;
}

bool FillBytes(void* address, uint8_t value, size_t size)
{
    if (!address || size == 0)
        return false;

    std::vector<uint8_t> bytes(size, value);
    return WriteBytes(address, bytes.data(), bytes.size());
}

bool PatchBytes(void* address, const void* bytes, size_t size)
{
    return WriteBytes(address, bytes, size);
}

bool ReadBytes(uintptr_t address, void* out, size_t size)
{
    return ReadBytes(reinterpret_cast<const void*>(address), out, size);
}

bool WriteBytes(uintptr_t address, const void* bytes, size_t size)
{
    return WriteBytes(reinterpret_cast<void*>(address), bytes, size);
}

bool FillBytes(uintptr_t address, uint8_t value, size_t size)
{
    return FillBytes(reinterpret_cast<void*>(address), value, size);
}

bool PatchBytes(uintptr_t address, const void* bytes, size_t size)
{
    return PatchBytes(reinterpret_cast<void*>(address), bytes, size);
}

} // namespace sh4xe::sdk
