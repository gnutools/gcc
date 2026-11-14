/* { dg-do run } */
/* { dg-require-effective-target lto } */
/* { dg-options "-O2 -flto -fno-partial-inlining -fno-ipa-sra -fno-early-inlining -fdump-ipa-inline-details" } */

#define COLD_WORK() \
  do { cold_heavy (); cold_heavy (); cold_heavy (); cold_heavy (); } while (0)

struct Wrap
{
  int pad;
  char buf[8];
};

char gbuf[16] = "hello";

__attribute__((noipa)) void
clobber (void)
{
  gbuf[3] = '%';
}

__attribute__((noipa)) void
cold_heavy (void)
{
  volatile int x = 1;
  (void) x;
}

static int
leaf_clean (const char *p, int i)
{
  if (p[i] == '%')
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

static int
leaf_idx (const char *p, int i)
{
  clobber ();
  if (p[i] == '%')
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

static int
leaf_ne (const char *p, int i)
{
  if (p[i] != '\0')
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

static inline int
leaf_anc (const char *p, int i)
{
  if (p[i] == '%')
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

static int
mid_anc (struct Wrap *w, int i)
{
  clobber ();
  return leaf_anc (w->buf, i);
}

static int
mid_pass (const char *p, int i)
{
  clobber ();
  return leaf_anc (p, i);
}

static const struct Wrap w_ro = { 1, "test" };

int
main (void)
{
  volatile int i = 0;
  volatile int i3 = 3;
  struct Wrap w_rw = { 1, "test" };

  if (leaf_clean ("hello world", i) != 0)
    __builtin_abort ();
  if (leaf_clean ("foo%bar", i3) != 1)
    __builtin_abort ();
  if (leaf_idx ("abcdef", i) != 0)
    __builtin_abort ();
  if (leaf_idx ("ab\0%cd", i3) != 1)
    __builtin_abort ();
  gbuf[3] = 'x';
  if (leaf_idx (gbuf, i3) != 1)
    __builtin_abort ();
  if (leaf_ne ("", i) != 0)
    __builtin_abort ();
  if (leaf_ne ("x", i) != 1)
    __builtin_abort ();
  if (mid_anc ((struct Wrap *) &w_ro, i) != 0)
    __builtin_abort ();
  if (mid_anc (&w_rw, i) != 0)
    __builtin_abort ();
  if (mid_pass ("abcdef", i) != 0)
    __builtin_abort ();
  if (mid_pass ("ab%def", 2) != 1)
    __builtin_abort ();
  return 0;
}

/* { dg-final { scan-wpa-ipa-dump "Known to be false:.*op0\\\[ref unknown_offset\\\] == 37" "inline" } } */
/* { dg-final { scan-wpa-ipa-dump "Known to be false:.*op0\\\[ref maybe_modified unknown_offset\\\] == 37" "inline" } } */
/* { dg-final { scan-wpa-ipa-dump "Known to be false:.*op0\\\[ref unknown_offset\\\] != 0" "inline" } } */
