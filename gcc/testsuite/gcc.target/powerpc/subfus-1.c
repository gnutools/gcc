/* { dg-do compile } */
/* { dg-require-effective-target powerpc_future_ok } */
/* { dg-options "-mdejagnu-cpu=future -O2" } */

/* Test whether the saturating subtract built-in generates subwus for 32-bit
   subtracts.  */

int do_sat_int  (int  a, int  b)
{
  return __builtin_saturate_subtract32 (a, b);		/* subwus  */
}

int do_sat_int_dot  (int  a, int  b, int  *p)
{
  int  r = __builtin_saturate_subtract32 (a, b);	/* subwus.  */
  if (r == 0)
    *p = 0;

  return r;
}

void do_sat_int_dot2  (int  a, int  b, int  *p, int *q)
{
  if (__builtin_saturate_subtract32 (a, b))		/* subwus.  */
    *p = 0;

  *q = a + b;
  return;
}

/* { dg-final { scan-assembler     {\msubwus\M} } } */
/* { dg-final { scan-assembler-not {\msubf\M}   } } */
