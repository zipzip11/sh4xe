#pragma once

namespace sh4xe::hooks::fps
{

bool InstallFpsUnlockHook();

// Re-asserts the frame-timing globals from inside the game thread. Must be
// called once per presented frame (the D3D8 EndScene detour drives this). This
// is what keeps the scheduler interval and the simulation delta-time coupled so
// the game can never run at the wrong speed -- see fps_unlock_hook.cpp for the
// full rationale.
void OnFramePresented();

void SetFpsUnlockEnabled(bool enabled);
bool FpsUnlockEnabled();

void SetTargetFps(int fps);
int TargetFps();
double TargetFrameSeconds();

// Most recently measured simulation rate (updates/sec) derived from the game's
// own per-step frame counter. 0 until enough frames have elapsed to measure.
int MeasuredSimFps();

} // namespace sh4xe::hooks::fps
