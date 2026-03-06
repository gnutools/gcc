/* { dg-do compile } */
/* { dg-require-effective-target powerpc_dense_math_ok } */
/* { dg-options "-mdejagnu-cpu=future -O2" } */

/* Test basic dense math support for MMA.  */

void
move_simple (__vector_quad *a, __vector_quad *b)
{
  /* 2 lxvp, 2 stxvp.   */
  __vector_quad c = *a;
  *b = c;
}

void
move_constraint_d (__vector_quad *a, __vector_quad *b)
{
  /* 2 lxvp, 2 stxvp.   */
  __vector_quad c = *a;
  __asm__ (" # %x0 (d constraint)" : "+d" (c));
  *b = c;
}

void
move_constraint_wD (__vector_quad *a, __vector_quad *b)
{
  /* 2 lxvp, dmxxinstdmr512, dmxxextfdmr512, 2 stxvp.   */
  __vector_quad c = *a;
  __asm__ (" # %A0 (wD constraint)" : "+wD" (c));
  *b = c;
}

void
clear_simple (__vector_quad *a)
{
  /* dmsetdmrz, dmxxextfdmr512, 2 stxvp.  */
  __builtin_mma_xxsetaccz (a);
}

void
clear_constraint_d (__vector_quad *a)
{
  __vector_quad z;

  /* dmsetdmrz, dmxxextfdmr512, 2 stxvp.  */
  __builtin_mma_xxsetaccz (&z);
  __asm__ (" # %x0 (d constraint)" : "+d" (z));
  *a = z;
}

void
clear_constraint_wD (__vector_quad *a)
{
  __vector_quad z;

  /* dmsetdmrz, dmxxextfdmr512, 2 stxvp.  */
  __builtin_mma_xxsetaccz (&z);
  __asm__ (" # %A0 (d constraint)" : "+wD" (z));
  *a = z;
}

/* { dg-final { scan-assembler-times {\mdmsetdmrz\M}       3 } } */
/* { dg-final { scan-assembler-times {\mdmxxextfdmr512\M}  4 } } */
/* { dg-final { scan-assembler-times {\mdmxxinstdmr512\M}  1 } } */
/* { dg-final { scan-assembler-not   {\mxxmfacc\M}           } } */
/* { dg-final { scan-assembler-not   {\mxxmtacc\M}           } } */
/* { dg-final { scan-assembler-not   {\mxxsetaccz\M}         } } */
