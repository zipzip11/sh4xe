#!/usr/bin/env python3
"""Generate reviewable C++ SDK headers from tools/sdk/generated/ps2_model.json."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MODEL = ROOT / "tools" / "sdk" / "generated" / "ps2_model.json"
DEFAULT_OUT_DIR = ROOT / "tools" / "sdk" / "generated" / "include" / "sh4"


def load_json(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            return json.load(handle)
    except FileNotFoundError as exc:
        raise SystemExit(f"missing SDK model: {path}\nrun tools/sdk/export_ps2_model.py first") from exc


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def hex32(value: int | None) -> str:
    return "0x00000000" if value is None else f"0x{value:08X}"


def cpp_string(value: str | None) -> str:
    return json.dumps(value or "")


def unique_name(name: str, seen: set[str]) -> str:
    base = name or "unnamed"
    candidate = base
    suffix = 2
    while candidate in seen:
        candidate = f"{base}_{suffix}"
        suffix += 1
    seen.add(candidate)
    return candidate


def enum_base(size: int | None) -> str:
    if size == 1:
        return "std::int8_t"
    if size == 2:
        return "std::int16_t"
    return "std::int32_t"


def emit_types_header(model: dict[str, Any]) -> str:
    lines = [
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace sh4::ps2",
        "{",
        "",
        "// Generated from the PS2 E3 DWARF model. Structs are forward declarations",
        "// until their layouts are manually validated for PC-runtime use.",
        "",
    ]

    seen_types: set[str] = set()
    for item in model.get("types", []):
        if item.get("kind") not in {"struct", "union"}:
            continue
        name = unique_name(item.get("name", "anon"), seen_types)
        size = item.get("size")
        kind = "union" if item.get("kind") == "union" else "struct"
        lines.append(f"// {kind} {name}: PS2 size={hex(size) if isinstance(size, int) else 'unknown'} die={hex(item.get('die', 0))}")
        for member in item.get("members", [])[:24]:
            off = member.get("offset")
            off_text = "????" if off is None else f"{off:04X}"
            lines.append(f"//   +0x{off_text} {member.get('type', 'void')} {member.get('name', 'field')}")
        if len(item.get("members", [])) > 24:
            lines.append("//   ...")
        lines.append(f"{kind} {name};")
        lines.append("")

    seen_enums: set[str] = set()
    for item in model.get("types", []):
        if item.get("kind") != "enum":
            continue
        name = unique_name(item.get("name", "anon_enum"), seen_enums)
        lines.append(f"enum class {name} : {enum_base(item.get('size'))}")
        lines.append("{")
        seen_values: set[str] = set()
        for const in item.get("constants", []):
            const_name = unique_name(const.get("name", "value"), seen_values)
            lines.append(f"    {const_name} = {const.get('value', 0)},")
        lines.append("};")
        lines.append("")

    lines.append("} // namespace sh4::ps2")
    lines.append("")
    return "\n".join(lines)


def emit_functions_header(model: dict[str, Any]) -> str:
    lines = [
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace sh4::ps2",
        "{",
        "",
        "struct FunctionInfo",
        "{",
        "    const char* name;",
        "    std::uint32_t low;",
        "    std::uint32_t high;",
        "    const char* return_type;",
        "    const char* source;",
        "};",
        "",
        "inline constexpr FunctionInfo kFunctions[] = {",
    ]
    for func in model.get("functions", []):
        name = func.get("name", "")
        ret = func.get("return_type", "void")
        source = func.get("source") or ""
        lines.append(
            f"    {{{cpp_string(name)}, {hex32(func.get('ps2_low'))}, {hex32(func.get('ps2_high'))}, "
            f"{cpp_string(ret)}, {cpp_string(source)}}},"
        )
    lines.extend(
        [
            "};",
            "",
            "} // namespace sh4::ps2",
            "",
        ]
    )
    return "\n".join(lines)


def emit_globals_header(model: dict[str, Any]) -> str:
    lines = [
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace sh4::ps2",
        "{",
        "",
        "struct GlobalInfo",
        "{",
        "    const char* name;",
        "    std::uint32_t addr;",
        "    const char* type;",
        "};",
        "",
        "inline constexpr GlobalInfo kGlobals[] = {",
    ]
    for glob in model.get("globals", []):
        lines.append(
            f"    {{{cpp_string(glob.get('name', ''))}, {hex32(glob.get('ps2_addr'))}, "
            f"{cpp_string(glob.get('type', 'void'))}}},"
        )
    lines.extend(
        [
            "};",
            "",
            "} // namespace sh4::ps2",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate reviewable PS2 SDK headers from normalized SDK JSON.")
    parser.add_argument("model", nargs="?", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("-o", "--out-dir", type=Path, default=DEFAULT_OUT_DIR)
    args = parser.parse_args()

    model = load_json(args.model)
    write_text(args.out_dir / "types_ps2.h", emit_types_header(model))
    write_text(args.out_dir / "funcs_ps2.h", emit_functions_header(model))
    write_text(args.out_dir / "globals_ps2.h", emit_globals_header(model))
    print(f"wrote headers under {args.out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
