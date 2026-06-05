#!/usr/bin/env python3
"""
Full DWARF v1 walk of SLUS_208.73 (.debug + .line), producing a quantified
summary of what source-level info is recoverable. Read-only. Emits summary.json.

DWARF v1 layout recap (little-endian here):
  .debug  = flat stream of DIEs. Each DIE:
              u32 length   (bytes of THIS die incl. the length word; children
                            follow inline, located via AT_sibling)
              u16 tag      (TAG_*)
              attributes until length is consumed; each attribute:
                u16 name|form   (low nibble = FORM_*, rest = AT_*)
                value sized by FORM
            A "null entry" is length<6 (just padding / sibling-chain end).
  .line   = per-CU line program: u32 len, u32 base_pc, then entries of
              {u32 line, u16 col, u32 pc_offset}.
"""
import argparse
import struct
import collections

from tool_paths import (
    add_elf_argument,
    add_output_argument,
    fail,
    read_elf_bytes,
    resolve_elf_path,
    resolve_output_path,
    write_json,
)


parser = argparse.ArgumentParser(description="Inventory recoverable DWARF v1 data in the SH4 E3 trial ELF.")
add_elf_argument(parser)
add_output_argument(parser, "summary.json")
args = parser.parse_args()

try:
    ELF = resolve_elf_path(args.elf)
    OUT = resolve_output_path(args.output, "summary.json")
    data = read_elf_bytes(ELF)
except Exception as exc:
    fail(str(exc))

# ---- ELF section table ----
(e_shoff,) = struct.unpack_from("<I", data, 32)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 46)
secs = []
for i in range(e_shnum):
    o = e_shoff + i*e_shentsize
    nm, typ, fl, addr, off, sz, lnk, info, al, ent = struct.unpack_from("<IIIIIIIIII", data, o)
    secs.append([nm, typ, off, sz, ent, lnk, info])
shstr_off = secs[e_shstrndx][2]
def sname(o):
    e = data.find(b"\0", shstr_off+o); return data[shstr_off+o:e].decode("latin1")
byname = {}
for s in secs:
    byname[sname(s[0])] = dict(off=s[2], size=s[3], ent=s[4], link=s[5], info=s[6])

if ".debug" not in byname or ".line" not in byname:
    fail(f"{ELF} is missing required .debug/.line sections")

dbg = byname[".debug"]; lin = byname[".line"]
DBASE, DSIZE = dbg["off"], dbg["size"]

# ---- DWARF v1 constants ----
TAG = {0x0001:"array_type",0x0002:"class_type",0x0003:"entry_point",
 0x0004:"enumeration_type",0x0005:"formal_parameter",0x0006:"global_subroutine",
 0x0007:"global_variable",0x0008:"imported_declaration",0x000a:"label",
 0x000b:"lexical_block",0x000c:"local_variable",0x000d:"member",
 0x000f:"pointer_type",0x0010:"reference_type",0x0011:"compile_unit",
 0x0012:"string_type",0x0013:"structure_type",0x0014:"subroutine",
 0x0015:"subroutine_type",0x0016:"typedef",0x0017:"union_type",
 0x0018:"unspecified_parameters",0x0019:"variant",0x001a:"common_block",
 0x001b:"common_inclusion",0x001c:"inheritance",0x001d:"inlined_subroutine",
 0x001e:"module",0x001f:"ptr_to_member_type",0x0020:"set_type",
 0x0021:"subrange_type",0x0022:"with_stmt"}
AT = {0x0010:"sibling",0x0020:"location",0x0030:"name",0x0050:"fund_type",
 0x0060:"mod_fund_type",0x0070:"user_def_type",0x0080:"mod_u_d_type",
 0x0090:"ordering",0x00a0:"subscr_data",0x00b0:"byte_size",0x00c0:"bit_offset",
 0x00d0:"bit_size",0x00f0:"element_list",0x0100:"stmt_list",0x0110:"low_pc",
 0x0120:"high_pc",0x0130:"language",0x0140:"member",0x0150:"discr",
 0x0160:"discr_value",0x0170:"visibility",0x0180:"import",0x0190:"string_length",
 0x01a0:"common_reference",0x01b0:"comp_dir",0x01c0:"const_value",
 0x01d0:"containing_type",0x01e0:"default_value",0x0200:"friends",
 0x0210:"inline",0x0220:"is_optimized",0x0230:"abstract_origin",
 0x0240:"extension",0x0250:"prototyped",0x0260:"specification",
 0x0270:"lower_bound",0x0290:"producer",0x02a0:"return_addr",0x02e0:"upper_bound"}
