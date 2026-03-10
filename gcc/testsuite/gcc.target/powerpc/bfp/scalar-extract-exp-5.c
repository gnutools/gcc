/* { dg-do compile { target { powerpc*-*-* } } } */
/* { dg-options "-mdejagnu-cpu=power9 -mvsx -mfloat128" } */
/* { dg-require-effective-target ilp32 } */
/* { dg-require-effective-target ppc_ieee128_ok } */
/* { dg-require-effective-target powerpc_vsx } */
/* { dg-prune-output ".-mfloat128. option may not be fully supported" } */

/* This test only runs on 32-bit configurations, where a compiler error
   should be issued because this builtin is not available on
   32-bit configurations.  */

#include <altivec.h>

unsigned long long int
get_exponent (__ieee128 *p)
{
  __ieee128 source = *p;

  return scalar_extract_exp (source);	/* { dg-error "requires quad-precision floating-point arithmetic" } */
}


