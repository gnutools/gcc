/* { dg-do compile { target { powerpc*-*-* } } } */
/* { dg-skip-if "" { powerpc*-*-darwin* powerpc-ibm-aix* } } */
/* { dg-options "-O2 -mdejagnu-cpu=power7 -falign-functions=16 -fno-unroll-loops" } */
/* Without -mno-powerpc64, we wouldn't use the counter register on ilp32, and
   get too many instructions in the loop for the expected loop alignment to
   apply, even if -fno-reorder-blocks were thrown in to avoid duplicating the
   loop body.  */
/* { dg-additional-options "-mno-powerpc64" { target ilp32 } } */
/* { dg-final { scan-assembler ".p2align 5" } } */

void f(double *a, double *b, double *c, unsigned long n) {
  unsigned long i;
  for (i=0; i < n; i++)
    a[i] = b[i] + c[i];
}
