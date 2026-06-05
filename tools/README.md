# tools

Offline asset tooling for Silent Hill 4. These are developer utilities, separate
from the runtime `scripts/` folder (game-side log/Lua) and `scripts/deploy.ps1`
(build helper).

## sh4arc.py

A single, dependency-free Python 3.8+ tool that unpacks and repacks Silent Hill 4
container archives. It is a from-scratch port and merge of two upstream C# tools:

- [`HunterStanton/sh4pac`](https://github.com/HunterStanton/sh4pac) — `.pac` sound archives (`SDPA`)
- [`HunterStanton/sh4bin`](https://github.com/HunterStanton/sh4bin) — `.bin` chunk archives (textures/mesh/anim/…)

The on-disk behaviour is kept byte-compatible with the originals, so a repacked
archive matches the game's file when the contents are unchanged.

### Usage

```text
python sh4arc.py analyze <archive>
python sh4arc.py unpack  <archive> <out_dir>
python sh4arc.py pack    <in_dir>  <archive>
```

The format is auto-detected (magic → extension → structure heuristic). Override
with `--format {pac,bin}` if detection guesses wrong.

```powershell
# Inspect a sound archive
python tools\sh4arc.py analyze "C:\GOG Games\Silent Hill 4\sound\foley.pac"

# Unpack, edit, repack a bin
python tools\sh4arc.py unpack chr_henry.bin henry_chunks
python tools\sh4arc.py pack   henry_chunks  chr_henry.bin
```

### Manifest sidecar (`_sh4arc.json`)

`unpack` writes a small `_sh4arc.json` next to the extracted files recording the
exact emit order plus anything not recoverable from the loose files (`.pac` sound
tags; the `.bin` header padding). `pack` honors it for byte-exact repacks, so
editing a file's *contents* and repacking reproduces the original layout.

This matters because `.bin` header padding is **not** a function of chunk count —
single-chunk bins ship with both `0x10` and `0x80` padding — so it can't be
re-derived from the files alone. If you add/remove/rename files, the manifest is
discarded (with a warning) and the tool falls back to natural sort plus the
count-based padding heuristic, so hand-assembled directories still work.

### Important constraints (inherited from the formats)

- **Order matters.** SH4 references sounds/chunks by index. Repacking uses the
  manifest order, or natural order (`0,1,…,10,11`) as a fallback. Don't rename or
  reorder extracted files, and don't add/remove entries — the game has no way to
  reach extra entries and will crash on missing ones.
- **`.pac` sound tags** are stored both in the manifest and in the filename
  (`{index}_{tag}_.wav`), so the tag survives a round trip either way.
- **`.bin` header padding** is taken from the manifest. Without one, the packer
  falls back to the game's count-based table and refuses unsupported counts
  rather than emitting an archive the game would corrupt or crash on.
- **`/movie/*.pac`** are raw MPEG video with a `.pac` extension, *not* this format.
  They lack the `SDPA` magic and are rejected.

### Verified against the GOG release

Every archive in a stock `C:\GOG Games\Silent Hill 4` install round-trips
byte-for-byte: all 7 `sound/*.pac` and 689 of 690 `data/*.bin`. The one exception
is a 0-byte placeholder (`message_pasthome0_k.bin`), which is reported as invalid
rather than crashing.

### Architecture

Each container format is a small handler class (`PacArchive`, `BinArchive`)
implementing a common protocol — `sniff` / `analyze` / `unpack` / `pack` — and the
`HANDLERS` registry is what the CLI dispatches through. Auto-detection tries each
handler's magic/structural `sniff`, then falls back to file extension. Supporting
another SH4 archive type later is just adding one more handler class to that list.

## dwarf1/

Repeatable recovery tooling for the PS2 CodeWarrior DWARF v1 debug information
left in the `SLUS_208.73` SH4 E3 2004 trial ELF. The production path is:

```text
python tools\dwarf1\extract.py
```

That regenerates `tools/dwarf1/model.json`, which the Ghidra scripts in
`tools/dwarf1/ghidra/` consume in this order:

1. `Sh4Types.java`
2. `Sh4Funcs.java`
3. `Sh4Globals.java`

Generated JSON outputs are ignored because they are reproducible from the trial
binary. Keep durable analysis scripts here; keep one-off probe scripts and local
caches out of git.
