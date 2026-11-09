/* x86emu.h — i386 interpreter for running original CEPC x86 modules on SH-4.
 *
 * Flat 32-bit memory model. Guest memory is a set of mapped regions
 * (module images + heap + stack). API imports trap through a callback
 * that dispatches to the native SH-4 CE implementation.
 */
#ifndef _X86EMU_H_
#define _X86EMU_H_

typedef unsigned long  XU32;
typedef unsigned short XU16;
typedef unsigned char  XU8;
typedef unsigned long long XU64;
typedef signed long    XS32;
typedef signed short   XS16;
typedef signed char    XS8;
typedef signed long long XS64;

typedef struct X86Mem X86Mem;
typedef struct X86Cpu X86Cpu;

/* memory interface: implemented by the host (ceload) */
struct X86Mem
{
	void *ctx;
	XU8  (*rd8)(void *ctx, XU32 a);
	void (*wr8)(void *ctx, XU32 a, XU8 v);
};

struct X86Cpu
{
	XU32 r[8];          /* EAX ECX EDX EBX ESP EBP ESI EDI */
	XU32 eip;
	XU32 flags;         /* CF PF AF ZF SF OF (+DF as separate flag) */
	int  df;
	XU16 seg[6];        /* es cs ss ds fs gs (FS used for TLS) */
	int  running;
	int  halted;
	XU32 tib;           /* linear base of FS (TLS) or 0 */
	/* trap callback: returns 1 if it handled the call at eip
	   (eip already pushed-ret handled by caller semantics) */
	void *trapctx;
	int  (*trap)(void *trapctx, X86Cpu *c, XU32 target);
};

#define XF_CF 0x001
#define XF_PF 0x004
#define XF_AF 0x010
#define XF_ZF 0x040
#define XF_SF 0x080
#define XF_OF 0x800

enum { XEAX, XECX, XEDX, XEBX, XESP, XEBP, XESI, XEDI };

void X86Init(X86Cpu *c);
int  X86Step(X86Cpu *c, X86Mem *m);          /* 1 insn; 0=ok else trap/exit */
int  X86Run(X86Cpu *c, X86Mem *m, XU32 maxinsns);

/* helpers used by host loader */
XU32 X86Pop(X86Cpu *c, X86Mem *m);
void X86Push(X86Cpu *c, X86Mem *m, XU32 v);

#endif
