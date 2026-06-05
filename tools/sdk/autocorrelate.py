#!/usr/bin/env python3
"""Propose PS2->PC function matches from anchor indexes (offline scorer).

This is the inference step the correlation workflow was missing. It does NOT touch
any MCP server itself -- it consumes two anchor indexes produced by
``export_anchors.py`` (one per binary) and scores candidate PS2<->PC function
pairs by the *behavioural anchors* they share:

  * string literals referenced by the function (highest precision -- the same
    error text / asset path / format string is compiled into both builds), and
  * distinctive numeric constants (rare floats like ``pi/15`` = 0.20943952, magic
    ints like 0x9C51, scale factors like 250.0).

Scoring is TF-IDF-flavoured: each shared anchor contributes a weight inversely
proportional to how many PC functions contain it, so ubiquitous values (0, 1,
1.0) carry almost nothing while a constant that appears in one function on each
side is near-decisive. A candidate's confidence also accounts for how much of the
PS2 function's own anchor mass was matched and the margin over the runner-up.

Output is a candidates list in the exact shape ``correlate_pc.py --manual`` merges,
with ``status: "candidate"`` -- it never marks anything ``validated``. A human still
confirms each match before runtime code may consume it.

Usage:
    python tools/sdk/autocorrelate.py \
        --ps2 tools/sdk/generated/ps2_anchors.json \
        --pc  tools/sdk/generated/pc_anchors.json \
        [--filter cam] [--top 5] [--min-score 1.5] \
        [-o tools/sdk/generated/candidates.json]

Then review and merge:
    python tools/sdk/correlate_pc.py --manual tools/sdk/generated/candidates.json
"""

from __future__ import annotations

import argparse
import json
import math
from collections import defaultdict
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
GEN = ROOT / "tools" / "sdk" / "generated"
DEFAULT_PS2 = GEN / "ps2_anchors.json"
DEFAULT_PC = GEN / "pc_anchors.json"
DEFAULT_OUT = GEN / "candidates.json"

# Numeric anchors that say nothing -- they appear in a large fraction of all
# functions. IDF already discounts them, but stop-listing keeps evidence readable
# and avoids rewarding coincidental overlap on trivial values.
STOP_CONSTS = {
    "i:0", "i:1", "i:-1", "i:2", "i:3", "i:4", "i:8", "i:16", "i:255", "i:256",
    "i:100", "i:1000", "i:-2", "i:10",
    "f:0.0", "f:1.0", "f:-1.0", "f:0.5", "f:-0.5", "f:2.0", "f:100.0", "f:255.0",
    "f:0.10000000149011612", "f:10.0",
}

# String anchors weaker than this length are too generic ("on", "%d", "ok") to
# count as evidence on their own.
MIN_STRING_LEN = 4


