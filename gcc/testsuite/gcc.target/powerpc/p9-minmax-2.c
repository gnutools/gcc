/* { dg-do compile } */
/* { dg-options "-mdejagnu-cpu=power9 -mvsx -O2" } */
/* { dg-require-effective-target powerpc_vsx } */
/* { dg-final { scan-assembler "xsmaxcdp"  } } */
/* { dg-final { scan-assembler "xsmincdp"  } } */

double
dbl_max (double a, double b)
{
  return (a > b) ? a : b;
}

double
dbl_min (double a, double b)
{
  return (a < b) ? a : b;
}

float
flt_max (float a, float b)
{
  return (a > b) ? a : b;
}

float
flt_min (float a, float b)
{
  return (a < b) ? a : b;
}
