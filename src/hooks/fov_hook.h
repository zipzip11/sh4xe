#pragma once

namespace sh4xe::hooks::fov
{

// Hooks the game's vertical-FOV setter (Camera_SetFovRadians @ 0x0041C990) so
// every FOV the game applies is multiplied by a user-controlled scale. The
// game re-applies FOV per scene/cutscene, so a persistent multiplier (rather
// than a one-shot absolute write) keeps the override in effect while preserving
// each scene's relative framing.
bool InstallFovHook();

// Multiplier applied to the game's vertical FOV. 1.0 = unchanged. The detour
// and the underlying game function both clamp the resulting angle, so callers
// only need a sane range here.
void SetFovScale(float scale);
float GetFovScale();

// Most recent base (unscaled) vertical FOV the game requested, in radians.
// 0 until the game sets FOV at least once. Useful for reporting the effective
// FOV back to the console.
float LastBaseFovRadians();

} // namespace sh4xe::hooks::fov
