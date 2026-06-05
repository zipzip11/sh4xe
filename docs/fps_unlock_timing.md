# Silent Hill 4 Frame-Timing Model and the FPS Unlock

Reverse-engineered from the GOG `SILENT HILL 4.exe` (fixed base `0x00400000`,
no ASLR). All addresses are absolute.

## The main loop is a real-time-gated fixed-timestep scheduler

`WinMain_MainLoop` (`0x00414A50`) drives the game with a deadline scheduler:

```c
// once at startup:
g_mainLoopFrameIntervalTicks = QueryPerformanceFrequency / 30;   // 0x00610DDC

// every loop iteration:
QueryPerformanceCounter(&now);                 // 0x13DC390
if (now <= deadline /*0xECB330*/) continue;    // too early -> just pump messages
    ... one update + render step ...
v17 = g_mainLoopFrameIntervalTicks;            // read @ 0x0041521C
deadline += v17;                               // advance by one frame interval
if (deadline < now) deadline = now + v17;      // catch-up, no spiral
```

So the engine runs **exactly one simulation+render step per
`g_mainLoopFrameIntervalTicks` of real time**. Stock that interval is `QPF/30`,
i.e. **30 steps per second**. Each step advances the world by "one frame."

The render is performed *inside* the step (the device present and scene build are
in the inner loop), so render rate is locked to simulation rate.

## The simulation is delta-time driven

`g_frameRate` (`0x005D5248`) is hardcoded to `30` in `Graphics_InitDevice`
(`0x00403169`) — it is **not** tied to the monitor refresh rate. Two helpers
expose it:

- `Game_GetFrameRate` (`0x0055AE50`) → `g_frameRate`
- `Game_GetFrameSeconds` (`0x0055AE60`) → `1.0 / g_frameRate`

`Game_GetFrameSeconds` has **hundreds** of callers across the gameplay code, and
spot-checking the core systems shows they all scale motion by it:

| System | Function | Idiom |
| --- | --- | --- |
| Player locomotion | `sub_53C250` | `pos += GetFrameSeconds() * speed * dir` |
| Enemies / characters | `sub_545160` | `vel *= GetFrameSeconds()*600`, `timer -= GetFrameSeconds()` |
| Camera follow | `Camera_UpdateFollow` (`0x00503EA0`) | slew clamped to `GetFrameSeconds() * angle` |
| Animation clock | `SetRenderFrameRate` (`0x00417DF0`) | per-frame step = `(1/rate) * 7680` ticks |

The only genuinely fixed-step logic found is cosmetic: particle/decal spawns
gated by `(g_simFrameCounter & mask) == 0` (e.g. `sub_53C250`, `sub_545160`).
These spawn twice as often at 60 fps, which is harmless.

`g_simFrameCounter` (`0x013DC324`) is bumped once per step in `Game_UpdateWorld`
(`0x0042D1B4`).

## Why naive unlocking doubles game speed

Real-time game speed is `(steps per second) × (game-seconds per step)`:

- Stock: `30 × (1/30) = 1.0` ✓
- Scheduler only bumped to 60, delta-time left at 1/30: `60 × (1/30) = 2.0` ✗ (2× fast)
- Scheduler **and** delta-time at 60: `60 × (1/60) = 1.0` ✓

Because the simulation is delta-time driven, correct-speed 60 fps is achievable —
but only if the scheduler interval and the delta-time sources move **together**.
If they ever decouple (even briefly), the whole game runs fast. The earlier
implementation applied these values from a 250 ms background thread with no
coupling guarantee, so the startup window (where `Graphics_InitDevice` writes
`g_frameRate = 30` and `WinMain` writes `g_mainLoopFrameIntervalTicks = QPF/30`
*after* the DLL loads) and any re-init could leave the scheduler at 60 while the
delta-time fell back to 1/30.

## The fix (`src/hooks/fps_unlock_hook.cpp`)

All timing globals are derived from a single `fps` value and re-asserted as one
coherent set, **once per presented frame**, from inside the game thread (driven
by the D3D8 `EndScene` detour via `OnFramePresented`):

| Global | Address | Value at `fps` |
| --- | --- | --- |
| `g_frameRate` | `0x005D5248` | `fps` |
| `g_mainLoopFrameIntervalTicks` | `0x00610DDC` | `QPF / fps` |
| `g_renderFrameRate` | `0x00ED57D4` | `(float)fps` |
| `g_renderFrameSeconds` | `0x00ED57D8` | `(float)(1/fps)` |
| `g_animFrameStepTicks` | `0x00ED57DC` | `(int)((1/fps) * 7680)` |

`Game_GetFrameRate` / `Game_GetFrameSeconds` are also detoured so callers get the
target values immediately, even in the single frame before a drifted global is
corrected.

Properties:

- **Coupled by construction** — interval and delta-time always come from the same
  `fps`, so `steps/sec × sec/step == 1.0` can never break.
- **Race-free** — re-asserted in the game thread every frame; a value the engine
  resets is corrected within one frame. Writes only happen on an actual mismatch,
  so the steady-state cost is a few reads per frame.
- **Self-reporting** — `MeasuredSimFps()` samples `g_simFrameCounter` deltas to
  report the achieved simulation rate; the console `fps` command prints it
  ("measured sim N/s"). It should read ~60 with the unlock on.
- **Safe fallback** — set the target to 30 (console: `fps 30` / `fps off`) to
  restore exact stock timing if any specific scene misbehaves at 60.

