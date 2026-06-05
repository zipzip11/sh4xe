#!/usr/bin/env python3
"""Create or update a PS2-to-PC function correlation worklist.

This is the first offline artifact for the SDK correlation workflow. It consumes
the normalized PS2 SDK model and emits a review queue or merges manually supplied
PC candidates. Automated MCP-backed scoring can be added here later, but runtime
headers should consume only entries whose status is "validated".
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MODEL = ROOT / "tools" / "sdk" / "generated" / "ps2_model.json"
DEFAULT_OUT = ROOT / "tools" / "sdk" / "generated" / "function_map.json"


def load_json(path: Path, default: Any = None) -> Any:
    if not path.exists():
        if default is not None:
            return default
        raise SystemExit(f"missing input: {path}")
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def write_json(path: Path, data: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(data, handle, indent=2, sort_keys=True)
        handle.write("\n")


def source_matches(func: dict[str, Any], filters: list[str]) -> bool:
    if not filters:
        return True
    haystack = f"{func.get('name', '')} {func.get('original_name', '')} {func.get('source', '')}".lower()
    return any(term.lower() in haystack for term in filters)


def make_entry(func: dict[str, Any]) -> dict[str, Any]:
    return {
        "ps2_symbol": func.get("name"),
        "ps2_original_symbol": func.get("original_name"),
        "ps2_low": func.get("ps2_low"),
        "ps2_high": func.get("ps2_high"),
        "source": func.get("source"),
        "prototype": {
            "return_type": func.get("return_type"),
            "params": [{"name": p.get("name"), "type": p.get("type")} for p in func.get("params", [])],
        },
        "pc_candidates": [],
        "validated_pc": None,
        "confidence": "unreviewed",
        "status": "unreviewed",
        "evidence": [],
        "notes": "Use ghidramcp/ida-pro-mcp to add candidates, then mark validated only after manual review.",
    }


def merge_manual_candidates(entries: list[dict[str, Any]], candidates: list[dict[str, Any]]) -> None:
    by_symbol = {entry.get("ps2_symbol"): entry for entry in entries}
    by_addr = {entry.get("ps2_low"): entry for entry in entries}
    for candidate in candidates:
        entry = by_symbol.get(candidate.get("ps2_symbol")) or by_addr.get(candidate.get("ps2_low"))
        if not entry:
            continue
        pc_candidate = {
            "pc_addr": candidate.get("pc_addr"),
            "pc_name": candidate.get("pc_name"),
            "score": candidate.get("score"),
            "evidence": candidate.get("evidence", []),
        }
        if pc_candidate not in entry["pc_candidates"]:
            entry["pc_candidates"].append(pc_candidate)
        if candidate.get("status") == "validated":
            entry["validated_pc"] = pc_candidate
            entry["confidence"] = candidate.get("confidence", "manual")
            entry["status"] = "validated"
            entry["evidence"].extend(candidate.get("evidence", []))
            entry["notes"] = candidate.get("notes", entry["notes"])


def main() -> int:
    parser = argparse.ArgumentParser(description="Build a PS2-to-PC SDK function correlation worklist.")
    parser.add_argument("model", nargs="?", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("-o", "--output", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--filter", action="append", default=[], help="substring filter for name/source")
    parser.add_argument("--manual", type=Path, help="optional JSON array of manually reviewed PC candidates")
    parser.add_argument("--limit", type=int, default=500, help="maximum unreviewed entries to emit")
    args = parser.parse_args()

    model = load_json(args.model)
    funcs = [f for f in model.get("functions", []) if source_matches(f, args.filter)]
    entries = [make_entry(func) for func in funcs[: max(0, args.limit)]]

    if args.manual:
        merge_manual_candidates(entries, load_json(args.manual, []))

    result = {
        "meta": {
            "source": str(args.model),
            "status": "review queue; validated_pc entries are the only runtime-safe matches",
            "filters": args.filter,
        },
        "functions": entries,
    }
    write_json(args.output, result)
    print(f"wrote {args.output}")
    print(f"  queued functions: {len(entries)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