def load_json(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            return json.load(handle)
    except FileNotFoundError as exc:
        raise SystemExit(f"missing anchor index: {path}\nrun tools/sdk/export_anchors.py first") from exc


def write_json(path: Path, data: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(data, handle, indent=2, sort_keys=True)
        handle.write("\n")


def tokens_for(func: dict[str, Any]) -> set[str]:
    """The set of comparable anchor tokens a function contributes."""
    out: set[str] = set()
    for s in func.get("strings", []):
        if isinstance(s, str) and len(s.strip()) >= MIN_STRING_LEN:
            out.add("s:" + s.strip())
    for c in func.get("consts", []):
        if isinstance(c, str) and c not in STOP_CONSTS:
            out.add(c)
    return out


def anchor_weight(token: str, df: int, total: int) -> float:
    """Inverse-document-frequency weight, with a precision bonus for strings."""
    # df is clamped >=1; a token present in every function -> ~0 weight.
    idf = math.log((total + 1) / (df + 0.5))
    if idf < 0.0:
        idf = 0.0
    if token.startswith("s:"):
        # Strings are high-precision; reward length (rarer the longer it is).
        length_bonus = min(len(token) - 2, 32) / 8.0
        return idf * (2.0 + length_bonus)
    return idf


def confidence_label(matched_mass: float, self_mass: float, score: float, runner_up: float,
                     has_string: bool) -> str:
    coverage = (matched_mass / self_mass) if self_mass > 0 else 0.0
    margin = (score / runner_up) if runner_up > 0 else math.inf
    if (coverage >= 0.45 and (has_string or score >= 4.0) and margin >= 1.5):
        return "high"
    if coverage >= 0.2 and score >= 1.5:
        return "medium"
    return "low"


def build_index(funcs: list[dict[str, Any]]) -> tuple[dict[str, set[str]], dict[str, int]]:
    """Return token -> set(func keys) and token -> document frequency for one binary."""
    postings: dict[str, set[str]] = defaultdict(set)
    for fn in funcs:
        key = str(fn.get("addr"))
        for tok in tokens_for(fn):
            postings[tok].add(key)
    df = {tok: len(keys) for tok, keys in postings.items()}
    return postings, df


def main() -> int:
    parser = argparse.ArgumentParser(description="Score PS2->PC function candidates from anchor indexes.")
    parser.add_argument("--ps2", type=Path, default=DEFAULT_PS2, help="PS2 (Ghidra) anchor index")
    parser.add_argument("--pc", type=Path, default=DEFAULT_PC, help="PC (IDA) anchor index")
    parser.add_argument("-o", "--output", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--filter", action="append", default=[],
                        help="only score PS2 functions whose name contains this substring")
    parser.add_argument("--top", type=int, default=5, help="max PC candidates kept per PS2 function")
    parser.add_argument("--min-score", type=float, default=1.5, help="drop candidates below this score")
    parser.add_argument("--summary", action="store_true", help="print a human-readable ranking")
    args = parser.parse_args()

    ps2 = load_json(args.ps2)
    pc = load_json(args.pc)
    pc_funcs = pc.get("functions", [])
    ps2_funcs = ps2.get("functions", [])

    pc_postings, pc_df = build_index(pc_funcs)
    pc_total = max(1, len(pc_funcs))
    pc_by_key = {str(fn.get("addr")): fn for fn in pc_funcs}

    candidates: list[dict[str, Any]] = []
    summary_rows: list[tuple[str, list[dict[str, Any]]]] = []

    for fn in ps2_funcs:
        name = fn.get("name") or ""
        if args.filter and not any(term.lower() in name.lower() for term in args.filter):
            continue
        ps2_tokens = tokens_for(fn)
        if not ps2_tokens:
            continue

        # Self-mass: how much evidence this PS2 function carries (using PC idf as the
        # shared weighting frame, so coverage is measured in matchable terms).
        self_mass = sum(anchor_weight(t, pc_df.get(t, 0), pc_total) for t in ps2_tokens)

        scores: dict[str, float] = defaultdict(float)
        shared: dict[str, list[str]] = defaultdict(list)
        for tok in ps2_tokens:
            w = anchor_weight(tok, pc_df.get(tok, 0), pc_total)
            if w <= 0.0:
                continue
            for pc_key in pc_postings.get(tok, ()):  # PC functions sharing this anchor
                scores[pc_key] += w
                shared[pc_key].append(tok)

        ranked = sorted(scores.items(), key=lambda kv: kv[1], reverse=True)
        ranked = [(k, s) for k, s in ranked if s >= args.min_score][: args.top]
        if not ranked:
            continue

        runner_up = ranked[1][1] if len(ranked) > 1 else 0.0
        pc_candidates = []
        for pc_key, score in ranked:
            ev = sorted(shared[pc_key], key=lambda t: anchor_weight(t, pc_df.get(t, 0), pc_total), reverse=True)
            has_string = any(t.startswith("s:") for t in ev)
            pc_fn = pc_by_key.get(pc_key, {})
            pc_candidates.append({
                "ps2_symbol": name,
                "ps2_low": fn.get("addr"),
                "pc_addr": pc_key,
                "pc_name": pc_fn.get("name"),
                "score": round(score, 3),
                "confidence": confidence_label(score, self_mass, score, runner_up, has_string),
                "status": "candidate",
                "evidence": [_pretty(t) for t in ev[:8]],
            })
        candidates.extend(pc_candidates)
        summary_rows.append((name, pc_candidates))

    write_json(args.output, candidates)

    by_conf = defaultdict(int)
    for c in candidates:
        by_conf[c["confidence"]] += 1
    print(f"wrote {args.output}")
    print(f"  ps2 functions scored : {len(summary_rows)}")
    print(f"  candidate rows        : {len(candidates)}  (high={by_conf['high']} medium={by_conf['medium']} low={by_conf['low']})")

    if args.summary:
        print()
        for name, cands in summary_rows:
            best = cands[0]
            print(f"{name:<32} -> {best['pc_name']:<14} {best['confidence']:<6} score={best['score']}")
            for t in best["evidence"][:4]:
                print(f"      - {t}")
    return 0


def _pretty(token: str) -> str:
    if token.startswith("s:"):
        return f'shared string "{token[2:]}"'
    if token.startswith("f:"):
        return f"shared const {token[2:]}"
    if token.startswith("i:"):
        try:
            v = int(token[2:])
            return f"shared int {v} (0x{v & 0xFFFFFFFF:X})"
        except ValueError:
            return f"shared int {token[2:]}"
    return f"shared {token}"


if __name__ == "__main__":
    raise SystemExit(main())
