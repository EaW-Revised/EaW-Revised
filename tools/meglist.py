import struct, sys, collections
def list_meg(p):
    d=open(p,'rb').read()
    # MEG v1: u32 numFilenames, u32 numFiles, then filename table (u16 len + str), then file table
    nfn,nf=struct.unpack_from('<II',d,0); off=8
    names=[]
    for _ in range(nfn):
        l=struct.unpack_from('<H',d,off)[0]; off+=2
        names.append(d[off:off+l].decode('latin1')); off+=l
    return names
for p in sys.argv[1:]:
    names=list_meg(p)
    print(f"== {p}: {len(names)} files")
    c=collections.Counter(n.rsplit('.',1)[-1].lower() for n in names)
    print(dict(c))
    for n in names[:400]:
        if n.lower().endswith(('.fx','.fxo','.fxh','.vsh','.psh','.txt')): print("  ",n)
