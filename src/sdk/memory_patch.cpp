#include "sdk/memory_patch.h"

#include "sdk/memory.h"

namespace sh4xe::sdk
{

DataPatch::DataPatch(uintptr_t address, const void* data, size_t size) : m_address(address)
{
    if (data && size)
    {
        const auto* bytes = static_cast<const uint8_t*>(data);
        m_patchBytes.assign(bytes, bytes + size);
    }
}

uintptr_t DataPatch::Address() const
{
    return m_address;
}

size_t DataPatch::Size() const
{
    return m_patchBytes.size();
}

bool DataPatch::IsApplied() const
{
    return m_applied;
}

bool DataPatch::Apply()
{
    if (m_applied)
        return true;

    if (!m_address || m_patchBytes.empty())
        return false;

    m_originalBytes.resize(m_patchBytes.size());
    if (!ReadBytes(m_address, m_originalBytes.data(), m_originalBytes.size()))
        return false;

    if (!WriteBytes(m_address, m_patchBytes.data(), m_patchBytes.size()))
        return false;

    m_applied = true;
    return true;
}

bool DataPatch::Restore()
{
    if (!m_applied)
        return true;

    if (m_originalBytes.empty())
        return false;

    if (!WriteBytes(m_address, m_originalBytes.data(), m_originalBytes.size()))
        return false;

    m_applied = false;
    return true;
}

} // namespace sh4xe::sdk
