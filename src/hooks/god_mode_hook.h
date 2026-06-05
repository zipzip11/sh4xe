#pragma once

#include <cstddef>

namespace sh4xe::hooks::god_mode
{

bool InstallGodModeHook();
void OnFramePresented();

void SetGodModeEnabled(bool enabled);
bool GodModeEnabled();

unsigned int SuppressedImpactCount();
void DescribeGodMode(char* out, std::size_t outSize);

} // namespace sh4xe::hooks::god_mode
