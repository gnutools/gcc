/* { dg-do compile } */
/* { dg-require-effective-target vect_int } */
/* { dg-require-effective-target powerpc_altivec_ok } */
/* { dg-options "-O2 -mno-vsx -maltivec" } */

/* Verify it emits error message on inlining even without LTO.  */

vector int c, a, b;

static inline void __attribute__ ((__always_inline__))
foo () /* { dg-error "inlining failed in call to .* target specific option mismatch" } */
{
  c = a + b;
}

__attribute__ ((target ("vsx")))
int main ()
{
  foo (); /* { dg-message "called from here" } */
  c = a + b;
}
