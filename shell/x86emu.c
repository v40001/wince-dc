/* x86emu.c — i386 interpreter core. Decodes+executes the instruction
 * set MSVC-generated CE 2.x code uses: data movement, ALU with full
 * flag semantics, control flow, string ops, stack ops, setcc/movsx.
 * Segments are flat; FS-relative accesses go to the TIB base.
 */
#include "x86emu.h"

#define M_RD8(m,a)   (m)->rd8((m)->ctx,(a))
#define M_WR8(m,a,v) (m)->wr8((m)->ctx,(a),(v))

static XU16 rd16(X86Mem *m, XU32 a)
	{ return (XU16)(M_RD8(m,a) | (M_RD8(m,a+1)<<8)); }
static XU32 rd32(X86Mem *m, XU32 a)
	{ return (XU32)(M_RD8(m,a) | (M_RD8(m,a+1)<<8) | (M_RD8(m,a+2)<<16) | ((XU32)M_RD8(m,a+3)<<24)); }
static void wr16(X86Mem *m, XU32 a, XU16 v)
	{ M_WR8(m,a,(XU8)v); M_WR8(m,a+1,(XU8)(v>>8)); }
static void wr32(X86Mem *m, XU32 a, XU32 v)
	{ M_WR8(m,a,(XU8)v); M_WR8(m,a+1,(XU8)(v>>8)); M_WR8(m,a+2,(XU8)(v>>16)); M_WR8(m,a+3,(XU8)(v>>24)); }

void X86Push(X86Cpu *c, X86Mem *m, XU32 v)
	{ c->r[XESP]-=4; wr32(m,c->r[XESP],v); }
XU32 X86Pop(X86Cpu *c, X86Mem *m)
	{ XU32 v=rd32(m,c->r[XESP]); c->r[XESP]+=4; return v; }

void X86Init(X86Cpu *c)
{
	int i;
	for (i=0;i<8;i++) c->r[i]=0;
	for (i=0;i<6;i++) c->seg[i]=0;
	c->eip=0; c->flags=0x202; c->df=0; c->tib=0;
	c->running=1; c->halted=0; c->trap=0; c->trapctx=0;
}

/* ---- flag helpers ---- */
static XU32 parity(XU32 v){ v^=v>>4; v^=v>>2; v^=v>>1; return (~v)&1; }

static void set_szp(X86Cpu *c, XU32 r, int bits)
{
	XU32 m = (bits==8)?0xff:(bits==16)?0xffff:0xffffffff;
	r &= m;
	c->flags &= ~(XF_SF|XF_ZF|XF_PF);
	if (r==0) c->flags |= XF_ZF;
	if (r & (1u<<(bits-1))) c->flags |= XF_SF;
	if (parity(r)) c->flags |= XF_PF;
}

/* ALU ops: op 0=add 1=or 2=adc 3=sbb 4=and 5=sub 6=xor 7=cmp */
static XU32 alu(X86Cpu *c, int op, XU32 a, XU32 b, int bits)
{
	XU64 res; XU32 r, m, cf;
	m = (bits==8)?0xff:(bits==16)?0xffff:0xffffffff;
	a&=m; b&=m;
	cf = c->flags&XF_CF;
	switch (op)
	{
	case 0: res=(XU64)a+b; break;
	case 1: case 4: case 6: res=(XU64)(a|b)&m; if(op==4)res=(XU64)(a&b)&m; if(op==6)res=(XU64)(a^b)&m; break;
	case 2: res=(XU64)a+b+cf; break;
	case 3: case 5: case 7: res=(XU64)a-b-(op==3?cf:0); break;
	default: res=0;
	}
	r=(XU32)res&m;
	c->flags &= ~(XF_CF|XF_OF|XF_AF);
	if (op==0||op==2)
	{
		if (res>m) c->flags|=XF_CF;
		if (((a^b^m)&(a^r))&(1u<<(bits-1))) c->flags|=XF_OF;
		if ((a^b^r)&0x10) c->flags|=XF_AF;
	}
	else if (op==3||op==5||op==7)
	{
		if ((XS64)res<0) c->flags|=XF_CF;
		if (((a^b)&(a^r))&(1u<<(bits-1))) c->flags|=XF_OF;
		if ((a^b^r)&0x10) c->flags|=XF_AF;
	}
	set_szp(c,r,bits);
	return r;
}

