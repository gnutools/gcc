/* { dg-do run } */
/* { dg-options "-Os -fno-early-inlining -fno-ipa-cp -fno-partial-inlining -fno-ipa-sra -fno-ipa-icf -fdump-ipa-inline-details" } */

#define COLD_WORK()							\
  do									\
    {									\
      cold_heavy ();							\
      cold_heavy ();							\
      cold_heavy ();							\
      cold_heavy ();							\
      cold_heavy ();							\
      cold_heavy ();							\
      cold_heavy ();							\
      cold_heavy ();							\
    }									\
  while (0)

__attribute__((noipa)) void
cold_heavy (void)
{
  volatile int x = 1;
  (void) x;
}

int
chk_eq_pct (const char *p, int i)
{
  if (p[i] == '%')
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
chk_eq_a (const char *p, int i)
{
  if (p[i] == 'a')
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
chk_ne_nul (const char *p, int i)
{
  if (p[i] != '\0')
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
chk_s_lt0 (const signed char *p, int i)
{
  if (p[i] < 0)
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
chk_s_gt0 (const signed char *p, int i)
{
  if (p[i] > 0)
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
chk_s_gt_f (const signed char *p, int i)
{
  if (p[i] > 'f')
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
chk_s_le_m1 (const signed char *p, int i)
{
  if (p[i] <= -1)
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
chk_u_gt_f (const unsigned char *p, int i)
{
  if (p[i] > 'f')
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
chk_u_gt120 (const unsigned char *p, int i)
{
  if (p[i] > 120)
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
chk_u_ge130 (const unsigned char *p, int i)
{
  if (p[i] >= 130)
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
chk_u_ge200 (const unsigned char *p, int i)
{
  if (p[i] >= 200)
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
chk_u_ge128_cast (const unsigned char *p, int i)
{
  if (p[i] >= 128)
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
chk_else_ge0 (const signed char *p, int i)
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
chk_else_s_le0 (const signed char *p, int i)
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
chk_else_u_le_f (const unsigned char *p, int i)
{
  if (p[i] <= 'f')
    return 0;
  else
    {
      COLD_WORK ();
      return 1;
    }
}

int
chk_arr_ptr (const char (*p)[7], int i)
{
  if ((*p)[i] == '%')
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
chk_switch (const char *p, int i)
{
  switch (p[i])
    {
    case '%':
    case '(':
    case '*':
    case '/':
      COLD_WORK ();
      return 1;
    default:
      return 0;
    }
}

int
chk_signed_to_uint (const signed char *p, int i)
{
  if ((unsigned int) p[i] > 127u)
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
chk_phi_loop (const char *c, int a, int b, int d)
{
  (void) a;
  (void) b;
  (void) d;
  while (*c)
    {
      if (*c == '~')
	{
	  COLD_WORK ();
	  return 1;
	}
      c++;
    }
  return 0;
}

int
chk_phi_diamond (const char *c, int flag, int a, int b)
{
  (void) a;
  (void) b;
  if (flag)
    c += 2;
  else
    c += 1;
  if (*c == '~')
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
chk_phi_two_parms (const char *p, const char *q, int flag, int a)
{
  (void) a;
  const char *c = flag ? p : q;
  if (*c == '~')
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

int
chk_phi_lit (const char *p, int flag, int a, int b)
{
  (void) a;
  (void) b;
  const char *c = flag ? p : "~bcdef";
  if (*c == '~')
    {
      COLD_WORK ();
      return 1;
    }
  return 0;
}

static int
chk_phi_mod_helper (char *c)
{
  int found = 0;
  while (*c)
    {
      if (*c == '$')
	found = 1;
      if (*c == 'a')
	c[1] = '$';
      c++;
    }
  return found;
}

__attribute__((noipa)) int
chk_phi_mod_caller (char *buf)
{
  return chk_phi_mod_helper (buf);
}

int
main (void)
{
  volatile int i = 0;
  volatile int im1 = -1;
  volatile int i2 = 2;
  volatile int i3 = 3;

  /* Valid foldable EQ_EXPR cases (3 calls inlined).  */
  if (chk_eq_pct ("abcdef", i) != 0)
    __builtin_abort ();
  if (chk_eq_pct ("xyabcdef" + 2, i) != 0)
    __builtin_abort ();
  if (chk_eq_pct ((const char *) u"abcdef", i) != 0)
    __builtin_abort ();

  /* Runtime safety checks for EQ_EXPR (must not fold away '%').  */
  if (chk_eq_pct ("x%abcdef" + 2, im1) != 1)
    __builtin_abort ();
  if (chk_eq_pct ("ab\0%def", i3) != 1)
    __builtin_abort ();

  /* EQ_EXPR missing vs present character.  */
  if (chk_eq_a ("bcdef", i) != 0)
    __builtin_abort ();
  if (chk_eq_a ("abcdef", i) != 1)
    __builtin_abort ();

  /* NE_EXPR on empty and all-NUL string literals.  */
  if (chk_ne_nul ("", i) != 0)
    __builtin_abort ();
  if (chk_ne_nul ("\0\0\0", i) != 0)
    __builtin_abort ();
  if (chk_ne_nul ("x", i) != 1)
    __builtin_abort ();

  /* Signed LT_EXPR, GT_EXPR, LE_EXPR.  */
  if (chk_s_lt0 ((const signed char *) "abcdef", i) != 0)
    __builtin_abort ();
  if (chk_s_lt0 ((const signed char *) "\xff\x01\x02", i) != 1)
    __builtin_abort ();

  if (chk_s_gt0 ((const signed char *) "\x80\x90", i) != 0)
    __builtin_abort ();
  if (chk_s_gt0 ((const signed char *) "\x01\x02", i) != 1)
    __builtin_abort ();

  if (chk_s_gt_f ((const signed char *) "abcdef", i) != 0)
    __builtin_abort ();

  if (chk_s_le_m1 ((const signed char *) "abcdef", i) != 0)
    __builtin_abort ();
  if (chk_s_le_m1 ((const signed char *) "\x80\x90", i) != 1)
    __builtin_abort ();

  /* Unsigned GT_EXPR and GE_EXPR (including wide UTF-16 string bytes).  */
  if (chk_u_gt_f ((const unsigned char *) "abcdef", i) != 0)
    __builtin_abort ();
  if (chk_u_gt120 ((const unsigned char *) "abcdef", i) != 0)
    __builtin_abort ();
  if (chk_u_ge130 ((const unsigned char *) "abcdef", i) != 0)
    __builtin_abort ();
  if (chk_u_ge130 ((const unsigned char *) u"abcdef", i) != 0)
    __builtin_abort ();
  if (chk_u_ge200 ((const unsigned char *) "abcdef", i) != 0)
    __builtin_abort ();
  if (chk_u_ge200 ((const unsigned char *) u"abcdef", i) != 0)
    __builtin_abort ();

  /* >= 128 is canonicalized to ((signed char) #) < 0 in c->param_ops and
     must not fold yet.  */
  if (chk_u_ge128_cast ((const unsigned char *) "abcdef", i) != 0)
    __builtin_abort ();

  /* Else-branch folding (GE_EXPR, LE_EXPR true -> inverted comparison false).  */
  if (chk_else_ge0 ((const signed char *) "abcdef", i) != 0)
    __builtin_abort ();
  if (chk_else_s_le0 ((const signed char *) "\x80\x90", i) != 0)
    __builtin_abort ();
  if (chk_else_u_le_f ((const unsigned char *) "abcdef", i) != 0)
    __builtin_abort ();

  /* Variable-index ARRAY_REF on pointer to array and switch predicate.  */
  if (chk_arr_ptr (&"abcdef", i) != 0)
    __builtin_abort ();
  if (chk_switch ("abcdef", i) != 0)
    __builtin_abort ();
  if (chk_switch ("ab%def", i2) != 1)
    __builtin_abort ();

  /* PHI handling: loop induction variable, diamond, two params, literal,
     and loop modification soundness.  */
  if (chk_phi_loop ("abcdef", 1, 2, 3) != 0)
    __builtin_abort ();
  if (chk_phi_loop ("ab~def", 1, 2, 3) != 1)
    __builtin_abort ();
  if (chk_phi_diamond ("abcdef", i, 1, 2) != 0)
    __builtin_abort ();
  if (chk_phi_diamond ("a~cdef", i, 1, 2) != 1)
    __builtin_abort ();
  if (chk_phi_two_parms ("abcdef", "~bcdef", 0, 0) != 1)
    __builtin_abort ();
  if (chk_phi_lit ("abcdef", 0, 1, 2) != 1)
    __builtin_abort ();
  char modbuf[] = "abcdef";
  if (chk_phi_mod_caller (modbuf) != 1)
    __builtin_abort ();

  return 0;
}

/* { dg-final { scan-ipa-dump "Known to be false:.*unknown_offset\\\] == 37" "inline" } } */
/* { dg-final { scan-ipa-dump "Known to be false:.*unknown_offset\\\] == 97" "inline" } } */
/* { dg-final { scan-ipa-dump "Known to be false:.*unknown_offset\\\] != 0" "inline" } } */
/* { dg-final { scan-ipa-dump "Known to be false:.*unknown_offset\\\] (?:< 0|<= -1)" "inline" } } */
/* { dg-final { scan-ipa-dump "Known to be false:.*unknown_offset\\\] (?:> 0|>= 1)" "inline" } } */
/* { dg-final { scan-ipa-dump "Known to be false:.*unknown_offset\\\] (?:> 102|>= 103)" "inline" } } */
/* { dg-final { scan-ipa-dump "Known to be false:.*unknown_offset\\\] (?:> 199|>= 200)" "inline" } } */
/* { dg-final { scan-ipa-dump-times "Inlined chk_eq_pct/" 3 "inline" } } */
/* { dg-final { scan-ipa-dump-times "Inlined chk_eq_a/" 1 "inline" } } */
/* { dg-final { scan-ipa-dump-times "Inlined chk_ne_nul/" 2 "inline" } } */
/* { dg-final { scan-ipa-dump-times "Inlined chk_s_lt0/" 1 "inline" } } */
/* { dg-final { scan-ipa-dump-times "Inlined chk_s_gt0/" 1 "inline" } } */
/* { dg-final { scan-ipa-dump-times "Inlined chk_s_gt_f/" 1 "inline" } } */
/* { dg-final { scan-ipa-dump-times "Inlined chk_s_le_m1/" 1 "inline" } } */
/* { dg-final { scan-ipa-dump-times "Inlined chk_u_gt_f/" 1 "inline" } } */
/* { dg-final { scan-ipa-dump-times "Inlined chk_u_gt120/" 1 "inline" } } */
/* { dg-final { scan-ipa-dump-times "Inlined chk_u_ge130/" 2 "inline" } } */
/* { dg-final { scan-ipa-dump-times "Inlined chk_u_ge200/" 2 "inline" } } */
/* { dg-final { scan-ipa-dump-times "Inlined chk_else_ge0/" 1 "inline" } } */
/* { dg-final { scan-ipa-dump-times "Inlined chk_else_s_le0/" 1 "inline" } } */
/* { dg-final { scan-ipa-dump-times "Inlined chk_else_u_le_f/" 1 "inline" } } */
/* { dg-final { scan-ipa-dump-times "Inlined chk_arr_ptr/" 1 "inline" } } */
/* { dg-final { scan-ipa-dump "Known to be false:.*unknown_offset\\\] == 40" "inline" } } */
/* { dg-final { scan-ipa-dump-not "Inlined chk_u_ge128_cast/" "inline" } } */
/* { dg-final { scan-ipa-dump-times "Inlined chk_phi_loop/" 1 "inline" } } */
/* { dg-final { scan-ipa-dump-times "Inlined chk_phi_diamond/" 1 "inline" } } */
/* { dg-final { scan-ipa-dump-not "Inlined chk_phi_two_parms/" "inline" } } */
/* { dg-final { scan-ipa-dump-not "Inlined chk_phi_lit/" "inline" } } */
