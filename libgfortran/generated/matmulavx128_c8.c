/* Implementation of the MATMUL intrinsic
   Copyright (C) 2002-2025 Free Software Foundation, Inc.
   Contributed by Thomas Koenig <tkoenig@gcc.gnu.org>.

This file is part of the GNU Fortran runtime library (libgfortran).

Libgfortran is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public
License as published by the Free Software Foundation; either
version 3 of the License, or (at your option) any later version.

Libgfortran is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

Under Section 7 of GPL version 3, you are granted additional
permissions described in the GCC Runtime Library Exception, version
3.1, as published by the Free Software Foundation.

You should have received a copy of the GNU General Public License and
a copy of the GCC Runtime Library Exception along with this program;
see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
<http://www.gnu.org/licenses/>.  */

#include "libgfortran.h"
#include <string.h>
#include <assert.h>


/* These are the specific versions of matmul with -mprefer-avx128.  */

#if defined (HAVE_GFC_COMPLEX_8)

/* Prototype for the BLAS ?gemm subroutine, a pointer to which can be
   passed to us by the front-end, in which case we call it for large
   matrices.  */

typedef void (*blas_call)(const char *, const char *, const int *, const int *,
                          const int *, const GFC_COMPLEX_8 *, const GFC_COMPLEX_8 *,
                          const int *, const GFC_COMPLEX_8 *, const int *,
                          const GFC_COMPLEX_8 *, GFC_COMPLEX_8 *, const int *,
                          int, int);

#if defined(HAVE_AVX) && defined(HAVE_FMA3) && defined(HAVE_AVX128)
void
matmul_c8_avx128_fma3 (gfc_array_c8 * const restrict retarray, 
	gfc_array_c8 * const restrict a, gfc_array_c8 * const restrict b, int try_blas,
	int blas_limit, blas_call gemm) __attribute__((__target__("avx,fma")));
