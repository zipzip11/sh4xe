# sh4xe

`sh4xe` is a small x86 MSVC hook bootstrap for the GOG release of Silent Hill 4.
It builds as a `dsound.dll` proxy that forwards DirectSound exports to the real
system DLL, starts mod initialization outside loader lock, initializes MinHook,
and brings up a minimal Lua runtime for later scripting work.

## Layout

```text
bin/      build outputs
build/    MSVC intermediate files
lib/      external dependencies
src/      proxy, bootstrap, hooks, scripting, and SH4 notes
```

## Build

Run `build.bat` from the repo root. It initializes the x86 MSVC environment,
builds `Release|Win32`, and deploys the DLL to the configured game directory.

The project intentionally targets Win32 because Silent Hill 4 is a 32-bit game.
The output DLL is written to `bin/<Configuration>/dsound.dll`.

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

## Upstream Notes

See `docs/upstream_randomizer_notes.md` for the README comparison and local
randomizer conventions that shaped this bootstrap.
