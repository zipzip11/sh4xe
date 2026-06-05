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

## Automatic PS2->PC correlation (MCP-backed)

The correlator can now *propose* PC matches instead of leaving every entry blank.
The matcher itself is offline and deterministic; it scores the behavioural
**anchors** two functions share -- string literals and distinctive numeric
constants (rare floats like `pi/15`, magic ints like `0x9C51`). Shared anchors
survive recompilation, so they link the PS2 and PC builds.

1. Export an anchor index from each live database over MCP. The PC side talks to
   the running IDA instance directly:

   ```powershell
   python tools\sdk\export_anchors.py --engine ida --url http://127.0.0.1:13337 `
       --min-addr 0x4f0000 --max-addr 0x540000 -o tools\sdk\generated\pc_anchors.json
   python tools\sdk\export_anchors.py --engine ghidra --url http://127.0.0.1:<port> `
       --filter cam --filter camera -o tools\sdk\generated\ps2_anchors.json
   ```

   Find a server's `/mcp` port with `netstat -ano -p tcp` and probe it with
   `python tools\sdk\mcp_client.py http://127.0.0.1:<port>`. (Where the Ghidra MCP
   is only reachable via a stdio bridge, `build_ps2_anchors_demo.py` shows the same
   index built from cached decompiles.)

2. Score candidates and review them:

   ```powershell
   python tools\sdk\autocorrelate.py --filter cam --summary
   python tools\sdk\correlate_pc.py --filter camera --manual tools\sdk\generated\candidates.json
   ```

   `autocorrelate.py` emits `candidates.json` with a `high`/`medium`/`low`
   confidence per match and the evidence behind it. It **never** writes
   `validated`. `correlate_pc.py --manual` folds those into the worklist's
   `pc_candidates`; a human still promotes the right one to `validated`.

## Boundaries

- `export_ps2_model.py` consumes `tools/dwarf1/model.json` and preserves PS2
  symbols, prototypes, globals, source paths, and aggregate layouts.
- `generate_headers.py` emits PS2 metadata headers. Structs/unions are forward
  declarations with layout comments until manually validated.
- `correlate_pc.py` defines the function-map format and merges reviewed
  candidates (manual or from `autocorrelate.py`).
- `mcp_client.py` / `export_anchors.py` are the only pieces that touch a live MCP
  server; `autocorrelate.py` is pure offline scoring over their JSON output.
- Runtime code should consume only entries whose correlation status is
  `validated`; unreviewed candidates and auto-proposed matches stay in generated
  SDK data until a human confirms them.
