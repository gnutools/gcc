/* { dg-do compile } */
/* { dg-require-effective-target vect_int } */
/* { dg-additional-options "-fdump-tree-slp-details" { target { powerpc*-*-* && { ! powerpc_vsx } } } } */

/* PR82255: Ensure we don't require a vec_construct cost when we aren't
   going to generate a strided load.  */

extern int abs (int __x) __attribute__ ((__nothrow__, __leaf__))
__attribute__ ((__const__));

static int
foo (unsigned char *w, int i, unsigned char *x, int j)
{
  int tot = 0;
  for (int a = 0; a < 16; a++)
    {
#pragma GCC unroll 16
      for (int b = 0; b < 16; b++)
	tot += abs (w[b] - x[b]);
      w += i;
      x += j;
    }
  return tot;
}

void
bar (unsigned char *w, unsigned char *x, int i, int *result)
{
  *result = foo (w, 16, x, i);
}

/* { dg-final { scan-tree-dump-times "vec_construct" 0 "vect" { xfail { powerpc*-*-* && { ! powerpc_vsx } } } } } */
/* Since r15-5523..5559 (we ICEing in between), we've failed the above when
   compiling for altivec (selected by check_vect_support_and_set_flags when the
   cpu under test doesn't supprot vsx), which makes for a long sequence of byte
   loads, but slp comes to the rescue and rearranges the sequence into a single
   REALIGN_LOAD, so we get reasonable code in the end.  Check that we still do:
   vect gets a single REALIGN_LOAD stmt for W, but we also match its add new
   stmt note and its value numbering dump.  slp1 adds a second REALIGN_LOAD,
   this one for X, but we also match its add new stmt note.  */
/* { dg-final { scan-tree-dump-times "REALIGN_LOAD" 3 "vect" { target { powerpc*-*-* && { ! powerpc_vsx } } } } } */
/* { dg-final { scan-tree-dump-times "REALIGN_LOAD" 3 "slp1" { target { powerpc*-*-* && { ! powerpc_vsx } } } } } */
