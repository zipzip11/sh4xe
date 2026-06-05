# Graphics / Post-FX Reversing Notes

## sh4xe overlay grade

The first suspected "flare/filter" layer was our own `d3d8_lighting` pass, not
the stock game binary. It runs from the `IDirect3DDevice8::EndScene` detour after
the game has finished drawing and before the proxy console draws.

Relevant proxy path:

- `hooks::d3d8::EndSceneDetour`
- `render::d3d8_lighting::Render`
- `render::tweaks::LightingScreenWarmth`
- `render::tweaks::LightingVignette`

The grade path draws screen-space `XYZRHW | DIFFUSE` triangles with no texture:

- warm center glow: `SRCALPHA / ONE` additive blend
- dark edge vignette: `SRCALPHA / INVSRCALPHA` blend

As of this note, both are defaulted to `0.0` so the full-screen overlay is
opt-in. The fixed-function light/material tweaks can still remain enabled.

Console checks:

- `lighting` reports the current boost/grade/vignette values.
- `lighting grade 0` disables the warm additive overlay.
- `lighting vignette 0` disables the edge darkening overlay.
- `lighting off` disables all sh4xe lighting tweaks, including material/light
  boosts.

## Stock D3D8 startup

IDA name:

- `Graphics_InitD3D8Device` at `0x00402F20`
- `Graphics_InitializeDeviceState` at `0x004026D0`
- `Graphics_RecreateSceneRenderTargets` at `0x00403440`
- `Graphics_InitializeRenderTargets` at `0x004034F0`
- `D3D8_CreateTextureTracked` at `0x00417480`

Useful notes:

- `Direct3DCreate8(220)` is called at `0x00402F66`.
- `IDirect3D8::CreateDevice` is called at `0x00403183`.
- The returned `IDirect3DDevice8*` is stored through global `0x006E7D94`, which
  sh4xe polls to install render hooks without racing startup.
- The same graphics init path seeds `g_frameRate` to `30` at `0x00403169`.
- `Graphics_InitializeDeviceState` seeds stock render states and texture-stage
  states. Notable original writes:
  - `0x00402816`: disables `D3DRS_DITHERENABLE`.
  - `0x004028A0`: disables `D3DRS_FOGENABLE` during baseline init.
  - `0x00402B3D`: sets stage-0 `MAGFILTER` to `D3DTEXF_LINEAR`.
  - `0x00402B51`: sets stage-0 `MINFILTER` to `D3DTEXF_LINEAR`.

If the visible artifact persists after disabling the sh4xe grade/vignette, the
next reversing target is stock full-screen draws: hook/log `DrawPrimitive*`,
`SetTexture`, `SetRenderState`, and texture-stage state around alpha-blended
screen-space quads late in the frame.

## sh4xe modern graphics profile

The active proxy-side graphics profile now makes four low-risk D3D8 state
upgrades:

- texture filtering defaults to anisotropic/trilinear instead of the stock
  bilinear path;
- texture mip LOD bias defaults to `-0.35` for sharper wall/floor detail;
- fog remains enabled, but fog start/end are scaled by `1.30` and fog density is
  scaled by `0.72`, which keeps atmosphere while reducing the muddy veil;
- `D3DRS_DITHERENABLE` is forced on by default for gentler fixed-function
  gradients.

Console controls:

- `graphics` reports the active profile.
- `graphics modern` restores the upgraded defaults.
- `graphics game` returns filter/fog/dither to stock-ish pass-through values.
- `filter <game|point|linear|aniso>` changes texture filtering.
- `filter bias <value>` changes mip LOD bias; `0` is neutral.
- `fog <on|off|soft|game>` toggles or presets fog behavior.
- `fog distance <scale>` and `fog density <scale>` tune the soft-fog curve.
- `graphics dither <on|off>` controls the dither render-state override.
