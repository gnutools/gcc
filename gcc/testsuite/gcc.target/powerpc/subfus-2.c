/* { dg-do compile { target lp64 } } */
/* { dg-require-effective-target powerpc_future_ok } */
/* { dg-options "-mdejagnu-cpu=future -O2" } */

/* Test whether the saturating subtract built-in generates subwus for 64-bit
   subtracts.  */

long do_sat_long  (long  a, long  b)
{
  return __builtin_saturate_subtract64 (a, b);		/* subwus  */
}

long do_sat_long_dot  (long  a, long  b, long  *p)
{
  long  r = __builtin_saturate_subtract64 (a, b);	/* subwus.  */
  if (r == 0)
    *p = 0;

  return r;
}

void do_sat_long_dot2  (long  a, long  b, long  *p, long *q)
{
  if (__builtin_saturate_subtract64 (a, b))		/* subwus.  */
    *p = 0;

  *q = a + b;
  return;
}

/* { dg-final { scan-assembler     {\msubdus\M} } } */
/* { dg-final { scan-assembler-not {\msubf\M}   } } */