A low-frequency watcher thread (500 ms) re-applies the same state as a safety net
for the window before the D3D device — and therefore the `EndScene` tick — exists.

## Procedural camera head-bob / walk-sway (fixed-step — corrected)

There are **two distinct** fixed-step procedural-camera systems, each with its own
phase accumulator. Neither routes through `Game_GetFrameSeconds`, so the per-frame
re-assertion above cannot reach them; both advance once per simulation step by a
*fixed* increment with the stock frame time (or `π/15`) inlined, and so both run at
**2× at 60 steps/sec** unless corrected.

**1. Horizontal walk-sway** — the active camera object holds a sway **phase angle**
at `camera+0x84`; the camera-mode position updaters advance it and feed
`sin()/cos()` into the camera *x/z* offset:

```c
// sub_534590 (mode 0) @ 0x5345A9, sub_534620 (mode 2) @ 0x534647 : STEP = 1/30
// sub_52D710 (walk path)      @ 0x52D863                         : STEP = 1/15
*(float*)(camera + 0x84) += move_speed * STEP;   // fmul dword ptr [const]
```

**2. Vertical head-bob (the one you *feel*)** — the PS2 `cam3GetShakeHeight`
(`game_camera_3ldk.c`), ported to `sub_4FFC40` and consumed by `sub_5009D0`
(`cam3DecidePosition`) as the camera height. It owns a **separate** phase
accumulator `flt_1083544` (used nowhere else) and a separate constant `π/15`:

```c
// sub_4FFC40 @ 0x4FFCDB : STEP = π/15 (flt_5BF7A4)
flt_1083544 += clamp(|moved_speed| / 250) * (π/15);   // fmul dword ptr [flt_5BF7A4]
return sinf(flt_1083544);                              // → vertical cam height
```

`moved_speed` is a genuine **velocity** — `Camera_SyncPlayerCameraState` computes it
as `(deltaPos / Game_GetFrameSeconds)`, which is fps-independent — so with `π/15`
inlined the vertical bob (and its footstep SFX `sub_56C9B0(0x9C51,…)` on each π wrap)
runs at exactly 2× at 60. This is the **"walking on your toes"** symptom. It is
gated by the "Head Motion" option (full / half / off). It was previously missed
because it is *not* one of the `camera+0x84` sway sites and uses a different phase
and constant.

The `1/30` (`flt_5B7E80`), `1/15` (`flt_5B7E88`) and `π/15` (`flt_5BF7A4`) constants
are all **shared** with real-time-period uses elsewhere (e.g. `flt_5BF7A4` is reused
in `Camera_UpdateFollow @0x504197` as a camera catch-up *angle* floor, already slewed
by `Game_GetFrameSeconds`), so they cannot be rescaled in place — see the audit
below. Instead the unlock repoints just these `fmul` operands at DLL-owned floats
(`g_bobIncrementStep30/15`, `g_bobShakeIncrement` in `fps_unlock_hook.cpp`) held at
the matching per-frame step, updated every frame from the same `fps`:

| Site STEP | Held value at `fps` | At stock 30 |
| --- | --- | --- |
| 1/30 | `1/fps` | `1/30` (no-op) |
| 1/15 | `2/fps` | `1/15` (no-op) |
| π/15 | `(π/15)·30/fps` | `π/15` (no-op) |

Because the held value equals the original constant at 30, the redirect is inert
when the unlock is off (`fps 30` / `fps off`). The operands are rewritten once, from
the game thread (EndScene), after verifying they still match the expected constant
address (so a different build is skipped rather than corrupted). The startup log
line reports the result, e.g.
`camera fps fix: … (sway 1/30=1,1 sway 1/15=1 look 1/15=1 bob π/15=1)`.

## Audit: baked frame-time constants — period vs. increment

Beyond `Game_GetFrameSeconds`, the engine inlines the stock frame time
`0.033333335` (1/30) and `0.06666667` (1/15) in two **different** idioms. The
distinction is what makes a blanket constant patch wrong:

- **Period / threshold (already fps-correct, leave alone):** a real-time accumulator
  built from the fps-aware delta is compared against the constant *as a duration in
  seconds* — e.g. `t += GetFrameSeconds(); if (t >= 1/30) ...` (`sub_469960`), or a
  30 fps frame count rescaled by `Game_GetFrameRate()/30` (`sub_50A5A0`). Same
  wall-clock rate at any fps.
- **Per-step increment (2× at 60, needs scaling):** the constant *is* the per-frame
  advance, `x += rate * (1/30)` — the head-bob sites above.

The general animation timestep is fine: `flt_614FF0 = 1/Game_GetFrameRate()` is
recomputed every active frame (`sub_45E830(1)`), so the hundreds of model/animation
callers of `sub_45E890()` already track the unlocked fps through our
`Game_GetFrameRate` detour.

One related increment site is **not** part of the bob and is deliberately left
alone: `sub_52DBD0` (`@0x52DC77`) integrates analog-stick camera *look* pitch by
`look_input * (1/15)` per frame, i.e. gamepad free-look pitches ~2× faster at 60.
Mouse-look already bypasses this path (see `camera_responsiveness_hook`); revisit
only if gamepad look speed needs an fps-independent rate.

## Known limitation

Cosmetic effects keyed to `(g_simFrameCounter & mask)` run at the simulation rate
and so emit at ~2× density at 60 fps. This is intentional to leave alone:
remapping the counter would desynchronise the state-machine timing that also
reads it. It is visual only and generally looks better, not worse.
