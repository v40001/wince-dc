/* ceload.c — CE e32/o32 ROM module loader + guest memory + import
 * binding for the x86 emulator. Each original CEPC module's flat
 * virtual image (objects placed at their rva) is registered at its
 * real vbase; the IAT is patched with the real export VAs of the
 * already-loaded modules, so inter-module calls execute the original
 * code as well (only kernel/PSL traps need shims).
 *
 * Module image file format (produced by tools/cemod.py):
 *   "CEM1" u32 | vbase u32 | entry u32 | nobj u32 |
 *   per obj: {rva u32, size u32, data[size]}
 * followed by string pool + import table + export table blobs.
 */
#include <windows.h>
#include <stdlib.h>
#include "x86emu.h"

#define MAXREG 48
#define MAXMOD 64

typedef struct
{
	XU32 base, size;   /* guest VA */
	XU8 *host;         /* host copy */
	const char *name;
} XRegion;

typedef struct
{
	char name[64];
	XU32 va;
	int  ord;
} XExport;

static XRegion s_reg[MAXREG];
static int s_nreg = 0;
static XExport s_exp[8192];
static int s_nexp = 0;

static XRegion *Find(XU32 a)
{
	int i;
	for (i=0;i<s_nreg;i++)
		if (a>=s_reg[i].base && a<s_reg[i].base+s_reg[i].size)
			return &s_reg[i];
	return 0;
}

static XU8 GRd8(void *ctx, XU32 a)
{
	XRegion *r=Find(a);
	if (!r) return 0xff;
	return r->host[a-r->base];
}
static void GWr8(void *ctx, XU32 a, XU8 v)
{
	XRegion *r=Find(a);
	if (r) r->host[a-r->base]=v;
}

static X86Mem s_mem = {0, GRd8, GWr8};

void CERegister(XU32 base, XU32 size, const XU8 *data, const char *name)
{
	XRegion *r;
	if (s_nreg>=MAXREG) return;
	r=&s_reg[s_nreg++];
	r->base=base; r->size=size; r->name=name;
	r->host=(XU8*)malloc(size);
	memset(r->host,0xff,size);
	if (data) memcpy(r->host,data,size);
}

static void RegExport(const char *mod, XU32 va, int ord)
{
	XExport *e;
	if (s_nexp>=8192) return;
	e=&s_exp[s_nexp++];
	lstrcpyA(e->name,mod);
	e->va=va; e->ord=ord;
}

/* dump a raw e32/o32 module blob (the file region that holds the e32)
 * into the guest: dataptr file offsets are relative to the blob.
 * Also parses the o32 to place objects at rva. */
int CELoadModuleBlob(const char *name, const XU8 *blob, XU32 blobsz)
{
	XU16 oc; XU32 vb, ent, i, top;
	const XU8 *e32=blob;
	oc = *(const XU16*)e32;
	if (oc==0 || oc>40) return -1;
	ent = *(const XU32*)(e32+4);
	vb  = *(const XU32*)(e32+8);
	top = 0;
	for (i=0;i<oc;i++)
	{
		const XU8 *o=e32+0x64+i*24;
		XU32 vsz,rva,psz,dp,ra,fl;
		vsz=*(const XU32*)(o+0); rva=*(const XU32*)(o+4);
		psz=*(const XU32*)(o+8); dp=*(const XU32*)(o+12);
		ra =*(const XU32*)(o+16); fl=*(const XU32*)(o+20);
		(void)ra;
		if (rva+vsz>top) top=rva+vsz;
	}
	{
		XU8 *img=(XU8*)malloc(top);
		memset(img,0,top);
		for (i=0;i<oc;i++)
		{
			const XU8 *o=e32+0x64+i*24;
			XU32 vsz=*(const XU32*)(o+0), rva=*(const XU32*)(o+4),
			     psz=*(const XU32*)(o+8), dp=*(const XU32*)(o+12);
			XU32 fo = dp; /* caller passes dp already file-relative */
			if (fo<blobsz)
				memcpy(img+rva, blob+fo, psz<vsz?psz:vsz);
		}
		CERegister(vb, top, img, name);
		free(img);
	}
	return (int)ent;
}

/* load a pre-flattened .img (objects already at rva, zero-padded) */
int CELoadImage(const char *name, XU32 vbase, const XU8 *img, XU32 sz)
{
	CERegister(vbase, sz, img, name);
	return 0;
}

/* export-table registration: host parses exports externally */
void CEDefExport(const char *mod, XU32 va, int ord)
	{ RegExport(mod, va, ord); }

XU32 CELookupExport(const char *mod)
{
	int i;
	for (i=0;i<s_nexp;i++)
		if (!lstrcmpiA(s_exp[i].name,mod)) return s_exp[i].va;
	return 0;
}

X86Mem *CEGuestMem(void) { return &s_mem; }
