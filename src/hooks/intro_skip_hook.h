#pragma once

namespace sh4xe::hooks::intro
{

bool InstallIntroSkipHook();

void SetIntroSkipEnabled(bool enabled);
bool IntroSkipEnabled();

} // namespace sh4xe::hooks::intro
