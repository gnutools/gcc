/* { dg-do run } */
/* { dg-options "-Os -fdump-tree-einline-details -fdump-tree-optimized" } */

#define STR10 "abcdefghij"
#define STR40 STR10 STR10 STR10 STR10
#define STR160 STR40 STR40 STR40 STR40
#define STR320 STR160 STR160

#define A10 "aaaaaaaaaa"
#define A50 A10 A10 A10 A10 A10
#define A300 A50 A50 A50 A50 A50 A50

#define Z10 "\0\0\0\0\0\0\0\0\0\0"
#define Z50 Z10 Z10 Z10 Z10 Z10
#define Z300 Z50 Z50 Z50 Z50 Z50 Z50

#define NEG10 "\x80\x81\x82\x83\x84\x85\x86\x87\x88\x89"
#define NEG50 NEG10 NEG10 NEG10 NEG10 NEG10
#define NEG300 NEG50 NEG50 NEG50 NEG50 NEG50 NEG50

__attribute__((noipa)) void
long_call (void)
{
  volatile int x = 1;
  (void) x;
}

#define COLD_WORK()							\
  do									\
    {									\
      long_call ();							\
      long_call ();							\
      long_call ();							\
      long_call ();							\
      long_call ();							\
      long_call ();							\
      long_call ();							\
      long_call ();							\
      long_call ();							\
      long_call ();							\
      long_call ();							\
    }									\
  while (0)

