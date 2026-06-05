#pragma once

#include <cstddef>
#include <cstdint>

namespace sh4xe::sdk
{

bool ReadBytes(const void* address, void* out, size_t size);
bool WriteBytes(void* address, const void* bytes, size_t size);
bool FillBytes(void* address, uint8_t value, size_t size);
bool PatchBytes(void* address, const void* bytes, size_t size);

bool ReadBytes(uintptr_t address, void* out, size_t size);
bool WriteBytes(uintptr_t address, const void* bytes, size_t size);
bool FillBytes(uintptr_t address, uint8_t value, size_t size);
bool PatchBytes(uintptr_t address, const void* bytes, size_t size);

template<typename T> bool Write(uintptr_t address, const T& value)
{
    return WriteBytes(address, &value, sizeof(T));
}

} // namespace sh4xe::sdk
