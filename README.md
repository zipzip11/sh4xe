# sh4xe

`sh4xe` is a research-oriented x86 MSVC hook bootstrap for the GOG release of
Silent Hill 4. It builds as a `dsound.dll` proxy that forwards DirectSound
exports to the real system DLL, starts mod initialization outside loader lock,
initializes MinHook, and hosts native/D3D8 experiments plus offline reversing
tools.

This is not a stable end-user modpack. Treat releases as experimental snapshots
for testing, reversing, and iterating on hooks.

## Status

- Research/dev preview: APIs, console commands, and hook behavior may change.
- Current runtime focus: proxy bootstrap, logging, D3D8 overlay/debug console,
  display/render tweaks, FPS/FOV experiments, god mode, enemy spawn testing, and
  Lua bootstrap support.
- Current tooling focus: SH4 archive round-tripping and PS2 DWARF v1 debug-info
  recovery from the E3 2004 trial binary.
- Parked work: camera responsiveness/mouselook lives under `src/hooks/parked/`
  and is not compiled or installed.

## Layout

```text
src/              runtime proxy/bootstrap/hooks/render/scripting code
src/hooks/parked/ disabled or experimental hook work kept for reference
docs/             reversing notes and research writeups
tools/            durable offline tooling
tools/dwarf1/     PS2 DWARF v1 recovery and Ghidra import scripts
lib/              vendored submodules
bin/, build/      generated MSVC outputs
```

## Build

Run `build.bat` from the repo root. It initializes the x86 MSVC environment,
builds `Release|Win32`, and deploys the DLL to the configured game directory.

The project intentionally targets Win32 because Silent Hill 4 is a 32-bit game.
The output DLL is written to `bin/<Configuration>/dsound.dll`.

GitHub Actions builds `Release|Win32` on `windows-latest` and uploads the DLL/PDB
as a CI artifact. The workflow overrides the CI toolset to the runner's VS 2022
toolchain (`v143`) for portability; local builds continue to use the checked-in
project defaults through `build.bat`.

## Console

The DLL installs a small D3D8 overlay console once the game creates its
`IDirect3DDevice8`. Press the backtick key to toggle it. Useful commands include
`display`, `fps`, `god`, `graphics`, `lighting`, and `spawn`; type `help` in the
overlay for the full list. The old camera responsiveness/mouselook hook is
parked under `src/hooks/parked/` and is not installed.

`god on` suppresses Henry's normal contact-impact damage path. `god off`
restores it.

## Install

Copy `bin/Release/dsound.dll` to the Silent Hill 4 game directory. At runtime it
creates `scripts/sh4xe.log` beside the game executable and loads an optional
`scripts/sh4xe.lua` if present.

Default deploy path:

```text
C:\GOG Games\Silent Hill 4\
```

Manual deploy:

```powershell
.\scripts\deploy.ps1 -Configuration Release
```

Build and deploy in one step:

```powershell
.\build.bat
```

Override the install path if needed:

```powershell
msbuild sh4xe.sln /p:Configuration=Release /p:Platform=Win32 /p:DeployAfterBuild=true /p:SH4InstallDir="D:\Games\Silent Hill 4\" /m
```

Copying into `Program Files (x86)` may require an elevated Visual Studio or
terminal.

## Releases

Releases are tag-driven experimental snapshots. Push a tag like
`v0.1.0-research.1` to build and publish a prerelease:

```powershell
git tag v0.1.0-research.1
git push origin v0.1.0-research.1
```

The release workflow packages `dsound.dll`, `dsound.pdb`, and a short install
note. It does not include game assets.

## Repository Direction

Keep the repo organized around research that can be repeated:

- Runtime experiments belong under `src/hooks/`, `src/render/`, or
  `src/scripting/`.
- Disabled experiments should move to `src/hooks/parked/` instead of staying in
  the active build.
- Reversing notes belong in `docs/`.
- Durable extraction/import tooling belongs in `tools/`; generated output,
  caches, and one-off probes should stay ignored.

## Upstream Notes

See `docs/upstream_randomizer_notes.md` for the README comparison and local
randomizer conventions that shaped this bootstrap.
