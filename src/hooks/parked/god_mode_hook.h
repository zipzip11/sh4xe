#pragma once

#include <cstddef>

// PARKED: experimental damage suppression. Not part of the active build unless
// it is intentionally moved back out of src/hooks/parked.

namespace sh4xe::hooks::god_mode
{

bool InstallGodModeHook();
void OnFramePresented();

void SetGodModeEnabled(bool enabled);
bool GodModeEnabled();

unsigned int SuppressedImpactCount();
void DescribeGodMode(char* out, std::size_t outSize);

} // namespace sh4xe::hooks::god_mode
