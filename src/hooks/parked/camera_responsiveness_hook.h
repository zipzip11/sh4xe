#pragma once

namespace sh4xe::hooks::camera
{

bool InstallCameraResponsivenessHook();

// Adds one relative mouse sample, in DirectInput mouse units, to the pending
// persistent camera-yaw adjustment consumed by the camera state hook.
void AddMouseDelta(long dx, long dy);

// Clears one-frame diagnostic samples. Must be called once per presented frame
// (the D3D8 EndScene detour drives this).
void OnFramePresented();

void SetResponsiveMouseEnabled(bool enabled);
bool ResponsiveMouseEnabled();

// Persistent yaw degrees per raw mouse count. Clamped internally.
void SetMouseSensitivity(float sensitivity);
float MouseSensitivity();

void SetMouseInvertY(bool invert);
bool MouseInvertY();

// Diagnostic: when on, logs mouse deltas, yaw deltas, and camera gates to
// sh4xe.log. Rate-limited. Console: camera debug.
void SetMouseDebug(bool enabled);
bool MouseDebug();

void ResetMouseLook();
float CurrentMouseLookX();
float CurrentMouseLookY();
bool MouseLookActive();

} // namespace sh4xe::hooks::camera
