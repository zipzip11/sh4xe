#!/usr/bin/env python3
"""sh4arc - Silent Hill 4 archive tool.

A single, dependency-free unpacker/repacker for the Silent Hill 4 container
formats. It is a from-scratch port and merge of two upstream C# tools:

  * .pac sound archives (``SDPA`` magic)  -- HunterStanton/sh4pac
  * .bin chunk archives (textures/mesh/...) -- HunterStanton/sh4bin

Both originals are MIT licensed. This rewrite keeps the on-disk behaviour
byte-compatible so repacked archives match the game's originals, while sharing
one CLI, one natural-sort implementation, and one extensible format registry.

Usage:
    python sh4arc.py analyze <archive>
    python sh4arc.py unpack  <archive> <out_dir>
    python sh4arc.py pack    <in_dir>  <archive>

Format is auto-detected (magic -> extension -> structure heuristic). Override
with ``--format {pac,bin}`` if detection guesses wrong.

Architecture
------------
Each container format is a small handler class implementing a common protocol
(``sniff`` / ``analyze`` / ``unpack`` / ``pack``). ``HANDLERS`` is the registry
the CLI dispatches through. Supporting another SH4 archive type later is just a
matter of adding one more handler class to that list -- nothing else changes.

Pure standard library. Python 3.8+.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys
from typing import Dict, List, Optional, Sequence, Tuple

# Sidecar written on unpack and honoured on pack so repacks are byte-exact even
# for archives whose header layout can't be re-derived from the loose files
# alone (see BinArchive: padding is not a function of chunk count).
MANIFEST = "_sh4arc.json"

# --------------------------------------------------------------------------- #
# Binary helpers (Silent Hill 4 is little-endian on every platform: PC/PS2/Xbox)
# --------------------------------------------------------------------------- #


def _u32(buf: bytes, off: int) -> int:
    return struct.unpack_from("<I", buf, off)[0]


def _read_u32(f) -> int:
    return struct.unpack("<I", f.read(4))[0]


def _write_u32(f, value: int) -> None:
    f.write(struct.pack("<I", value & 0xFFFFFFFF))


# --------------------------------------------------------------------------- #
# Natural sort
#
# Repack order is load-bearing: SH4 references many chunks/sounds by index, so a
# repacked archive MUST emit files in the same order the unpacker named them
# (0, 1, 2, ... 10, 11 -- not the lexical 0, 1, 10, 11, 2). This reproduces the
# upstream C# ``CustomSort`` exactly: split each name into digit / non-digit
# runs, left-pad digit runs with spaces and non-digit runs with U+FFFF, then
# order by the padded string. Operating on basenames is equivalent to upstream's
# full-path sort because every entry shares the same directory prefix.
# --------------------------------------------------------------------------- #


def natural_sorted(names: Sequence[str]) -> List[str]:
    if not names:
        return []
    max_len = max(len(n) for n in names)

    def key(name: str) -> str:
        parts = []
        for run in re.findall(r"\d+|\D+", name):
            pad = " " if run[0].isdigit() else "\uffff"
            parts.append(run.rjust(max_len, pad))
        return "".join(parts)

    return sorted(names, key=key)


def _list_files(in_dir: str) -> List[str]:
    """Basenames of regular files in ``in_dir``, excluding the manifest."""
    return [
        name for name in os.listdir(in_dir)
        if name != MANIFEST and os.path.isfile(os.path.join(in_dir, name))
    ]


# --------------------------------------------------------------------------- #
# Manifest sidecar
#
# unpack() records the exact emit order (and any layout that isn't recoverable
# from the files themselves) so pack() can reproduce the original byte-for-byte.
# If the directory is edited so the file set no longer matches the manifest, we
# discard it and fall back to natural sort + the per-format heuristics, so
# hand-assembled directories still work.
# --------------------------------------------------------------------------- #


def _write_manifest(out_dir: str, data: dict) -> None:
    data = {"tool": "sh4arc", **data}
    with open(os.path.join(out_dir, MANIFEST), "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)


def _read_manifest(in_dir: str, fmt: Optional[str] = None) -> Optional[dict]:
    path = os.path.join(in_dir, MANIFEST)
    if not os.path.isfile(path):
        return None
    try:
        with open(path, encoding="utf-8") as f:
            man = json.load(f)
    except (ValueError, OSError):
        return None
    if man.get("tool") != "sh4arc":
        return None
    if fmt is not None and man.get("format") != fmt:
        return None
    return man


def _resolve_order(in_dir: str, fmt: str) -> Tuple[List[str], Optional[dict]]:
    """Return (ordered basenames, manifest-or-None).

    The manifest order is authoritative when it still matches the directory
    exactly; otherwise fall back to natural sort (warning if a stale manifest
    was present)."""
    present = _list_files(in_dir)
    man = _read_manifest(in_dir, fmt)
    if man and set(man.get("order", [])) == set(present):
        return list(man["order"]), man
    if man:
        print("  WARNING: directory contents changed since unpack; ignoring "
              "manifest and falling back to natural sort")
    return natural_sorted(present), None


class ArchiveError(Exception):
    """Raised for malformed archives or unsupported pack requests."""


# --------------------------------------------------------------------------- #
# PAC -- Silent Hill 4 sound archive (SILENT HILL 4/sound/*.pac)
#
# NOTE: the .pac files under /movie/ are raw MPEG-1/2 video with a .pac
# extension, NOT this format. They lack the SDPA magic, so sniff() rejects them.
#
# Layout (all little-endian):
#   char[4] magic = "SDPA"
#   u32     file_count
#   file_count x {
#       u32 offset       absolute offset of the sound payload
#       u32 size         payload length in bytes
#       u32 tag          per-sound value of unknown meaning, preserved verbatim
#   }
#   ... payloads ...
#
# The header is exactly 8 + file_count*12 bytes; payloads follow contiguously.
# Unpacked files are named "{index}_{tag}_.wav" so the tag survives a round trip.
# --------------------------------------------------------------------------- #


class PacArchive:
    name = "pac"
    description = "Silent Hill 4 .pac sound archive (SDPA)"
    extensions = (".pac",)
    MAGIC = b"SDPA"
    DEFAULT_TAG = 0x2C  # upstream fallback when a filename tag can't be parsed

    @classmethod
    def sniff(cls, head: bytes, file_size: int) -> bool:
        return head[:4] == cls.MAGIC

    @classmethod
    def _read_header(cls, f) -> List[Tuple[int, int, int]]:
        if f.read(4) != cls.MAGIC:
            raise ArchiveError("not a valid .pac file (missing SDPA magic)")
        count = _read_u32(f)
        return [
            (_read_u32(f), _read_u32(f), _read_u32(f))  # offset, size, tag
            for _ in range(count)
        ]

    @classmethod
    def analyze(cls, path: str) -> None:
        with open(path, "rb") as f:
            entries = cls._read_header(f)
        print(f"format          : {cls.description}")
        print(f"sounds          : {len(entries)}")
        print("-" * 52)
        for i, (offset, size, tag) in enumerate(entries):
            print(f"[{i}] offset=0x{offset:08X} size={size} tag={tag} (0x{tag:X})")

    @classmethod
    def unpack(cls, path: str, out_dir: str) -> None:
        with open(path, "rb") as f:
            entries = cls._read_header(f)
            os.makedirs(out_dir, exist_ok=True)
            print(f"sounds in .pac  : {len(entries)}")
            order: List[str] = []
            tags: Dict[str, int] = {}
            for i, (offset, size, tag) in enumerate(entries):
                f.seek(offset)
                payload = f.read(size)
                out_name = f"{i}_{tag}_.wav"
                with open(os.path.join(out_dir, out_name), "wb") as out:
                    out.write(payload)
                order.append(out_name)
                tags[out_name] = tag
                print(f"  extracted {out_name} ({size} bytes)")
        _write_manifest(out_dir, {"format": cls.name, "order": order, "tags": tags})

    @staticmethod
    def _tag_from_name(path: str) -> Optional[int]:
        # Canonical name is "{index}_{tag}_.wav"; pull the token between the
        # first and second underscore of the basename.
        tokens = os.path.basename(path).split("_")
        if len(tokens) >= 2:
            try:
                return int(tokens[1])
            except ValueError:
                return None
        return None

    @classmethod
    def pack(cls, in_dir: str, path: str) -> None:
        order, man = _resolve_order(in_dir, cls.name)
        tag_map = (man or {}).get("tags", {})
        count = len(order)
        print(f"sounds in .pac  : {count}")

        header_size = 8 + (count * 3) * 4
        body = bytearray()
        with open(path, "wb") as f:
            f.write(cls.MAGIC)
            _write_u32(f, count)
            running = 0
            for name in order:
                src = os.path.join(in_dir, name)
                size = os.path.getsize(src)
                _write_u32(f, header_size + running)
                _write_u32(f, size)
                tag = tag_map.get(name)
                if tag is None:
                    tag = cls._tag_from_name(name)
                if tag is None:
                    tag = cls.DEFAULT_TAG
                    print(f"  WARNING: no tag for '{name}', defaulting to "
                          f"0x{cls.DEFAULT_TAG:X}; the game may behave incorrectly")
                _write_u32(f, tag)
                running += size
                with open(src, "rb") as inp:
                    body += inp.read()
            f.write(body)
        print(f"wrote {path}")


# --------------------------------------------------------------------------- #
# BIN -- Silent Hill 4 chunk archive (textures, meshes, animations, ...)
#
# Layout (all little-endian):
#   u32   file_count
#   file_count x u32 offset    absolute offset of each chunk
#   ... zero padding up to a count-dependent header size ...
#   ... chunks ...
#
# There are no stored sizes: chunk N spans [offset[N], offset[N+1]), and the
# last chunk runs to EOF. The header is zero-padded to a "safe" size chosen from
# the chunk count -- the game crashes with arbitrary padding, so the original
# bins only use the discrete sizes reproduced in _header_padding().
# --------------------------------------------------------------------------- #


class BinArchive:
    name = "bin"
    description = "Silent Hill 4 .bin chunk archive"
    extensions = (".bin",)

    @staticmethod
    def _identify(magic: int, magic2: int) -> Tuple[str, str]:
        """Return (human_name, extension) from the chunk's leading two u16s."""
        if magic == magic2:
            return ("Textures", ".textures")
        if magic == 0x7000 and magic2 == 0x0FC0:
            return ("Shadow mesh", ".shadow_mesh")
        if magic == 0xFF11:
            return ("World collision mesh", ".coll_mesh")
        if magic == 0x0003:
            return ("Object 3D mesh", ".mesh")
        if magic == 0x8581:
            return (".SDB file", ".sdb")
        if magic == 0x4554:
            return ("Monster ID list (unused by the game)", ".monsterIDList")
        if magic == 0x4C53:
            return ("SLGT file", ".slgt")
        if magic == 0x0001 and magic2 == 0xFF01:
            return ("Animation", ".anim")
        if magic == 0x0001 and magic2 == 0xFC03:
            return ("World 3D Mesh", ".world_mesh")
        return ("Unknown chunk type", ".chunk")

    @classmethod
    def sniff(cls, head: bytes, file_size: int) -> bool:
        # No magic -- validate structurally: a plausible count followed by
        # strictly increasing offsets that sit past the header and inside file.
        if len(head) < 8:
            return False
        count = _u32(head, 0)
        if count <= 0 or count > 0x10000:
            return False
        header_end = 4 + 4 * count
        if file_size < header_end or len(head) < header_end:
            return False
        prev = header_end - 1
        for i in range(count):
            offset = _u32(head, 4 + 4 * i)
            if offset <= prev or offset > file_size:
                return False
            prev = offset
        return True

    @staticmethod
    def _read_offsets(f, file_size: int) -> List[int]:
        if file_size < 8:
            raise ArchiveError("file is too small to be a .bin archive")
        count = _read_u32(f)
        if count <= 0 or 4 + 4 * count > file_size:
            raise ArchiveError(
                f"not a valid .bin archive (implausible chunk count {count})"
            )
        return [_read_u32(f) for _ in range(count)]

    @classmethod
    def _classify(cls, data: bytes) -> Tuple[str, str]:
        # Some shipped chunks are shorter than the 4-byte magic probe.
        if len(data) < 4:
            return ("Empty/short chunk", ".chunk")
        magic, magic2 = struct.unpack_from("<HH", data, 0)
        return cls._identify(magic, magic2)

    @classmethod
    def analyze(cls, path: str) -> None:
        file_size = os.path.getsize(path)
        with open(path, "rb") as f:
            offsets = cls._read_offsets(f, file_size)
            print(f"format          : {cls.description}")
            print(f"chunks          : {len(offsets)}")
            print("-" * 52)
            for i, offset in enumerate(offsets):
                end = offsets[i + 1] if i + 1 < len(offsets) else file_size
                f.seek(offset)
                kind, _ = cls._classify(f.read(min(4, end - offset)))
                print(f"[{i}] offset=0x{offset:08X} size={end - offset:>8} {kind}")

    @classmethod
    def unpack(cls, path: str, out_dir: str) -> None:
        file_size = os.path.getsize(path)
        with open(path, "rb") as f:
            offsets = cls._read_offsets(f, file_size)
            os.makedirs(out_dir, exist_ok=True)
            print(f"chunks in .bin  : {len(offsets)}")
            order: List[str] = []
            for i, offset in enumerate(offsets):
                end = offsets[i + 1] if i + 1 < len(offsets) else file_size
                f.seek(offset)
                data = f.read(end - offset)
                kind, ext = cls._classify(data)
                out_name = f"{i}{ext}"
                with open(os.path.join(out_dir, out_name), "wb") as out:
                    out.write(data)
                order.append(out_name)
                print(f"  extracted {out_name} ({len(data)} bytes) [{kind}]")
        # Record the real header size (offset of the first chunk). It is NOT a
        # function of chunk count -- single-chunk bins use both 0x10 and 0x80 in
        # the shipped game -- so without this a repack can't be byte-exact.
        _write_manifest(out_dir, {
            "format": cls.name,
            "padding": offsets[0] if offsets else 0,
            "order": order,
        })

    @staticmethod
    def _header_padding(count: int) -> int:
        # Best-effort fallback for hand-assembled directories with no manifest.
        # The game only tolerates a discrete set of header sizes; picking the
        # wrong one loads but corrupts/crashes later. Mirror the upstream table.
        if count < 0x1F:
            return 0x80
        if 0x1F < count < 0x3F:
            return 0x100
        if 0x3F < count < 0x5F:
            return 0x180
        if 0x5F < count < 0x7F:
            return 0x200
        if 0x7F < count < 0x9F:
            return 0x280
        if 0x9F < count < 0xBF:
            return 0x300
        return 0  # boundary values / very large counts are unsupported

    @classmethod
    def pack(cls, in_dir: str, path: str) -> None:
        order, man = _resolve_order(in_dir, cls.name)
        count = len(order)
        print(f"chunks in .bin  : {count}")

        header_size = 4 + 4 * count
        # Prefer the original padding from the manifest; otherwise fall back to
        # the count-based heuristic table.
        if man is not None and man.get("padding"):
            padding = man["padding"]
        else:
            padding = cls._header_padding(count)
        if padding == 0 or padding < header_size:
            raise ArchiveError(
                f"unsupported chunk count {count}: the game has no known-safe "
                f"header padding for it (boundary values 0x1F/0x3F/0x5F/0x7F/"
                f"0x9F/0xBF and counts >= 0xBF are not supported). Unpack with "
                f"sh4arc to get a manifest that records the exact padding."
            )

        body = bytearray()
        with open(path, "wb") as f:
            _write_u32(f, count)
            running = 0
            for name in order:
                src = os.path.join(in_dir, name)
                _write_u32(f, padding + running)
                running += os.path.getsize(src)
                with open(src, "rb") as inp:
                    body += inp.read()
            f.write(b"\x00" * (padding - header_size))  # zero-pad the header
            f.write(body)
        print(f"wrote {path}")


