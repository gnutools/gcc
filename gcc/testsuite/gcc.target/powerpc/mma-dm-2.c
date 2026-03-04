/* { dg-do compile } */
/* { dg-require-effective-target powerpc_dense_math_ok } */
/* { dg-options "-mdejagnu-cpu=power10 -mno-dense-math -O2" } */

/* Test basic dense math support for MMA.  */

void
move_simple (__vector_quad *a, __vector_quad *b)
{
  /* 2 lxvp, xxmtacc, xxftacc 2 stxvp.   */
  __vector_quad c = *a;
  *b = c;
}

void
move_constraint_d (__vector_quad *a, __vector_quad *b)
{
  /* 2 lxvp, xxmtacc, xxftacc, 2 stxvp.   */
  __vector_quad c = *a;
  __asm__ (" # %x0 (d constraint)" : "+d" (c));
  *b = c;
}

void
move_constraint_wD (__vector_quad *a, __vector_quad *b)
{
  /* 2 lxvp, xxmtacc, xxftacc, 2 stxvp.   */
  __vector_quad c = *a;
  __asm__ (" # %A0 (wD constraint)" : "+wD" (c));
  *b = c;
}

void
clear_simple (__vector_quad *a)
{
  /* xxsetaccz, xxmfacc, 2 stxvp.  */
  __builtin_mma_xxsetaccz (a);
}

void
clear_constraint_d (__vector_quad *a)
{
  __vector_quad z;

  /* xxsetaccz, xxmfacc, 2 stxvp.  */
  __builtin_mma_xxsetaccz (&z);
  __asm__ (" # %x0 (d constraint)" : "+d" (z));
  *a = z;
}

void
clear_constraint_wD (__vector_quad *a)
{
  __vector_quad z;

  /* xxsetaccz, xxmfacc, 2 stxvp.  */
  __builtin_mma_xxsetaccz (&z);
  __asm__ (" # %A0 (d constraint)" : "+wD" (z));
  *a = z;
}

/* { dg-final { scan-assembler-not   {\mdmsetdmrz\M}        } } */
/* { dg-final { scan-assembler-not   {\mdmxxextfdmr512\M}   } } */
/* { dg-final { scan-assembler-not   {\mdmxxinstdmr512\M}   } } */
/* { dg-final { scan-assembler-times {\mxxmfacc\M}        6 } } */
/* { dg-final { scan-assembler-times {\mxxmtacc\M}        3 } } */
/* { dg-final { scan-assembler-times {\mxxsetaccz\M}      3 } } */
