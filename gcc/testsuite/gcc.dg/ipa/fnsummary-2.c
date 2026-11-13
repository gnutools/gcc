/* { dg-do compile } */
/* { dg-options "-O2 -fno-partial-inlining -fno-ipa-sra -fdump-ipa-fnsummary-details -fdump-ipa-inline-details" } */

struct S
{
  int a;
  int b;
};

static const struct S cs = { 10, 42 };

void ext (void);
void cold_work (void);

static int __attribute__((noinline))
test_agg (const struct S *p)
{
  ext ();
  if (p->b != 42)
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
caller (void)
{
  return test_agg (&cs);
}

/* { dg-final { scan-ipa-dump "op0\\\[ref maybe_modified offset: 32\\\] != 42" "fnsummary" } } */
/* { dg-final { scan-ipa-dump "Known to be false:.*op0\\\[ref maybe_modified offset: 32\\\] != 42" "inline" } } */
