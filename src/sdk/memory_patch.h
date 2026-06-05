#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sh4xe::sdk
{

class DataPatch
{
public:
    DataPatch() = default;
    DataPatch(uintptr_t address, const void* data, size_t size);

    DataPatch(const DataPatch&) = delete;
    DataPatch& operator=(const DataPatch&) = delete;
    DataPatch(DataPatch&&) = default;
    DataPatch& operator=(DataPatch&&) = default;

    template<typename T> static DataPatch FromValue(uintptr_t address, const T& value)
    {
        return DataPatch(address, &value, sizeof(T));
    }

    uintptr_t Address() const;
    size_t Size() const;
    bool IsApplied() const;

    bool Apply();
    bool Restore();

private:
    uintptr_t m_address = 0;
    std::vector<uint8_t> m_patchBytes;
    std::vector<uint8_t> m_originalBytes;
    bool m_applied = false;
};

} // namespace sh4xe::sdk
