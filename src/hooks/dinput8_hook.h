#pragma once

namespace sh4xe::hooks::dinput8
{

// Hooks DirectInput8 so that, while the overlay console is open, the game's
// keyboard/gamepad reads return empty state. Installed via a probe device whose
// shared vtable also covers the game's real devices, so it does not race startup.
bool InstallInputHook();

} // namespace sh4xe::hooks::dinput8