/* shifts: op 0=rol 1=ror 2=rcl 3=rcr 4=shl 5=shr 7=sar (6=shl alias) */
static XU32 shft(X86Cpu *c, int op, XU32 v, XU32 cnt, int bits)
{
	XU32 m, msb, cf=0, r;
	int i;
	m = (bits==8)?0xff:(bits==16)?0xffff:0xffffffff;
	v&=m; cnt&=31; msb=1u<<(bits-1);
	if (!cnt) return v;
	if (op==4||op==6)
	{
		for(i=0;i<(int)cnt;i++){ cf=(v&msb)?1:0; v=(v<<1)&m; }
		c->flags&=~(XF_CF|XF_OF);
		if(cf)c->flags|=XF_CF;
		if(cnt==1 && ((v^cf^m)&msb?0:0,((v>> (bits-1))&1)!=cf)) c->flags|=XF_OF;
		if(cnt==1 && (((v&msb)?1:0)!=cf)) c->flags|=XF_OF;
		set_szp(c,v,bits); return v;
	}
	if (op==5)
	{
		for(i=0;i<(int)cnt;i++){ cf=v&1; v>>=1; }
		c->flags&=~(XF_CF|XF_OF);
		if(cf)c->flags|=XF_CF;
		if(cnt==1 && (v&msb)) c->flags|=XF_OF;
		set_szp(c,v,bits); return v;
	}
	if (op==7)
	{
		for(i=0;i<(int)cnt;i++){ cf=v&1; v=(v>>1)|(v&msb); }
		c->flags&=~(XF_CF|XF_OF);
		if(cf)c->flags|=XF_CF;
		if(cnt==1)c->flags&=~XF_OF;
		set_szp(c,v,bits); return v;
	}
	if (op==0){ cnt%=bits; r=((v<<cnt)|(v>>(bits-cnt)))&m; cf=r&1; goto done; }
	if (op==1){ cnt%=bits; r=((v>>cnt)|(v<<(bits-cnt)))&m; cf=(r&msb)?1:0; goto done; }
	if (op==2){ /* rcl */ XU64 e=v|((XU64)(c->flags&1)<<bits);
		for(i=0;i<(int)cnt%(bits+1);i++){cf=(e>>bits)&1;e=((e<<1)|cf)&(((XU64)1<<(bits+1))-1);}r=(XU32)e&m;goto done; }
	if (op==3){ XU64 e=v|((XU64)(c->flags&1)<<bits);
		for(i=0;i<(int)cnt%(bits+1);i++){cf=e&1;e=(e>>1)|((XU64)cf<<bits);}r=(XU32)e&m;goto done; }
	return v;
done:
	c->flags&=~XF_CF; if(cf)c->flags|=XF_CF; return r;
}

/* ---- ModRM/SIB ---- */
typedef struct { XU32 ea; int reg; int isreg; int mod; } ModRM;

static XU32 seg_base(X86Cpu *c, int segidx)
{
	if (segidx==4) return c->tib; /* FS = TLS */
	return 0;
}

static ModRM modrm(X86Cpu *c, X86Mem *m, int opsize)
{
	ModRM r; XU8 b; int mod,segov;
	b=M_RD8(m,c->eip++);
	r.mod=(b>>6)&3; r.reg=(b>>3)&7; r.isreg=(r.mod==3);
	r.ea=0;
	if (r.isreg){ r.ea=b&7; return r; }
	{
		int rm=b&7;
		segov=-1;
		/* segment override prefixes already consumed before this call
		   are applied by caller via c->segbase_pending — simplified:
		   FS handled here via ds check isn't reachable; we handle FS
		   prefix in main loop by adding tib after decode. */
		if (r.mod==0)
		{
			if (rm==5){ r.ea=rd32(m,c->eip); c->eip+=4; }
			else if (rm==4)
			{
				XU8 s=M_RD8(m,c->eip++);
				int ss=(s>>6)&3,idx=(s>>3)&7,bs=s&7;
				r.ea=c->r[bs]+((idx==4)?0:(c->r[idx]<<ss));
			}
			else r.ea=c->r[rm];
		}
		else if (r.mod==1)
		{
			XS8 dsp=(XS8)M_RD8(m,c->eip++);
			if (rm==4)
			{
				XU8 s=M_RD8(m,c->eip++);
				int ss=(s>>6)&3,idx=(s>>3)&7,bs=s&7;
				r.ea=c->r[bs]+((idx==4)?0:(c->r[idx]<<ss))+dsp;
			}
			else r.ea=c->r[rm]+dsp;
		}
		else
		{
			XS32 dsp=(XS32)rd32(m,c->eip); c->eip+=4;
			if (rm==4)
			{
				XU8 s=M_RD8(m,c->eip++);
				int ss=(s>>6)&3,idx=(s>>3)&7,bs=s&7;
				r.ea=c->r[bs]+((idx==4)?0:(c->r[idx]<<ss))+dsp;
			}
			else r.ea=c->r[rm]+dsp;
		}
	}
	return r;
}

