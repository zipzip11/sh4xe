# AGENTS.md

This project is an x86 MSVC `dsound.dll` proxy/bootstrap for the GOG version of
Silent Hill 4. Keep changes focused on native hooking, proxy loading, logging,
Lua bootstrap support, D3D8 overlay/debug tooling, and offline reversing tools
that directly support those areas.

## Structure

- `src/`: runtime project code.
- `src/proxy/`: `dsound.dll` export proxy.
- `src/core/`: DLL bootstrap, paths, logging, and runtime initialization.
- `src/hooks/`: active MinHook-based native hooks.
- `src/hooks/parked/`: disabled or experimental hook work kept for reference.
  The camera responsiveness/mouselook hook is currently parked here and should
  not be re-enabled unless explicitly requested.
- `src/render/`: D3D8 overlay console, display handling, lighting, and render
  tweaks.
- `src/scripting/`: minimal embedded Lua support.
- `src/sh4/`: recovered SH4 addresses and lightweight game-structure notes.
- `docs/`: reversing notes and writeups.
- `tools/`: offline developer tooling, not runtime `scripts/`.
- `tools/dwarf1/`: PS2 CodeWarrior DWARF v1 recovery toolkit for the
  `SLUS_208.73` SH4 E3 2004 trial binary. `extract.py` regenerates the
  normalized model, and `ghidra/Sh4Types.java`, `Sh4Funcs.java`, and
  `Sh4Globals.java` apply recovered types, function signatures, source comments,
  and globals in Ghidra.
- `lib/`: vendored submodules, currently MinHook and Lua.
- `bin/`, `build/`, `extracted/`: generated local outputs.

## Build

Target `Win32`, not x64. Silent Hill 4 is a 32-bit game.

**Preferred: run `build.bat`.** It sets up the x86 MSVC environment
(`VsDevCmd.bat -arch=x86 -host_arch=x64`) and runs the Release/Win32 build plus
deploy in one step. Use it instead of invoking `msbuild` by hand; raw `msbuild`
only works from an already-initialized MSVC developer shell.

```bat
build.bat
```

First-time setup needs the submodules:

```powershell
git submodule update --init --recursive
```

Output: `bin/Release/dsound.dll`, auto-deployed by `build.bat` to the game dir.
Default install path is `C:\GOG Games\Silent Hill 4`.

Manual build, only from a developer command prompt where `msbuild` is on PATH:

```powershell
msbuild sh4xe.sln /p:Configuration=Release /p:Platform=Win32 /m
# add /p:DeployAfterBuild=true to also copy the DLL into the game dir
```

GitHub Actions builds use `.github/workflows/build.yml` and
`.github/workflows/release.yml`. Those workflows override CI builds to
`PlatformToolset=v143` and `WindowsTargetPlatformVersion=10.0` so GitHub-hosted
Windows runners can build the project even if the local checkout targets newer
Visual Studio defaults.

## Runtime Notes

The DLL writes `scripts/sh4xe.log` next to the game executable and loads optional
`scripts/sh4xe.lua`. The D3D8 console is toggled with backtick. Current active
commands cover `help`, `clear`, `echo`, `display`, `filter`, `fog`, `fov`,
`fps`, `god`, `graphics`, `lighting`, and `spawn`.

The camera responsiveness/mouselook hook is disabled and parked in
`src/hooks/parked/`. The overlay still reports old `camera`/`mouselook` inputs as
parked so stale notes do not silently appear to work.

Releases are experimental research snapshots, not stable modpack builds. Tags
matching `v*` trigger the release workflow, which creates a prerelease containing
`dsound.dll`, `dsound.pdb`, and install notes.

## Tools

- `tools/sh4arc.py` is a dependency-free Python 3.8+ SH4 archive tool for
  unpacking, analyzing, and repacking `.pac` and `.bin` containers.
- `tools/dwarf1/` is for source-level debug recovery from the PS2 E3 trial ELF's
  DWARF v1 `.debug` and `.line` sections. See `tools/dwarf1/README.md` and
  `docs/sh4_debug_section_analysis.html` before changing it.
- DWARF JSON outputs (`model.json`, `summary.json`, `srctree.json`,
  `samples.json`) are reproducible and ignored. Regenerate them locally when
  needed instead of committing them.
- The DWARF extractor and Ghidra scripts currently contain machine-specific paths
  to the trial ELF and `model.json`; update those paths locally if the checkout
  moves.

## Guidelines

Use existing MSVC project patterns and keep the proxy self-contained. Avoid
unnecessary dependencies, large refactors, or gameplay patches unless requested.
Do not edit vendored submodules except to intentionally update or pin them.

Keep generated outputs, caches, extracted assets, and one-off probes out of git.
Prefer repeatable scripts and documented pipelines under `tools/` for reversing
work that should survive the next checkout.
