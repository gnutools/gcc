/* { dg-do compile } */
/* { dg-require-effective-target vect_float } */
/* { dg-additional-options "-fdump-tree-optimized-details-blocks" } */

float *x;
float parm;
float
test (int start, int end)
{
  int i;
  for (i = start; i < end; ++i)
    {
      float tem = x[i];
      x[i] = parm * tem;
    }
}

/* { dg-final { scan-tree-dump "vectorized 1 loops" "vect" } } */
/* If we need a non-vectorized prologue to align the stores for a vectorized
   loop, the vectorizer may introduce a condition that eventually resolves to a
   constant, and the removal of the edge and recomputation of probabilities
   gets estimated execution counts wrong.  On ia32, the condition is resolved
   in fre5; on powerpc-elf, in dom3.  */
/* { dg-final { scan-tree-dump-not "Invalid sum" "optimized" { xfail { ia32 || { powerpc*-*-* && { vect_no_store_align && { ! vect_hw_misalign } } } } } } } */ /* PR 119293 */
