#pragma once

// PARKED / EXPERIMENTAL -- not compiled into the shipping build and not wired into
// the bootstrap. See docs/freecam_design.md. This is reference scaffolding for a
// free camera: the input/state/integration half is complete, but the engine
// camera *orientation* (view-matrix) sink still needs to be located and validated
// in-engine before this can free-look in every camera mode. As shipped here it
// can only translate the apartment/first-person camera (verified position
// globals). Do not enable without reading the design note.

namespace sh4xe::hooks::freecam
{

// Installs a hook on the camera-decide function so the override runs after the
// engine updates the camera but before the scene is rendered. Returns false if
// the hook could not be installed. Off by default after install.
bool InstallFreecamHook();

void SetEnabled(bool enabled);
bool Enabled();
void Toggle();

// Per-tick driver (input sampling + integration + override). Normally called from
// the camera-decide detour installed by InstallFreecamHook(); exposed for testing
// or for driving from an alternate tick.
void Tick();

} // namespace sh4xe::hooks::freecam
