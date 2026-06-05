#!/usr/bin/env python3
"""
Full DWARF v1 extractor for SLUS_208.73 -> model.json (the normalized model the
Ghidra applier consumes). Read-only on the ELF.

model.json:
  meta     : counts + image base
  types    : { die_off(str) : type-record }    type-defining DIEs
  funcs    : [ {name, low, high, ret, params:[{name,ref,loc}], locals:[...], file} ]
  globals  : [ {name, addr, ref} ]
  files    : [ path, ... ]                       file id table
  lines    : [ [addr, file_id, line], ... ]      sorted by addr (optional apply)

type-ref token (recursive, JSON):
  {"k":"f","t":<fund code>}          fundamental
  {"k":"u","o":<die_off>}            ref to a type-defining DIE
  {"k":"ptr","e":<ref>}              pointer to
  {"k":"const","e":<ref>} / "vol"    cv-qualified (Ghidra ignores; kept for fidelity)
"""
import struct, json, collections, sys

ELF = r"C:\Users\tux\Downloads\Silent Hill 4 - E3 Trial Version (E3 2004)\SILENT_HILL_4_VER_E3\SLUS_208.73"
data = open(ELF, "rb").read()
(e_shoff,) = struct.unpack_from("<I", data, 32)
ess, esn, esi = struct.unpack_from("<HHH", data, 46)
secs = [struct.unpack_from("<IIIIIIIIII", data, e_shoff+i*ess) for i in range(esn)]
sh = secs[esi][4]
def sn(o):
    e = data.find(b"\0", sh+o); return data[sh+o:e].decode("latin1")
byn = {sn(s[0]): s for s in secs}
DB, DS = byn[".debug"][4], byn[".debug"][5]
LB, LS = byn[".line"][4], byn[".line"][5]

def rattr(p):
    af = struct.unpack_from("<H", data, p)[0]; p += 2; form = af & 0xf; name = af & 0xfff0
    if form in (1, 2, 6): v = struct.unpack_from("<I", data, p)[0]; p += 4
    elif form == 3: n = struct.unpack_from("<H", data, p)[0]; p += 2; v = data[p:p+n]; p += n
    elif form == 4: n = struct.unpack_from("<I", data, p)[0]; p += 4; v = data[p:p+n]; p += n
    elif form == 5: v = struct.unpack_from("<H", data, p)[0]; p += 2
    elif form == 7: v = struct.unpack_from("<Q", data, p)[0]; p += 8
    elif form == 8:
        e = data.find(b"\0", p); v = data[p:e].decode("latin1"); p = e+1
    else: raise ValueError(f"form {form:#x}")
    return name, form, v, p

# ---- flat DIE walk ----
dies = []; p = 0
while p+4 <= DS:
    L = struct.unpack_from("<I", data, DB+p)[0]
    if L < 6:
        p += (4 if L == 0 else L)
        if L < 4: break
        continue
    tag = struct.unpack_from("<H", data, DB+p+4)[0]
    ap = DB+p+6; end = DB+p+L; at = {}
    try:
        while ap < end:
            nm, fm, v, ap = rattr(ap); at[nm] = (fm, v)
    except Exception:
        pass
    dies.append((p, tag, L, at)); p += L
off2idx = {d[0]: i for i, d in enumerate(dies)}

# AT codes
A_SIB,A_LOC,A_NAME,A_FT,A_MFT,A_UDT,A_MUD = 0x10,0x20,0x30,0x50,0x60,0x70,0x80
A_BYTE,A_SUBSCR,A_ELEM,A_LOW,A_HIGH = 0xb0,0xa0,0xf0,0x110,0x120
# tags
T_ARR,T_ENUM,T_FPARAM,T_GSUB,T_GVAR,T_LOCAL,T_MEMBER,T_PTR,T_CU = \
    0x01,0x04,0x05,0x06,0x07,0x0c,0x0d,0x0f,0x11
T_STRUCT,T_SUB,T_SUBT,T_UNION = 0x13,0x14,0x15,0x17

def aval(at, code, d=None):
    x = at.get(code); return x[1] if x else d

