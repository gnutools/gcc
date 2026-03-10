/* { dg-do compile { target { powerpc*-*-* } } } */
/* { dg-options "-mdejagnu-cpu=power9 -mvsx -mfloat128" } */
/* { dg-require-effective-target ilp32 } */
/* { dg-require-effective-target ppc_ieee128_ok } */
/* { dg-require-effective-target powerpc_vsx } */
/* { dg-prune-output ".-mfloat128. option may not be fully supported" } */

/* This test only runs on 32-bit configurations, producing a compiler
   error because the builtin requires 64 bits.  */
#include <altivec.h>

unsigned long long int
get_significand (__ieee128 *p)
{
  __ieee128 source = *p;

  return (long long int) __builtin_vec_scalar_extract_sig (source); /* { dg-error "requires quad-precision floating-point arithmetic" } */
}
