/* x86run.c — host-side harness: load a flat CE module .img at its
 * vbase and run the i386 interpreter on its real x86 code. Traps on
 * calls outside the loaded regions (imports/kernel). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../shell/x86emu.h"

#define MAXREG 64
typedef struct { XU32 base,size; XU8 *host; const char *nm; } Reg;
static Reg s_r[MAXREG]; static int s_nr;

static Reg *find(XU32 a){int i;for(i=0;i<s_nr;i++)if(a>=s_r[i].base&&a<s_r[i].base+s_r[i].size)return &s_r[i];return 0;}
static XU8 rd8(void*c,XU32 a){Reg*r=find(a);return r?r->host[a-r->base]:0xff;}
static void wr8(void*c,XU32 a,XU8 v){Reg*r=find(a);if(r)r->host[a-r->base]=v;}
static X86Mem s_mem={0,rd8,wr8};

static void reg(XU32 b,XU32 s,const XU8*d,const char*n)
{Reg*r=&s_r[s_nr++];r->base=b;r->size=s;r->nm=n;r->host=malloc(s);memset(r->host,0xff,s);if(d)memcpy(r->host,d,s);}

static int trap(void*ctx,X86Cpu*c,XU32 tgt)
{
	Reg*r=find(tgt);
	(void)ctx;
	fprintf(stderr,"[trap] eip=%x -> %x (%s) eax=%x ebx=%x ecx=%x edx=%x esp=%x\n",
		c->eip,tgt,r?r->nm:"?",c->r[0],c->r[3],c->r[1],c->r[2],c->r[4]);
	if(!r)return 1; /* outside any module: report and stop */
	return 0;
}

int main(int argc,char**argv)
{
	FILE*f;XU8*img;long sz;XU32 vb,ent;X86Cpu cpu;
	if(argc<3){fprintf(stderr,"x86run <mod.img> <vbase> [entry]\n");return 1;}
	vb=strtoul(argv[2],0,0);
	f=fopen(argv[1],"rb");if(!f)return 2;
	fseek(f,0,SEEK_END);sz=ftell(f);fseek(f,0,SEEK_SET);
	img=malloc(sz);fread(img,1,sz,f);fclose(f);
	reg(vb,sz,img,argv[1]);
	/* stack region */
	reg(0x30000000,0x40000,0,"stack");
	X86Init(&cpu);
	cpu.eip=vb+(argc>3?strtoul(argv[3],0,0):0);
	cpu.r[XESP]=0x30000000+0x3f000;
	cpu.trap=trap;cpu.trapctx=0;
	{
		int r=X86Run(&cpu,&s_mem,2000000);
		fprintf(stderr,"done r=%d eip=%x\n",r,cpu.eip);
	}
	return 0;
}
