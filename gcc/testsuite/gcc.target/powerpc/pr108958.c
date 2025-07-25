/* { dg-do compile } */
/* { dg-require-effective-target int128 } */
/* { dg-require-effective-target lp64 } */
/* { dg-options "-mdejagnu-cpu=power9 -O2" } */

#ifndef TYPE
#define TYPE unsigned long long
#endif

/* PR target/108958, when zero extending a DImode to a TImode, and the TImode variable is in a VSX register, generate:

	mtvsrdd vreg,0,gpr

   instead of:

	mr tmp,gpr
	li tmp+1,0
	mtvsrdd vreg,tmp+1,tmp.  */

void
gpr_to_vsx (TYPE x, __uint128_t *p)
{
  /* mtvsrdd 0,0,3
     stvx 0,0(4)  */

  __uint128_t y = x;
  __asm__ (" # %x0" : "+wa" (y));
  *p = y;
}

void
gpr_to_gpr (TYPE x, __uint128_t *p)
{
  /* mr 2,3
     li 3,0
     std 2,0(4)
     std 3,8(4)  */

  __uint128_t y = x;
  __asm__ (" # %0" : "+r" (y));
  *p = y;
}

/* { dg-final { scan-assembler-times {\mli\M}              1 } } */
/* { dg-final { scan-assembler-times {\mmtvsrdd .*,0,.*\M} 1 } } */
/* { dg-final { scan-assembler-times {\mstd\M}             2 } } */
/* { dg-final { scan-assembler-times {\mstxv\M}            1 } } */
