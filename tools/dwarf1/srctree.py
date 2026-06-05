#!/usr/bin/env python3
"""Aggregate the recoverable source tree: unique files, top dirs, funcs/file."""
import struct, collections, json
ELF=r"C:\Users\tux\Downloads\Silent Hill 4 - E3 Trial Version (E3 2004)\SILENT_HILL_4_VER_E3\SLUS_208.73"
data=open(ELF,"rb").read()
(e_shoff,)=struct.unpack_from("<I",data,32)
ess,esn,esi=struct.unpack_from("<HHH",data,46)
secs=[struct.unpack_from("<IIIIIIIIII",data,e_shoff+i*ess) for i in range(esn)]
sh=secs[esi][4]
def sn(o):
    e=data.find(b"\0",sh+o);return data[sh+o:e].decode("latin1")
byn={sn(s[0]):s for s in secs}
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
files=collections.Counter()      # source path -> func count
topdirs=collections.Counter()
p=0
cur_file=None
while p+4<=DS:
    L=struct.unpack_from("<I",data,DB+p)[0]
    if L<6:
        p+=(4 if L==0 else L)
        if L<4:break
        continue
    tag=struct.unpack_from("<H",data,DB+p+4)[0]
    ap=DB+p+6;end=DB+p+L
    nm=None;low=None;is_func=tag in(0x06,0x14)
    try:
        while ap<end:
            a,fm,v,ap=rattr(ap)
            if a==0x30:nm=v
            elif a==0x110:low=v
    except Exception:pass
    if tag==0x11 and nm:
        cur_file=nm
    if is_func and low is not None and cur_file:
        files[cur_file]+=1
    p+=end-(DB+p)  # == L
# normalize: collapse \ and group by leading dir under the project root
def norm(pth): return pth.replace("\\","/")
uniq=sorted(files)
print(f"unique source files referenced: {len(uniq)}")
print(f"total functions attributed     : {sum(files.values())}")
# top-level project roots
roots=collections.Counter()
for f in files:
    n=norm(f)
    parts=n.split("/")
    root=parts[0] if len(parts)<3 else "/".join(parts[:3])
    roots[root]+=files[f]
print("\n== functions by source root ==")
for r,c in roots.most_common(25):
    print(f"   {c:>5}  {r}")
# subsystem dirs under F:/E3/sh4/
sub=collections.Counter()
for f in files:
    n=norm(f)
    if "/sh4/" in n:
        after=n.split("/sh4/",1)[1]
        d=after.rsplit("/",1)[0] if "/" in after else "(root)"
        sub[d]+=files[f]
print("\n== functions by sh4 subsystem dir ==")
for d,c in sub.most_common(40):
    print(f"   {c:>5}  sh4/{d}")
print("\n== top 25 files by function count ==")
for f,c in files.most_common(25):
    print(f"   {c:>4}  {f}")
json.dump({norm(k):v for k,v in files.items()}, open("srctree.json","w"), indent=0)