FT = {0x0001:"char",0x0002:"signed char",0x0003:"unsigned char",0x0004:"short",
 0x0005:"signed short",0x0006:"unsigned short",0x0007:"int",0x0008:"signed int",
 0x0009:"unsigned int",0x000a:"long",0x000b:"signed long",0x000c:"unsigned long",
 0x000d:"pointer",0x000e:"float",0x000f:"double",0x0010:"long double",
 0x0011:"complex",0x0012:"double complex",0x0013:"long long?",0x0014:"void",
 0x0015:"bool",0x8008:"long long",0x8208:"signed long long",
 0x8108:"unsigned long long"}

def read_attr(p):
    """returns (name, form, value, newp) ; value type depends on form"""
    af = struct.unpack_from("<H", data, p)[0]; p += 2
    form = af & 0xf; name = af & 0xfff0
    if form == 0x1:   v = struct.unpack_from("<I", data, p)[0]; p += 4      # ADDR
    elif form == 0x2: v = struct.unpack_from("<I", data, p)[0]; p += 4      # REF
    elif form == 0x3:                                                       # BLOCK2
        n = struct.unpack_from("<H", data, p)[0]; p += 2; v = data[p:p+n]; p += n
    elif form == 0x4:                                                       # BLOCK4
        n = struct.unpack_from("<I", data, p)[0]; p += 4; v = data[p:p+n]; p += n
    elif form == 0x5: v = struct.unpack_from("<H", data, p)[0]; p += 2      # DATA2
    elif form == 0x6: v = struct.unpack_from("<I", data, p)[0]; p += 4      # DATA4
    elif form == 0x7: v = struct.unpack_from("<Q", data, p)[0]; p += 8      # DATA8
    elif form == 0x8:                                                       # STRING
        e = data.find(b"\0", p); v = data[p:e].decode("latin1"); p = e+1
    else:
        raise ValueError(f"bad form {form:#x} at {p:#x}")
    return name, form, v, p

# ---- flat walk ----
tag_hist = collections.Counter()
attr_hist = collections.Counter()
cu_paths = []
producers = collections.Counter()
languages = collections.Counter()
named_funcs = 0
func_with_pc = 0
globals_with_addr = 0
locals_count = 0
params_count = 0
structs = []      # (name, byte_size, [members])  -- sample only
enums = []        # (name, [consts]) -- sample only
typedefs = 0
unions = 0
struct_count = enum_count = array_count = ptr_count = 0
member_total = 0

# index DIEs for member grouping: store (offset, tag, attrs, end_with_children?)
# We'll do a single pass; to grab struct members, when we hit structure/union/enum
# we record its sibling, then collect following member/enumerator DIEs until sibling.
p = 0
n_dies = 0
# Pre-pass: build list of (offset, tag, length, attrs-dict-lite)
dies = []
while p + 4 <= DSIZE:
    length = struct.unpack_from("<I", data, DBASE+p)[0]
    if length < 6:
        p += 4 if length == 0 else length
        if length == 0:
            # avoid infinite loop on stray zeros: bail if too many
            pass
        if length < 4:
            break
        continue
    tag = struct.unpack_from("<H", data, DBASE+p+4)[0]
    end = p + length
    ap = DBASE + p + 6
    attrs = {}
    try:
        while ap < DBASE + end:
            name, form, v, ap2 = read_attr(ap)
            attrs[name] = (form, v)
            ap = ap2
    except Exception:
        pass
    dies.append((p, tag, length, attrs))
    n_dies += 1
    p = end

# build offset->index for sibling navigation
off2idx = {d[0]: i for i, d in enumerate(dies)}