int
test_eq (const char *p, int i)
{
  if (p[i] == '%')
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
test_eq_neg (const signed char *p, int i)
{
  if (p[i] == -50)
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
test_ne_nul (const char *p, int i)
{
  if (p[i] != '\0')
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
test_lt0 (const signed char *p, int i)
{
  if (p[i] < 0)
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
test_gt0 (const signed char *p, int i)
{
  if (p[i] > 0)
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
test_gt_j (const signed char *p, int i)
{
  if (p[i] > 'j')
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
test_le_m1 (const signed char *p, int i)
{
  if (p[i] <= -1)
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
test_ge130 (const unsigned char *p, int i)
{
  if (p[i] >= 130)
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
test_ge200 (const unsigned char *p, int i)
{
  if (p[i] >= 200)
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
test_else_ge0 (const signed char *p, int i)
{
  if (p[i] >= 0)
    return 0;
  else
    {
      COLD_WORK ();
      return 1;
    }
}

int
test_else_le0 (const signed char *p, int i)
{
  if (p[i] <= 0)
    return 0;
  else
    {
      COLD_WORK ();
      return 1;
    }
}

int
test_else_le_j (const signed char *p, int i)
{
  if (p[i] <= 'j')
    return 0;
  else
    {
      COLD_WORK ();
      return 1;
    }
}

int
test_else_lt130 (const unsigned char *p, int i)
{
  if (p[i] < 130)
    return 0;
  else
    {
      COLD_WORK ();
      return 1;
    }
}

int
test_loop_z (const char *c)
{
  while (*c)
    {
      COLD_WORK ();
      c++;
    }
  return 0;
}

int
test_loop_pct (const char *c, int a, int b, int d)
{
  (void) a;
  (void) b;
  (void) d;
  while (*c)
    {
      if (*c == '%')
	{
	  COLD_WORK ();
	  return 1;
	}
      c++;
    }
  return 0;
}

int
main (void)
{
  volatile int i = 5;
  volatile int i301 = 301;

  if (test_eq (STR320, i) != 0)
    __builtin_abort ();
  if (test_eq_neg ((const signed char *) NEG300, i) != 0)
    __builtin_abort ();
  if (test_ne_nul (Z300, i) != 0)
    __builtin_abort ();
  if (test_lt0 ((const signed char *) STR320, i) != 0)
    __builtin_abort ();
  if (test_gt0 ((const signed char *) NEG300, i) != 0)
    __builtin_abort ();
  if (test_gt_j ((const signed char *) STR320, i) != 0)
    __builtin_abort ();
  if (test_le_m1 ((const signed char *) STR320, i) != 0)
    __builtin_abort ();
  if (test_ge130 ((const unsigned char *) STR320, i) != 0)
    __builtin_abort ();
  if (test_ge200 ((const unsigned char *) STR320, i) != 0)
    __builtin_abort ();
  if (test_else_ge0 ((const signed char *) STR320, i) != 0)
    __builtin_abort ();
  if (test_else_le0 ((const signed char *) NEG300, i) != 0)
    __builtin_abort ();
  if (test_else_le_j ((const signed char *) STR320, i) != 0)
    __builtin_abort ();
  if (test_else_lt130 ((const unsigned char *) STR320, i) != 0)
    __builtin_abort ();

  /* Long string (> 256 chars) with embedded NUL followed by '%'.  */
  if (test_eq (A300 "\0%bcdef", i301) != 1)
    __builtin_abort ();

  /* Pointer loop (PHI induction variable) over > 256-char strings.  */
  if (test_loop_z (Z300) != 0)
    __builtin_abort ();
  if (test_loop_pct (STR320, 1, 2, 3) != 0)
    __builtin_abort ();
  if (test_loop_pct ("ab%def", 1, 2, 3) != 1)
    __builtin_abort ();

  return 0;
}

/* { dg-final { scan-tree-dump "Known to be false:.*op0\\\[ref unknown_offset\\\] == 37" "einline" } } */
/* { dg-final { scan-tree-dump "Known to be false:.*op0\\\[ref unknown_offset\\\] == -50" "einline" } } */
/* { dg-final { scan-tree-dump "Known to be false:.*op0\\\[ref unknown_offset\\\] != 0" "einline" } } */
/* { dg-final { scan-tree-dump "Known to be false:.*op0\\\[ref unknown_offset\\\] (?:< 0|<= -1)" "einline" } } */
/* { dg-final { scan-tree-dump "Known to be false:.*op0\\\[ref unknown_offset\\\] (?:> 0|>= 1)" "einline" } } */
/* { dg-final { scan-tree-dump "Known to be false:.*op0\\\[ref unknown_offset\\\] (?:> 106|>= 107)" "einline" } } */
/* { dg-final { scan-tree-dump "Known to be false:.*op0\\\[ref unknown_offset\\\] (?:> 199|>= 200)" "einline" } } */
/* { dg-final { scan-tree-dump-times "optimized:  Inlining test_eq/" 1 "einline" } } */
/* { dg-final { scan-tree-dump "optimized:  Inlining test_eq_neg/" "einline" } } */
/* { dg-final { scan-tree-dump "optimized:  Inlining test_ne_nul/" "einline" } } */
/* { dg-final { scan-tree-dump "optimized:  Inlining test_lt0/" "einline" } } */
/* { dg-final { scan-tree-dump "optimized:  Inlining test_gt0/" "einline" } } */
/* { dg-final { scan-tree-dump "optimized:  Inlining test_gt_j/" "einline" } } */
/* { dg-final { scan-tree-dump "optimized:  Inlining test_le_m1/" "einline" } } */
/* { dg-final { scan-tree-dump "optimized:  Inlining test_ge130/" "einline" } } */
/* { dg-final { scan-tree-dump "optimized:  Inlining test_ge200/" "einline" } } */
/* { dg-final { scan-tree-dump "optimized:  Inlining test_else_ge0/" "einline" } } */
/* { dg-final { scan-tree-dump "optimized:  Inlining test_else_le0/" "einline" } } */
/* { dg-final { scan-tree-dump "optimized:  Inlining test_else_le_j/" "einline" } } */
/* { dg-final { scan-tree-dump "optimized:  Inlining test_else_lt130/" "einline" } } */
/* { dg-final { scan-tree-dump "optimized:  Inlining test_loop_z/" "einline" } } */
/* { dg-final { scan-tree-dump-times "optimized:  Inlining test_loop_pct/" 1 "einline" } } */
/* { dg-final { scan-tree-dump-not "  test_eq_neg \\(" "optimized" } } */
/* { dg-final { scan-tree-dump-not "  test_ne_nul \\(" "optimized" } } */
/* { dg-final { scan-tree-dump-not "  test_lt0 \\(" "optimized" } } */
/* { dg-final { scan-tree-dump-not "  test_gt0 \\(" "optimized" } } */
/* { dg-final { scan-tree-dump-not "  test_gt_j \\(" "optimized" } } */
/* { dg-final { scan-tree-dump-not "  test_le_m1 \\(" "optimized" } } */
/* { dg-final { scan-tree-dump-not "  test_ge130 \\(" "optimized" } } */
/* { dg-final { scan-tree-dump-not "  test_ge200 \\(" "optimized" } } */
/* { dg-final { scan-tree-dump-not "  test_else_ge0 \\(" "optimized" } } */
/* { dg-final { scan-tree-dump-not "  test_else_le0 \\(" "optimized" } } */
/* { dg-final { scan-tree-dump-not "  test_else_le_j \\(" "optimized" } } */
/* { dg-final { scan-tree-dump-not "  test_else_lt130 \\(" "optimized" } } */
/* { dg-final { scan-tree-dump-not "  test_loop_z \\(" "optimized" } } */