static XU32 ea_rd(X86Mem*m,ModRM*e,int bits)
{
	if(e->isreg) return (bits==8)?((e->ea<4)?(e->ea):(e->ea)):(e->ea);
	return (bits==8)?M_RD8(m,e->ea):(bits==16)?rd16(m,e->ea):rd32(m,e->ea);
}
/* register rm operands need cpu ctx; use these instead */
static XU32 rmrd(X86Cpu*c,X86Mem*m,ModRM*e,int bits)
{
	if(e->isreg){
		if(bits==8){int i=e->ea;return (i<4)?(c->r[i]&0xff):((c->r[i-4]>>8)&0xff);}
		if(bits==16)return c->r[e->ea]&0xffff;
		return c->r[e->ea];
	}
	if(bits==8)return M_RD8(m,e->ea);
	if(bits==16)return rd16(m,e->ea);
	return rd32(m,e->ea);
}
static void rmwr(X86Cpu*c,X86Mem*m,ModRM*e,int bits,XU32 v)
{
	if(e->isreg){
		if(bits==8){int i=e->ea;if(i<4)c->r[i]=(c->r[i]&~0xff)|(v&0xff);else c->r[i-4]=(c->r[i-4]&~0xff00)|((v&0xff)<<8);return;}
		if(bits==16){c->r[e->ea]=(c->r[e->ea]&~0xffff)|(v&0xffff);return;}
		c->r[e->ea]=v;return;
	}
	if(bits==8)M_WR8(m,e->ea,(XU8)v);
	else if(bits==16)wr16(m,e->ea,(XU16)v);
	else wr32(m,e->ea,v);
}
static XU32 regrd(X86Cpu*c,int reg,int bits)
{
	if(bits==8)return (reg<4)?(c->r[reg]&0xff):((c->r[reg-4]>>8)&0xff);
	if(bits==16)return c->r[reg]&0xffff;
	return c->r[reg];
}
static void regwr(X86Cpu*c,int reg,int bits,XU32 v)
{
	if(bits==8){if(reg<4)c->r[reg]=(c->r[reg]&~0xff)|(v&0xff);else c->r[reg-4]=(c->r[reg-4]&~0xff00)|((v&0xff)<<8);return;}
	if(bits==16){c->r[reg]=(c->r[reg]&~0xffff)|(v&0xffff);return;}
	c->r[reg]=v;
}

static int cond(X86Cpu*c,int cc)
{
	int cf=(c->flags&XF_CF)?1:0,zf=(c->flags&XF_ZF)?1:0,
	    sf=(c->flags&XF_SF)?1:0,of=(c->flags&XF_OF)?1:0,
	    pf=(c->flags&XF_PF)?1:0;
	switch(cc)
	{
	case 0:return of; case 1:return !of; case 2:return cf; case 3:return !cf;
	case 4:return zf; case 5:return !zf; case 6:return cf|zf; case 7:return !(cf|zf);
	case 8:return sf; case 9:return !sf; case 10:return pf; case 11:return !pf;
	case 12:return sf!=of; case 13:return sf==of; case 14:return zf||(sf!=of);
	case 15:return !zf&&(sf==of);
	}
	return 0;
}

