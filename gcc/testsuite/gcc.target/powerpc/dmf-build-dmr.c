/* { dg-do compile } */
/* { dg-options "-mdejagnu-cpu=future -O2" } */

typedef unsigned char vec_t __attribute__((vector_size(16)));

void
foo2 (__dmr1024 *dst, vec_t *src)
{
  __builtin_mma_build_dmr (dst, src[0], src[1], src[2], src[3], src[4], src[5], src[6], src[7]);
}

/* { dg-final { scan-assembler-times {\mdmxxinstdmr512\M} 2 } } */
/* { dg-final { scan-assembler-times {\mlxv\M} 8 } } */
