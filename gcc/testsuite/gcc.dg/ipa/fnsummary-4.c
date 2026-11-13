/* { dg-do compile } */
/* { dg-options "-O2 -fno-partial-inlining -fno-ipa-cp -fno-ipa-sra -fno-early-inlining -fdump-ipa-inline-details" } */

struct S
{
  int a;
  int b;
};

static const struct S cz = { 0, 0 };
void ext (void);
void cold_work (void);

static int
leaf (const struct S *p)
{
  if (p->b != 0)
    {
      cold_work ();
      cold_work ();
      cold_work ();
      cold_work ();
      return 1;
    }
  return 0;
}

int
mid (const struct S *p)
{
  ext ();
  return leaf (p);
}

int
caller (void)
{
  return mid (&cz);
}

/* { dg-final { scan-ipa-dump "op0\\\[ref maybe_modified offset: 32\\\] != 0" "inline" } } */
/* { dg-final { scan-ipa-dump "Known to be false:.*op0\\\[ref offset: 32\\\] != 0" "inline" } } */
