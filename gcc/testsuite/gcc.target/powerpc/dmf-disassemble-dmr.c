/* { dg-do compile } */
/* { dg-options "-mdejagnu-cpu=future -O2" } */

typedef unsigned char vec_t __attribute__((vector_size(16)));

void
bar (vec_t *dst, __dmr1024 *src)
{
  vec_t res[8];
  __builtin_mma_disassemble_dmr (res, src);
  dst[0] = res[0];
  dst[2] = res[1];
  dst[4] = res[2];
  dst[6] = res[3];
  dst[8] = res[4];
  dst[10] = res[5];
  dst[12] = res[6];
  dst[14] = res[7];
}

/* { dg-final { scan-assembler-times {\mdmxxextfdmr512\M} 2 } } */
/* { dg-final { scan-assembler-times {\mstxv\M} 8 } } */