for i, (off, tag, length, attrs) in enumerate(dies):
    tag_hist[TAG.get(tag, f"user_0x{tag:04x}" if tag>=0x8000 else f"0x{tag:04x}")] += 1
    for a in attrs: attr_hist[AT.get(a, f"0x{a:04x}")] += 1
    nm = attrs.get(0x0030, (None,None))[1]
    if tag == 0x0011:  # compile_unit
        if nm: cu_paths.append(nm)
        for acode in (0x0290,0x0250,0x0240,0x0258&0xfff0):
            pass
        # producer often stored as a string attr; pick any string attr that isn't name
        for acode,(form,v) in attrs.items():
            if form==0x8 and acode!=0x0030 and isinstance(v,str) and ("Metrowerks" in v or "Compiler" in v or "Assembler" in v):
                producers[v]+=1
        lang = attrs.get(0x0130,(None,None))[1]
        if lang is not None: languages[lang]+=1
    elif tag in (0x0006,0x0014,0x0015,0x0003):  # subroutine kinds
        if nm: named_funcs += 1
        if 0x0110 in attrs and 0x0120 in attrs: func_with_pc += 1
    elif tag == 0x0007:  # global_variable
        if 0x0020 in attrs: globals_with_addr += 1
    elif tag == 0x000c: locals_count += 1
    elif tag == 0x0005: params_count += 1
    elif tag == 0x000d: member_total += 1
    elif tag == 0x0016: typedefs += 1
    elif tag == 0x0017: unions += 1
    elif tag == 0x0013: struct_count += 1
    elif tag == 0x0004: enum_count += 1
    elif tag == 0x0001: array_count += 1
    elif tag == 0x000f: ptr_count += 1

# Sample a few real structs with members (walk children via sibling span)
def children_span(i):
    off, tag, length, attrs = dies[i]
    sib = attrs.get(0x0010,(None,None))[1]
    # children are dies between i+1 and the die whose offset == sib
    res = []
    j = i+1
    while j < len(dies):
        coff = dies[j][0]
        if sib is not None and coff >= sib: break
        res.append(j);
        # skip nested children of this child by jumping to its sibling if present
        csib = dies[j][3].get(0x0010,(None,None))[1]
        if csib is not None:
            # advance j to index of csib
            nj = off2idx.get(csib)
            if nj and nj > j: j = nj; continue
        j += 1
    return res

def loc_offset(block):
    # member location: typically OP_CONST(0x04?) variants; MW uses a leading
    # atom byte then u32. Grab the trailing u32 as best-effort offset.
    if isinstance(block, (bytes, bytearray)) and len(block) >= 5:
        return struct.unpack_from("<I", block, len(block)-4)[0]
    return None

sample_structs = []
for i,(off,tag,length,attrs) in enumerate(dies):
    if tag==0x0013 and 0x0030 in attrs and 0x00b0 in attrs:  # named struct w/ size
        size = attrs[0x00b0][1]
        if not (8 <= size <= 4096): continue
        mems=[]
        for ci in children_span(i):
            coff,ctag,clen,cattrs = dies[ci]
            if ctag==0x000d and 0x0030 in cattrs:
                moff = None
                if 0x0020 in cattrs: moff = loc_offset(cattrs[0x0020][1])
                ft = cattrs.get(0x0050,(None,None))[1]
                udt = cattrs.get(0x0070,(None,None))[1]
                tname = FT.get(ft) if ft is not None else (f"@{udt:#x}" if udt else "?")
                mems.append([cattrs[0x0030][1], moff, tname])
        if len(mems) >= 4:
            sample_structs.append([attrs[0x0030][1], size, mems])
        if len(sample_structs) >= 12: break

# sample functions with params
sample_funcs = []
for i,(off,tag,length,attrs) in enumerate(dies):
    if tag in (0x0006,0x0014) and 0x0030 in attrs and 0x0110 in attrs:
        ps=[]; locs=[]
        for ci in children_span(i):
            coff,ctag,clen,cattrs = dies[ci]
            if ctag==0x0005 and 0x0030 in cattrs:  # formal_parameter
                ft=cattrs.get(0x0050,(None,None))[1]; udt=cattrs.get(0x0070,(None,None))[1]
                ps.append([cattrs[0x0030][1], FT.get(ft) if ft is not None else (f"@{udt:#x}" if udt else "?")])
            elif ctag==0x000c and 0x0030 in cattrs:
                locs.append(cattrs[0x0030][1])
        if ps or locs:
            sample_funcs.append([attrs[0x0030][1],
                                 attrs[0x0110][1], attrs.get(0x0120,(None,None))[1],
                                 ps, len(locs)])
        if len(sample_funcs) >= 15: break

