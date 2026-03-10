/* PR target/17381 - Unnecessary register move for float extend */
/* { dg-do compile } */
/* { dg-options "-mdejagnu-cpu=power7 -O2" } */
/* Up to power6, we compress the floating-point constant 1.0 and share it with
   1.0f, but the float_extend comes out as a second fmr.  */

double d;
float test1(float fParm)
{
  d = fParm + 1.0;
  return fParm + 1.0f;
}
/* { dg-final { scan-assembler-times "fmr" 1 } } */
