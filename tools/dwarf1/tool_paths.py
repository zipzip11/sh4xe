#!/usr/bin/env python3
"""Path and JSON helpers for the SH4 DWARF v1 command-line tools."""
from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any


SCRIPT_DIR = Path(__file__).resolve().parent


class ToolError(RuntimeError):
    pass


def add_elf_argument(parser) -> None:
    parser.add_argument(
        "elf",
        nargs="?",
        help="Path to the unstripped PS2 E3 SLUS_208.73 ELF. Prompts if omitted.",
    )


def add_output_argument(parser, default_name: str) -> None:
    parser.add_argument(
        "-o",
        "--output",
        help=f"Output JSON path. Defaults to {SCRIPT_DIR / default_name}.",
    )


def resolve_elf_path(value: str | None) -> Path:
    if value:
        path = Path(value).expanduser()
    else:
        try:
            raw = input("Path to SLUS_208.73 ELF: ").strip()
        except EOFError:
            raw = ""
        if not raw:
            raise ToolError("ELF path is required; pass it as an argument or enter it when prompted.")
        path = Path(raw.strip('"')).expanduser()

    if not path.is_file():
        raise ToolError(f"ELF not found: {path}")
    return path


def read_elf_bytes(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) < 52:
        raise ToolError(f"{path} is too small to be a 32-bit ELF")
    if data[:4] != b"\x7fELF":
        raise ToolError(f"{path} does not look like an ELF file")
    if data[4] != 1 or data[5] != 1:
        raise ToolError(f"{path} is not a little-endian 32-bit ELF")
    return data


def resolve_output_path(value: str | None, default_name: str) -> Path:
    path = Path(value).expanduser() if value else SCRIPT_DIR / default_name
    if path.exists() and path.is_dir():
        path = path / default_name
    return path


def write_json(path: Path, payload: Any, **kwargs: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        json.dump(payload, handle, **kwargs)


def fail(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)
