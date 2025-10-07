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

/* Expand a bfloat16 floating point binary operation:

   ICODE: Operation to perform.
   OP0:   Result (BFmode or SFmode).
   OP1:   First input argument (BFmode or SFmode).
   OP2:   Second input argument (BFmode or SFmode).
   TMP0:  Temporary for result (V4SFmode).
   TMP1:  Temporary for first input argument (V4SFmode).
   TMP2:  Temporary for second input argument (V4SFmode).

   The operation is done as a V4SFmode vector operation.  This is because
   converting BFmode from a scalar BFmode to SFmode to do the operation and
   back again takes quite a bit of time.  GCC will only generate the native
   operation if -Ofast is used.  The float16.md code that calls this function
   adds various combine operations to do the operation in V4SFmode instead of
   SFmode.  */
	
void
bfloat16_binary_op_as_v4sf (enum rtx_code icode,
			    rtx op0,
			    rtx op1,
			    rtx op2,
			    rtx tmp0,
			    rtx tmp1,
			    rtx tmp2)
{
  if (GET_CODE (tmp0) == SCRATCH)
    tmp0 = gen_reg_rtx (V4SFmode);

  if (GET_CODE (tmp1) == SCRATCH)
    tmp1 = gen_reg_rtx (V4SFmode);

  if (GET_CODE (tmp2) == SCRATCH)
    tmp2 = gen_reg_rtx (V4SFmode);

  /* Convert operand1 and operand2 to V4SFmode format.  We use SPLAT for
     registers to get the value into the upper 32-bits.  We can use XXSPLTW
     to splat words instead of VSPLTIH since the XVCVBF16SPN instruction
     ignores the odd half-words, and XXSPLTW can operate on all VSX registers
     instead of just the Altivec registers.  Using SPLAT instead of a shift
     also insure that other bits are not a signalling NaN.  If we are using
     XXSPLTIW or XXSPLTIB to load the constant the other bits are duplicated.  */

  /* Operand1.  */
  if (GET_MODE (op1) == BFmode)
    {
      emit_insn (gen_xxspltw_bf (tmp1, op1));
      emit_insn (gen_xvcvbf16spn_bf (tmp1, tmp1));
    }

  else if (GET_MODE (op1) == SFmode)
    emit_insn (gen_vsx_splat_v4sf (tmp1,
				   force_reg (SFmode, op1)));

  else
    gcc_unreachable ();

  /* Operand2.  */
  if (GET_MODE (op2) == BFmode)
    {
      if (REG_P (op2) || SUBREG_P (op2))
	emit_insn (gen_xxspltw_bf (tmp2, op2));

      else if (op2 == CONST0_RTX (BFmode))
	emit_move_insn (tmp2, CONST0_RTX (V4SFmode));

      else if (fp16_xxspltiw_constant (op2, BFmode))
	{
	  rtx op2_bf = gen_lowpart (BFmode, tmp2);
	  emit_move_insn (op2_bf, op2);
	}

      else
	gcc_unreachable ();

      emit_insn (gen_xvcvbf16spn_bf (tmp2, tmp2));
    }

  else if (GET_MODE (op2) == SFmode)
    {
      if (REG_P (op2) || SUBREG_P (op2))
	emit_insn (gen_vsx_splat_v4sf (tmp2, op2));

      else if (op2 == CONST0_RTX (SFmode))
	emit_move_insn (tmp2, CONST0_RTX (V4SFmode));

      else if (GET_CODE (op2) == CONST_DOUBLE)
	{
	  rtvec v = rtvec_alloc (4);
	  RTVEC_ELT (v, 0) = op2;
	  RTVEC_ELT (v, 1) = op2;
	  RTVEC_ELT (v, 2) = op2;
	  RTVEC_ELT (v, 3) = op2;
	  emit_insn (gen_rtx_SET (tmp2,
				  gen_rtx_CONST_VECTOR (V4SFmode, v)));
	}

      else
	emit_insn (gen_vsx_splat_v4sf (tmp2,
				       force_reg (SFmode, op2)));
    }

  else
    gcc_unreachable ();

  /* Do the operation in V4SFmode.  */
  emit_insn (gen_rtx_SET (tmp0,
			  gen_rtx_fmt_ee (icode, V4SFmode, tmp1, tmp2)));

  /* Convert V4SF result back to scalar mode.  */
  if (GET_MODE (op0) == BFmode)
    emit_insn (gen_xvcvspbf16_bf (op0, tmp0));

  else if (GET_MODE (op0) == SFmode)
    {
      rtx element = GEN_INT (WORDS_BIG_ENDIAN ? 2 : 3);
      emit_insn (gen_vsx_extract_v4sf (op0, tmp0, element));
    }

  else
    gcc_unreachable ();
}