int X86Step(X86Cpu *c, X86Mem *m)
{
	XU32 op, i, a, b, t;
	int bits, rep=0, repne=0, fs=0;
	ModRM e;
	if (!c->running) return 1;
	/* prefixes */
	for (;;)
	{
		op=M_RD8(m,c->eip);
		if (op==0xf3){rep=1;c->eip++;continue;}
		if (op==0xf2){repne=1;c->eip++;continue;}
		if (op==0x64){fs=1;c->eip++;continue;}
		if (op==0x66||op==0x67||op==0xf0||op==0x26||op==0x2e||op==0x36||op==0x3e||op==0x65){c->eip++;continue;}
		break;
	}
	c->eip++;
	switch(op)
	{
	/* ---- ADD/OR/ADC/SBB/AND/SUB/XOR/CMP r<->rm ---- */
	case 0x00: case 0x01: case 0x02: case 0x03:
	case 0x08: case 0x09: case 0x0a: case 0x0b:
	case 0x10: case 0x11: case 0x12: case 0x13:
	case 0x18: case 0x19: case 0x1a: case 0x1b:
	case 0x20: case 0x21: case 0x22: case 0x23:
	case 0x28: case 0x29: case 0x2a: case 0x2b:
	case 0x30: case 0x31: case 0x32: case 0x33:
	case 0x38: case 0x39: case 0x3a: case 0x3b:
		{
			int grp=op>>3, dir=op&2, w=op&1;
			bits=w?32:8;
			e=modrm(c,m,bits);
			if(fs&&!e.isreg)e.ea+=c->tib;
			if(dir){ a=regrd(c,e.reg,bits); b=rmrd(c,m,&e,bits); }
			else   { a=rmrd(c,m,&e,bits); b=regrd(c,e.reg,bits); }
			t=alu(c,grp,a,b,bits);
			if(grp!=7)
			{
				if(dir)regwr(c,e.reg,bits,t);
				else rmwr(c,m,&e,bits,t);
			}
		}
		break;
	/* imm ALU to accumulator */
	case 0x04: case 0x05: case 0x0c: case 0x0d:
	case 0x14: case 0x15: case 0x1c: case 0x1d:
	case 0x24: case 0x25: case 0x2c: case 0x2d:
	case 0x34: case 0x35: case 0x3c: case 0x3d:
		{
			int grp=op>>3; bits=(op&1)?32:8;
			b=(bits==8)?M_RD8(m,c->eip):(bits==16)?rd16(m,c->eip):rd32(m,c->eip);
			c->eip+=(bits==8)?1:(bits==16?2:4);
			t=alu(c,grp,c->r[XEAX],b,bits);
			if(grp!=7)regwr(c,XEAX,bits,t);
		}
		break;
	/* ---- inc/dec reg ---- */
	case 0x40: case 0x41: case 0x42: case 0x43:
	case 0x44: case 0x45: case 0x46: case 0x47:
		{
			int cf=c->flags&XF_CF;
			c->r[op&7]=alu(c,0,c->r[op&7],1,32);
			c->flags=(c->flags&~XF_CF)|cf;
		}
		break;
	case 0x48: case 0x49: case 0x4a: case 0x4b:
	case 0x4c: case 0x4d: case 0x4e: case 0x4f:
		{
			int cf=c->flags&XF_CF;
			c->r[op&7]=alu(c,5,c->r[op&7],1,32);
			c->flags=(c->flags&~XF_CF)|cf;
		}
		break;
	/* ---- push/pop ---- */
	case 0x50: case 0x51: case 0x52: case 0x53:
	case 0x54: case 0x55: case 0x56: case 0x57:
		X86Push(c,m,c->r[op&7]); break;
	case 0x58: case 0x59: case 0x5a: case 0x5b:
	case 0x5c: case 0x5d: case 0x5e: case 0x5f:
		c->r[op&7]=X86Pop(c,m); break;
	case 0x68: t=rd32(m,c->eip);c->eip+=4;X86Push(c,m,t);break;
	case 0x6a: t=(XS8)M_RD8(m,c->eip);c->eip++;X86Push(c,m,t);break;
	case 0x60: {XU32 sp=c->r[XESP];int i2;for(i2=0;i2<8;i2++)X86Push(c,m,(i2==XESP)?sp:c->r[i2]);}break;
	case 0x61: {int i2;for(i2=7;i2>=0;i2--){XU32 v=X86Pop(c,m);if(i2!=XESP)c->r[i2]=v;}}break;
	case 0x9c: X86Push(c,m,(c->flags&0xed5)|(c->df?0x400:0));break;
	case 0x9d: c->flags=(X86Pop(c,m)&0xed5)|2;c->df=0;break;
	/* ---- jcc short ---- */
	case 0x70: case 0x71: case 0x72: case 0x73:
	case 0x74: case 0x75: case 0x76: case 0x77:
	case 0x78: case 0x79: case 0x7a: case 0x7b:
	case 0x7c: case 0x7d: case 0x7e: case 0x7f:
		{ XS8 dsp=(XS8)M_RD8(m,c->eip);c->eip++; if(cond(c,op&15))c->eip+=dsp; }
		break;
	/* ---- 0x80/81/83 group1 ALU imm ---- */
	case 0x80: case 0x81: case 0x83:
		{
			bits=(op==0x80)?8:32;
			e=modrm(c,m,bits);
			if(fs&&!e.isreg)e.ea+=c->tib;
			if(op==0x83){b=(XS8)M_RD8(m,c->eip);c->eip++;}
			else{b=(bits==8)?M_RD8(m,c->eip):rd32(m,c->eip);c->eip+=(bits==8?1:4);}
			t=alu(c,e.reg,rmrd(c,m,&e,bits),b,bits);
			if(e.reg!=7)rmwr(c,m,&e,bits,t);
		}
		break;
	/* ---- test ---- */
	case 0x84: case 0x85:
		bits=(op&1)?32:8;e=modrm(c,m,bits);
		if(fs&&!e.isreg)e.ea+=c->tib;
		alu(c,4,rmrd(c,m,&e,bits),regrd(c,e.reg,bits),bits);break;
	case 0xa8: case 0xa9:
		bits=(op&1)?32:8;
		b=(bits==8)?M_RD8(m,c->eip):rd32(m,c->eip);c->eip+=(bits==8?1:4);
		alu(c,4,c->r[XEAX],b,bits);break;
	/* ---- xchg ---- */
	case 0x86: case 0x87:
		bits=(op&1)?32:8;e=modrm(c,m,bits);
		if(fs&&!e.isreg)e.ea+=c->tib;
		a=rmrd(c,m,&e,bits);b=regrd(c,e.reg,bits);
		rmwr(c,m,&e,bits,b);regwr(c,e.reg,bits,a);break;
	case 0x90: break;
	case 0x91: case 0x92: case 0x93: case 0x94:
	case 0x95: case 0x96: case 0x97:
		{t=c->r[op&7];c->r[op&7]=c->r[XEAX];c->r[XEAX]=t;}break;
	/* ---- mov ---- */
	case 0x88: case 0x89:
		bits=(op&1)?32:8;e=modrm(c,m,bits);
		if(fs&&!e.isreg)e.ea+=c->tib;
		rmwr(c,m,&e,bits,regrd(c,e.reg,bits));break;
	case 0x8a: case 0x8b:
		bits=(op&1)?32:8;e=modrm(c,m,bits);
		if(fs&&!e.isreg)e.ea+=c->tib;
		regwr(c,e.reg,bits,rmrd(c,m,&e,bits));break;
	case 0x8c: e=modrm(c,m,16);rmwr(c,m,&e,16,c->seg[e.reg&7]);break;
	case 0x8e: e=modrm(c,m,16);c->seg[e.reg&7]=(XU16)rmrd(c,m,&e,16);break;
	case 0xb0: case 0xb1: case 0xb2: case 0xb3:
	case 0xb4: case 0xb5: case 0xb6: case 0xb7:
		regwr(c,op&7,8,M_RD8(m,c->eip));c->eip++;break;
	case 0xb8: case 0xb9: case 0xba: case 0xbb:
	case 0xbc: case 0xbd: case 0xbe: case 0xbf:
		c->r[op&7]=rd32(m,c->eip);c->eip+=4;break;
	case 0xa0: a=rd32(m,c->eip);c->eip+=4;if(fs)a+=c->tib;regwr(c,XEAX,8,M_RD8(m,a));break;
	case 0xa1: a=rd32(m,c->eip);c->eip+=4;if(fs)a+=c->tib;c->r[XEAX]=rd32(m,a);break;
	case 0xa2: a=rd32(m,c->eip);c->eip+=4;if(fs)a+=c->tib;M_WR8(m,a,(XU8)c->r[XEAX]);break;
	case 0xa3: a=rd32(m,c->eip);c->eip+=4;if(fs)a+=c->tib;wr32(m,a,c->r[XEAX]);break;
	/* ---- movs/lods/stos/scas/cmps ---- */
	case 0xa4: case 0xa5:
		{
			int sz=(op&1)?4:1;
			do{
				t=(sz==4)?rd32(m,c->r[XESI]):M_RD8(m,c->r[XESI]);
				if(sz==4)wr32(m,c->r[XEDI],t);else M_WR8(m,c->r[XEDI],(XU8)t);
				i=c->df?(XU32)(-(XS32)sz):(XU32)sz;c->r[XESI]+=i;c->r[XEDI]+=i;
				if(rep)c->r[XECX]--;
			}while(rep&&c->r[XECX]);
		}
		break;
	case 0xaa: case 0xab:
		{
			int sz=(op&1)?4:1;
			do{
				if(sz==4)wr32(m,c->r[XEDI],c->r[XEAX]);else M_WR8(m,c->r[XEDI],(XU8)c->r[XEAX]);
				c->r[XEDI]+=c->df?-sz:sz;
				if(rep)c->r[XECX]--;
			}while(rep&&c->r[XECX]);
		}
		break;
	case 0xac: case 0xad:
		{
			int sz=(op&1)?4:1;
			do{
				if(sz==4)c->r[XEAX]=rd32(m,c->r[XESI]);else regwr(c,XEAX,8,M_RD8(m,c->r[XESI]));
				c->r[XESI]+=c->df?-sz:sz;
				if(rep)c->r[XECX]--;
			}while(rep&&c->r[XECX]);
		}
		break;
	case 0xae: case 0xaf:
		{
			int sz=(op&1)?4:1;XU32 v;
			do{
				v=(sz==4)?rd32(m,c->r[XEDI]):M_RD8(m,c->r[XEDI]);
				alu(c,7,c->r[XEAX],v,sz*8);
				c->r[XEDI]+=c->df?-sz:sz;
				if(rep||repne)c->r[XECX]--;
			}while((rep||repne)&&c->r[XECX]&&((repne&&!(c->flags&XF_ZF))||(rep&&(c->flags&XF_ZF))));
		}
		break;
	case 0xa6: case 0xa7:
		{
			int sz=(op&1)?4:1;XU32 v1,v2;
			do{
				v1=(sz==4)?rd32(m,c->r[XESI]):M_RD8(m,c->r[XESI]);
				v2=(sz==4)?rd32(m,c->r[XEDI]):M_RD8(m,c->r[XEDI]);
				alu(c,7,v1,v2,sz*8);
				i=c->df?-sz:sz;c->r[XESI]+=i;c->r[XEDI]+=i;
				if(rep||repne)c->r[XECX]--;
			}while((rep||repne)&&c->r[XECX]&&((repne&&!(c->flags&XF_ZF))||(rep&&(c->flags&XF_ZF))));
		}
		break;
	/* ---- movzx/movsx, lea, jcc near, setcc, imul3, shifts2, bt ---- */
	case 0x0f:
		{
			XU8 o2=M_RD8(m,c->eip++);
			if(o2>=0x80&&o2<=0x8f)
			{XS32 dsp=(XS32)rd32(m,c->eip);c->eip+=4;if(cond(c,o2&15))c->eip+=dsp;break;}
			if(o2>=0x90&&o2<=0x9f)
			{e=modrm(c,m,8);if(fs&&!e.isreg)e.ea+=c->tib;rmwr(c,m,&e,8,cond(c,o2&15)?1:0);break;}
			switch(o2)
			{
			case 0x1f: e=modrm(c,m,32);break; /* nop hint */
			case 0x31: /* rdtsc */ c->r[XEAX]=0;c->r[XEDX]=0;break;
			case 0xaf: e=modrm(c,m,32);if(fs&&!e.isreg)e.ea+=c->tib;
				{XS64 r=(XS64)(XS32)c->r[e.reg]*(XS64)(XS32)rmrd(c,m,&e,32);
				c->r[e.reg]=(XU32)r;
				c->flags&=~(XF_CF|XF_OF);if(r!=(XS32)r)c->flags|=XF_CF|XF_OF;}break;
			case 0xb6: case 0xb7:
				bits=(o2&1)?16:8;e=modrm(c,m,bits);
				if(fs&&!e.isreg)e.ea+=c->tib;
				c->r[e.reg]=rmrd(c,m,&e,bits);break;
			case 0xbe: case 0xbf:
				bits=(o2&1)?16:8;e=modrm(c,m,bits);
				if(fs&&!e.isreg)e.ea+=c->tib;
				{c->r[e.reg]=(bits==8)?(XU32)(XS8)rmrd(c,m,&e,8):(XU32)(XS16)rmrd(c,m,&e,16);}break;
			case 0xa3: case 0xb3: case 0xbb: case 0xab:
				e=modrm(c,m,32);if(fs&&!e.isreg)e.ea+=c->tib;
				{XU32 bt=rmrd(c,m,&e,32);c->flags&=~XF_CF;if(bt&(1u<<(c->r[e.reg]&31)))c->flags|=XF_CF;
				if(o2==0xab)bt|=1u<<(c->r[e.reg]&31);
				else if(o2==0xb3)bt&=~(1u<<(c->r[e.reg]&31));
				else if(o2==0xbb)bt^=1u<<(c->r[e.reg]&31);
				if(o2!=0xa3)rmwr(c,m,&e,32,bt);}break;
			case 0xba:
				e=modrm(c,m,32);if(fs&&!e.isreg)e.ea+=c->tib;
				{XU8 imm=M_RD8(m,c->eip++);XU32 bt=rmrd(c,m,&e,32);
				c->flags&=~XF_CF;if(bt&(1u<<(imm&31)))c->flags|=XF_CF;
				if(e.reg==5)bt|=1u<<(imm&31);else if(e.reg==6)bt&=~(1u<<(imm&31));
				else if(e.reg==7)bt^=1u<<(imm&31);
				if(e.reg>=5)rmwr(c,m,&e,32,bt);}break;
			case 0xbc: case 0xbd: /* bsf/bsr */
				e=modrm(c,m,32);if(fs&&!e.isreg)e.ea+=c->tib;
				{XU32 v=rmrd(c,m,&e,32);int i2,f=-1;
				if(o2==0xbc){for(i2=0;i2<32;i2++)if(v&(1u<<i2)){f=i2;break;}}
				else{for(i2=31;i2>=0;i2--)if(v&(1u<<i2)){f=i2;break;}}
				c->flags&=~XF_ZF;if(f<0)c->flags|=XF_ZF;else c->r[e.reg]=f;}break;
			case 0xc0: case 0xc1: /* xadd */
				bits=(o2&1)?32:8;e=modrm(c,m,bits);
				if(fs&&!e.isreg)e.ea+=c->tib;
				{a=rmrd(c,m,&e,bits);b=regrd(c,e.reg,bits);
				rmwr(c,m,&e,bits,alu(c,0,a,b,bits));regwr(c,e.reg,bits,a);}break;
			case 0xb0: case 0xb1: /* cmpxchg */
				bits=(o2&1)?32:8;e=modrm(c,m,bits);
				if(fs&&!e.isreg)e.ea+=c->tib;
				{a=rmrd(c,m,&e,bits);alu(c,7,c->r[XEAX],a,bits);
				if(c->flags&XF_ZF)rmwr(c,m,&e,bits,regrd(c,e.reg,bits));
				else regwr(c,XEAX,bits,a);}break;
			default:
				return 2;
			}
		}
		break;
	/* ---- lea / mov ptr ---- */
	case 0x8d:
		e=modrm(c,m,32);
		if(fs&&!e.isreg)e.ea+=c->tib;
		c->r[e.reg]=e.ea;break;
	case 0x8f: e=modrm(c,m,32);t=X86Pop(c,m);rmwr(c,m,&e,32,t);break;
	case 0xc4: case 0xc5: /* les/lds -> load ea base+seg */
		e=modrm(c,m,32);if(fs&&!e.isreg)e.ea+=c->tib;
		c->r[e.reg]=rd32(m,e.ea);c->seg[(op==0xc4)?0:3]=rd16(m,e.ea+4);break;
	/* ---- c6/c7 mov imm ---- */
	case 0xc6: case 0xc7:
		bits=(op&1)?32:8;e=modrm(c,m,bits);
		if(fs&&!e.isreg)e.ea+=c->tib;
		t=(bits==8)?M_RD8(m,c->eip):rd32(m,c->eip);c->eip+=(bits==8?1:4);
		rmwr(c,m,&e,bits,t);break;
	/* ---- shifts group2 ---- */
	case 0xc0+0x60: /* 0x120 invalid placeholder */
		break;
	case 0xd0: case 0xd1: case 0xd2: case 0xd3:
	case 0xc0: case 0xc1:
		{
			XU32 cnt;
			bits=(op&1)?32:8;e=modrm(c,m,bits);
			if(fs&&!e.isreg)e.ea+=c->tib;
			if(op==0xd0||op==0xd1)cnt=1;
			else if(op==0xd2||op==0xd3)cnt=c->r[XECX]&0xff;
			else{cnt=M_RD8(m,c->eip);c->eip++;}
			rmwr(c,m,&e,bits,shft(c,e.reg,rmrd(c,m,&e,bits),cnt,bits));
		}
		break;
	/* ---- f6/f7: not/neg/mul/imul/div/idiv/test ---- */
	case 0xf6: case 0xf7:
		{
			bits=(op&1)?32:8;e=modrm(c,m,bits);
			if(fs&&!e.isreg)e.ea+=c->tib;
			a=rmrd(c,m,&e,bits);
			switch(e.reg)
			{
			case 0: case 1:
				b=(bits==8)?M_RD8(m,c->eip):rd32(m,c->eip);c->eip+=(bits==8?1:4);
				alu(c,4,a,b,bits);break;
			case 2:
				rmwr(c,m,&e,bits,~a);break;
			case 3:
				rmwr(c,m,&e,bits,alu(c,5,0,a,bits));break;
			case 4: /* mul */
				if(bits==8){XU16 r=(XU16)(c->r[XEAX]&0xff)*(XU16)a;c->r[XEAX]=(c->r[XEAX]&~0xffff)|r;
					c->flags&=~(XF_CF|XF_OF);if(r>0xff)c->flags|=XF_CF|XF_OF;}
				else{XU64 r=(XU64)c->r[XEAX]*a;c->r[XEAX]=(XU32)r;c->r[XEDX]=(XU32)(r>>32);
					c->flags&=~(XF_CF|XF_OF);if(c->r[XEDX])c->flags|=XF_CF|XF_OF;}break;
			case 5: /* imul */
				if(bits==8){XS16 r=(XS16)(XS8)(c->r[XEAX]&0xff)*(XS16)(XS8)a;c->r[XEAX]=(c->r[XEAX]&~0xffff)|(XU16)r;
					c->flags&=~(XF_CF|XF_OF);if(r!=(XS8)r)c->flags|=XF_CF|XF_OF;}
				else{XS64 r=(XS64)(XS32)c->r[XEAX]*(XS64)(XS32)a;c->r[XEAX]=(XU32)r;c->r[XEDX]=(XU32)(r>>32);
					c->flags&=~(XF_CF|XF_OF);if(r!=(XS32)r)c->flags|=XF_CF|XF_OF;}break;
			case 6: /* div */
				if(bits==8){XU16 dd=(XU16)c->r[XEAX];if(!a)return 3;c->r[XEAX]=(c->r[XEAX]&~0xffff)|((dd/a)&0xff)|(((dd%a)&0xff)<<8);}
				else{XU64 dd=((XU64)c->r[XEDX]<<32)|c->r[XEAX];if(!a)return 3;c->r[XEAX]=(XU32)(dd/a);c->r[XEDX]=(XU32)(dd%a);}break;
			case 7: /* idiv */
				if(bits==8){XS16 dd=(XS16)c->r[XEAX];if(!a)return 3;c->r[XEAX]=(c->r[XEAX]&~0xffff)|(((dd/(XS8)a)&0xff)|(((dd%(XS8)a)&0xff)<<8));}
				else{XS64 dd=(XS64)(((XU64)c->r[XEDX]<<32)|c->r[XEAX]);XS32 sa=(XS32)a;if(!sa)return 3;c->r[XEAX]=(XU32)(dd/sa);c->r[XEDX]=(XU32)(dd%sa);}break;
			}
		}
		break;
	/* ---- fe/ff group ---- */
	case 0xfe:
		e=modrm(c,m,8);
		if(fs&&!e.isreg)e.ea+=c->tib;
		if(e.reg==0){int cf=c->flags&XF_CF;rmwr(c,m,&e,8,alu(c,0,rmrd(c,m,&e,8),1,8));c->flags=(c->flags&~XF_CF)|cf;}
		else if(e.reg==1){int cf=c->flags&XF_CF;rmwr(c,m,&e,8,alu(c,5,rmrd(c,m,&e,8),1,8));c->flags=(c->flags&~XF_CF)|cf;}
		break;
	case 0xff:
		e=modrm(c,m,32);
		if(fs&&!e.isreg)e.ea+=c->tib;
		switch(e.reg)
		{
		case 0:{int cf=c->flags&XF_CF;rmwr(c,m,&e,32,alu(c,0,rmrd(c,m,&e,32),1,32));c->flags=(c->flags&~XF_CF)|cf;break;}
		case 1:{int cf=c->flags&XF_CF;rmwr(c,m,&e,32,alu(c,5,rmrd(c,m,&e,32),1,32));c->flags=(c->flags&~XF_CF)|cf;break;}
		case 2:X86Push(c,m,c->eip);c->eip=rmrd(c,m,&e,32);goto callcheck;
		case 3:/* callf */X86Push(c,m,c->seg[1]);X86Push(c,m,c->eip);c->eip=rd32(m,e.ea);c->seg[1]=rd16(m,e.ea+4);goto callcheck;
		case 4:c->eip=rmrd(c,m,&e,32);goto callcheck;
		case 5:c->eip=rd32(m,e.ea);goto callcheck;
		case 6:X86Push(c,m,rmrd(c,m,&e,32));break;
		}
callcheck:
		if((op==0xff&&(e.reg==2||e.reg==3||e.reg==4||e.reg==5))&&c->trap)
		{
			/* call/jmp target may be an API thunk address */
			if(c->trap(c->trapctx,c,c->eip))return 0;
		}
		break;
	/* ---- jmp/call/ret ---- */
	case 0xe8:
		{XS32 dsp=(XS32)rd32(m,c->eip);c->eip+=4;X86Push(c,m,c->eip);c->eip+=dsp;
		if(c->trap&&c->trap(c->trapctx,c,c->eip))return 0;}
		break;
	case 0xe9: c->eip+=(XS32)rd32(m,c->eip)+4;break;
	case 0xeb: c->eip+=(XS8)M_RD8(m,c->eip)+1;break;
	case 0xea:{XS32 o=rd32(m,c->eip);c->seg[1]=rd16(m,c->eip+4);c->eip=o;}break;
	case 0xc2: i=rd16(m,c->eip);c->eip+=2;c->eip=X86Pop(c,m);c->r[XESP]+=i;break;
	case 0xc3: c->eip=X86Pop(c,m);break;
	case 0xca: i=rd16(m,c->eip);c->eip+=2;c->eip=X86Pop(c,m);c->seg[1]=(XU16)X86Pop(c,m);c->r[XESP]+=i;break;
	case 0xcb: c->eip=X86Pop(c,m);c->seg[1]=(XU16)X86Pop(c,m);break;
	/* ---- loops ---- */
	case 0xe0: case 0xe1: case 0xe2: case 0xe3:
		{XS8 dsp=(XS8)M_RD8(m,c->eip);c->eip++;
		if(op!=0xe3)c->r[XECX]--;
		{int tk=(op==0xe3)?(c->r[XECX]==0):(op==0xe0)?(c->r[XECX]&&!(c->flags&XF_ZF)):(op==0xe1)?(c->r[XECX]&&(c->flags&XF_ZF)):(c->r[XECX]!=0);
		if(tk)c->eip+=dsp;}}break;
	/* ---- misc ---- */
	case 0x98: c->r[XEAX]=(XU32)(XS16)(XS8)(c->r[XEAX]&0xff);break;/*cbw->cwde: use32*/
	case 0x99: c->r[XEDX]=(c->r[XEAX]&0x80000000)?0xffffffff:0;break;
	case 0xf8: c->flags&=~XF_CF;break;
	case 0xf9: c->flags|=XF_CF;break;
	case 0xfa: case 0xfb: break; /* cli/sti */
	case 0xfc: c->df=0;break;
	case 0xfd: c->df=1;break;
	case 0xf5: c->flags^=XF_CF;break;
	case 0xf4: c->halted=1;return 4;
	case 0xcc: return 5; /* int3 */
	case 0xcd: i=M_RD8(m,c->eip);c->eip++;return 0x100|(int)i; /* int n */
	case 0x9e: /* sahf */ c->flags=(c->flags&~0xd5)|((c->r[XEAX]>>8)&0xd5)|2;break;
	case 0x9f: /* lahf */ c->r[XEAX]=(c->r[XEAX]&~0xff00)|((c->flags&0xd5)<<8);break;
	case 0x27: case 0x2f: case 0x37: case 0x3f: /* BCD: stub */ break;
	case 0xd4: case 0xd5: c->eip++;break; /* aam/aad stub */
	case 0xd6: case 0x62: case 0x82: case 0x9b:
	case 0xdb: case 0xdc: case 0xdd: case 0xde:
	case 0xd8: case 0xd9: case 0xda: case 0xdf:
		/* FPU/x87 — stubbed: skip modrm forms minimally */
		e=modrm(c,m,32);
		break;
	default:
		return 6;
	}
	return 0;
}

int X86Run(X86Cpu *c, X86Mem *m, XU32 maxinsns)
{
	XU32 n=0; int r;
	while(c->running&&!c->halted&&n<maxinsns)
	{
		r=X86Step(c,m);
		if(r)return r;
		n++;
	}
	return 0;
}
