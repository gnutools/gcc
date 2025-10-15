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

/* Expand a bfloat16 scalar floating point operation:

   ICODE:   Operation to perform.
   RESULT:  Result of the operation.
   OP1:     Input operand1.
   OP2:     Input operand2.
   OP3:     Input operand3 or NULL_RTX.
   SUBTYPE: Describe the operation.

   The operation is done as a V4SFmode vector operation.  This is because
   converting BFmode from a scalar BFmode to SFmode to do the operation and
   back again takes quite a bit of time.  GCC will only generate the native
   operation if -Ofast is used.  The float16.md code that calls this function
   adds various combine operations to do the operation in V4SFmode instead of
   SFmode.  */
	
void
bfloat16_operation_as_v4sf (enum rtx_code icode,
			    rtx result,
			    rtx op1,
			    rtx op2,
			    rtx op3,
			    enum fp16_operation subtype)
{
  gcc_assert (can_create_pseudo_p ());

  rtx result_v4sf = gen_reg_rtx (V4SFmode);
  rtx ops_orig[3] = { op1, op2, op3 };
  rtx ops_v4sf[3];
  size_t n_opts;

  switch (subtype)
    {
    case FP16_BINARY:
      n_opts = 2;
      gcc_assert (op3 == NULL_RTX);
      break;

    case FP16_FMA:
    case FP16_FMS:
    case FP16_NFMA:
    case FP16_NFMS:
      gcc_assert (icode == FMA);
      n_opts = 3;
      break;

    default:
      gcc_unreachable ();
    }

  for (size_t i = 0; i < n_opts; i++)
    {
      rtx op = ops_orig[i];
      rtx tmp = ops_v4sf[i] = gen_reg_rtx (V4SFmode);

      gcc_assert (op != NULL_RTX);

      /* Remove truncation/extend added.  */
      if (GET_CODE (op) == FLOAT_EXTEND || GET_CODE (op) == FLOAT_TRUNCATE)
	op = XEXP (op, 0);

      /* Convert operands to V4SFmode format.  We use SPLAT for registers to
	 get the value into the upper 32-bits.  We can use XXSPLTW to splat
	 words instead of VSPLTIH since the XVCVBF16SPN instruction ignores the
	 odd half-words, and XXSPLTW can operate on all VSX registers instead
	 of just the Altivec registers.  Using SPLAT instead of a shift also
	 insure that other bits are not a signalling NaN.  If we are using
	 XXSPLTIW or XXSPLTIB to load the constant the other bits are
	 duplicated.  */

      if (op == CONST0_RTX (SFmode) || op == CONST0_RTX (BFmode))
	emit_move_insn (tmp, CONST0_RTX (V4SFmode));

      else if (GET_MODE (op) == BFmode)
	{
	  emit_insn (gen_xxspltw_bf (tmp, force_reg (BFmode, op)));
	  emit_insn (gen_xvcvbf16spn_bf (tmp, tmp));
	}

      else if (GET_MODE (op) == SFmode)
	{
	  if (GET_CODE (op) == CONST_DOUBLE)
	    {
	      rtvec v = rtvec_alloc (4);

	      for (size_t i = 0; i < 4; i++)
		RTVEC_ELT (v, i) = op;

	      emit_insn (gen_rtx_SET (tmp,
				      gen_rtx_CONST_VECTOR (V4SFmode, v)));
	    }

	  else
	    emit_insn (gen_vsx_splat_v4sf (tmp,
					   force_reg (SFmode, op)));
	}

      else
	gcc_unreachable ();
    }

  /* Do the operation in V4SFmode.  */
  switch (subtype)
    {
    case FP16_BINARY:
      emit_insn (gen_rtx_SET (result_v4sf,
			      gen_rtx_fmt_ee (icode, V4SFmode,
					      ops_v4sf[0],
					      ops_v4sf[1])));
      break;

    case FP16_FMA:
    case FP16_FMS:
    case FP16_NFMA:
    case FP16_NFMS:
      {
	rtx op1 = ops_v4sf[0];
	rtx op2 = ops_v4sf[1];
	rtx op3 = ops_v4sf[2];

	if (subtype == FP16_FMS || subtype == FP16_NFMS)
	  op3 = gen_rtx_NEG (V4SFmode, op3);

	rtx op_fma = gen_rtx_FMA (V4SFmode, op1, op2, op3);

	if (subtype == FP16_NFMA || subtype == FP16_NFMS)
	  op_fma = gen_rtx_NEG (V4SFmode, op_fma);

	emit_insn (gen_rtx_SET (result_v4sf, op_fma));
      }
      break;

    default:
      gcc_unreachable ();
    }

  /* Convert V4SF result back to scalar mode.  */
  if (GET_MODE (result) == BFmode)
    emit_insn (gen_xvcvspbf16_bf (result, result_v4sf));

  else if (GET_MODE (result) == SFmode)
    {
      rtx element = GEN_INT (WORDS_BIG_ENDIAN ? 2 : 3);
      emit_insn (gen_vsx_extract_v4sf (result, result_v4sf, element));
    }

  else
    gcc_unreachable ();
}
