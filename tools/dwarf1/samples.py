#!/usr/bin/env python3
"""Fully decode a handful of structs / functions / enums into clean C, resolving
pointer-modifier blocks and type refs. Prototypes the importer transform and
produces the evidence embedded in the HTML deliverable. Emits samples.json."""
import argparse
import struct

from tool_paths import (
    add_elf_argument,
    add_output_argument,
    fail,
    read_elf_bytes,
    resolve_elf_path,
    resolve_output_path,
    write_json,
)


DEFAULT_STRUCTS = ["sfCharacter", "ktLight", "ktScene", "sgAnime", "sfCldObject", "sgIKHandle"]
DEFAULT_FUNC_PREFIXES = ("sfCharacter", "sgAnime", "ktLight", "GetRootTrans", "sfCld")

parser = argparse.ArgumentParser(description="Resolve sample DWARF structs, functions, and enums to C-like output.")
add_elf_argument(parser)
add_output_argument(parser, "samples.json")
parser.add_argument("--struct", dest="structs", action="append", help="Struct name to emit. May be repeated.")
parser.add_argument("--func-prefix", dest="func_prefixes", action="append", help="Function name prefix to emit. May be repeated.")
parser.add_argument("--enum-count", type=int, default=2, help="Number of enums to sample. Default: 2.")
args = parser.parse_args()

try:
    ELF = resolve_elf_path(args.elf)
    OUT = resolve_output_path(args.output, "samples.json")
    data = read_elf_bytes(ELF)
except Exception as exc:
    fail(str(exc))
(e_shoff,)=struct.unpack_from("<I",data,32)
ess,esn,esi=struct.unpack_from("<HHH",data,46)
secs=[struct.unpack_from("<IIIIIIIIII",data,e_shoff+i*ess) for i in range(esn)]
sh=secs[esi][4]
def sn(o):
    e=data.find(b"\0",sh+o);return data[sh+o:e].decode("latin1")
byn={sn(s[0]):s for s in secs}
if ".debug" not in byn:
    fail(f"{ELF} is missing required .debug section")
DB,DS=byn[".debug"][4],byn[".debug"][5]

def rattr(p):
    af=struct.unpack_from("<H",data,p)[0];p+=2;form=af&0xf;name=af&0xfff0
    if form in(1,2,6):v=struct.unpack_from("<I",data,p)[0];p+=4
    elif form==3:n=struct.unpack_from("<H",data,p)[0];p+=2;v=data[p:p+n];p+=n
    elif form==4:n=struct.unpack_from("<I",data,p)[0];p+=4;v=data[p:p+n];p+=n
    elif form==5:v=struct.unpack_from("<H",data,p)[0];p+=2
    elif form==7:v=struct.unpack_from("<Q",data,p)[0];p+=8
    elif form==8:e=data.find(b"\0",p);v=data[p:e].decode("latin1");p=e+1
    else:raise ValueError(form)
    return name,form,v,p

dies=[];p=0
while p+4<=DS:
    L=struct.unpack_from("<I",data,DB+p)[0]
    if L<6:
        p+=(4 if L==0 else L)
        if L<4:break
        continue
    tag=struct.unpack_from("<H",data,DB+p+4)[0]
    ap=DB+p+6;end=DB+p+L;at={}
    try:
        while ap<end:
            nm,fm,v,ap=rattr(ap);at[nm]=(fm,v)
    except Exception:pass
    dies.append((p,tag,L,at));p+=L
off2idx={d[0]:i for i,d in enumerate(dies)}

FT={0x1:"char",0x2:"signed char",0x3:"unsigned char",0x4:"short",0x5:"signed short",
 0x6:"unsigned short",0x7:"int",0x8:"int",0x9:"unsigned int",0xa:"long",0xb:"long",
 0xc:"unsigned long",0xe:"float",0xf:"double",0x10:"long double",0x14:"void",
 0x15:"bool",0x8008:"long long",0x8108:"unsigned long long"}
TAGNAME={0x1:"array",0x2:"class",0x4:"enum",0xf:"pointer",0x10:"ref",0x13:"struct",
 0x15:"funcptr",0x16:"typedef",0x17:"union",0x12:"string"}

def die_name(idx):
    off,tag,L,at=dies[idx]
    if 0x30 in at: return at[0x30][1]
    return f"anon_{off:x}"

def base_typename(idx):
    off,tag,L,at=dies[idx]
    if tag in (0x13,):  return "struct "+die_name(idx)
    if tag==0x17:       return "union "+die_name(idx)
    if tag==0x04:       return "enum "+die_name(idx)
    if tag==0x02:       return "class "+die_name(idx)
    if tag==0x15:       return "<func>"          # subroutine_type -> function pointer base
    if tag==0x01:       return arr_typename(idx)
    if tag==0x0f:                                # explicit pointer_type (rare here)
        if 0x70 in at: return base_typename(off2idx[at[0x70][1]])+" *"
        if 0x50 in at: return FT.get(at[0x50][1],"?")+" *"
        return "void *"
    if tag==0x16:                                # typedef (none in practice)
        if 0x70 in at: return base_typename(off2idx[at[0x70][1]])
    return die_name(idx)

def arr_typename(idx):
    # best-effort: DWARF1 subscr_data is complex; just show element + [] count if trivial
    off,tag,L,at=dies[idx]
    return "array"

def apply_mods(modbytes, base):
    """modbytes precede the value; 0x01 ptr,0x02 ref,0x03 const,0x04 volatile."""
    s=base
    for m in modbytes:
        if m==0x01: s+=" *"
        elif m==0x02: s+=" &"
        elif m==0x03: s="const "+s
        elif m==0x04: s="volatile "+s
        else: s+=" /*mod%02x*/"%m
    return s

