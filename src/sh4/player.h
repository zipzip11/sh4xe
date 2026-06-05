#pragma once

#include <cstddef>
#include <cstdint>

namespace sh4xe::sh4::player
{

inline constexpr int kHenryPlayerIndex = 0;

// The player blocks are addressed as dword_10A0F20 + player * 0x420 in
// collision/movement helpers. Only the offsets that have been observed in IDA
// are named here; leave the rest opaque until their ownership is proven.
namespace state_offset
{
inline constexpr std::size_t kStride = 0x420;
inline constexpr std::size_t kPosition = 0x40;
inline constexpr std::size_t kPreviousPosition = 0x70;
inline constexpr std::size_t kActionOrAnimationState = 0x214;
inline constexpr std::size_t kLiveModelHandle = 0x220;
inline constexpr std::size_t kControlState = 0x228;
inline constexpr std::size_t kControlAux = 0x230;
} // namespace state_offset

struct Vec4
{
    float x;
    float y;
    float z;
    float w;
};

static_assert(sizeof(Vec4) == 0x10, "SH4 vectors are four floats");

// Entries consumed by Player_AccumulateContactImpact (0x0053F4E0). The function
// iterates kPlayerContactCount entries at kPlayerContactList, stride 0x30, and
// adds the float at +0x04 for valid entries. The validity/owner fields are still
// intentionally unnamed.
struct ContactEntryPartial
{
    std::uint8_t unknown00[0x04];
    float impact;
    std::uint8_t unknown08[0x28];
};

static_assert(sizeof(ContactEntryPartial) == 0x30, "player contact entries are 48 bytes");

} // namespace sh4xe::sh4::player