def die_name(idx, prefix="anon"):
    off, tag, L, at = dies[idx]
    nm = aval(at, A_NAME)
    return nm if nm else f"{prefix}_{off:x}"

# ---- type-ref resolution ----
def mod_wrap(mods, inner):
    for m in mods:
        if m == 0x01: inner = {"k": "ptr", "e": inner}
        elif m == 0x02: inner = {"k": "ptr", "e": inner}        # reference -> ptr
        elif m == 0x03: inner = {"k": "const", "e": inner}
        elif m == 0x04: inner = {"k": "vol", "e": inner}
        else: inner = {"k": "ptr", "e": inner}                  # unknown mod: be safe
    return inner

def typeref(at):
    if A_FT in at:   return {"k": "f", "t": aval(at, A_FT)}
    if A_UDT in at:  return {"k": "u", "o": aval(at, A_UDT)}
    if A_MFT in at:
        b = aval(at, A_MFT); ft = struct.unpack_from("<H", b, len(b)-2)[0]; mods = b[:-2]
        return mod_wrap(mods, {"k": "f", "t": ft})
    if A_MUD in at:
        b = aval(at, A_MUD); ref = struct.unpack_from("<I", b, len(b)-4)[0]; mods = b[:-4]
        return mod_wrap(mods, {"k": "u", "o": ref})
    return {"k": "f", "t": 0x14}     # void

# ---- subscript (array) decode: returns (elem_ref, count|None) ----
def decode_array(at):
    blk = aval(at, A_SUBSCR)
    if not isinstance(blk, (bytes, bytearray)):
        return {"k": "f", "t": 0x14}, None
    q = 0; count = None; elem = None
    try:
        while q < len(blk):
            fmt = blk[q]; q += 1
            if fmt in (0x0, 0x1, 0x2, 0x3):          # FT index, [bound][bound]
                idx_ft = struct.unpack_from("<H", blk, q)[0]; q += 2
                lo = hi = None
                if fmt == 0x0:   # C_C
                    lo = struct.unpack_from("<i", blk, q)[0]; q += 4
                    hi = struct.unpack_from("<i", blk, q)[0]; q += 4
                    count = hi - lo + 1
                else:
                    # X bounds are location blocks (block2) - skip them
                    for _ in range((0 if fmt==0x0 else (1 if fmt in (0x1,0x2) else 2))):
                        bl = struct.unpack_from("<H", blk, q)[0]; q += 2 + bl
                    if fmt == 0x1:  # C_X : const low, expr high
                        lo = struct.unpack_from("<i", blk, q-0)[0] if False else None
            elif fmt in (0x4,0x5,0x6,0x7):           # UT index
                struct.unpack_from("<I", blk, q)[0]; q += 4
                # bounds similar to above; best-effort skip
                if fmt == 0x4:
                    lo = struct.unpack_from("<i", blk, q)[0]; q += 4
                    hi = struct.unpack_from("<i", blk, q)[0]; q += 4
                    count = hi - lo + 1
            elif fmt == 0x8:                          # FMT_ET : element type
                # remaining = a type attribute: u16 (attr|form) then value
                nm, fmv, v, q2 = rattr(DB+0)  # placeholder; parse from blk instead
                # parse attr from blk directly
                af = struct.unpack_from("<H", blk, q)[0]; q += 2; form = af & 0xf; an = af & 0xfff0
                if form == 5:    # DATA2 -> fund
                    ft = struct.unpack_from("<H", blk, q)[0]; q += 2; elem = {"k": "f", "t": ft}
                elif form == 2:  # REF -> udt
                    rf = struct.unpack_from("<I", blk, q)[0]; q += 4; elem = {"k": "u", "o": rf}
                elif form == 3:  # BLOCK2 -> mod type
                    bl = struct.unpack_from("<H", blk, q)[0]; q += 2
                    sub = blk[q:q+bl]; q += bl
                    if an == A_MFT:
                        ft = struct.unpack_from("<H", sub, len(sub)-2)[0]; elem = mod_wrap(sub[:-2], {"k":"f","t":ft})
                    else:
                        rf = struct.unpack_from("<I", sub, len(sub)-4)[0]; elem = mod_wrap(sub[:-4], {"k":"u","o":rf})
                else:
                    break
                break
            else:
                break
    except Exception:
        pass
    return (elem or {"k": "f", "t": 0x14}), count