def resolve_type(at):
    """Given an attr-dict of a member/param/var, return a C type string."""
    if 0x50 in at:                       # fund_type
        return FT.get(at[0x50][1], f"FT_{at[0x50][1]:#x}")
    if 0x70 in at:                       # user_def_type ref
        r=at[0x70][1]; i=off2idx.get(r)
        return base_typename(i) if i is not None else f"udt_{r:#x}"
    if 0x60 in at:                       # mod_fund_type: [mods..][u16 ft]
        b=at[0x60][1]; ft=struct.unpack_from("<H",b,len(b)-2)[0]; mods=b[:-2]
        return apply_mods(mods, FT.get(ft,f"FT_{ft:#x}"))
    if 0x80 in at:                       # mod_u_d_type: [mods..][u32 ref]
        b=at[0x80][1]; ref=struct.unpack_from("<I",b,len(b)-4)[0]; mods=b[:-4]
        i=off2idx.get(ref)
        base=base_typename(i) if i is not None else f"udt_{ref:#x}"
        return apply_mods(mods, base)
    return "void"

def member_offset(at):
    if 0x20 in at:
        b=at[0x20][1]
        if isinstance(b,bytes) and len(b)>=5 and b[0]==0x04:
            return struct.unpack_from("<I",b,1)[0]
    return None

def children(i):
    off,tag,L,at=dies[i]
    sib=at.get(0x10,(None,None))[1]
    j=i+1; res=[]
    while j<len(dies):
        coff=dies[j][0]
        if sib is not None and coff>=sib: break
        res.append(j)
        csib=dies[j][3].get(0x10,(None,None))[1]
        if csib is not None and csib>coff:
            nj=off2idx.get(csib)
            if nj and nj>j: j=nj; continue
        j+=1
    return res

want_structs = args.structs or DEFAULT_STRUCTS
want_funcs_prefix = tuple(args.func_prefixes) if args.func_prefixes else DEFAULT_FUNC_PREFIXES
want_enums = max(0, args.enum_count)

out={"structs":[],"funcs":[],"enums":[]}

# structs
done=set()
for i,(off,tag,L,at) in enumerate(dies):
    if tag==0x13 and 0x30 in at and at[0x30][1] in want_structs and at[0x30][1] not in done:
        nm=at[0x30][1]; size=at.get(0xb0,(0,0))[1]
        mems=[]
        for ci in children(i):
            coff,ctag,cL,cat=dies[ci]
            if ctag==0x0d and 0x30 in cat:
                mems.append({"name":cat[0x30][1],"off":member_offset(cat),
                             "type":resolve_type(cat)})
        out["structs"].append({"name":nm,"size":size,"members":mems})
        done.add(nm)
    if len(done)>=len(want_structs):break

# functions (with resolved return + params)
fcount=0
for i,(off,tag,L,at) in enumerate(dies):
    if tag in (0x06,) and 0x30 in at and 0x110 in at and at[0x30][1].startswith(want_funcs_prefix):
        nm=at[0x30][1]
        ret=resolve_type(at) if (0x50 in at or 0x70 in at or 0x60 in at or 0x80 in at) else "void"
        params=[]
        for ci in children(i):
            coff,ctag,cL,cat=dies[ci]
            if ctag==0x05 and 0x30 in cat:
                params.append({"name":cat[0x30][1],"type":resolve_type(cat)})
        out["funcs"].append({"name":nm,"low":at[0x110][1],
                             "high":at.get(0x120,(0,0))[1],"ret":ret,"params":params})
        fcount+=1
    if fcount>=10:break

# enums
ec=0
for i,(off,tag,L,at) in enumerate(dies):
    if tag==0x04 and 0x30 in at and 0xf0 in at:
        nm=at[0x30][1]; bs=at.get(0xb0,(0,1))[1]; blk=at[0xf0][1]
        consts=[]; q=0; w=bs if bs in (1,2,4) else 4
        # try width = byte_size, fall back to 4 if it desyncs
        def parse(width):
            r=[];p=0
            while p+width< len(blk):
                val=int.from_bytes(blk[p:p+width],"little",signed=True); p+=width
                e=blk.find(b"\0",p)
                if e<0:break
                r.append((blk[p:e].decode("latin1"),val));p=e+1
            return r
        consts=parse(w) or parse(4) or parse(1)
        out["enums"].append({"name":nm,"size":bs,"consts":consts[:12]})
        ec+=1
    if ec>=want_enums:break

write_json(OUT, out, indent=1)
print(f"samples written: {OUT}")

# pretty print as C
def fmt_member(m):
    t=m["type"]
    star = t.endswith("*")
    base=t.rstrip(" *"); stars="*"*t.count("*")
    return f"    /* +0x{(m['off'] or 0):03x} */ {base} {stars}{m['name']};"
for s in out["structs"]:
    print(f"struct {s['name']} {{        // sizeof = 0x{s['size']:x} ({s['size']})")
    for m in s["members"]:
        print(fmt_member(m))
    print("};\n")
print("// ---- function signatures ----")
for f in out["funcs"]:
    ps=", ".join(f"{p['type']} {p['name']}" for p in f["params"]) or "void"
    print(f"{f['ret']} {f['name']}({ps});   // {f['low']:#x}-{f['high']:#x}")
print("\n// ---- enums ----")
for en in out["enums"]:
    print(f"enum {en['name']} {{   // size {en['size']}")
    for cn,cv in en["consts"]:
        print(f"    {cn} = {cv},")
    print("};")
