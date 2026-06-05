# Silent Hill 4 Randomizer Notes

Compared sources:

* Upstream README: https://github.com/HunterStanton/SilentHill4Randomizer
* Local archive: `archive/SilentHill4Randomizer/readme.md`

## README Observations

* The project targets the GOG version of Silent Hill 4; older retail support is
  noted as planned but not implemented.
* Installation uses the 32-bit Ultimate ASI Loader renamed to `dsound.dll`.
  This confirms both the x86 constraint and the game-directory `dsound` loader
  convention.
* Release layout expects a `/scripts/` folder and extra `.bin` files under
  `/data/`. Missing `randomizer_x.bin` files are called out as causing invisible
  player/menu rendering problems and crashes.
* Upstream asks bug reports to include `/scripts/randomizer.log`; the archived
  copy spells this as `<SH4 Install Directory>/scripts/randomizer.log`.
* The randomizer raises room memory allocation from roughly 67 MB to roughly
  134 MB. The local implementation writes:
  `0x00551f98 = 0x08000000`, `0x00551fa9 = 0x02000000`, and
  `0x00551fc5 = 0x08000000`.
* Special thanks credit ThirteenAG, thelink2012, gabime, Thomas Monkman, and
  contributors for hooking, logging, and INI parsing libraries.

## Local Code Observations

* The archived project builds a Win32 `.asi` plugin through Premake, with static
  runtime enabled and output under `data/SilentHill4Randomizer/scripts`.
* `dllmain.cpp` supports ASI-loader mode through `InitializeASI`, but directly
  calls it from `DllMain` if UAL is not detected.
* Initialization is delayed through `CallbackHandler::RegisterCallback` and a
  pattern check, then installs many call/jump hooks using injector helpers.
* Logging is initialized as `randomizer.log`; under ASI install conventions that
  lands in the `scripts` directory.
* File replacement logic in `FileHooks.cpp` redirects several game `data/*.bin`
  paths to `randomizer_*.bin` files.

## Bootstrap Choices Here

* This project is a real `dsound.dll` proxy instead of an ASI plugin.
* All DirectSound exports from the local 32-bit Windows `dsound.dll` are
  exported with matching ordinals and forwarded manually to the system DLL.
* The hook bootstrap does no game-memory patching yet. The room-memory addresses
  are recorded in `src/sh4/addresses.h` as deliberate future targets.
* MinHook is initialized immediately on the worker thread so native hooks can be
  added without changing the loader skeleton.
* Lua is embedded but optional: `scripts/sh4xe.lua` is loaded if present, making
  scripting support available without requiring data files today.

