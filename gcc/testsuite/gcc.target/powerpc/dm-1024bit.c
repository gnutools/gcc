/* { dg-do compile } */
/* { dg-require-effective-target powerpc_dense_math_ok } */
/* { dg-options "-mdejagnu-cpu=future -O2" } */

/* Test basic load/store for __dmr type.  */

#ifndef CONSTRAINT
#if defined(USE_D)
#define CONSTRAINT "d"

#elif defined(USE_V)
#define CONSTRAINT "v"

#elif defined(USE_WA)
#define CONSTRAINT "wa"

#else
#define CONSTRAINT "wD"
#endif
#endif
const char constraint[] = CONSTRAINT;

void foo_mem_asm (__dmr *p, __dmr *q)
{
  /* 2 LXVP instructions.  */
  __dmr vq = *p;

  /* 2 DMXXINSTDMR512 instructions to transfer VSX to DMR.  */
  __asm__ ("# foo (" CONSTRAINT ") %A0" : "+" CONSTRAINT (vq));
  /* 2 DMXXEXTFDMR512 instructions to transfer DMR to VSX.  */

  /* 2 STXVP instructions.  */
  *q = vq;
}

void foo_mem_asm2 (__dmr *p, __dmr *q)
{
  /* 2 LXVP instructions.  */
  __dmr vq = *p;
  __dmr vq2;
  __dmr vq3;

  /* 2 DMXXINSTDMR512 instructions to transfer VSX to DMR.  */
  __asm__ ("# foo1 (" CONSTRAINT ") %A0" : "+" CONSTRAINT (vq));
  /* 2 DMXXEXTFDMR512 instructions to transfer DMR to VSX.  */

  vq2 = vq;
  __asm__ ("# foo2 (wa) %0" : "+wa" (vq2));

  /* 2 STXVP instructions.  */
  *q = vq2;
}

void foo_mem (__dmr *p, __dmr *q)
{
  /* 2 LXVP, 2 STXVP instructions, no DMR transfer.  */
  *q = *p;
}

/* { dg-final { scan-assembler-times {\mdmxxextfdmr512\M}  4 } } */
/* { dg-final { scan-assembler-times {\mdmxxinstdmr512\M}  4 } } */
/* { dg-final { scan-assembler-times {\mlxvp\M}           12 } } */
/* { dg-final { scan-assembler-times {\mstxvp\M}          12 } } */
