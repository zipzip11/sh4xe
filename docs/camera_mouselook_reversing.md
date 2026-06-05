# Silent Hill 4 Camera Mouse Hook Notes

Reverse-engineered from the GOG `SILENT HILL 4.exe` (fixed base `0x00400000`,
no ASLR; all addresses absolute), with PS2 E3 prototype symbols used only as
cross-reference for camera naming and constants.

## The important correction

There are two look-input camera models in the PC executable:

- `Camera_UpdateFollow` (`0x00503EA0`) is a free-look orbit camera. It multiplies
  `Camera_GetLookInputH/V` by 30 degrees yaw and 8 degrees pitch, then writes the
  rotated camera position back so the next frame starts from the moved position.
  For that mode, look input behaves like an angular rate.
- Normal exploration usually does **not** use that handler. The per-frame camera
  tick `sub_5025D0` falls back to `DfltOutDoor`, whose handler is `sub_5033C0`.
  That handler rebuilds from the player-following/master camera state each frame,
  then applies `Camera_GetLookInputH/V` as a bounded chase-camera deflection:

```c
yaw   = Camera_GetLookInputH(0) * 0.27925268;   // 16 degrees
pitch = Camera_GetLookInputV(0) * -0.2617994;   // 15 degrees
sub_4FEDC0(..., yaw, pitch);
```

The old mouse hook treated every camera as the first model. Runtime logs proved
the DirectInput path, hooks, and camera gates were all working, but the visible
camera did not react because the active chase camera only saw a one-frame mouse
delta. As soon as the hook cleared that delta at `EndScene`, the deflection
collapsed back to zero.

## Input plumbing

`Camera_GetLookInputH/V` (`0x004F86B0` / `0x004F8700`) perform the camera gates
and forced-look checks, then call `Input_GetRightStickX/Y`
(`0x00554350` / `0x00554370`). Hooking the right-stick getters preserves the
game's own camera gating while bypassing the physical stick deadzone.

The detours also filter by return address so the mouse value is only injected
when the caller is `Camera_GetLookInputH/V`. Other direct right-stick consumers
receive the stock gamepad value.

## Hook model

The current hook does **not** emulate a held right stick. That was visible, but
felt wrong because it fought the chase camera's own search-view spring.

Instead, DirectInput mouse deltas are accumulated until the game refreshes its
camera/player state in `sub_532990` (`0x00532990`). The detour calls the original
state refresh, then consumes the pending horizontal mouse delta and adds it to:

- `flt_10A0FC4` (`0x010A0FC4`), the persistent/master camera yaw.
- `flt_10A1BF4` (`0x010A1BF4`), the working yaw already copied out for this
  frame.

`camera sens` is still expressed in degrees per raw mouse count. The hook clamps
a single update to 45 degrees as a focus-regain spike guard, normalizes the yaw to
`[-pi, pi]`, and leaves `Camera_GetLookInputH/V` returning the stock values. The
right-stick getter detours remain installed only for diagnostics.

This makes horizontal mouse movement persist in the same camera state that
`DfltOutDoor` rebuilds from, avoiding the old one-frame/invisible path and the
later fake-stick springiness.