# ---- location decode ----
def loc_member_off(at):
    b = aval(at, A_LOC)
    if isinstance(b, (bytes, bytearray)) and len(b) >= 5 and b[0] == 0x04:
        return struct.unpack_from("<I", b, 1)[0]
    return None
def loc_global_addr(at):
    b = aval(at, A_LOC)
    if isinstance(b, (bytes, bytearray)) and len(b) >= 5 and b[0] == 0x03:
        return struct.unpack_from("<I", b, 1)[0]
    return None
def loc_raw(at):
    b = aval(at, A_LOC)
    return b.hex() if isinstance(b, (bytes, bytearray)) else None

def children(i):
    off, tag, L, at = dies[i]
    sib = aval(at, A_SIB)
    j = i+1; res = []
    while j < len(dies):
        coff = dies[j][0]
        if sib is not None and coff >= sib: break
        res.append(j)
        csib = aval(dies[j][3], A_SIB)
        if csib is not None and csib > coff:
            nj = off2idx.get(csib)
            if nj and nj > j: j = nj; continue
        j += 1
    return res

# ---- build model ----
types = {}
funcs = []
globs = []
file_ids = {}
files = []
cur_file = None

def file_id(path):
    if path not in file_ids:
        file_ids[path] = len(files); files.append(path)
    return file_ids[path]

for i, (off, tag, L, at) in enumerate(dies):
    if tag == T_CU:
        nm = aval(at, A_NAME)
        if nm: cur_file = nm
    elif tag in (T_STRUCT, T_UNION):
        rec = {"kind": "struct" if tag == T_STRUCT else "union",
               "name": die_name(i), "size": aval(at, A_BYTE, 0), "members": []}
        for ci in children(i):
            coff, ctag, cL, cat = dies[ci]
            if ctag == T_MEMBER and A_NAME in cat:
                rec["members"].append({"name": aval(cat, A_NAME),
                                       "off": loc_member_off(cat),
                                       "ref": typeref(cat)})
        types[str(off)] = rec
    elif tag == T_ENUM:
        bs = aval(at, A_BYTE, 4); blk = aval(at, A_ELEM)
        consts = []
        if isinstance(blk, (bytes, bytearray)):
            w = bs if bs in (1, 2, 4) else 4
            def parse(width):
                r = []; q = 0
                while q + width < len(blk):
                    val = int.from_bytes(blk[q:q+width], "little", signed=True); q += width
                    e = blk.find(b"\0", q)
                    if e < 0: break
                    r.append([blk[q:e].decode("latin1"), val]); q = e+1
                return r
            consts = parse(w)
            if not consts and w != 4: consts = parse(4)
        types[str(off)] = {"kind": "enum", "name": die_name(i),
                           "size": bs if bs in (1, 2, 4) else 4, "consts": consts}
    elif tag == T_ARR:
        elem, count = decode_array(at)
        types[str(off)] = {"kind": "array", "ref": elem, "count": count,
                           "size": aval(at, A_BYTE)}
    elif tag == T_SUBT:        # subroutine_type -> function pointer target
        ret = typeref(at) if (A_FT in at or A_UDT in at or A_MFT in at or A_MUD in at) else {"k": "f", "t": 0x14}
        params = []
        for ci in children(i):
            coff, ctag, cL, cat = dies[ci]
            if ctag == T_FPARAM:
                params.append(typeref(cat))
        types[str(off)] = {"kind": "func", "ret": ret, "params": params}
    elif tag == T_PTR:         # explicit pointer_type (rare)
        inner = typeref(at)
        types[str(off)] = {"kind": "ptr", "ref": inner, "size": 4}
    elif tag in (T_GSUB, T_SUB) and A_NAME in at and A_LOW in at:
        ret = typeref(at) if (A_FT in at or A_UDT in at or A_MFT in at or A_MUD in at) else {"k": "f", "t": 0x14}
        params = []; locals_ = []
        for ci in children(i):
            coff, ctag, cL, cat = dies[ci]
            if ctag == T_FPARAM and A_NAME in cat:
                params.append({"name": aval(cat, A_NAME), "ref": typeref(cat), "loc": loc_raw(cat)})
            elif ctag == T_LOCAL and A_NAME in cat:
                locals_.append({"name": aval(cat, A_NAME), "ref": typeref(cat), "loc": loc_raw(cat)})
        funcs.append({"name": aval(at, A_NAME), "low": aval(at, A_LOW),
                      "high": aval(at, A_HIGH), "ret": ret, "params": params,
                      "locals": locals_, "file": file_id(cur_file) if cur_file else None})
    elif tag == T_GVAR and A_NAME in at:
        addr = loc_global_addr(at)
        if addr is not None:
            globs.append({"name": aval(at, A_NAME), "addr": addr, "ref": typeref(at)})

