# DWARF v1 recovery toolkit — `SLUS_208.73` (SH4 E3 2004 trial)

Recovers source-level info (types, function signatures, globals, source-file map)
from the **DWARF version 1** debug data that **Metrowerks CodeWarrior for PS2**
left in the `.debug` + `.line` sections of the unstripped E3-trial binary, and
applies it to the Ghidra database.

Full writeup: [`../../docs/sh4_debug_section_analysis.html`](../../docs/sh4_debug_section_analysis.html)

## Pipeline (production)

| Step | File | Output |
|------|------|--------|
| 1. Parse `.debug`/`.line` → normalized model | `extract.py` | `model.json` |
| 2. Build recovered types into Ghidra `/dwarf1` | `ghidra/Sh4Types.java` | 2,510 structs · 404 unions · 194 enums |
| 3. Apply function signatures + source comments | `ghidra/Sh4Funcs.java` | 8,833 functions |
| 4. Place typed globals at their addresses | `ghidra/Sh4Globals.java` | 3,376 typed · 4,041 labeled |

```bash
python extract.py            # regenerates model.json from the ELF
```

Then in Ghidra (see Requirements) run `Sh4Types` → `Sh4Funcs` → `Sh4Globals`
in that order. `Sh4Funcs` accepts an optional source-path substring arg to limit
the apply to one module (e.g. `sf_chara.c`) for validation.

## Exploration / analysis

These are repeatable helpers for validating or exploring the recovered model; not
needed to apply the model in Ghidra.

| File | Purpose | Output |
|------|---------|--------|
| `analyze.py` | full DIE walk → recovery inventory | `summary.json` |
| `srctree.py` | reconstruct the 504-file source tree | `srctree.json` |
| `samples.py` | resolve a sample set to clean C (resolver prototype) | `samples.json` |

Historical `probe*.py` scripts were one-off format-reversal scratch files and are
not kept in the repo. The durable evidence is in the HTML writeup plus the
repeatable scripts above.

## Requirements

- **Python 3** for the extractor (stdlib only).
- **Ghidra 12.x** with the GhidraMCP bridge, started with
  `GHIDRA_MCP_ALLOW_SCRIPTS=1`, and the `$USER_HOME/ghidra_scripts` bundle
  **enabled** in Script Manager → Bundle Manager. The applier scripts use Gson
  (bundled with Ghidra) to read `model.json`.

## Machine-specific paths

- `extract.py` hardcodes the ELF path under `~/Downloads/...`.
- `ghidra/*.java` hardcode `MODEL = .../tools/dwarf1/model.json`.

Adjust these if the repo or binary moves.

## Notes on the encoding (CodeWarrior DWARF 1)

- DIE = `u32 length` · `u16 tag` · form-typed attributes (form = low nibble).
- No `typedef`/`pointer` DIEs: pointers/const are inline **modifier blocks**
  (`AT_mod_u_d_type` = `[mods][u32 DIE-ref]`, mod `0x01` = pointer).
- Member offset = `AT_location` block `04 <u32 off> 07`; global addr = `03 <u32 addr>`.
- `@anonN` type names are **reused per compile-unit** with different layouts — the
  applier gives each anon DIE a unique `anon_<dieoff>` name and dedupes only real
  names (picking the largest definition as canonical).

Generated `*.json` are git-ignored (reproducible from the scripts).