internal_proto(matmul_c8_avx128_fma3);
void
matmul_c8_avx128_fma3 (gfc_array_c8 * const restrict retarray,
	gfc_array_c8 * const restrict a, gfc_array_c8 * const restrict b, int try_blas,
	int blas_limit, blas_call gemm)
{
  const GFC_COMPLEX_8 * restrict abase;
  const GFC_COMPLEX_8 * restrict bbase;
  GFC_COMPLEX_8 * restrict dest;

  index_type rystride, axstride, aystride, bxstride, bystride;
  index_type x, y, n, count, xcount, ycount;
  index_type axstride_bytes, aystride_bytes, bxstride_bytes, bystride_bytes,
	     rxstride_bytes, rystride_bytes;

  assert (GFC_DESCRIPTOR_RANK (a) == 2
          || GFC_DESCRIPTOR_RANK (b) == 2);

/* C[xcount,ycount] = A[xcount, count] * B[count,ycount]

   Either A or B (but not both) can be rank 1:

   o One-dimensional argument A is implicitly treated as a row matrix
     dimensioned [1,count], so xcount=1.

   o One-dimensional argument B is implicitly treated as a column matrix
     dimensioned [count, 1], so ycount=1.
*/

  if (retarray->base_addr == NULL)
    {
      index_type stride0;
      if (GFC_DESCRIPTOR_BYTES_COUNTED_STRIDES (retarray))
	stride0 = sizeof (GFC_COMPLEX_8);
      else
	stride0 = 1;

      if (GFC_DESCRIPTOR_RANK (a) == 1)
        {
	  GFC_DESCRIPTOR_DIMENSION_SET(retarray, 0, 0,
				       GFC_DESCRIPTOR_EXTENT(b,1) - 1, stride0);
        }
      else if (GFC_DESCRIPTOR_RANK (b) == 1)
        {
	  GFC_DESCRIPTOR_DIMENSION_SET(retarray, 0, 0,
				       GFC_DESCRIPTOR_EXTENT(a,0) - 1, stride0);
        }
      else
        {
	  GFC_DESCRIPTOR_DIMENSION_SET(retarray, 0, 0,
				       GFC_DESCRIPTOR_EXTENT(a,0) - 1, stride0);

          GFC_DESCRIPTOR_DIMENSION_SET(retarray, 1, 0,
				       GFC_DESCRIPTOR_EXTENT(b,1) - 1,
				       GFC_DESCRIPTOR_EXTENT(retarray,0)
				       * stride0);
        }

      retarray->base_addr
	= xmallocarray (size0 ((array_t *) retarray), sizeof (GFC_COMPLEX_8));
      retarray->offset = 0;
    }
  else if (unlikely (compile_options.bounds_check))
    {
      index_type ret_extent, arg_extent;

      if (GFC_DESCRIPTOR_RANK (a) == 1)
	{
	  arg_extent = GFC_DESCRIPTOR_EXTENT(b,1);
	  ret_extent = GFC_DESCRIPTOR_EXTENT(retarray,0);
	  if (arg_extent != ret_extent)
	    runtime_error ("Array bound mismatch for dimension 1 of "
	    		   "array (%ld/%ld) ",
			   (long int) ret_extent, (long int) arg_extent);
	}
      else if (GFC_DESCRIPTOR_RANK (b) == 1)
	{
	  arg_extent = GFC_DESCRIPTOR_EXTENT(a,0);
	  ret_extent = GFC_DESCRIPTOR_EXTENT(retarray,0);
	  if (arg_extent != ret_extent)
	    runtime_error ("Array bound mismatch for dimension 1 of "
	    		   "array (%ld/%ld) ",
			   (long int) ret_extent, (long int) arg_extent);
	}
      else
	{
	  arg_extent = GFC_DESCRIPTOR_EXTENT(a,0);
	  ret_extent = GFC_DESCRIPTOR_EXTENT(retarray,0);
	  if (arg_extent != ret_extent)
	    runtime_error ("Array bound mismatch for dimension 1 of "
	    		   "array (%ld/%ld) ",
			   (long int) ret_extent, (long int) arg_extent);

	  arg_extent = GFC_DESCRIPTOR_EXTENT(b,1);
	  ret_extent = GFC_DESCRIPTOR_EXTENT(retarray,1);
	  if (arg_extent != ret_extent)
	    runtime_error ("Array bound mismatch for dimension 2 of "
	    		   "array (%ld/%ld) ",
			   (long int) ret_extent, (long int) arg_extent);
	}
    }


  if (GFC_DESCRIPTOR_RANK (retarray) == 1)
    {
      /* One-dimensional result may be addressed in the code below
	 either as a row or a column matrix. We want both cases to
	 work. */
      rystride = GFC_DESCRIPTOR_STRIDE_UNITS(retarray,0);
      rxstride_bytes = rystride_bytes = GFC_DESCRIPTOR_STRIDE_BYTES(retarray,0);
    }
  else
    {
      rystride = GFC_DESCRIPTOR_STRIDE_UNITS(retarray,1);
      rxstride_bytes = GFC_DESCRIPTOR_STRIDE_BYTES(retarray,0);
      rystride_bytes = GFC_DESCRIPTOR_STRIDE_BYTES(retarray,1);
    }

  if (GFC_DESCRIPTOR_RANK (a) == 1)
    {
      /* Treat it as a a row matrix A[1,count]. */
      axstride = GFC_DESCRIPTOR_STRIDE_UNITS(a,0);
      aystride = 1;
      axstride_bytes = GFC_DESCRIPTOR_STRIDE_BYTES(a,0);
      aystride_bytes = sizeof (GFC_COMPLEX_8);

      xcount = 1;
      count = GFC_DESCRIPTOR_EXTENT(a,0);
    }
  else
    {
      axstride = GFC_DESCRIPTOR_STRIDE_UNITS(a,0);
      aystride = GFC_DESCRIPTOR_STRIDE_UNITS(a,1);
      axstride_bytes = GFC_DESCRIPTOR_STRIDE_BYTES(a,0);
      aystride_bytes = GFC_DESCRIPTOR_STRIDE_BYTES(a,1);

      count = GFC_DESCRIPTOR_EXTENT(a,1);
      xcount = GFC_DESCRIPTOR_EXTENT(a,0);
    }

  if (count != GFC_DESCRIPTOR_EXTENT(b,0))
    {
      if (count > 0 || GFC_DESCRIPTOR_EXTENT(b,0) > 0)
	runtime_error ("Incorrect extent in argument B in MATMUL intrinsic "
		       "in dimension 1: is %ld, should be %ld",
		       (long int) GFC_DESCRIPTOR_EXTENT(b,0), (long int) count);
    }

  if (GFC_DESCRIPTOR_RANK (b) == 1)
    {
      /* Treat it as a column matrix B[count,1] */
      bxstride = GFC_DESCRIPTOR_STRIDE_UNITS(b,0);
      bxstride_bytes = GFC_DESCRIPTOR_STRIDE_BYTES(b,0);

      /* bystride should never be used for 1-dimensional b.
         The value is only used for calculation of the
         memory by the buffer.  */
      bystride = 256;
      bystride_bytes = 99999999;
      ycount = 1;
    }
  else
    {
      bxstride = GFC_DESCRIPTOR_STRIDE_UNITS(b,0);
      bystride = GFC_DESCRIPTOR_STRIDE_UNITS(b,1);
      bxstride_bytes = GFC_DESCRIPTOR_STRIDE_BYTES(b,0);
      bystride_bytes = GFC_DESCRIPTOR_STRIDE_BYTES(b,1);
      ycount = GFC_DESCRIPTOR_EXTENT(b,1);
    }

  abase = a->base_addr;
  bbase = b->base_addr;
  dest = retarray->base_addr;

  /* Now that everything is set up, we perform the multiplication
     itself.  */

#define POW3(x) (((float) (x)) * ((float) (x)) * ((float) (x)))
#define min(a,b) ((a) <= (b) ? (a) : (b))
#define max(a,b) ((a) >= (b) ? (a) : (b))

  if (try_blas
      && rxstride_bytes == sizeof (GFC_COMPLEX_8)
      && (axstride_bytes == sizeof (GFC_COMPLEX_8)
	  || aystride_bytes == sizeof (GFC_COMPLEX_8))
      && (bxstride_bytes == sizeof (GFC_COMPLEX_8)
	  || bystride_bytes == sizeof (GFC_COMPLEX_8))
      && (((float) xcount) * ((float) ycount) * ((float) count)
          > POW3(blas_limit)))
    {
      const int m = xcount, n = ycount, k = count, ldc = rystride;
      const GFC_COMPLEX_8 one = 1, zero = 0;
      const int lda = (axstride_bytes == sizeof (GFC_COMPLEX_8)) ? aystride : axstride,
		ldb = (bxstride_bytes == sizeof (GFC_COMPLEX_8)) ? bystride : bxstride;

      if (lda > 0 && ldb > 0 && ldc > 0 && m > 1 && n > 1 && k > 1)
	{
	  assert (gemm != NULL);
	  const char *transa, *transb;
	  if (try_blas & 2)
	    transa = "C";
	  else
	    transa = axstride_bytes == sizeof (GFC_COMPLEX_8) ? "N" : "T";

	  if (try_blas & 4)
	    transb = "C";
	  else
	    transb = bxstride_bytes == sizeof (GFC_COMPLEX_8) ? "N" : "T";

	  gemm (transa, transb , &m,
		&n, &k,	&one, abase, &lda, bbase, &ldb, &zero, dest,
		&ldc, 1, 1);
	  return;
	}
    }

  if (rxstride_bytes == sizeof (GFC_COMPLEX_8)
      && axstride_bytes == sizeof (GFC_COMPLEX_8)
      && bxstride_bytes == sizeof (GFC_COMPLEX_8)
      && GFC_DESCRIPTOR_RANK (b) != 1)
    {
      /* This block of code implements a tuned matmul, derived from
         Superscalar GEMM-based level 3 BLAS,  Beta version 0.1

               Bo Kagstrom and Per Ling
               Department of Computing Science
               Umea University
               S-901 87 Umea, Sweden

	 from netlib.org, translated to C, and modified for matmul.m4.  */

      GFC_COMPLEX_8 *c;
      const index_type m = xcount, n = ycount, k = count;

      /* System generated locals */
      index_type a_dim1, b_dim1,
		 i1, i2, i3, i4, i5, i6;

      /* Local variables */
      GFC_COMPLEX_8 f11, f12, f21, f22, f31, f32, f41, f42,
		 f13, f14, f23, f24, f33, f34, f43, f44;
      index_type i, j, l, ii, jj, ll;
      index_type isec, jsec, lsec, uisec, ujsec, ulsec;
      GFC_COMPLEX_8 *t1;

      c = retarray->base_addr;

      /* Parameter adjustments */
      a_dim1 = aystride;
      b_dim1 = bystride;

#define A_ARRAY_ELEM(i,j) \
    (ARRAY_ELEM_AT_OFFSET (abase, (i) * sizeof (GFC_COMPLEX_8) + (j) * aystride_bytes))

#define B_ARRAY_ELEM(i,j) \
    (ARRAY_ELEM_AT_OFFSET (bbase, (i) * sizeof (GFC_COMPLEX_8) + (j) * bystride_bytes))

#define C_ARRAY_ELEM(i,j) \
    (ARRAY_ELEM_AT_OFFSET (c, (i) * sizeof (GFC_COMPLEX_8) + (j) * rystride_bytes))

      /* Empty result first.  */
      for (j=0; j<n; j++)
	for (i=0; i<m; i++)
	  C_ARRAY_ELEM (i, j) = (GFC_COMPLEX_8)0;

      /* Early exit if possible */
      if (m == 0 || n == 0 || k == 0)
	return;

      /* Adjust size of t1 to what is needed.  */
      index_type t1_dim, a_sz;
      if (aystride_bytes == sizeof (GFC_COMPLEX_8))
        a_sz = rystride;
      else
        a_sz = a_dim1;

      t1_dim = a_sz * 256 + b_dim1;
      if (t1_dim > 65536)
	t1_dim = 65536;

      t1 = malloc (t1_dim * sizeof(GFC_COMPLEX_8));

      /* Start turning the crank. */
      i1 = n;
      for (jj = 0; jj < i1; jj += 512)
	{
	  /* Computing MIN */
	  i2 = 512;
	  i3 = n - jj;
	  jsec = min(i2,i3);
	  ujsec = jsec - jsec % 4;
	  i2 = k;
	  for (ll = 0; ll < i2; ll += 256)
	    {
	      /* Computing MIN */
	      i3 = 256;
	      i4 = k - ll;
	      lsec = min(i3,i4);
	      ulsec = lsec - lsec % 2;

	      i3 = m;
	      for (ii = 0; ii < i3; ii += 256)
		{
		  /* Computing MIN */
		  i4 = 256;
		  i5 = m - ii;
		  isec = min(i4,i5);
		  uisec = isec - isec % 2;
		  i4 = ll + ulsec;
		  for (l = ll; l < i4; l += 2)
		    {
		      i5 = ii + uisec;
		      for (i = ii; i < i5; i += 2)
			{
			  t1[l - ll + 1 + ((i - ii + 1) << 8) - 257] =
					A_ARRAY_ELEM (i, l);
			  t1[l - ll + 2 + ((i - ii + 1) << 8) - 257] =
					A_ARRAY_ELEM (i, l + 1);
			  t1[l - ll + 1 + ((i - ii + 2) << 8) - 257] =
					A_ARRAY_ELEM (i + 1, l);
			  t1[l - ll + 2 + ((i - ii + 2) << 8) - 257] =
					A_ARRAY_ELEM (i + 1, l + 1);
			}
		      if (uisec < isec)
			{
			  t1[l - ll + 1 + (isec << 8) - 257] =
				    A_ARRAY_ELEM (ii + isec - 1, l);
			  t1[l - ll + 2 + (isec << 8) - 257] =
				    A_ARRAY_ELEM (ii + isec - 1, l + 1);
			}
		    }
		  if (ulsec < lsec)
		    {
		      i4 = ii + isec;
		      for (i = ii; i< i4; ++i)
			{
			  t1[lsec + ((i - ii + 1) << 8) - 257] =
				    A_ARRAY_ELEM (i, ll + lsec - 1);
			}
		    }

		  uisec = isec - isec % 4;
		  i4 = jj + ujsec;
		  for (j = jj; j < i4; j += 4)
		    {
		      i5 = ii + uisec;
		      for (i = ii; i < i5; i += 4)
			{
			  f11 = C_ARRAY_ELEM (i, j);
			  f21 = C_ARRAY_ELEM (i + 1, j);
			  f12 = C_ARRAY_ELEM (i, j + 1);
			  f22 = C_ARRAY_ELEM (i + 1, j + 1);
			  f13 = C_ARRAY_ELEM (i, j + 2);
			  f23 = C_ARRAY_ELEM (i + 1, j + 2);
			  f14 = C_ARRAY_ELEM (i, j + 3);
			  f24 = C_ARRAY_ELEM (i + 1, j + 3);
			  f31 = C_ARRAY_ELEM (i + 2, j);
			  f41 = C_ARRAY_ELEM (i + 3, j);
			  f32 = C_ARRAY_ELEM (i + 2, j + 1);
			  f42 = C_ARRAY_ELEM (i + 3, j + 1);
			  f33 = C_ARRAY_ELEM (i + 2, j + 2);
			  f43 = C_ARRAY_ELEM (i + 3, j + 2);
			  f34 = C_ARRAY_ELEM (i + 2, j + 3);
			  f44 = C_ARRAY_ELEM (i + 3, j + 3);
			  i6 = ll + lsec;
			  for (l = ll; l < i6; ++l)
			    {
			      f11 += t1[l - ll + 1 + ((i - ii + 1) << 8) - 257]
				      * B_ARRAY_ELEM (l, j);
			      f21 += t1[l - ll + 1 + ((i - ii + 2) << 8) - 257]
				      * B_ARRAY_ELEM (l, j);
			      f12 += t1[l - ll + 1 + ((i - ii + 1) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 1);
			      f22 += t1[l - ll + 1 + ((i - ii + 2) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 1);
			      f13 += t1[l - ll + 1 + ((i - ii + 1) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 2);
			      f23 += t1[l - ll + 1 + ((i - ii + 2) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 2);
			      f14 += t1[l - ll + 1 + ((i - ii + 1) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 3);
			      f24 += t1[l - ll + 1 + ((i - ii + 2) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 3);
			      f31 += t1[l - ll + 1 + ((i - ii + 3) << 8) - 257]
				      * B_ARRAY_ELEM (l, j);
			      f41 += t1[l - ll + 1 + ((i - ii + 4) << 8) - 257]
				      * B_ARRAY_ELEM (l, j);
			      f32 += t1[l - ll + 1 + ((i - ii + 3) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 1);
			      f42 += t1[l - ll + 1 + ((i - ii + 4) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 1);
			      f33 += t1[l - ll + 1 + ((i - ii + 3) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 2);
			      f43 += t1[l - ll + 1 + ((i - ii + 4) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 2);
			      f34 += t1[l - ll + 1 + ((i - ii + 3) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 3);
			      f44 += t1[l - ll + 1 + ((i - ii + 4) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 3);
			    }
			  C_ARRAY_ELEM (i, j) = f11;
			  C_ARRAY_ELEM (i + 1, j) = f21;
			  C_ARRAY_ELEM (i, j + 1) = f12;
			  C_ARRAY_ELEM (i + 1, j + 1) = f22;
			  C_ARRAY_ELEM (i, j + 2) = f13;
			  C_ARRAY_ELEM (i + 1, j + 2) = f23;
			  C_ARRAY_ELEM (i, j + 3) = f14;
			  C_ARRAY_ELEM (i + 1, j + 3) = f24;
			  C_ARRAY_ELEM (i + 2, j) = f31;
			  C_ARRAY_ELEM (i + 3, j) = f41;
			  C_ARRAY_ELEM (i + 2, j + 1) = f32;
			  C_ARRAY_ELEM (i + 3, j + 1) = f42;
			  C_ARRAY_ELEM (i + 2, j + 2) = f33;
			  C_ARRAY_ELEM (i + 3, j + 2) = f43;
			  C_ARRAY_ELEM (i + 2, j + 3) = f34;
			  C_ARRAY_ELEM (i + 3, j + 3) = f44;
			}
		      if (uisec < isec)
			{
			  i5 = ii + isec;
			  for (i = ii + uisec; i < i5; ++i)
			    {
			      f11 = C_ARRAY_ELEM (i, j);
			      f12 = C_ARRAY_ELEM (i, j + 1);
			      f13 = C_ARRAY_ELEM (i, j + 2);
			      f14 = C_ARRAY_ELEM (i, j + 3);
			      i6 = ll + lsec;
			      for (l = ll; l < i6; ++l)
				{
				  f11 += t1[l - ll + 1 + ((i - ii + 1) << 8) -
					  257] * B_ARRAY_ELEM (l, j);
				  f12 += t1[l - ll + 1 + ((i - ii + 1) << 8) -
					  257] * B_ARRAY_ELEM (l, j + 1);
				  f13 += t1[l - ll + 1 + ((i - ii + 1) << 8) -
					  257] * B_ARRAY_ELEM (l, j + 2);
				  f14 += t1[l - ll + 1 + ((i - ii + 1) << 8) -
					  257] * B_ARRAY_ELEM (l, j + 3);
				}
			      C_ARRAY_ELEM (i, j) = f11;
			      C_ARRAY_ELEM (i, j + 1) = f12;
			      C_ARRAY_ELEM (i, j + 2) = f13;
			      C_ARRAY_ELEM (i, j + 3) = f14;
			    }
			}
		    }
		  if (ujsec < jsec)
		    {
		      i4 = jj + jsec;
		      for (j = jj + ujsec; j < i4; ++j)
			{
			  i5 = ii + uisec;
			  for (i = ii; i < i5; i += 4)
			    {
			      f11 = C_ARRAY_ELEM (i, j);
			      f21 = C_ARRAY_ELEM (i + 1, j);
			      f31 = C_ARRAY_ELEM (i + 2, j);
			      f41 = C_ARRAY_ELEM (i + 3, j);
			      i6 = ll + lsec;
			      for (l = ll; l < i6; ++l)
				{
				  f11 += t1[l - ll + 1 + ((i - ii + 1) << 8) -
					  257] * B_ARRAY_ELEM (l, j);
				  f21 += t1[l - ll + 1 + ((i - ii + 2) << 8) -
					  257] * B_ARRAY_ELEM (l, j);
				  f31 += t1[l - ll + 1 + ((i - ii + 3) << 8) -
					  257] * B_ARRAY_ELEM (l, j);
				  f41 += t1[l - ll + 1 + ((i - ii + 4) << 8) -
					  257] * B_ARRAY_ELEM (l, j);
				}
			      C_ARRAY_ELEM (i, j) = f11;
			      C_ARRAY_ELEM (i + 1, j) = f21;
			      C_ARRAY_ELEM (i + 2, j) = f31;
			      C_ARRAY_ELEM (i + 3, j) = f41;
			    }
			  i5 = ii + isec;
			  for (i = ii + uisec; i < i5; ++i)
			    {
			      f11 = C_ARRAY_ELEM (i, j);
			      i6 = ll + lsec;
			      for (l = ll; l < i6; ++l)
				{
				  f11 += t1[l - ll + 1 + ((i - ii + 1) << 8) -
					  257] * B_ARRAY_ELEM (l, j);
				}
			      C_ARRAY_ELEM (i, j) = f11;
			    }
			}
		    }
		}
	    }
	}
      free(t1);
      return;
#undef A_ARRAY_ELEM
#undef B_ARRAY_ELEM
#undef C_ARRAY_ELEM
    }
  else if (rxstride_bytes == sizeof (GFC_COMPLEX_8)
	   && aystride_bytes == sizeof (GFC_COMPLEX_8)
	   && bxstride_bytes == sizeof (GFC_COMPLEX_8))
    {
      if (GFC_DESCRIPTOR_RANK (a) != 1)
	{
	  const GFC_COMPLEX_8 *restrict abase_x;
	  const GFC_COMPLEX_8 *restrict bbase_y;
	  GFC_COMPLEX_8 *restrict dest_y;
	  GFC_COMPLEX_8 s;

	  for (y = 0; y < ycount; y++)
	    {
	      bbase_y = PTR_ADD_OFFSET (bbase, y * bystride_bytes);
	      dest_y = PTR_ADD_OFFSET (dest, y * rystride_bytes);
	      for (x = 0; x < xcount; x++)
		{
		  abase_x = PTR_ADD_OFFSET (abase, x * axstride_bytes);
		  s = (GFC_COMPLEX_8) 0;
		  for (n = 0; n < count; n++)
		    s += abase_x[n] * bbase_y[n];
		  dest_y[x] = s;
		}
	    }
	}
      else
	{
	  const GFC_COMPLEX_8 *restrict bbase_y;
	  GFC_COMPLEX_8 s;

	  for (y = 0; y < ycount; y++)
	    {
	      bbase_y = PTR_ADD_OFFSET (bbase, y * bystride_bytes);
	      s = (GFC_COMPLEX_8) 0;
	      for (n = 0; n < count; n++)
		s += GFC_DESCRIPTOR1_ELEM (a, n) * bbase_y[n];
	      ARRAY_ELEM_AT_OFFSET (dest, y * rystride_bytes) = s;
	    }
	}
    }
  else if (GFC_DESCRIPTOR_RANK (a) == 1)
    {
      const GFC_COMPLEX_8 *restrict bbase_y;
      GFC_COMPLEX_8 s;

      for (y = 0; y < ycount; y++)
	{
	  bbase_y = PTR_ADD_OFFSET (bbase, y * bystride_bytes);
	  s = (GFC_COMPLEX_8) 0;
	  for (n = 0; n < count; n++)
	    s += GFC_DESCRIPTOR1_ELEM (a, n)
	         * ARRAY_ELEM_AT_OFFSET (bbase_y, n * bxstride_bytes);
	  GFC_DESCRIPTOR1_ELEM (retarray, y) = s;
	}
    }
  else if (axstride_bytes < aystride_bytes)
    {
      for (y = 0; y < ycount; y++)
	for (x = 0; x < xcount; x++)
	  GFC_DESCRIPTOR2_ELEM (retarray, x, y) = (GFC_COMPLEX_8)0;

      for (y = 0; y < ycount; y++)
	for (n = 0; n < count; n++)
	  for (x = 0; x < xcount; x++)
	    /* dest[x,y] += a[x,n] * b[n,y] */
	    GFC_DESCRIPTOR2_ELEM (retarray, x, y)
		    += GFC_DESCRIPTOR2_ELEM (a, x, n)
		       * GFC_DESCRIPTOR2_ELEM (b, n, y);
    }
  else
    {
      const GFC_COMPLEX_8 *restrict abase_x;
      const GFC_COMPLEX_8 *restrict bbase_y;
      GFC_COMPLEX_8 *restrict dest_y;
      GFC_COMPLEX_8 s;

      for (y = 0; y < ycount; y++)
	{
	  bbase_y = PTR_ADD_OFFSET (bbase, y * bystride_bytes);
	  dest_y = PTR_ADD_OFFSET (dest, y * rystride_bytes);
	  for (x = 0; x < xcount; x++)
	    {
	      abase_x = PTR_ADD_OFFSET (abase, x * axstride_bytes);
	      s = (GFC_COMPLEX_8) 0;
	      for (n = 0; n < count; n++)
		s += ARRAY_ELEM_AT_OFFSET (abase_x, n * aystride_bytes)
		     * ARRAY_ELEM_AT_OFFSET (bbase_y, n * bxstride_bytes);
	      ARRAY_ELEM_AT_OFFSET (dest_y, x * rxstride_bytes) = s;
	    }
	}
    }
}
#undef POW3
#undef min
#undef max

#endif

#if defined(HAVE_AVX) && defined(HAVE_FMA4) && defined(HAVE_AVX128)
void
matmul_c8_avx128_fma4 (gfc_array_c8 * const restrict retarray, 
	gfc_array_c8 * const restrict a, gfc_array_c8 * const restrict b, int try_blas,
	int blas_limit, blas_call gemm) __attribute__((__target__("avx,fma4")));
internal_proto(matmul_c8_avx128_fma4);
void
matmul_c8_avx128_fma4 (gfc_array_c8 * const restrict retarray,
	gfc_array_c8 * const restrict a, gfc_array_c8 * const restrict b, int try_blas,
	int blas_limit, blas_call gemm)
{
  const GFC_COMPLEX_8 * restrict abase;
  const GFC_COMPLEX_8 * restrict bbase;
  GFC_COMPLEX_8 * restrict dest;

  index_type rystride, axstride, aystride, bxstride, bystride;
  index_type x, y, n, count, xcount, ycount;
  index_type axstride_bytes, aystride_bytes, bxstride_bytes, bystride_bytes,
	     rxstride_bytes, rystride_bytes;

  assert (GFC_DESCRIPTOR_RANK (a) == 2
          || GFC_DESCRIPTOR_RANK (b) == 2);

/* C[xcount,ycount] = A[xcount, count] * B[count,ycount]

   Either A or B (but not both) can be rank 1:

   o One-dimensional argument A is implicitly treated as a row matrix
     dimensioned [1,count], so xcount=1.

   o One-dimensional argument B is implicitly treated as a column matrix
     dimensioned [count, 1], so ycount=1.
*/

  if (retarray->base_addr == NULL)
    {
      index_type stride0;
      if (GFC_DESCRIPTOR_BYTES_COUNTED_STRIDES (retarray))
	stride0 = sizeof (GFC_COMPLEX_8);
      else
	stride0 = 1;

      if (GFC_DESCRIPTOR_RANK (a) == 1)
        {
	  GFC_DESCRIPTOR_DIMENSION_SET(retarray, 0, 0,
				       GFC_DESCRIPTOR_EXTENT(b,1) - 1, stride0);
        }
      else if (GFC_DESCRIPTOR_RANK (b) == 1)
        {
	  GFC_DESCRIPTOR_DIMENSION_SET(retarray, 0, 0,
				       GFC_DESCRIPTOR_EXTENT(a,0) - 1, stride0);
        }
      else
        {
	  GFC_DESCRIPTOR_DIMENSION_SET(retarray, 0, 0,
				       GFC_DESCRIPTOR_EXTENT(a,0) - 1, stride0);

          GFC_DESCRIPTOR_DIMENSION_SET(retarray, 1, 0,
				       GFC_DESCRIPTOR_EXTENT(b,1) - 1,
				       GFC_DESCRIPTOR_EXTENT(retarray,0)
				       * stride0);
        }

      retarray->base_addr
	= xmallocarray (size0 ((array_t *) retarray), sizeof (GFC_COMPLEX_8));
      retarray->offset = 0;
    }
  else if (unlikely (compile_options.bounds_check))
    {
      index_type ret_extent, arg_extent;

      if (GFC_DESCRIPTOR_RANK (a) == 1)
	{
	  arg_extent = GFC_DESCRIPTOR_EXTENT(b,1);
	  ret_extent = GFC_DESCRIPTOR_EXTENT(retarray,0);
	  if (arg_extent != ret_extent)
	    runtime_error ("Array bound mismatch for dimension 1 of "
	    		   "array (%ld/%ld) ",
			   (long int) ret_extent, (long int) arg_extent);
	}
      else if (GFC_DESCRIPTOR_RANK (b) == 1)
	{
	  arg_extent = GFC_DESCRIPTOR_EXTENT(a,0);
	  ret_extent = GFC_DESCRIPTOR_EXTENT(retarray,0);
	  if (arg_extent != ret_extent)
	    runtime_error ("Array bound mismatch for dimension 1 of "
	    		   "array (%ld/%ld) ",
			   (long int) ret_extent, (long int) arg_extent);
	}
      else
	{
	  arg_extent = GFC_DESCRIPTOR_EXTENT(a,0);
	  ret_extent = GFC_DESCRIPTOR_EXTENT(retarray,0);
	  if (arg_extent != ret_extent)
	    runtime_error ("Array bound mismatch for dimension 1 of "
	    		   "array (%ld/%ld) ",
			   (long int) ret_extent, (long int) arg_extent);

	  arg_extent = GFC_DESCRIPTOR_EXTENT(b,1);
	  ret_extent = GFC_DESCRIPTOR_EXTENT(retarray,1);
	  if (arg_extent != ret_extent)
	    runtime_error ("Array bound mismatch for dimension 2 of "
	    		   "array (%ld/%ld) ",
			   (long int) ret_extent, (long int) arg_extent);
	}
    }


  if (GFC_DESCRIPTOR_RANK (retarray) == 1)
    {
      /* One-dimensional result may be addressed in the code below
	 either as a row or a column matrix. We want both cases to
	 work. */
      rystride = GFC_DESCRIPTOR_STRIDE_UNITS(retarray,0);
      rxstride_bytes = rystride_bytes = GFC_DESCRIPTOR_STRIDE_BYTES(retarray,0);
    }
  else
    {
      rystride = GFC_DESCRIPTOR_STRIDE_UNITS(retarray,1);
      rxstride_bytes = GFC_DESCRIPTOR_STRIDE_BYTES(retarray,0);
      rystride_bytes = GFC_DESCRIPTOR_STRIDE_BYTES(retarray,1);
    }

  if (GFC_DESCRIPTOR_RANK (a) == 1)
    {
      /* Treat it as a a row matrix A[1,count]. */
      axstride = GFC_DESCRIPTOR_STRIDE_UNITS(a,0);
      aystride = 1;
      axstride_bytes = GFC_DESCRIPTOR_STRIDE_BYTES(a,0);
      aystride_bytes = sizeof (GFC_COMPLEX_8);

      xcount = 1;
      count = GFC_DESCRIPTOR_EXTENT(a,0);
    }
  else
    {
      axstride = GFC_DESCRIPTOR_STRIDE_UNITS(a,0);
      aystride = GFC_DESCRIPTOR_STRIDE_UNITS(a,1);
      axstride_bytes = GFC_DESCRIPTOR_STRIDE_BYTES(a,0);
      aystride_bytes = GFC_DESCRIPTOR_STRIDE_BYTES(a,1);

      count = GFC_DESCRIPTOR_EXTENT(a,1);
      xcount = GFC_DESCRIPTOR_EXTENT(a,0);
    }

  if (count != GFC_DESCRIPTOR_EXTENT(b,0))
    {
      if (count > 0 || GFC_DESCRIPTOR_EXTENT(b,0) > 0)
	runtime_error ("Incorrect extent in argument B in MATMUL intrinsic "
		       "in dimension 1: is %ld, should be %ld",
		       (long int) GFC_DESCRIPTOR_EXTENT(b,0), (long int) count);
    }

  if (GFC_DESCRIPTOR_RANK (b) == 1)
    {
      /* Treat it as a column matrix B[count,1] */
      bxstride = GFC_DESCRIPTOR_STRIDE_UNITS(b,0);
      bxstride_bytes = GFC_DESCRIPTOR_STRIDE_BYTES(b,0);

      /* bystride should never be used for 1-dimensional b.
         The value is only used for calculation of the
         memory by the buffer.  */
      bystride = 256;
      bystride_bytes = 99999999;
      ycount = 1;
    }
  else
    {
      bxstride = GFC_DESCRIPTOR_STRIDE_UNITS(b,0);
      bystride = GFC_DESCRIPTOR_STRIDE_UNITS(b,1);
      bxstride_bytes = GFC_DESCRIPTOR_STRIDE_BYTES(b,0);
      bystride_bytes = GFC_DESCRIPTOR_STRIDE_BYTES(b,1);
      ycount = GFC_DESCRIPTOR_EXTENT(b,1);
    }

  abase = a->base_addr;
  bbase = b->base_addr;
  dest = retarray->base_addr;

  /* Now that everything is set up, we perform the multiplication
     itself.  */

#define POW3(x) (((float) (x)) * ((float) (x)) * ((float) (x)))
#define min(a,b) ((a) <= (b) ? (a) : (b))
#define max(a,b) ((a) >= (b) ? (a) : (b))

  if (try_blas
      && rxstride_bytes == sizeof (GFC_COMPLEX_8)
      && (axstride_bytes == sizeof (GFC_COMPLEX_8)
	  || aystride_bytes == sizeof (GFC_COMPLEX_8))
      && (bxstride_bytes == sizeof (GFC_COMPLEX_8)
	  || bystride_bytes == sizeof (GFC_COMPLEX_8))
      && (((float) xcount) * ((float) ycount) * ((float) count)
          > POW3(blas_limit)))
    {
      const int m = xcount, n = ycount, k = count, ldc = rystride;
      const GFC_COMPLEX_8 one = 1, zero = 0;
      const int lda = (axstride_bytes == sizeof (GFC_COMPLEX_8)) ? aystride : axstride,
		ldb = (bxstride_bytes == sizeof (GFC_COMPLEX_8)) ? bystride : bxstride;

      if (lda > 0 && ldb > 0 && ldc > 0 && m > 1 && n > 1 && k > 1)
	{
	  assert (gemm != NULL);
	  const char *transa, *transb;
	  if (try_blas & 2)
	    transa = "C";
	  else
	    transa = axstride_bytes == sizeof (GFC_COMPLEX_8) ? "N" : "T";

	  if (try_blas & 4)
	    transb = "C";
	  else
	    transb = bxstride_bytes == sizeof (GFC_COMPLEX_8) ? "N" : "T";

	  gemm (transa, transb , &m,
		&n, &k,	&one, abase, &lda, bbase, &ldb, &zero, dest,
		&ldc, 1, 1);
	  return;
	}
    }

  if (rxstride_bytes == sizeof (GFC_COMPLEX_8)
      && axstride_bytes == sizeof (GFC_COMPLEX_8)
      && bxstride_bytes == sizeof (GFC_COMPLEX_8)
      && GFC_DESCRIPTOR_RANK (b) != 1)
    {
      /* This block of code implements a tuned matmul, derived from
         Superscalar GEMM-based level 3 BLAS,  Beta version 0.1

               Bo Kagstrom and Per Ling
               Department of Computing Science
               Umea University
               S-901 87 Umea, Sweden

	 from netlib.org, translated to C, and modified for matmul.m4.  */

      GFC_COMPLEX_8 *c;
      const index_type m = xcount, n = ycount, k = count;

      /* System generated locals */
      index_type a_dim1, b_dim1,
		 i1, i2, i3, i4, i5, i6;

      /* Local variables */
      GFC_COMPLEX_8 f11, f12, f21, f22, f31, f32, f41, f42,
		 f13, f14, f23, f24, f33, f34, f43, f44;
      index_type i, j, l, ii, jj, ll;
      index_type isec, jsec, lsec, uisec, ujsec, ulsec;
      GFC_COMPLEX_8 *t1;

      c = retarray->base_addr;

      /* Parameter adjustments */
      a_dim1 = aystride;
      b_dim1 = bystride;

#define A_ARRAY_ELEM(i,j) \
    (ARRAY_ELEM_AT_OFFSET (abase, (i) * sizeof (GFC_COMPLEX_8) + (j) * aystride_bytes))

#define B_ARRAY_ELEM(i,j) \
    (ARRAY_ELEM_AT_OFFSET (bbase, (i) * sizeof (GFC_COMPLEX_8) + (j) * bystride_bytes))

#define C_ARRAY_ELEM(i,j) \
    (ARRAY_ELEM_AT_OFFSET (c, (i) * sizeof (GFC_COMPLEX_8) + (j) * rystride_bytes))

      /* Empty result first.  */
      for (j=0; j<n; j++)
	for (i=0; i<m; i++)
	  C_ARRAY_ELEM (i, j) = (GFC_COMPLEX_8)0;

      /* Early exit if possible */
      if (m == 0 || n == 0 || k == 0)
	return;

      /* Adjust size of t1 to what is needed.  */
      index_type t1_dim, a_sz;
      if (aystride_bytes == sizeof (GFC_COMPLEX_8))
        a_sz = rystride;
      else
        a_sz = a_dim1;

      t1_dim = a_sz * 256 + b_dim1;
      if (t1_dim > 65536)
	t1_dim = 65536;

      t1 = malloc (t1_dim * sizeof(GFC_COMPLEX_8));

      /* Start turning the crank. */
      i1 = n;
      for (jj = 0; jj < i1; jj += 512)
	{
	  /* Computing MIN */
	  i2 = 512;
	  i3 = n - jj;
	  jsec = min(i2,i3);
	  ujsec = jsec - jsec % 4;
	  i2 = k;
	  for (ll = 0; ll < i2; ll += 256)
	    {
	      /* Computing MIN */
	      i3 = 256;
	      i4 = k - ll;
	      lsec = min(i3,i4);
	      ulsec = lsec - lsec % 2;

	      i3 = m;
	      for (ii = 0; ii < i3; ii += 256)
		{
		  /* Computing MIN */
		  i4 = 256;
		  i5 = m - ii;
		  isec = min(i4,i5);
		  uisec = isec - isec % 2;
		  i4 = ll + ulsec;
		  for (l = ll; l < i4; l += 2)
		    {
		      i5 = ii + uisec;
		      for (i = ii; i < i5; i += 2)
			{
			  t1[l - ll + 1 + ((i - ii + 1) << 8) - 257] =
					A_ARRAY_ELEM (i, l);
			  t1[l - ll + 2 + ((i - ii + 1) << 8) - 257] =
					A_ARRAY_ELEM (i, l + 1);
			  t1[l - ll + 1 + ((i - ii + 2) << 8) - 257] =
					A_ARRAY_ELEM (i + 1, l);
			  t1[l - ll + 2 + ((i - ii + 2) << 8) - 257] =
					A_ARRAY_ELEM (i + 1, l + 1);
			}
		      if (uisec < isec)
			{
			  t1[l - ll + 1 + (isec << 8) - 257] =
				    A_ARRAY_ELEM (ii + isec - 1, l);
			  t1[l - ll + 2 + (isec << 8) - 257] =
				    A_ARRAY_ELEM (ii + isec - 1, l + 1);
			}
		    }
		  if (ulsec < lsec)
		    {
		      i4 = ii + isec;
		      for (i = ii; i< i4; ++i)
			{
			  t1[lsec + ((i - ii + 1) << 8) - 257] =
				    A_ARRAY_ELEM (i, ll + lsec - 1);
			}
		    }

		  uisec = isec - isec % 4;
		  i4 = jj + ujsec;
		  for (j = jj; j < i4; j += 4)
		    {
		      i5 = ii + uisec;
		      for (i = ii; i < i5; i += 4)
			{
			  f11 = C_ARRAY_ELEM (i, j);
			  f21 = C_ARRAY_ELEM (i + 1, j);
			  f12 = C_ARRAY_ELEM (i, j + 1);
			  f22 = C_ARRAY_ELEM (i + 1, j + 1);
			  f13 = C_ARRAY_ELEM (i, j + 2);
			  f23 = C_ARRAY_ELEM (i + 1, j + 2);
			  f14 = C_ARRAY_ELEM (i, j + 3);
			  f24 = C_ARRAY_ELEM (i + 1, j + 3);
			  f31 = C_ARRAY_ELEM (i + 2, j);
			  f41 = C_ARRAY_ELEM (i + 3, j);
			  f32 = C_ARRAY_ELEM (i + 2, j + 1);
			  f42 = C_ARRAY_ELEM (i + 3, j + 1);
			  f33 = C_ARRAY_ELEM (i + 2, j + 2);
			  f43 = C_ARRAY_ELEM (i + 3, j + 2);
			  f34 = C_ARRAY_ELEM (i + 2, j + 3);
			  f44 = C_ARRAY_ELEM (i + 3, j + 3);
			  i6 = ll + lsec;
			  for (l = ll; l < i6; ++l)
			    {
			      f11 += t1[l - ll + 1 + ((i - ii + 1) << 8) - 257]
				      * B_ARRAY_ELEM (l, j);
			      f21 += t1[l - ll + 1 + ((i - ii + 2) << 8) - 257]
				      * B_ARRAY_ELEM (l, j);
			      f12 += t1[l - ll + 1 + ((i - ii + 1) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 1);
			      f22 += t1[l - ll + 1 + ((i - ii + 2) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 1);
			      f13 += t1[l - ll + 1 + ((i - ii + 1) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 2);
			      f23 += t1[l - ll + 1 + ((i - ii + 2) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 2);
			      f14 += t1[l - ll + 1 + ((i - ii + 1) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 3);
			      f24 += t1[l - ll + 1 + ((i - ii + 2) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 3);
			      f31 += t1[l - ll + 1 + ((i - ii + 3) << 8) - 257]
				      * B_ARRAY_ELEM (l, j);
			      f41 += t1[l - ll + 1 + ((i - ii + 4) << 8) - 257]
				      * B_ARRAY_ELEM (l, j);
			      f32 += t1[l - ll + 1 + ((i - ii + 3) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 1);
			      f42 += t1[l - ll + 1 + ((i - ii + 4) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 1);
			      f33 += t1[l - ll + 1 + ((i - ii + 3) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 2);
			      f43 += t1[l - ll + 1 + ((i - ii + 4) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 2);
			      f34 += t1[l - ll + 1 + ((i - ii + 3) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 3);
			      f44 += t1[l - ll + 1 + ((i - ii + 4) << 8) - 257]
				      * B_ARRAY_ELEM (l, j + 3);
			    }
			  C_ARRAY_ELEM (i, j) = f11;
			  C_ARRAY_ELEM (i + 1, j) = f21;
			  C_ARRAY_ELEM (i, j + 1) = f12;
			  C_ARRAY_ELEM (i + 1, j + 1) = f22;
			  C_ARRAY_ELEM (i, j + 2) = f13;
			  C_ARRAY_ELEM (i + 1, j + 2) = f23;
			  C_ARRAY_ELEM (i, j + 3) = f14;
			  C_ARRAY_ELEM (i + 1, j + 3) = f24;
			  C_ARRAY_ELEM (i + 2, j) = f31;
			  C_ARRAY_ELEM (i + 3, j) = f41;
			  C_ARRAY_ELEM (i + 2, j + 1) = f32;
			  C_ARRAY_ELEM (i + 3, j + 1) = f42;
			  C_ARRAY_ELEM (i + 2, j + 2) = f33;
			  C_ARRAY_ELEM (i + 3, j + 2) = f43;
			  C_ARRAY_ELEM (i + 2, j + 3) = f34;
			  C_ARRAY_ELEM (i + 3, j + 3) = f44;
			}
		      if (uisec < isec)
			{
			  i5 = ii + isec;
			  for (i = ii + uisec; i < i5; ++i)
			    {
			      f11 = C_ARRAY_ELEM (i, j);
			      f12 = C_ARRAY_ELEM (i, j + 1);
			      f13 = C_ARRAY_ELEM (i, j + 2);
			      f14 = C_ARRAY_ELEM (i, j + 3);
			      i6 = ll + lsec;
			      for (l = ll; l < i6; ++l)
				{
				  f11 += t1[l - ll + 1 + ((i - ii + 1) << 8) -
					  257] * B_ARRAY_ELEM (l, j);
				  f12 += t1[l - ll + 1 + ((i - ii + 1) << 8) -
					  257] * B_ARRAY_ELEM (l, j + 1);
				  f13 += t1[l - ll + 1 + ((i - ii + 1) << 8) -
					  257] * B_ARRAY_ELEM (l, j + 2);
				  f14 += t1[l - ll + 1 + ((i - ii + 1) << 8) -
					  257] * B_ARRAY_ELEM (l, j + 3);
				}
			      C_ARRAY_ELEM (i, j) = f11;
			      C_ARRAY_ELEM (i, j + 1) = f12;
			      C_ARRAY_ELEM (i, j + 2) = f13;
			      C_ARRAY_ELEM (i, j + 3) = f14;
			    }
			}
		    }
		  if (ujsec < jsec)
		    {
		      i4 = jj + jsec;
		      for (j = jj + ujsec; j < i4; ++j)
			{
			  i5 = ii + uisec;
			  for (i = ii; i < i5; i += 4)
			    {
			      f11 = C_ARRAY_ELEM (i, j);
			      f21 = C_ARRAY_ELEM (i + 1, j);
			      f31 = C_ARRAY_ELEM (i + 2, j);
			      f41 = C_ARRAY_ELEM (i + 3, j);
			      i6 = ll + lsec;
			      for (l = ll; l < i6; ++l)
				{
				  f11 += t1[l - ll + 1 + ((i - ii + 1) << 8) -
					  257] * B_ARRAY_ELEM (l, j);
				  f21 += t1[l - ll + 1 + ((i - ii + 2) << 8) -
					  257] * B_ARRAY_ELEM (l, j);
				  f31 += t1[l - ll + 1 + ((i - ii + 3) << 8) -
					  257] * B_ARRAY_ELEM (l, j);
				  f41 += t1[l - ll + 1 + ((i - ii + 4) << 8) -
					  257] * B_ARRAY_ELEM (l, j);
				}
			      C_ARRAY_ELEM (i, j) = f11;
			      C_ARRAY_ELEM (i + 1, j) = f21;
			      C_ARRAY_ELEM (i + 2, j) = f31;
			      C_ARRAY_ELEM (i + 3, j) = f41;
			    }
			  i5 = ii + isec;
			  for (i = ii + uisec; i < i5; ++i)
			    {
			      f11 = C_ARRAY_ELEM (i, j);
			      i6 = ll + lsec;
			      for (l = ll; l < i6; ++l)
				{
				  f11 += t1[l - ll + 1 + ((i - ii + 1) << 8) -
					  257] * B_ARRAY_ELEM (l, j);
				}
			      C_ARRAY_ELEM (i, j) = f11;
			    }
			}
		    }
		}
	    }
	}
      free(t1);
      return;
#undef A_ARRAY_ELEM
#undef B_ARRAY_ELEM
#undef C_ARRAY_ELEM
    }
  else if (rxstride_bytes == sizeof (GFC_COMPLEX_8)
	   && aystride_bytes == sizeof (GFC_COMPLEX_8)
	   && bxstride_bytes == sizeof (GFC_COMPLEX_8))
    {
      if (GFC_DESCRIPTOR_RANK (a) != 1)
	{
	  const GFC_COMPLEX_8 *restrict abase_x;
	  const GFC_COMPLEX_8 *restrict bbase_y;
	  GFC_COMPLEX_8 *restrict dest_y;
	  GFC_COMPLEX_8 s;

	  for (y = 0; y < ycount; y++)
	    {
	      bbase_y = PTR_ADD_OFFSET (bbase, y * bystride_bytes);
	      dest_y = PTR_ADD_OFFSET (dest, y * rystride_bytes);
	      for (x = 0; x < xcount; x++)
		{
		  abase_x = PTR_ADD_OFFSET (abase, x * axstride_bytes);
		  s = (GFC_COMPLEX_8) 0;
		  for (n = 0; n < count; n++)
		    s += abase_x[n] * bbase_y[n];
		  dest_y[x] = s;
		}
	    }
	}
      else
	{
	  const GFC_COMPLEX_8 *restrict bbase_y;
	  GFC_COMPLEX_8 s;

	  for (y = 0; y < ycount; y++)
	    {
	      bbase_y = PTR_ADD_OFFSET (bbase, y * bystride_bytes);
	      s = (GFC_COMPLEX_8) 0;
	      for (n = 0; n < count; n++)
		s += GFC_DESCRIPTOR1_ELEM (a, n) * bbase_y[n];
	      ARRAY_ELEM_AT_OFFSET (dest, y * rystride_bytes) = s;
	    }
	}
    }
  else if (GFC_DESCRIPTOR_RANK (a) == 1)
    {
      const GFC_COMPLEX_8 *restrict bbase_y;
      GFC_COMPLEX_8 s;

      for (y = 0; y < ycount; y++)
	{
	  bbase_y = PTR_ADD_OFFSET (bbase, y * bystride_bytes);
	  s = (GFC_COMPLEX_8) 0;
	  for (n = 0; n < count; n++)
	    s += GFC_DESCRIPTOR1_ELEM (a, n)
	         * ARRAY_ELEM_AT_OFFSET (bbase_y, n * bxstride_bytes);
	  GFC_DESCRIPTOR1_ELEM (retarray, y) = s;
	}
    }
  else if (axstride_bytes < aystride_bytes)
    {
      for (y = 0; y < ycount; y++)
	for (x = 0; x < xcount; x++)
	  GFC_DESCRIPTOR2_ELEM (retarray, x, y) = (GFC_COMPLEX_8)0;

      for (y = 0; y < ycount; y++)
	for (n = 0; n < count; n++)
	  for (x = 0; x < xcount; x++)
	    /* dest[x,y] += a[x,n] * b[n,y] */
	    GFC_DESCRIPTOR2_ELEM (retarray, x, y)
		    += GFC_DESCRIPTOR2_ELEM (a, x, n)
		       * GFC_DESCRIPTOR2_ELEM (b, n, y);
    }
  else
    {
      const GFC_COMPLEX_8 *restrict abase_x;
      const GFC_COMPLEX_8 *restrict bbase_y;
      GFC_COMPLEX_8 *restrict dest_y;
      GFC_COMPLEX_8 s;

      for (y = 0; y < ycount; y++)
	{
	  bbase_y = PTR_ADD_OFFSET (bbase, y * bystride_bytes);
	  dest_y = PTR_ADD_OFFSET (dest, y * rystride_bytes);
	  for (x = 0; x < xcount; x++)
	    {
	      abase_x = PTR_ADD_OFFSET (abase, x * axstride_bytes);
	      s = (GFC_COMPLEX_8) 0;
	      for (n = 0; n < count; n++)
		s += ARRAY_ELEM_AT_OFFSET (abase_x, n * aystride_bytes)
		     * ARRAY_ELEM_AT_OFFSET (bbase_y, n * bxstride_bytes);
	      ARRAY_ELEM_AT_OFFSET (dest_y, x * rxstride_bytes) = s;
	    }
	}
    }
}
#undef POW3
#undef min
#undef max

#endif

#endif

