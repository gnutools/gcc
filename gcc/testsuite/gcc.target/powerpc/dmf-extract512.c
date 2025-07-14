/* { dg-do compile } */
/* { dg-options "-mdejagnu-cpu=future -O2" } */

typedef unsigned char vec_t __attribute__((vector_size(16)));

void
bar (vec_t *dst, __dmr *src)
{
  vec_t res[4];
  __builtin_mma_dmr_extract512 (res, src);
  dst[0] = res[0];
  dst[2] = res[1];
  dst[4] = res[2];
  dst[6] = res[3];
}

/* { dg-final { scan-assembler-times {\mdmxxextfdmr512\M} 1 } } */
/* { dg-final { scan-assembler-times {\mstxv\M} 4 } } */