# ---- .line -> flat sorted records ----
# map each line program to its owning CU file via stmt_list offset
lines = []
# build stmt_list(.line offset) -> file by scanning CUs
cu_stmt = []
cur = None
for off, tag, L, at in dies:
    if tag == T_CU:
        nm = aval(at, A_NAME); st = aval(at, 0x100)
        if nm is not None and st is not None:
            cu_stmt.append((st, nm))
cu_stmt.sort()
import bisect
stmt_offsets = [s for s, _ in cu_stmt]
def file_for_stmt(o):
    j = bisect.bisect_right(stmt_offsets, o) - 1
    return cu_stmt[j][1] if 0 <= j < len(cu_stmt) else None

lp = 0
while lp + 8 <= LS:
    plen = struct.unpack_from("<I", data, LB+lp)[0]
    base = struct.unpack_from("<I", data, LB+lp+4)[0]
    if plen < 8 or lp+plen > LS: break
    fp = file_for_stmt(lp)
    fid = file_id(fp) if fp else None
    n = (plen-8)//10
    for k in range(n):
        eo = LB+lp+8+k*10
        ln = struct.unpack_from("<I", data, eo)[0]
        pc = base + struct.unpack_from("<I", data, eo+6)[0]
        lines.append([pc, fid, ln])
    lp += plen
lines.sort()

model = dict(
    meta=dict(image_base=0x00100000, debug_size=DS, line_size=LS,
              dies=len(dies), types=len(types), funcs=len(funcs),
              globals=len(globs), files=len(files), lines=len(lines),
              producer="MW MIPS C Compiler (2.4.1.01) PlayStation2",
              dwarf_version=1),
    types=types, funcs=funcs, globals=globs, files=files, lines=lines)

json.dump(model, open("model.json", "w"))
print("== model.json written ==")
for k, v in model["meta"].items():
    print(f"  {k:14}: {v}")
# spot checks
print("\nspot: a struct with array member?")
for o, t in types.items():
    if t.get("kind") == "struct" and any(m["ref"].get("k") == "u" and types.get(str(m["ref"].get("o")), {}).get("kind") == "array" for m in t["members"]):
        print("  struct", t["name"], "size", t["size"])
        for m in t["members"][:8]:
            r = m["ref"]; rk = r.get("k")
            if rk == "u":
                tt = types.get(str(r["o"]), {})
                desc = f"{tt.get('kind')}:{tt.get('name', '')}"
                if tt.get("kind") == "array":
                    el = tt["ref"]; desc += f"[{tt.get('count')}] of " + (FT_NAME.get(el['t']) if el.get('k')=='f' else 'udt') if False else f"[{tt.get('count')}]"
            else:
                desc = rk
            print(f"     +{(m['off'] or 0):#05x} {m['name']:<16} {desc}")
        break
print("\nsample funcs w/ locals:")
nshown = 0
for f in funcs:
    if f["locals"]:
        print(f"  {f['name']}  params={len(f['params'])} locals={len(f['locals'])} file={files[f['file']] if f['file'] is not None else '?'}")
        nshown += 1
        if nshown >= 5: break
print(f"\nlines: {len(lines):,}  first={lines[0]}  last={lines[-1]}")
