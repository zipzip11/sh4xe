#!/usr/bin/env python3
"""Export per-function behavioural anchors from a live RE database over MCP.

An "anchor index" is the input ``autocorrelate.py`` scores against: for each
function, the string literals and distinctive numeric constants it references.
Those survive recompilation across PS2<->PC, so shared anchors link the two builds.

Two engines, one wire protocol (MCP streamable HTTP, see ``mcp_client``):

  * ``--engine ida``    : enumerates PC functions via ``func_query`` and decompiles
                          each via ``decompile``. Point ``--url`` at the live IDA
                          instance (default http://127.0.0.1:13337).
  * ``--engine ghidra`` : takes the PS2 function list from ``ps2_model.json`` (real
                          DWARF names + addresses) and decompiles each via
                          ``decompile_function``. Point ``--url`` at the Ghidra MCP
                          server (find its port with ``netstat -ano -p tcp`` and
                          probe ``/mcp``).

Anchors are extracted from decompiler pseudocode by stripping comments, then
pulling string literals, float constants (rounded to float32 precision so both
decompilers agree), and small integer constants (addresses/offsets are dropped).

Examples:
    python tools/sdk/export_anchors.py --engine ida \
        --url http://127.0.0.1:13337 --min-addr 0x4f0000 --max-addr 0x540000 \
        -o tools/sdk/generated/pc_anchors.json

    python tools/sdk/export_anchors.py --engine ghidra \
        --url http://127.0.0.1:8192 --filter cam --filter camera \
        -o tools/sdk/generated/ps2_anchors.json
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path
from typing import Any

from mcp_client import McpClient
from autocorrelate import write_json  # reuse the newline-stable JSON writer

try:
    from export_ps2_model import load_json  # reuse loader
except ImportError:  # pragma: no cover - allow running from another cwd
    import json

    def load_json(path: Path) -> dict[str, Any]:
        with Path(path).open("r", encoding="utf-8") as handle:
            return json.load(handle)

ROOT = Path(__file__).resolve().parents[2]
GEN = ROOT / "tools" / "sdk" / "generated"
PS2_MODEL = GEN / "ps2_model.json"

# An int this large is almost certainly an address/offset, not a semantic magic
# value; those are platform-specific and would be false anchors.
ADDR_THRESHOLD = 0x100000

_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT = re.compile(r"//[^\n]*")
_STRING = re.compile(r'"((?:[^"\\]|\\.)*)"')
_FLOAT = re.compile(r"[-+]?\d+\.\d+(?:[eE][-+]?\d+)?")
_HEX = re.compile(r"0[xX][0-9a-fA-F]+")
_DEC = re.compile(r"\b\d+\b")


def extract_anchors(code: str) -> tuple[list[str], list[str]]:
    """Return (strings, const_tokens) for one function's pseudocode."""
    code = _BLOCK_COMMENT.sub(" ", code)
    code = _LINE_COMMENT.sub(" ", code)

    strings: list[str] = []
    for m in _STRING.finditer(code):
        s = m.group(1)
        if s:
            strings.append(s)
    code = _STRING.sub(" ", code)

    consts: set[str] = set()
    for m in _FLOAT.finditer(code):
        try:
            consts.add("f:" + format(float(m.group(0)), ".7g"))
        except ValueError:
            pass
    code = _FLOAT.sub(" ", code)

    for m in _HEX.finditer(code):
        v = int(m.group(0), 16)
        if 0 < abs(v) < ADDR_THRESHOLD:
            consts.add(f"i:{v}")
    for m in _DEC.finditer(code):
        v = int(m.group(0))
        if 0 < abs(v) < ADDR_THRESHOLD:
            consts.add(f"i:{v}")

    return sorted(set(strings)), sorted(consts)


# --------------------------------------------------------------------------- #
# Engine adapters
# --------------------------------------------------------------------------- #

