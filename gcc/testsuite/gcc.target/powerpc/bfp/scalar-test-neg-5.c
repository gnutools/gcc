/* { dg-do compile { target { powerpc*-*-* } } } */
/* { dg-options "-mdejagnu-cpu=power8 -mvsx -mfloat128" } */
/* { dg-require-effective-target ppc_ieee128_ok } */
/* { dg-require-effective-target powerpc_vsx } */
/* { dg-prune-output ".-mfloat128. option may not be fully supported" } */

#include <altivec.h>
#include <stdbool.h>

bool
test_neg (__ieee128 *p)
{
  __ieee128 source = *p;

  return __builtin_vec_scalar_test_neg (source); /* { dg-error "'__builtin_vsx_scalar_test_neg_qp' requires" } */
}
