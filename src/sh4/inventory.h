#pragma once

namespace sh4xe::sh4::inventory
{

struct ResourceAnchor
{
    const char* name;
    const char* note;
};

// Runtime inventory slots are not mapped yet. These anchors are the known item
// UI/model resources found so far and are kept separate from the player contact
// arrays, which are damage/impact structures rather than inventory.
inline constexpr ResourceAnchor kKnownResourceAnchors[] = {
    {"item_model.bin", "item model resource bundle"},
    {"item_model2.bin", "secondary item model resource bundle"},
    {"item_l.bin", "item UI/list resource bundle"},
    {"message_item_msg_*.bin", "localized item text resources"},
};

} // namespace sh4xe::sh4::inventory
