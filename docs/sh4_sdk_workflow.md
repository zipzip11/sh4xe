# Silent Hill 4 SDK Workflow From The PS2 E3 Database

This note sketches a practical path from the recovered PS2 E3 Ghidra database to
Windows modding artifacts for this `dsound.dll` proxy. The assumed inputs are:

- a Ghidra database for the PS2 E3 trial with symbols, prototypes, structs,
  enums, and DWARF-derived metadata already applied;
- an IDA database for the Windows GOG executable loaded through `ida-pro-mcp`;
- the PS2 database loaded through `ghidramcp`;
- the repeatable DWARF extraction/import tooling in `tools/dwarf1/`.

## Goal

The SDK should not pretend the PS2 E3 binary and the Windows retail executable
are the same program. It should package verified knowledge in layers:

- portable recovered types: enums, POD structs, constants, source names, and
  comments derived from DWARF;
- PC-specific function maps: Windows addresses, calling conventions, confidence,
  and evidence for each match;
- runtime-safe headers: declarations that can be included by hooks without
  importing unverified gameplay patches;
- correlation notes: why a PS2 symbol was matched to a PC function or rejected.

## Generation Pipeline

1. Export the PS2 recovered model.
   `tools/dwarf1/extract.py` already produces `model.json` from the E3 ELF. A
   follow-up exporter can also query `ghidramcp` for Ghidra-side improvements
   made after DWARF import: renamed functions, corrected prototypes, edited
   structs, enum cleanup, source comments, and category paths.

2. Normalize SDK names.
   Use stable namespaces such as `sh4::ps2` for raw recovered names and
   `sh4::pc` for Windows address bindings. Preserve original names in metadata,
   but sanitize C++ identifiers consistently. For duplicate or anonymous DWARF
   names, keep the existing `anon_<dieoff>` style until a human assigns a better
   name.

3. Generate portable headers.
   A script can emit:

   - `include/sh4/types/*.h` for structs, unions, enums, and typedef-like aliases;
   - `include/sh4/funcs_ps2.h` for PS2 prototypes and addresses;
   - `include/sh4/globals_ps2.h` for recovered globals;
   - static layout checks for known sizes and offsets.

   These headers should avoid declaring PC call targets until a PC address has
   been validated.

4. Correlate PS2 functions to PC functions.
   Use `ghidramcp` to enumerate PS2 functions and `ida-pro-mcp` to inspect PC
   functions. Automated matching can rank candidates by symbol/source name,
   function size, string references, global references, call graph neighborhood,
   switch constants, float constants, and instruction-level behavior. The output
   should be a scored map, not a truth table.

5. Emit PC binding artifacts.
   For validated matches, generate small data files and optional headers:

   - `tools/sdk/function_map.json`: PS2 symbol, PC address, confidence, evidence;
   - `src/sh4/addresses.h` candidates only after they are used by active code;
   - `include/sh4/pc_funcs.h` with typed wrappers for high-confidence call sites;
   - `docs/sdk/correlation/*.md` for manual notes on important subsystems.

## What Can Be Automated

- Exporting Ghidra types, enums, function signatures, source paths, comments, and
  globals from the PS2 database.
- Producing sanitized C/C++ declarations from `model.json` and Ghidra-applied
  type edits.
- Detecting duplicate type names, anonymous type collisions, invalid identifiers,
  unsupported calling conventions, and layout holes.
- Ranking likely PC matches using names, constants, strings, call graph shape,
  referenced globals, and decompiler signatures.
- Generating reviewable reports that show candidate PS2 and PC functions side by
  side with address, prototype, size, callers, callees, strings, and constants.
- Emitting checked headers for types whose size and member offsets are known.

## What Needs Manual Validation

- Any PC function address that will be called or patched by the runtime DLL.
- Calling conventions and stack cleanup. The PS2 ABI tells us nothing about the
  Windows compiler's `cdecl`, `thiscall`, `stdcall`, hidden return buffers, or
  register usage.
- Struct layouts that cross platform-specific boundaries: pointers, alignment,
  file handles, Direct3D/DirectInput objects, thread state, and platform IO.
- Globals whose PC storage differs by build, allocation timing, or indirection.
- Functions that were inlined, split, merged, optimized differently, or removed
  between the E3 trial and the PC build.
- Gameplay behavior where E3 content differs from retail.

## Transfer Risks

- Platform ABI mismatch: PS2 pointers, alignment, endian-sensitive fields, and
  MIPS calling patterns do not directly describe x86 Windows code.
- Build drift: the E3 trial may contain debug-only code, different file IDs,
  different room/enemy logic, or different compiler output from the GOG PC EXE.
- Name overconfidence: a recovered PS2 symbol is strong semantic evidence, but a
  PC address still needs binary evidence.
- Type overreach: applying a large struct too early can make decompiler output
  look plausible while hiding offset mistakes.
- Runtime safety: the mod should only expose active addresses that hooks actually
  use; experimental maps belong in generated SDK data or parked notes.

## Recommended First Milestone

Build `tools/sdk/export_ps2_model.py` and `tools/sdk/correlate_pc.py` as offline
tools:

- `export_ps2_model.py` reads `model.json` plus optional `ghidramcp` corrections
  and emits normalized type/prototype JSON.
- `correlate_pc.py` queries both MCP servers, produces ranked PC candidates, and
  writes `function_map.json` with evidence and confidence.
- A small header generator consumes only validated entries and emits C++ headers
  with `static_assert` layout checks.

Keep generated SDK outputs out of the runtime until a hook needs them. When a
hook graduates from SDK data into active code, copy only the validated address or
type slice into the appropriate `src/sh4/` header and cite the correlation note.
