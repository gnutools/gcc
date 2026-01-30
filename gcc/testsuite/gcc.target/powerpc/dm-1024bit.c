/* { dg-do compile } */
/* { dg-require-effective-target powerpc_dense_math_ok } */
/* { dg-options "-mdejagnu-cpu=future -O2" } */

/* Test basic load/store for __dm1024 type.  */

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

void foo_mem_asm (__dm1024 *p, __dm1024 *q)
{
  /* 2 LXVP instructions.  */
  __dm1024 vq = *p;

  /* 2 DMXXINSTDMR512 instructions to transfer VSX to dense math register.  */
  __asm__ ("# foo (" CONSTRAINT ") %A0" : "+" CONSTRAINT (vq));
  /* 2 DMXXEXTFDMR512 instructions to transfer dense math register to VSX.  */

  /* 2 STXVP instructions.  */
  *q = vq;
}

void foo_mem_asm2 (__dm1024 *p, __dm1024 *q)
{
  /* 2 LXVP instructions.  */
  __dm1024 vq = *p;
  __dm1024 vq2;
  __dm1024 vq3;

  /* 2 DMXXINSTDMR512 instructions to transfer VSX to dense math register.  */
  __asm__ ("# foo1 (" CONSTRAINT ") %A0" : "+" CONSTRAINT (vq));
  /* 2 DMXXEXTFDMR512 instructions to transfer dense math register to VSX.  */

  vq2 = vq;
  __asm__ ("# foo2 (wa) %0" : "+wa" (vq2));

  /* 2 STXVP instructions.  */
  *q = vq2;
}

void foo_mem (__dm1024 *p, __dm1024 *q)
{
  /* 2 LXVP, 2 STXVP instructions, no dense math transfer.  */
  *q = *p;
}

/* { dg-final { scan-assembler-times {\mdmxxextfdmr512\M}  4 } } */
/* { dg-final { scan-assembler-times {\mdmxxinstdmr512\M}  4 } } */
/* { dg-final { scan-assembler-times {\mlxvp\M}           12 } } */
/* { dg-final { scan-assembler-times {\mstxvp\M}          12 } } */
