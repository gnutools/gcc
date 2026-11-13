/* { dg-do run } */
/* { dg-require-effective-target lto } */
/* { dg-options "-O2 -flto -fno-partial-inlining -fno-ipa-sra -fdump-ipa-inline-details" } */

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

/* { dg-final { scan-wpa-ipa-dump "op0\\\[ref maybe_modified offset: 32\\\] == 42" "inline" } } */
/* { dg-final { scan-wpa-ipa-dump-not {Known to be false:[^\n]*maybe_modified} "inline" } } */
