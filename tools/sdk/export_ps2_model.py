#!/usr/bin/env python3
"""Normalize tools/dwarf1/model.json into SDK-oriented JSON.

The DWARF extractor keeps a faithful low-level model for Ghidra import. This
script reshapes that data into stable, reviewable SDK input: sanitized names,
rendered C-ish type strings, source paths, aggregate layouts, PS2 function
addresses, and globals. It deliberately does not infer Windows PC addresses.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MODEL = ROOT / "tools" / "dwarf1" / "model.json"
DEFAULT_OUT = ROOT / "tools" / "sdk" / "generated" / "ps2_model.json"

FUNDAMENTAL_TYPES = {
    0x0001: "char",
    0x0002: "signed char",
    0x0003: "unsigned char",
    0x0004: "short",
    0x0005: "signed short",
    0x0006: "unsigned short",
    0x0007: "int",
    0x0008: "int",
    0x0009: "unsigned int",
    0x000A: "long",
    0x000B: "long",
    0x000C: "unsigned long",
    0x000E: "float",
    0x000F: "double",
    0x0010: "long double",
    0x0014: "void",
    0x0015: "bool",
    0x8008: "long long",
    0x8108: "unsigned long long",
    0x8208: "long long",
}

IDENT_RE = re.compile(r"[^A-Za-z0-9_]")


def load_json(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            return json.load(handle)
    except FileNotFoundError as exc:
        raise SystemExit(f"missing input model: {path}\nrun tools/dwarf1/extract.py first") from exc
    except json.JSONDecodeError as exc:
        raise SystemExit(f"invalid JSON in {path}: {exc}") from exc


def write_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(data, handle, indent=2, sort_keys=True)
        handle.write("\n")


def sanitize_identifier(name: str | None, fallback: str) -> str:
    if not name:
        name = fallback
    name = name.replace("::", "_")
    name = IDENT_RE.sub("_", name)
    name = re.sub(r"_+", "_", name).strip("_")
    if not name:
        name = fallback
    if name[0].isdigit():
        name = "_" + name
    return name


class TypeRenderer:
    def __init__(self, types: dict[str, Any]) -> None:
        self.types = types

    def type_record(self, die_off: int | str | None) -> dict[str, Any] | None:
        if die_off is None:
            return None
        return self.types.get(str(die_off))

    def type_name(self, die_off: int | str | None) -> str:
        rec = self.type_record(die_off)
        fallback = f"anon_{die_off:x}" if isinstance(die_off, int) else f"anon_{die_off}"
        if not rec:
            return fallback
        return sanitize_identifier(rec.get("name"), fallback)

    def render(self, ref: dict[str, Any] | None) -> str:
        if not ref:
            return "void"

        kind = ref.get("k")
        if kind == "f":
            return FUNDAMENTAL_TYPES.get(int(ref.get("t", 0)), f"fund_{int(ref.get('t', 0)):x}")
        if kind == "u":
            rec = self.type_record(ref.get("o"))
            name = self.type_name(ref.get("o"))
            if rec and rec.get("kind") in {"struct", "union", "enum"}:
                return name
            return name
        if kind == "ptr":
            return f"{self.render(ref.get('e'))} *"
        if kind == "const":
            return f"const {self.render(ref.get('e'))}"
        if kind == "vol":
            return f"volatile {self.render(ref.get('e'))}"
        return "void"


def normalize_types(model: dict[str, Any], renderer: TypeRenderer) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for die, rec in sorted(model.get("types", {}).items(), key=lambda item: int(item[0])):
        kind = rec.get("kind")
        if kind not in {"struct", "union", "enum"}:
            continue

        item: dict[str, Any] = {
            "die": int(die),
            "kind": kind,
            "name": sanitize_identifier(rec.get("name"), f"anon_{int(die):x}"),
            "original_name": rec.get("name"),
            "size": rec.get("size", 0),
        }
        if kind in {"struct", "union"}:
            members = []
            for index, member in enumerate(rec.get("members", [])):
                members.append(
                    {
                        "name": sanitize_identifier(member.get("name"), f"field_{index:x}"),
                        "original_name": member.get("name"),
                        "offset": member.get("off"),
                        "type": renderer.render(member.get("ref")),
                        "ref": member.get("ref"),
                    }
                )
            item["members"] = members
        else:
            item["constants"] = [
                {"name": sanitize_identifier(name, f"value_{index:x}"), "original_name": name, "value": value}
                for index, (name, value) in enumerate(rec.get("consts", []))
            ]
        out.append(item)
    return out


def normalize_functions(model: dict[str, Any], renderer: TypeRenderer) -> list[dict[str, Any]]:
    files = model.get("files", [])
    out = []
    for func in sorted(model.get("funcs", []), key=lambda item: item.get("low", 0)):
        params = []
        for index, param in enumerate(func.get("params", [])):
            params.append(
                {
                    "name": sanitize_identifier(param.get("name"), f"arg{index}"),
                    "original_name": param.get("name"),
                    "type": renderer.render(param.get("ref")),
                    "loc": param.get("loc"),
                    "ref": param.get("ref"),
                }
            )
        file_id = func.get("file")
        out.append(
            {
                "name": sanitize_identifier(func.get("name"), f"sub_{func.get('low', 0):x}"),
                "original_name": func.get("name"),
                "ps2_low": func.get("low"),
                "ps2_high": func.get("high"),
                "return_type": renderer.render(func.get("ret")),
                "params": params,
                "source": files[file_id] if isinstance(file_id, int) and file_id < len(files) else None,
            }
        )
    return out


def normalize_globals(model: dict[str, Any], renderer: TypeRenderer) -> list[dict[str, Any]]:
    out = []
    for glob in sorted(model.get("globals", []), key=lambda item: item.get("addr", 0)):
        out.append(
            {
                "name": sanitize_identifier(glob.get("name"), f"global_{glob.get('addr', 0):x}"),
                "original_name": glob.get("name"),
                "ps2_addr": glob.get("addr"),
                "type": renderer.render(glob.get("ref")),
                "ref": glob.get("ref"),
            }
        )
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description="Export normalized PS2 E3 SDK data from DWARF model.json.")
    parser.add_argument("model", nargs="?", type=Path, default=DEFAULT_MODEL, help="path to tools/dwarf1/model.json")
    parser.add_argument("-o", "--output", type=Path, default=DEFAULT_OUT, help="output SDK JSON path")
    args = parser.parse_args()

    model = load_json(args.model)
    renderer = TypeRenderer(model.get("types", {}))
    sdk = {
        "meta": {
            "source": str(args.model),
            "image_base": model.get("meta", {}).get("image_base"),
            "dwarf_version": model.get("meta", {}).get("dwarf_version"),
            "producer": model.get("meta", {}).get("producer"),
            "note": "PS2 E3 recovered data only; PC addresses require separate validation.",
        },
        "types": normalize_types(model, renderer),
        "functions": normalize_functions(model, renderer),
        "globals": normalize_globals(model, renderer),
    }
    write_json(args.output, sdk)
    print(f"wrote {args.output}")
    print(f"  types     : {len(sdk['types'])}")
    print(f"  functions : {len(sdk['functions'])}")
    print(f"  globals   : {len(sdk['globals'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
