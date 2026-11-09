#!/usr/bin/env python3
# ce_rom_res.py — extract real resources (icons/bitmaps/cursors/menus/
# dialogs/strings) from a Windows CE ROM flat image (nk.bin/0winceos).
#
# Format: modules use the CE e32/o32 ROM image format (not PE). The ROM
# TOC (found via ROMHDR) lists module name + e32 header physaddr; each
# e32 is followed by objcnt o32 entries {vsize,rva,psize,dataptr,
# realaddr,flags,rsvd} — dataptr is the physical ROM address of the
# object's data. Resource data is laid out flat at e32_off+rva for
# modules whose e32 heads their file region.
#
# Resource trees are standard IMAGE_RESOURCE_DIRECTORYs somewhere inside
# the module's .rsrc object; we find them by signature scan, then solve
# each tree's base (file_off = base + rva) by matching leaf sizes to
# real DIB payloads.
#
# Usage: python3 tools/ce_rom_res.py <flat.bin> <outdir>

import struct, sys, os, re

def leaves(d, rbase):
    out = []
    def walk(off, path, dep):
        if dep > 4 or off + 16 > len(d):
            return
        nn, ni = struct.unpack('<HH', d[off+12:off+16])
        if not (0 <= nn+ni <= 400):
            return
        for i in range(nn+ni):
            eoff = off+16+i*8
            if eoff+8 > len(d):
                return
            name, o = struct.unpack('<II', d[eoff:eoff+8])
            o2 = o & 0x7fffffff
            if o & 0x80000000:
                if rbase+o2+16 <= len(d):
                    walk(rbase+o2, path+[name], dep+1)
            else:
                if rbase+o2+16 <= len(d):
                    rva, size, cp, res = struct.unpack(
                        '<IIII', d[rbase+o2:rbase+o2+16])
                    out.append((path+[name], rva, size))
    walk(rbase, [], 0)
    return out

def dib_size(d, off):
    """Total size of the icon/bitmap DIB at off, or 0."""
    if off+40 > len(d) or d[off:off+4] != b'\x28\x00\x00\x00':
        return 0
    w, = struct.unpack('<i', d[off+4:off+8])
    h2, = struct.unpack('<i', d[off+8:off+12])
    pl, = struct.unpack('<H', d[off+12:off+14])
    bpp, = struct.unpack('<H', d[off+14:off+16])
    if w not in (8, 12, 16, 20, 24, 32, 40, 48, 64) \
            or h2 not in (w, 2*w) or pl != 1:
        return 0
    nclr, = struct.unpack('<I', d[off+32:off+36])
    npal = nclr if nclr else (1 << bpp if bpp <= 8 else 0)
    xs = ((w*bpp+31)//32)*4
    ast = ((w+31)//32)*4
    h = h2 if h2 == w else h2//2
    return 40 + npal*4 + xs*h + (ast*h if h2 == w*2 else 0)

def find_resdirs(d):
    """All top-level resource directory candidates -> list of offsets."""
    out = []
    for off in range(0x10000, len(d)-0x100, 4):
        ch, tim, mj, mn, nn, ni = struct.unpack('<IIHHHH', d[off:off+16])
        if ch or nn or not (4 <= ni <= 25):
            continue
        if tim > 0x70000000:
            continue
        names = []
        ok = True
        for i in range(ni):
            name, o = struct.unpack('<II', d[off+16+i*8:off+24+i*8])
            if name > 60 or (i and name < names[-1]):
                ok = False
                break
            names.append(name)
        if ok:
            out.append(off)
    return out

def solve_base(d, rdir, ic):
    """Find base such that file_off = base + rva for this tree's icons."""
    r1, s1 = ic[0][1], ic[0][2]
    best = None
    for off in range(rdir, min(rdir+0x100000, len(d)-0x400), 4):
        if dib_size(d, off) == s1:
            b = off - r1
            if b <= 0:
                continue
            n = sum(1 for _, rv, sz in ic if dib_size(d, b+rv) == sz)
            if n >= len(ic)*0.7:
                return b
            if not best or n > best[1]:
                best = (b, n)
    return best[0] if best else None

def emit_icos(d, out, rdir, base, idata, gdata):
    """Write each RT_GROUP_ICON as a real multi-resolution .ico."""
    n = 0
    for gid, (go, gsz) in gdata.items():
        buf = d[go:go+gsz]
        if len(buf) < 6:
            continue
        resv, typ, cnt = struct.unpack('<HHH', buf[:6])
        if typ not in (1, 2) or cnt > 20:
            continue
        members = []
        for i in range(cnt):
            e = buf[6+i*14:6+i*14+14]
            if len(e) < 14:
                break
            w, h, nc, r2, pl, bpp, sz2, iid = struct.unpack('<BBBBHHIH', e)
            if iid in idata:
                members.append((w, h, nc, bpp, sz2, idata[iid]))
        if not members:
            continue
        hdr = struct.pack('<HHH', 0, 1, len(members))
        ents = b''
        dat = b''
        o0 = 6 + 16*len(members)
        for w, h, nc, bpp, sz2, off in members:
            ents += struct.pack('<BBBBHHII', w, h, nc, 0, 1, bpp,
                                sz2, o0+len(dat))
            dat += d[off:off+sz2]
        fn = os.path.join(out, 'r%x_g%d.ico' % (rdir, gid))
        open(fn, 'wb').write(hdr+ents+dat)
        n += 1
    return n

def main(flat, outdir):
    d = open(flat, 'rb').read()
    os.makedirs(outdir, exist_ok=True)
    total = 0
    for rdir in find_resdirs(d):
        lv = leaves(d, rdir)
        ic = sorted([l for l in lv if l[0][0] == 3], key=lambda x: x[1])
        grps = sorted([l for l in lv if l[0][0] == 14], key=lambda x: x[1])
        if not ic:
            continue
        base = solve_base(d, rdir, ic)
        if not base:
            print('rdir=%#x NO BASE' % rdir)
            continue
        idata = {}
        for path, rva, sz in ic:
            if len(path) > 1 and dib_size(d, base+rva) == sz:
                idata[path[1]] = base+rva
        gdata = {}
        for path, rva, sz in grps:
            if len(path) > 1:
                gdata[path[1]] = (base+rva, sz)
        n = emit_icos(d, outdir, rdir, base, idata, gdata)
        print('rdir=%#x base=%#x icons=%d groups=%d .ico=%d'
              % (rdir, base, len(idata), len(gdata), n))
        total += n
    print(total, '.ico written')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
