/* Subroutines for the C front end on the PowerPC architecture.
   Copyright (C) 2002-2025 Free Software Foundation, Inc.

   Contributed by Zack Weinberg <zack@codesourcery.com>
   and Paolo Bonzini <bonzini@gnu.org>

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published
   by the Free Software Foundation; either version 3, or (at your
   option) any later version.

   GCC is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
   or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
   License for more details.

   You should have received a copy of the GNU General Public License
   along with GCC; see the file COPYING3.  If not see
   <http://www.gnu.org/licenses/>.  */

/* 16-bit floating point support.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "rtl.h"
#include "tree.h"
#include "memmodel.h"
#include "tm_p.h"
#include "stringpool.h"
#include "expmed.h"
#include "optabs.h"
#include "regs.h"
#include "insn-attr.h"
#include "flags.h"
#include "attribs.h"
#include "explow.h"
#include "expr.h"
#include "common/common-target.h"
#include "rs6000-internal.h"

/* Expand a 16-bit vector operation:

   ICODE:   Operation to perform.
   RESULT:  Result of the operation.
   OP1:     Input operand1.
   OP2:     Input operand2.
   OP3:     Input operand3 or NULL_RTX.
   SUBTYPE: Describe the operation.  */
	
void
fp16_vectorization (enum rtx_code icode,
		    rtx result,
		    rtx op1,
		    rtx op2,
		    rtx op3,
		    enum fp16_operation subtype)
{
  gcc_assert (can_create_pseudo_p ());

  machine_mode result_mode = GET_MODE (result);
  rtx op_orig[3] = { op1, op2, op3 };
  rtx op_hi[3];
  rtx op_lo[3];
  rtx result_hi;
  rtx result_lo;
  size_t n_opts;

  switch (subtype)
    {
    case FP16_BINARY:
      n_opts = 2;
      break;

    case FP16_FMA:
    case FP16_FMS:
    case FP16_NFMA:
    case FP16_NFMS:
      n_opts = 3;
      break;

    default:
      gcc_unreachable ();
    }

  /* Allocate 2 temporaries for the results and the input operands.  */
  result_hi = gen_reg_rtx (V4SFmode);
  result_lo = gen_reg_rtx (V4SFmode);

  for (size_t i = 0; i < n_opts; i++)
    {
      gcc_assert (op_orig[i] != NULL_RTX);
      op_hi[i] = gen_reg_rtx (V4SFmode);	/* high register.  */
      op_lo[i] = gen_reg_rtx (V4SFmode);	/* low register.  */

      rtx interleave_hi = gen_reg_rtx (result_mode);
      rtx interleave_lo = gen_reg_rtx (result_mode);
      rtx orig = op_orig[i];

      rs6000_expand_interleave (interleave_hi, orig, orig, !BYTES_BIG_ENDIAN);
      rs6000_expand_interleave (interleave_lo, orig, orig,  BYTES_BIG_ENDIAN);

      if (result_mode == V8HFmode)
	{
	  emit_insn (gen_xvcvhpsp_v8hf (op_hi[i], interleave_hi));
	  emit_insn (gen_xvcvhpsp_v8hf (op_lo[i], interleave_lo));
	}

      else if (result_mode == V8BFmode)
	{
	  emit_insn (gen_xvcvbf16spn_v8bf (op_hi[i], interleave_hi));
	  emit_insn (gen_xvcvbf16spn_v8bf (op_lo[i], interleave_lo));
	}

      else
	gcc_unreachable ();
    }

  /* Do 2 sets of V4SFmode operations.  */
  switch (subtype)
    {
    case FP16_BINARY:
      emit_insn (gen_rtx_SET (result_hi,
			      gen_rtx_fmt_ee (icode, V4SFmode,
					      op_hi[0],
					      op_hi[1])));

      emit_insn (gen_rtx_SET (result_lo,
			      gen_rtx_fmt_ee (icode, V4SFmode,
					      op_lo[0],
					      op_lo[1])));
      break;

    case FP16_FMA:
    case FP16_FMS:
    case FP16_NFMA:
    case FP16_NFMS:
      {
	rtx op1_hi = op_hi[0];
	rtx op2_hi = op_hi[1];
	rtx op3_hi = op_hi[2];

	rtx op1_lo = op_lo[0];
	rtx op2_lo = op_lo[1];
	rtx op3_lo = op_lo[2];

	if (subtype == FP16_FMS || subtype == FP16_NFMS)
	  {
	    op3_hi = gen_rtx_NEG (V4SFmode, op3_hi);
	    op3_lo = gen_rtx_NEG (V4SFmode, op3_lo);
	  }

	rtx op_fma_hi = gen_rtx_FMA (V4SFmode, op1_hi, op2_hi, op3_hi);
	rtx op_fma_lo = gen_rtx_FMA (V4SFmode, op1_lo, op2_lo, op3_lo);

	if (subtype == FP16_NFMA || subtype == FP16_NFMS)
	  {
	    op_fma_hi = gen_rtx_NEG (V4SFmode, op_fma_hi);
	    op_fma_lo = gen_rtx_NEG (V4SFmode, op_fma_lo);
	  }

	emit_insn (gen_rtx_SET (result_hi, op_fma_hi));
	emit_insn (gen_rtx_SET (result_lo, op_fma_lo));
      }
      break;

    default:
      gcc_unreachable ();
    }

  /* Combine the 2 V4SFmode operations into one V8HFmode/V8BFmode vector.  */
  if (result_mode == V8HFmode)
    emit_insn (gen_vec_pack_trunc_v4sf_v8hf (result, result_hi, result_lo));

  else if (result_mode == V8BFmode)
    emit_insn (gen_vec_pack_trunc_v4sf_v8bf (result, result_hi, result_lo));

  else
    gcc_unreachable ();

  return;
}