# --------------------------------------------------------------------------- #
# Format registry + detection
# --------------------------------------------------------------------------- #

HANDLERS = (PacArchive, BinArchive)
_BY_NAME = {h.name: h for h in HANDLERS}


def _detect_for_archive(path: str, forced: Optional[str]):
    """Pick a handler for an existing archive: magic -> extension -> heuristic."""
    if forced:
        return _BY_NAME[forced]

    file_size = os.path.getsize(path)
    with open(path, "rb") as f:
        head = f.read(min(file_size, 1 << 16))

    for handler in HANDLERS:  # magic / structural sniff
        if handler.sniff(head, file_size):
            return handler

    ext = os.path.splitext(path)[1].lower()  # extension fallback
    for handler in HANDLERS:
        if ext in handler.extensions:
            return handler

    raise ArchiveError(
        f"could not determine the format of '{path}'. "
        f"Pass --format {{{','.join(_BY_NAME)}}} to force one."
    )


def _detect_for_pack(in_dir: str, out_path: str, forced: Optional[str]):
    """Pick a handler when packing: --format -> manifest -> extension -> contents."""
    if forced:
        return _BY_NAME[forced]

    man = _read_manifest(in_dir)  # written by unpack; authoritative
    if man and man.get("format") in _BY_NAME:
        return _BY_NAME[man["format"]]

    ext = os.path.splitext(out_path)[1].lower()
    for handler in HANDLERS:
        if ext in handler.extensions:
            return handler

    # Content heuristic: PAC unpacks to "*_*_.wav", BIN to numbered chunks.
    names = _list_files(in_dir)
    if any(re.match(r"\d+_.*_\.wav$", n) for n in names):
        return PacArchive

    raise ArchiveError(
        f"could not infer the target format from '{out_path}'. "
        f"Pass --format {{{','.join(_BY_NAME)}}} or use a .pac/.bin extension."
    )


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="sh4arc",
        description="Unpack/repack Silent Hill 4 .pac and .bin archives.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    fmt_choices = list(_BY_NAME)

    p_analyze = sub.add_parser("analyze", help="print archive structure")
    p_analyze.add_argument("archive")
    p_analyze.add_argument("--format", choices=fmt_choices)

    p_unpack = sub.add_parser("unpack", help="extract an archive to a directory")
    p_unpack.add_argument("archive")
    p_unpack.add_argument("out_dir")
    p_unpack.add_argument("--format", choices=fmt_choices)

    p_pack = sub.add_parser("pack", help="build an archive from a directory")
    p_pack.add_argument("in_dir")
    p_pack.add_argument("archive")
    p_pack.add_argument("--format", choices=fmt_choices)

    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        if args.command == "analyze":
            _detect_for_archive(args.archive, args.format).analyze(args.archive)
        elif args.command == "unpack":
            handler = _detect_for_archive(args.archive, args.format)
            print(f"detected format : {handler.description}")
            handler.unpack(args.archive, args.out_dir)
        elif args.command == "pack":
            handler = _detect_for_pack(args.in_dir, args.archive, args.format)
            print(f"target format   : {handler.description}")
            handler.pack(args.in_dir, args.archive)
    except (ArchiveError, FileNotFoundError, IsADirectoryError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
