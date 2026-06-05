# Freecam: investigation and design

Status: **investigated; parked scaffold only** (`src/hooks/parked/freecam_hook.*`).
Not wired into the shipping build. Needs in-engine validation before it can be
promoted to an active, opt-in feature.

## Is there a built-in / disabled debug camera?

No usable one.

- The PS2 E3 trial (`SLUS_208.73`, Ghidra) has debug subsystems, but they are
  **Phenom** (enemy/creature) debug and **sound** debug (`PhenomDebug`,
  `sfDebugRead`, `SetDebugHandler`) -- no free-fly / fly-cam toggle.
- `draw_fps_cam` looks promising by name but is a **red herring**: it is the
  key-config UI for the "camera reverse" option (`misc_keyconf.c`), nothing to do
  with a first-person/fly camera.
- The Windows GOG build (IDA) has no disabled camera mode that just needs a flag
  flipped. Camera mode lives in `dword_FD5A90` (values 1/2/9/10 = the normal
  follow / apartment / event cameras); there is no spare "free" mode.
- There **are** camera-override primitives that a freecam could borrow:
  `CameraEngine_SetBypassCamera(pos, watch, ang)` (sets `ceW.BypassCamUseFlag`
  and a forced view matrix, otherworld camera) and `Camera3ldkForcePos` /
  `cam3SetLookAt` (apartment). The PC FOV choke point `Camera_SetFovRadians`
  (`0x0041C990`) is already hooked.

## Where must a freecam override the camera? (key finding)

**Not at Direct3D.** SH4 is a PS2 port and keeps the PS2's software/VU transform
model: it builds its own camera matrices on the CPU and does not drive Direct3D's
fixed-function view matrix.

Evidence in the GOG EXE:

- There is exactly **one** `call dword ptr [eax+94h]` (vtable index 37 =
  `IDirect3DDevice8::SetTransform`), at `0x5AF8C1` in `sub_5AF8B0`, and it is
  called with a **single** argument and followed by a stub (`sub_5B23C0`
  = `return 0`). `SetTransform(State, pMatrix)` takes two args, so this is not a
  per-frame view/projection setter. There are **no** `SetTransform(D3DTS_VIEW)`
  call sites.
- The camera code computes matrices directly: the apartment camera
  (`game_camera_3ldk.c`) builds `cam3_work.mtx` via `cam3Rotate`/`cam3Move` and
  hands it to `GameCameraMatrixSet`; the otherworld camera builds
  `ceW.View2WldMat` in `CameraEngine_CalcCameraMatrix`.

**Consequence:** hooking `IDirect3DDevice8::SetTransform` would do nothing. The
override has to happen at the engine's camera-matrix sink, which differs per
camera mode.

## Override points (PC)

| Camera mode (`dword_FD5A90`) | System | Decided output | Override seam |
| --- | --- | --- | --- |
| apartment / first-person | `sub_5009D0` (= `cam3DecidePosition`) | position globals `0x1083500` (x), `0x1083504`/`flt_1083540` (y), `0x1083508` (z), `0x108350C` (w=1) | hook `sub_5009D0`'s exit; for free-look also override the rotation fed to the `cam3Rotate` equivalent |
| follow / otherworld | `Camera_UpdateFollow` (`0x503EA0`) + the `CameraEngine` (`ceW.*`) | `ceW.View2WldMat`, `ceW.CamPos`, `ceW.ViewVec` | drive `BypassCamUseFlag` + `BypassView2WldMat` (PS2 `CameraEngine_SetBypassCamera`) once the PC equivalents are located |

Position-only override is easy (write the `0x1083500` block after the camera
update). **Free *look*** is the hard part: orientation comes from the per-mode
matrix build, so a full freecam needs to locate and write the PC camera
view/world matrix sink (the PC equivalent of `GameCameraMatrixSet` /
`sfCameraSetViewWorldMatrix`). That is the open RE item.

## Design (parked scaffold)

`src/hooks/parked/freecam_hook.*` implements the platform-independent half so the
RE seam is the only thing left:

- **State & input** (`GetAsyncKeyState`, no DirectInput dependency): WASD + R/F
  for up/down, arrow keys for yaw/pitch, Shift/Ctrl for speed, a toggle key.
  Integrates a position and a yaw/pitch each tick; builds a forward/right basis.
- **Freeze:** while active, suppress player locomotion so the body does not walk
  off (write the player move-speed / input to zero, or gate the player update).
- **`ApplyOverride()` seam:** currently writes the verified apartment-cam
  **position** globals (`0x1083500` block). Orientation and the otherworld path
  are explicit `TODO`s pending the matrix-sink RE above.
- **Timing:** must run *after* the camera update and *before* render -- i.e. from
  a hook on the camera-decide function (`sub_5009D0`), **not** from the D3D8
  `EndScene` tick (that is after the scene is already built).

## Why it is parked, not shipped

- The orientation/matrix sink is not yet validated, so the current scaffold can
  only translate the apartment camera, not free-look in all modes.
- It cannot be exercised in this environment (no in-engine run), and shipping an
  unvalidated camera hook risks the working build.

## To promote it to an opt-in feature

1. Locate the PC camera view/world-matrix sink (correlate PS2
   `GameCameraMatrixSet` / `sfCameraSetViewWorldMatrix` with
   `tools/sdk/autocorrelate.py`, then confirm in IDA).
2. Hook the camera-decide / matrix-set function; override pos+orientation there.
3. Add a `freecam` console command (mirror the existing `fov`/`fps` commands) and
   compile the module into the build (add it to `sh4xe.vcxproj`, move out of
   `parked/`).
4. Validate in-engine in both an apartment room and an otherworld stage.