# ---- .line stats ----
LB, LS = lin["off"], lin["size"]
lp = 0; n_programs=0; n_line_entries=0; min_pc=0xffffffff; max_pc=0
while lp + 8 <= LS:
    plen = struct.unpack_from("<I", data, LB+lp)[0]
    base = struct.unpack_from("<I", data, LB+lp+4)[0]
    if plen < 8 or lp+plen > LS: break
    body = plen - 8
    nent = body // 10
    n_programs += 1
    n_line_entries += nent
    # sample first/last addr
    if nent:
        last = struct.unpack_from("<I", data, LB+lp+8+(nent-1)*10+6)[0]
        min_pc=min(min_pc, base); max_pc=max(max_pc, base+last)
    lp += plen

comment = byname.get(".comment")
comment_data = data[comment["off"]:comment["off"] + comment["size"]].split(b"\0") if comment else []
symtab = byname.get(".symtab")
symn = symtab["size"] // 16 if symtab else 0

summary = dict(
  producer=comment_data,
  debug_size=DSIZE, line_size=LS,
  total_dies=n_dies,
  tag_hist=dict(tag_hist.most_common()),
  attr_hist=dict(attr_hist.most_common(25)),
  compile_units=len(cu_paths),
  cu_sample=cu_paths[:60],
  producers=dict(producers),
  languages={hex(k):v for k,v in languages.items()},
  named_funcs=named_funcs, func_with_pc=func_with_pc,
  globals_with_addr=globals_with_addr,
  params_total=params_count, locals_total=locals_count,
  member_total=member_total, typedefs=typedefs, unions=unions,
  struct_count=struct_count, enum_count=enum_count,
  array_count=array_count, ptr_count=ptr_count,
  sample_structs=sample_structs[:12],
  sample_funcs=sample_funcs[:15],
  line_programs=n_programs, line_entries=n_line_entries,
  line_pc_min=hex(min_pc), line_pc_max=hex(max_pc),
  symtab_symbols=symn,
)

write_json(OUT, summary, indent=1, default=lambda o: o.decode("latin1") if isinstance(o, bytes) else str(o))
print(f"summary written: {OUT}")

# ---- console report ----
print(f"DIEs walked        : {n_dies:,}")
print(f".debug size        : {DSIZE:,} bytes")
print(f".line  size        : {LS:,} bytes  ({n_programs:,} line-programs, {n_line_entries:,} entries)")
print(f"  pc coverage      : {hex(min_pc)} .. {hex(max_pc)}")
print(f"compile units      : {len(cu_paths):,}")
print(f"named functions    : {named_funcs:,}  (with low/high pc: {func_with_pc:,})")
print(f"globals w/ address : {globals_with_addr:,}")
print(f"struct types       : {struct_count:,}   members total: {member_total:,}")
print(f"union types        : {unions:,}")
print(f"enum types         : {enum_count:,}")
print(f"typedefs           : {typedefs:,}")
print(f"pointer types      : {ptr_count:,}   array types: {array_count:,}")
print(f"params / locals    : {params_count:,} / {locals_count:,}")
print(f".symtab symbols    : {symn:,}")
print(f"languages          : {dict(languages)}")
print("\nTop tags:")
for k,v in tag_hist.most_common(20): print(f"   {k:<24} {v:>8,}")
print("\nSample source paths:")
for p_ in cu_paths[:25]: print("   ", p_)
print("\nSample structs (name | size | first members):")
for nm,sz,mems in sample_structs[:8]:
    ms = ", ".join(f"{m[0]}@{m[1]}" for m in mems[:6])
    print(f"   {nm:<22} sz={sz:<5} {ms}")
print("\nSample functions (name | pc | params):")
for nm,lo,hi,ps,nl in sample_funcs[:10]:
    pstr=", ".join(f"{t} {n}" for n,t in ps)
    print(f"   {nm:<28} {lo:#x} ({pstr}) locals={nl}")