def ida_functions(client: McpClient, min_addr: int, max_addr: int, min_size: int) -> list[dict[str, str]]:
    out: list[dict[str, str]] = []
    offset = 0
    while True:
        res = client.call("func_query", {"queries": [{"sort_by": "addr", "count": 500,
                                                       "offset": offset, "min_size": min_size}]})
        block = res["result"][0] if isinstance(res, dict) and "result" in res else res[0]
        rows = block.get("data", [])
        if not rows:
            break
        for row in rows:
            addr = int(row["addr"], 16)
            if addr < min_addr or (max_addr and addr >= max_addr):
                continue
            out.append({"addr": row["addr"], "name": row.get("name", "")})
        nxt = block.get("next_offset")
        if not nxt or nxt == offset:
            break
        offset = nxt
        # Stop early once we have walked past the requested window.
        if max_addr and rows and int(rows[-1]["addr"], 16) >= max_addr:
            break
    return out


def ida_decompile(client: McpClient, addr: str) -> str:
    res = client.call("decompile", {"addr": addr})
    if isinstance(res, dict):
        return res.get("code") or ""
    return res if isinstance(res, str) else ""


def ghidra_functions(filters: list[str]) -> list[dict[str, str]]:
    model = load_json(PS2_MODEL)
    out = []
    for fn in model.get("functions", []):
        name = fn.get("name") or ""
        src = fn.get("source") or ""
        low = fn.get("ps2_low")
        if low is None:
            continue
        if filters and not any(t.lower() in f"{name} {src}".lower() for t in filters):
            continue
        out.append({"addr": hex(low), "name": name})
    return out


def ghidra_decompile(client: McpClient, addr: str) -> str:
    res = client.call("decompile_function", {"address": addr})
    if isinstance(res, dict):
        return res.get("result") or res.get("code") or res.get("decompilation") or ""
    return res if isinstance(res, str) else ""


def main() -> int:
    parser = argparse.ArgumentParser(description="Export a per-function anchor index over MCP.")
    parser.add_argument("--engine", choices=["ida", "ghidra"], required=True)
    parser.add_argument("--url", default="http://127.0.0.1:13337", help="MCP base URL")
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--filter", action="append", default=[], help="(ghidra) name/source substring")
    parser.add_argument("--min-addr", type=lambda s: int(s, 0), default=0, help="(ida) window start")
    parser.add_argument("--max-addr", type=lambda s: int(s, 0), default=0, help="(ida) window end (exclusive)")
    parser.add_argument("--min-size", type=lambda s: int(s, 0), default=0x20, help="(ida) skip tiny funcs")
    parser.add_argument("--limit", type=int, default=0, help="cap functions processed (0 = all)")
    parser.add_argument("--timeout", type=float, default=60.0)
    args = parser.parse_args()

    client = McpClient(args.url, timeout=args.timeout)

    if args.engine == "ida":
        funcs = ida_functions(client, args.min_addr, args.max_addr, args.min_size)
        decompile = ida_decompile
        binary = "SILENT HILL 4.exe"
    else:
        funcs = ghidra_functions(args.filter)
        decompile = ghidra_decompile
        binary = "SLUS_208.73"

    if args.limit:
        funcs = funcs[: args.limit]

    records = []
    for i, fn in enumerate(funcs, 1):
        try:
            code = decompile(client, fn["addr"])
        except Exception as exc:  # noqa: BLE001 - keep going, note the gap
            print(f"  ! {fn['addr']} {fn['name']}: {exc}")
            continue
        if not code:
            continue
        strings, consts = extract_anchors(code)
        if not strings and not consts:
            continue
        records.append({"addr": fn["addr"], "name": fn["name"], "strings": strings, "consts": consts})
        if i % 50 == 0:
            print(f"  .. {i}/{len(funcs)} ({len(records)} with anchors)")

    write_json(args.output, {"engine": args.engine, "binary": binary, "functions": records})
    print(f"wrote {args.output}")
    print(f"  functions processed : {len(funcs)}")
    print(f"  functions w/ anchors: {len(records)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
