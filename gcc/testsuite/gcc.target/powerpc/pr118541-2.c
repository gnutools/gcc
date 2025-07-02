/* { dg-do compile } */
/* { dg-options "-mdejagnu-cpu=power9 -Ofast" } */
/* { dg-require-effective-target powerpc_vsx } */

/* PR target/118541 says that the ordered comparison functions like isgreater
   should not optimize floating point conditional moves to use
   x{s,v}cmp{eq,gt,ge}{dp,qp} and xxsel since that instruction can cause traps
   if one of the arguments is a signaling NaN.  */

/* Verify isgreater does generate xscmpgtdp when NaNs are not allowed.  */

double
ordered_compare (double a, double b, double c, double d)
{
  /*
   * xscmpgtdp 1,1,2
   * xxsel     1,4,3,1
   * blr
   */

  return __builtin_isgreater (a, b) ? c : d;
}

/* { dg-final { scan-assembler     {\mxscmpg[te]dp\M}       } } */
/* { dg-final { scan-assembler     {\mxxsel\M}              } } */
/* { dg-final { scan-assembler-not {\mxscmpudp\M|\mfcmpu\M} } } */
