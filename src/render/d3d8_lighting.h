#pragma once

namespace sh4xe::render::d3d8_lighting
{

// Draws the optional DX8 fixed-function lighting grade over the completed
// scene. The pass is state-blocked and safe to call from EndScene before the
// overlay console.
void Render(void* device);

} // namespace sh4xe::render::d3d8_lighting
