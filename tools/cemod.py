#!/usr/bin/env python3
# cemod.py — dump every CE ROM module as a flat virtual image (.img):
# objects placed at their rva, vbase/entry/imports in manifest.json.
#
# CE ROM module format (e32 at TOC-listed offset):
#   +0x00 objcnt u16, imageflags u16
#   +0x04 entryrva u32
#   +0x08 vbase u32
#   ... rominfo[9] {rva,psize} at +0x24
#   +0x64 o32[objcnt] entries, EACH 24 BYTES:
#       {vsize, rva, psize, dataptr, realaddr, flags}
#   dataptr/realaddr are physical ROM addrs (base 0x80200000);
#   realaddr-rva == module load base.
#
# usage: python3 tools/cemod.py flat.bin outdir/

import struct, json, os, sys, re

def main(flat, outdir):
    d = open(flat,'rb').read()
    BASE = 0x80200000
    os.makedirs(outdir, exist_ok=True)
    # find TOC entries: name ptr + module phys pairs (40B pitch in TOC area)
    mods = {}
    for off in range(0x10000, len(d)-40, 4):
        nm, mb = struct.unpack('<II', d[off+16:off+24])
        if not (BASE<=nm<BASE+len(d) and BASE<=mb<BASE+len(d)):
            continue
        noff = nm-BASE; e32 = mb-BASE
        end = d.find(b'\0', noff)
        if not (0 < end-noff < 40):
            continue
        s = d[noff:end].decode('latin1','replace')
        if not re.match(r'^[a-zA-Z0-9_. -]{3,}\.(exe|dll|cpl|drv|nls)$', s):
            continue
        oc, fl = struct.unpack('<HH', d[e32:e32+4])
        if 1 <= oc <= 40 and fl:
            mods[e32] = s
    man = {}
    for e32, nm in sorted(mods.items()):
        oc, = struct.unpack('<H', d[e32:e32+2])
        ent, = struct.unpack('<I', d[e32+4:e32+8])
        vb,  = struct.unpack('<I', d[e32+8:e32+12])
        objs = []
        ok = True
        for i in range(oc):
            vsz,rva,psz,dp,ra,fl2 = struct.unpack(
                '<IIIIII', d[e32+0x64+i*24:e32+0x64+i*24+24])
            if not (0 <= rva < 0x400000):
                ok = False
                break
            objs.append(dict(vsz=vsz,rva=rva,psz=psz,dp=dp,ra=ra,fl=fl2))
        if not ok or not objs:
            continue
        top = max(o['rva']+o['vsz'] for o in objs)
        if top > 0x2000000:
            continue
        img = bytearray(top)
        for o in objs:
            fo = o['dp']-BASE
            if 0 <= fo < len(d):
                n = min(o['psz'], o['vsz'], len(d)-fo)
                img[o['rva']:o['rva']+n] = d[fo:fo+n]
        safe = nm.replace('/','_')
        open(os.path.join(outdir, safe+'.img'),'wb').write(bytes(img))
        man[safe] = dict(e32=e32, entry=ent, vbase=vb, objs=objs)
        print('%-18s e32=%#x vb=%#x entry=%#x objs=%d img=%#x'
              % (nm, e32, vb, ent, len(objs), top))
    json.dump(man, open(os.path.join(outdir,'manifest.json'),'w'), indent=1)

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
