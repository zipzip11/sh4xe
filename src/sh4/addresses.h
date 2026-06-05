#pragma once

#include <cstdint>

namespace sh4xe::sh4::addr
{

// Global where the game stores its IDirect3DDevice8* after CreateDevice
// succeeds (see graphics init sub_402F20: &dword_6E7D94 is the returned-device
// out-param). The EXE has a fixed base (0x00400000, no ASLR), so this absolute
// address is stable. We poll it to hook the device without racing startup.
inline constexpr uintptr_t kDirect3DDevice8Ptr = 0x006E7D94;

// int __cdecl Camera_SetFovRadians(Camera* camera, float vfov)
// The lowest-level vertical-FOV setter. It clamps vfov to
// [0.0099999998, 3.1315928] rad (~0.57deg .. ~179.4deg), stores it at
// camera+0x40, and marks the projection dirty (camera+0x280 |= 1) so
// Camera_BuildProjectionMatrix (0x0041CA90) recomputes it. Both the one-time
// camera init (InitMainCamera @ 0x0055BD10, which seeds the 35deg default) and
// the per-scene SetCurrentCameraFov (0x0055BE50) funnel through here, so it is
// the single choke point for overriding FOV at runtime.
inline constexpr uintptr_t kCameraSetFovRadians = 0x0041C990;

// Default vertical FOV the game seeds at camera init: 0x3F1C61AA float bits,
// i.e. 0.61086524 rad == 35.0 degrees (push @ 0x0055BD87).
inline constexpr float kDefaultVerticalFovRadians = 0.61086524f;

// Central frame-rate helpers and scheduler globals. The main loop (WinMain @
// 0x00414A50) is a real-time-gated FIXED-timestep scheduler: it runs exactly one
// update+render step per kMainLoopFrameIntervalTicks of QueryPerformanceCounter
// time, advancing a deadline by that interval each step. The simulation is almost
// entirely delta-time driven through Game_GetFrameSeconds (player sub_53C250,
// enemies sub_545160, Camera_UpdateFollow all do `value * GetFrameSeconds()`), so
// correct-speed 60 FPS requires moving the scheduler interval and the delta-time
// TOGETHER: interval -> QPF/60 AND frame rate -> 60 (so seconds -> 1/60). Bumping
// only the scheduler doubles game speed -- that decoupling is the speedup bug.
// fps_unlock_hook.cpp enforces the coupling by deriving every value below from a
// single fps and re-asserting them once per presented frame.
// - Game_GetFrameRate returns dword_5D5248, hardcoded to 30 during D3D init
//   (sub_402F20 @ 0x00403169); never refresh-rate dependent.
// - Game_GetFrameSeconds returns 1.0 / dword_5D5248.
// - MainLoopFrameIntervalTicks is QueryPerformanceFrequency / 30, seeded once in
//   WinMain and not otherwise touched at runtime.
inline constexpr uintptr_t kGameFrameRateValue = 0x005D5248;
inline constexpr uintptr_t kGameGetFrameRate = 0x0055AE50;
inline constexpr uintptr_t kGameGetFrameSeconds = 0x0055AE60;
inline constexpr uintptr_t kMainLoopFrameIntervalTicks = 0x00610DDC;

// Per-step simulation frame counter: incremented once per update inside
// Game_UpdateWorld (sub_42D1A0 @ 0x0042D1B4). A few cosmetic effects key off it
// with `(counter & mask) == 0`, so they spawn twice as often at 60 FPS (harmless).
// We sample it only to report the achieved simulation rate. The engine resets it
// to 0 on some state transitions (sub_42CCD0).
inline constexpr uintptr_t kSimFrameCounter = 0x013DC324;

// Render/animation subsystem frame constants written by SetRenderFrameRate
// (sub_417DF0). The stock startup path calls it with 30.0f.
inline constexpr uintptr_t kRenderFrameRateFloat = 0x00ED57D4;
inline constexpr uintptr_t kRenderFrameSecondsFloat = 0x00ED57D8;
inline constexpr uintptr_t kRenderFrameStepTicks = 0x00ED57DC;

// Procedural camera "head bob" / walk-sway. The active camera object stores a bob
// PHASE ANGLE at camera+0x84; the walk-camera position updaters advance it once
// per simulation step with a FIXED increment that does NOT route through
// Game_GetFrameSeconds:
//     *(float*)(camera+0x84) += move_speed * STEP
// where STEP is the inlined stock frame time -- 1/30 for two camera modes
// (sub_534590 / sub_534620) and 1/15 for a third walk path (sub_52D710). sin()/
// cos() of that phase then bob/sway the camera, and a footstep SFX fires on each
// wrap, so at 60 steps/sec the whole bob (and its footsteps) run at 2x.
//
// We cannot rescale the shared 1/30 (flt_5B7E80) and 1/15 (flt_5B7E88) constants:
// they are also read elsewhere as real-time PERIODS accumulated against
// Game_GetFrameSeconds (e.g. sub_469960, sub_50A5A0), which are already fps-correct
// and would break. Instead the fps unlock repoints just these three `fmul dword
// ptr [const]` operands to DLL-owned floats held at the matching per-frame step:
// (1/30)*30/fps = 1/fps and (1/15)*30/fps = 2/fps (== 1/30 and 1/15 at the stock
// 30, so the redirect is a no-op when the unlock is off). Each instruction is the
// 6-byte `fmul m32fp` (D8 0D <abs32>); the operand to repoint is the 4 bytes at
// the instruction address + kCamBobIncrementDispOffset.
inline constexpr uintptr_t kCamBobIncrementSite30A = 0x005345A9; // sub_534590 (camera mode 0)
inline constexpr uintptr_t kCamBobIncrementSite30B = 0x00534647; // sub_534620 (camera mode 2)
inline constexpr uintptr_t kCamBobIncrementSite15 = 0x0052D863;  // sub_52D710 (walk path)
inline constexpr uintptr_t kCamLookPitchIncrementSite15 = 0x0052DC71; // sub_52DBD0 gamepad look pitch
inline constexpr uintptr_t kCamBobIncrementConst30 = 0x005B7E80; // shared 1/30, expected operand
inline constexpr uintptr_t kCamBobIncrementConst15 = 0x005B7E88; // shared 1/15, expected operand
inline constexpr unsigned int kCamBobIncrementDispOffset = 2;    // operand offset within the fmul

// The VERTICAL head bob proper -- the PS2 `cam3GetShakeHeight` (game_camera_3ldk.c),
// ported to sub_4FFC40 and fed into sub_5009D0 (cam3DecidePosition) -> cam height.
// This is a SEPARATE phase from the horizontal sway above: it owns its own phase
// accumulator flt_1083544 (used nowhere else) and advances it once per simulation
// step with NO Game_GetFrameSeconds term:
//     flt_1083544 += clamp(|moved_speed|/250) * (PI/15);   // fmul dword ptr [flt_5BF7A4]
// sin(flt_1083544) becomes the up/down camera offset, and a footstep SFX
// (sub_56C9B0(0x9C51, ...)) fires on each +/-PI wrap. moved_speed is a genuine
// velocity -- Camera_SyncPlayerCameraState computes it as (deltaPos / GetFrameSeconds),
// so it is fps-INDEPENDENT; with PI/15 inlined, the whole bob (and footsteps) runs at
// 2x at 60 steps/sec. This is the "walking on your toes" bob the user reported, and it
// is NOT one of the camera+0x84 sway sites above, so the existing redirect never
// reached it. We repoint just this fmul operand at a DLL float held at the matching
// per-frame step (PI/15)*30/fps (== PI/15 at the stock 30, so inert when off).
//
// The PI/15 constant flt_5BF7A4 is SHARED: Camera_UpdateFollow (@0x504197) reuses it
// as a camera catch-up ANGLE floor (radians), already slewed by Game_GetFrameSeconds,
// so it must be left alone -- hence operand redirect here, never a constant rewrite.
inline constexpr uintptr_t kCamBobShakeIncrementSite = 0x004FFCDB;  // sub_4FFC40 vertical head bob
inline constexpr uintptr_t kCamBobShakeIncrementConst = 0x005BF7A4; // shared PI/15, expected operand

// Startup intro sequence:
// - Startup_RunOpeningSplash loads snap_opening.bin and fades the static
//   logo/legal splash screens.
// - Startup_PlayBootMovie plays movie\m_00.pac, the blocking boot intro reel.
// - TaskAdvanceState is the small task-state helper both routines use when
//   their part of the startup sequence has completed.
inline constexpr uintptr_t kStartupRunOpeningSplash = 0x00519160;
inline constexpr uintptr_t kStartupPlayBootMovie = 0x005211B0;
inline constexpr uintptr_t kTaskAdvanceState = 0x00555C80;

} // namespace sh4xe::sh4::addr
