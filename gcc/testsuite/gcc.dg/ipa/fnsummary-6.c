/* { dg-do compile } */
/* { dg-options "-Os -fdump-tree-einline-details -fdump-tree-optimized" } */

void long_call (void);

void
test (const char *p)
{
  if (p[0] == '%')
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
    }
  long_call ();
  if (p[0] == '%')
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
    }
}

int
main (void)
{
  test ("test");
  test ("xtest" + 1);
  return 0;
}

/* { dg-final { scan-tree-dump "Known to be false:.*op0\\\[ref maybe_modified offset: 0\\\] == 37" "einline" } } */
/* { dg-final { scan-tree-dump-times "Inlining test/" 2 "einline" } } */
/* { dg-final { scan-tree-dump-not "  test \\(" "optimized" } } */
