/* { dg-do run } */
/* { dg-options "-O2 -fno-partial-inlining -fno-ipa-sra -fdump-ipa-cp-details -fdump-ipa-inline-details" } */

struct S
{
  int a;
  int b;
};

struct S ms;

__attribute__((noipa)) void
clobber (void)
{
  ms.b = 99;
}

__attribute__((noipa)) void
cold_heavy (void)
{
  volatile int x = 1;
  (void) x;
}

static int __attribute__((noinline))
callee (struct S *p)
{
  clobber ();
  if (p->b != 42)
    return 1;
  cold_heavy ();
  cold_heavy ();
  cold_heavy ();
  cold_heavy ();
  return 0;
}

static inline int
leaf_check (struct S *p)
{
  if (p->b != 42)
    return 1;
  cold_heavy ();
  cold_heavy ();
  cold_heavy ();
  cold_heavy ();
  return 0;
}

static int __attribute__((noinline))
mid (struct S *p)
{
  clobber ();
  return leaf_check (p);
}

int
main (void)
{
  ms.a = 10;
  ms.b = 42;
  if (callee (&ms) != 1)
    __builtin_abort ();
  ms.b = 42;
  if (mid (&ms) != 1)
    __builtin_abort ();
  return 0;
}

/* { dg-final { scan-ipa-dump-not {Known to be false:[^\n]*maybe_modified} "cp" } } */
/* { dg-final { scan-ipa-dump-not {Known to be false:[^\n]*maybe_modified} "inline" } } */
