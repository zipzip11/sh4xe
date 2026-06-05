# SH4 SDK Tools

Early SDK-generation tools for turning the recovered PS2 E3 DWARF model into
reviewable Windows modding artifacts.

## Workflow

1. Regenerate the DWARF model:

   ```powershell
   python tools\dwarf1\extract.py C:\path\to\SLUS_208.73
   ```

2. Normalize the PS2 model:

   ```powershell
   python tools\sdk\export_ps2_model.py
   ```

   Output: `tools/sdk/generated/ps2_model.json`.

3. Emit reviewable PS2 headers:

   ```powershell
   python tools\sdk\generate_headers.py
   ```

   Output: `tools/sdk/generated/include/sh4/`.

4. Create a PC-correlation review queue:

   ```powershell
   python tools\sdk\correlate_pc.py --filter camera --limit 100
   ```

   Output: `tools/sdk/generated/function_map.json`.

## Boundaries

- `export_ps2_model.py` consumes `tools/dwarf1/model.json` and preserves PS2
  symbols, prototypes, globals, source paths, and aggregate layouts.
- `generate_headers.py` emits PS2 metadata headers. Structs/unions are forward
  declarations with layout comments until manually validated.
- `correlate_pc.py` defines the function-map format and can merge manually
  reviewed candidates. It does not yet query MCP servers or infer matches.
- Runtime code should consume only entries whose correlation status is
  `validated`; unreviewed candidates stay in generated SDK data.
