/* { dg-do compile } */
/* { dg-options "-Os -fdump-tree-einline-details -fdump-tree-optimized" } */

void long_call (void);

int
test (const char *p, int i)
{
  if (p[i] == '%')
    {
      long_call ();
      long_call ();
      long_call ();
      long_call ();
      long_call ();
      long_call ();
      long_call ();
      long_call ();
      long_call ();
      long_call ();
      long_call ();
      return 1;
    }
  return 0;
}

int
main (void)
{
  volatile int i = 0;
  return test ("test", i) + test ("xtest" + 1, i);
}

/* { dg-final { scan-tree-dump "Known to be false:.*op0\\\[ref unknown_offset\\\] == 37" "einline" } } */
/* { dg-final { scan-tree-dump-times "Inlining test/" 2 "einline" } } */
/* { dg-final { scan-tree-dump-not "  test \\(" "optimized" } } */

