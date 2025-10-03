;; Machine description for IBM RISC System 6000 (POWER) for GNU C compiler
;; Copyright (C) 1990-2025 Free Software Foundation, Inc.
;; Contributed by Richard Kenner (kenner@vlsi1.ultra.nyu.edu)

;; This file is part of GCC.

;; GCC is free software; you can redistribute it and/or modify it
;; under the terms of the GNU General Public License as published
;; by the Free Software Foundation; either version 3, or (at your
;; option) any later version.

;; GCC is distributed in the hope that it will be useful, but WITHOUT
;; ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
;; or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
;; License for more details.

;; You should have received a copy of the GNU General Public License
;; along with GCC; see the file COPYING3.  If not see
;; <http://www.gnu.org/licenses/>.

;; Support for _Float16 (HFmode) and __bfloat16 (BFmode)

;; Mode iterator for 16-bit floating modes.
(define_mode_iterator FP16 [(BF "TARGET_BFLOAT16")
			    (HF "TARGET_FLOAT16")])

;; Mode iterator for 16-bit floating modes on machines with hardware
;; support.
(define_mode_iterator FP16_HW [(BF "TARGET_BFLOAT16_HW")
			       (HF "TARGET_FLOAT16_HW")])

;; Mode iterator for floating point modes other than SF/DFmode that we
;; convert to/from _Float16 (HFmode) via DFmode.
(define_mode_iterator FP16_CONVERT [TF KF IF SD DD TD])

;; Code iterator giving the basic operations for bfloat16 floating point
;; operations.
(define_code_iterator BF_OPS [plus minus mult])

;; Code attribute that gives the standard name for the bfloat16
;; operations done via V4SF vector.
(define_code_attr BF_OPS_NAME [(plus  "add")
			       (minus "sub")
			       (mult  "mul")])

;; UNSPEC constants
(define_c_enum "unspec"
  [UNSPEC_V8BF_SHIFT_LEFT_32BIT
   UNSPEC_XVCVBF16SPN_BF
   UNSPEC_XVCVBF16SPN_V8BF
   UNSPEC_XXSPLTW_BF
   UNSPEC_XVCVSPBF16_BF])


;; _Float16 and __bfloat16 moves
(define_expand "mov<mode>"
  [(set (match_operand:FP16 0 "nonimmediate_operand")
	(match_operand:FP16 1 "any_operand"))]
  ""
{
  if (MEM_P (operands[0]) && !REG_P (operands[1]))
    operands[1] = force_reg (<MODE>mode, operands[1]);
})

;; On power10, we can load up HFmode and BFmode constants with xxspltiw
;; or pli.
(define_insn "*mov<mode>_xxspltiw"
  [(set (match_operand:FP16 0 "gpc_reg_operand" "=wa,wa,?r,?r")
	(match_operand:FP16 1 "fp16_xxspltiw_constant" "j,eP,j,eP"))]
  "TARGET_POWER10 && TARGET_PREFIXED"
{
  rtx op1 = operands[1];
  const REAL_VALUE_TYPE *rtype = CONST_DOUBLE_REAL_VALUE (op1);
  long real_words[1];

  if (op1 == CONST0_RTX (<MODE>mode))
    return (vsx_register_operand (operands[0], <MODE>mode)
	    ? "xxspltib %x0,0"
	    : "li %0,0");

  real_to_target (real_words, rtype, <MODE>mode);
  operands[2] = GEN_INT (real_words[0]);
  return (vsx_register_operand (operands[0], <MODE>mode)
	  ? "xxspltiw %x0,%2"
	  : "pli %0,%2");
}
  [(set_attr "type" "vecsimple,vecsimple,*,*")
   (set_attr "prefixed" "no,yes,no,yes")])

(define_insn "*mov<mode>_internal"
  [(set (match_operand:FP16 0 "nonimmediate_operand"
                    "=wa,       wa,       Z,         r,          r,
                      m,        r,        wa,        wa,         r")

	(match_operand:FP16 1 "any_operand"
                    "wa,        Z,        wa,        r,          m,
                     r,         wa,       r,         j,          j"))]
  "gpc_reg_operand (operands[0], <MODE>mode)
    || gpc_reg_operand (operands[1], <MODE>mode)"
  "@
   xxlor %x0,%x1,%x1
   lxsihzx %x0,%y1
   stxsihx %x1,%y0
   mr %0,%1
   lhz%U1%X1 %0,%1
   sth%U0%X0 %1,%0
   mfvsrwz %0,%x1
   mtvsrwz %x0,%1
   xxspltib %x0,0
   li %0,0"
  [(set_attr "type" "vecsimple, fpload,    fpstore,   *,         load,
                     store,     mtvsr,     mfvsr,     vecsimple, *")])


;; Convert IEEE 16-bit floating point to/from other floating point modes.

(define_insn "extendhf<mode>2"
  [(set (match_operand:SFDF 0 "vsx_register_operand" "=wa")
	(float_extend:SFDF
	 (match_operand:HF 1 "vsx_register_operand" "wa")))]
  "TARGET_FLOAT16_HW"
  "xscvhpdp %x0,%x1"
  [(set_attr "type" "fpsimple")])

(define_insn "trunc<mode>hf2"
  [(set (match_operand:HF 0 "vsx_register_operand" "=wa")
	(float_truncate:HF
	 (match_operand:SFDF 1 "vsx_register_operand" "wa")))]
  "TARGET_FLOAT16_HW"
  "xscvdphp %x0,%x1"
  [(set_attr "type" "fpsimple")])


;; Convert BFmode to SFmode/DFmode.
;; 3 instructions are generated:
;;	VSPLTH		-- duplicate BFmode into all elements
;;	XVCVBF16SPN	-- convert even BFmode elements to SFmode
;;	XSCVSPNDP	-- convert memory format of SFmode to DFmode.
(define_insn_and_split "extendbf<mode>2"
  [(set (match_operand:SFDF 0 "vsx_register_operand" "=wa")
	(float_extend:SFDF
	 (match_operand:BF 1 "vsx_register_operand" "v")))
   (clobber (match_scratch:V8BF 2 "=v"))]
  "TARGET_BFLOAT16_HW"
  "#"
  "&& 1"
  [(pc)]
{
  rtx op0 = operands[0];
  rtx op1 = operands[1];
  rtx op2_v8bf = operands[2];

  if (GET_CODE (op2_v8bf) == SCRATCH)
    op2_v8bf = gen_reg_rtx (V8BFmode);

  rtx op2_v4sf = gen_lowpart (V4SFmode, op2_v8bf);

  /* XXSLDWI -- shift BFmode element into the upper 32 bits.  */
  emit_insn (gen_v8bf_shift_left_32bit (op2_v8bf, op1));

  /* XVCVBF16SPN -- convert even V8BFmode elements to V4SFmode.  */
  emit_insn (gen_xvcvbf16spn_v8bf (op2_v4sf, op2_v8bf));

  /* XSCVSPNDP -- convert single V4SFmode element to DFmode.  */
  emit_insn (GET_MODE (op0) == SFmode
	     ? gen_vsx_xscvspdpn_sf (op0, op2_v4sf)
	     : gen_vsx_xscvspdpn (op0, op2_v4sf));

  DONE;
}
  [(set_attr "type" "fpsimple")
   (set_attr "length" "12")])

;; Vector shift left by 32 bits to get the bfloat16 value into the
;; upper 32 bits for the conversion.
(define_insn "v8bf_shift_left_32bit"
  [(set (match_operand:V8BF 0 "register_operand" "=wa")
        (unspec:V8BF [(match_operand:BF 1 "register_operand" "wa")]
                     UNSPEC_V8BF_SHIFT_LEFT_32BIT))]
  "TARGET_BFLOAT16_HW"
  "xxsldwi %x0,%x1,%x1,1"
  [(set_attr "type" "vecperm")])

;; Convert SFmode/DFmode to BFmode.
;; 2 instructions are generated:
;;	XSCVDPSPN	-- convert SFmode/DFmode scalar to V4SFmode
;;	XVCVSPBF16	-- convert V4SFmode to even V8BFmode

(define_insn_and_split "trunc<mode>bf2"
  [(set (match_operand:BF 0 "vsx_register_operand" "=wa")
	(float_truncate:BF
	 (match_operand:SFDF 1 "vsx_register_operand" "wa")))
   (clobber (match_scratch:V4SF 2 "=wa"))]
  "TARGET_BFLOAT16_HW"
  "#"
  "&& 1"
  [(pc)]
{
  rtx op0 = operands[0];
  rtx op1 = operands[1];
  rtx op2 = operands[2];

  if (GET_CODE (op2) == SCRATCH)
    op2 = gen_reg_rtx (V4SFmode);

  emit_insn (GET_MODE (op1) == SFmode
	     ? gen_vsx_xscvdpspn_sf (op2, op1)
	     : gen_vsx_xscvdpspn (op2, op1));

  emit_insn (gen_xvcvspbf16_bf (op0, op2));
  DONE;
}
  [(set_attr "type" "fpsimple")])

;; Use DFmode to convert to/from 16-bit floating point types for
;; scalar floating point types other than SF/DFmode.
(define_expand "extend<FP16_HW:mode><FP16_CONVERT:mode>2"
  [(set (match_operand:FP16_CONVERT 0 "vsx_register_operand")
	(float_extend:FP16_CONVERT
	 (match_operand:FP16_HW 1 "vsx_register_operand")))]
  ""
{
  rtx df_tmp = gen_reg_rtx (DFmode);
  emit_insn (gen_extend<FP16_HW:mode>df2 (df_tmp, operands[1]));

  /* convert_move handles things like conversion to Decimal types that
     we don't have extenddfdd2 insns, so a call is made to do the
     conversion.  */
  convert_move (operands[0], df_tmp, 0);
  DONE;
})

(define_expand "trunc<FP16_CONVERT:mode><FP16_HW:mode>2"
  [(set (match_operand:FP16_HW 0 "vsx_register_operand")
	(float_truncate:FP16_HW
	 (match_operand:FP16_CONVERT 1 "vsx_register_operand")))]
  ""
{
  rtx df_tmp = gen_reg_rtx (DFmode);

  /* convert_move handles things like conversion from Decimal types
     that we don't have truncdddf2 insns, so a call is made for
     the conversion.  */
  convert_move (df_tmp, operands[1], 0);

  emit_insn (gen_truncdf<FP16_HW:mode>2 (operands[0], df_tmp));
  DONE;
})

;; Convert integers to 16-bit floating point modes.
(define_expand "float<GPR:mode><FP16_HW:mode>2"
  [(set (match_operand:FP16_HW 0 "vsx_register_operand")
	(float:FP16_HW
	 (match_operand:GPR 1 "nonimmediate_operand")))]
  ""
{
  rtx df_tmp = gen_reg_rtx (DFmode);
  emit_insn (gen_float<GPR:mode>df2 (df_tmp, operands[1]));
  emit_insn (gen_truncdf<FP16_HW:mode>2 (operands[0], df_tmp));
  DONE;
})

(define_expand "floatuns<GPR:mode><FP16_HW:mode>2"
  [(set (match_operand:FP16_HW 0 "vsx_register_operand")
	(unsigned_float:FP16_HW
	 (match_operand:GPR 1 "nonimmediate_operand")))]
  ""
{
  rtx df_tmp = gen_reg_rtx (DFmode);
  emit_insn (gen_floatuns<GPR:mode>df2 (df_tmp, operands[1]));
  emit_insn (gen_truncdf<FP16_HW:mode>2 (operands[0], df_tmp));
  DONE;
})

;; Convert 16-bit floating point modes to integers
(define_expand "fix_trunc<FP16_HW:mode><GPR:mode>2"
  [(set (match_operand:GPR 0 "vsx_register_operand")
	(fix:GPR
	 (match_operand:FP16_HW 1 "vsx_register_operand")))]
  ""
{
  rtx df_tmp = gen_reg_rtx (DFmode);
  emit_insn (gen_extend<FP16_HW:mode>df2 (df_tmp, operands[1]));
  emit_insn (gen_fix_truncdf<GPR:mode>2 (operands[0], df_tmp));
  DONE;
})

(define_expand "fixuns_trunc<FP16_HW:mode><GPR:mode>2"
  [(set (match_operand:GPR 0 "vsx_register_operand")
	(unsigned_fix:GPR
	 (match_operand:FP16_HW 1 "vsx_register_operand")))]
  ""
{
  rtx df_tmp = gen_reg_rtx (DFmode);
  emit_insn (gen_extend<FP16_HW:mode>df2 (df_tmp, operands[1]));
  emit_insn (gen_fixuns_truncdf<GPR:mode>2 (operands[0], df_tmp));
  DONE;
})

(define_insn "xvcvbf16spn_v8bf"
  [(set (match_operand:V4SF 0 "vsx_register_operand" "=wa")
	(unspec:V4SF [(match_operand:V8BF 1 "vsx_register_operand" "wa")]
		     UNSPEC_XVCVBF16SPN_V8BF))]
  "TARGET_BFLOAT16"
  "xvcvbf16spn %x0,%x1"
  [(set_attr "type" "vecfloat")])


;; Bfloat16 floating point operations.  We convert the 16-bit scalar to a
;; V4SF vector, do the operation, and then convert the value back to
;; 16-bit format.  We only care about the 2nd element that the scalar
;; value in it.  For plus, minus, and mult the other 3 elements can be
;; 0.  This means we can combine a load (which sets the other bits to
;; 0) with the conversion to vector.  For divide, the divisor must not
;; be 0, so we use a splat operation to guarantee that we are not
;; dividing by 0.

(define_insn_and_split "<BF_OPS_NAME>bf3"
  [(set (match_operand:BF 0 "vsx_register_operand" "=wa,wa,wa")
	(BF_OPS:BF
	 (match_operand:BF 1 "vsx_register_operand" "wa,wa,wa")
	 (match_operand:BF 2 "fp16_reg_or_constant_operand" "wa,j,eP")))
   (clobber (match_scratch:V4SF 3 "=&wa,&wa,&wa"))
   (clobber (match_scratch:V4SF 4 "=&wa,&wa,&wa"))
   (clobber (match_scratch:V4SF 5 "=&wa,&wa,&wa"))]
  "TARGET_BFLOAT16_HW"
  "#"
  "&& 1"
  [(pc)]
{
  rtx op0 = operands[0];
  rtx op1 = operands[1];
  rtx op2 = operands[2];
  rtx tmp0 = operands[3];
  rtx tmp1 = operands[4];
  rtx tmp2 = operands[5];

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
  emit_insn (gen_xxspltw_bf (tmp1, op1));
  emit_insn (gen_xvcvbf16spn_bf (tmp1, tmp1));

  /* Operand2.  */
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

  /* Do the operation in V4SFmode.  */
  emit_insn (gen_<BF_OPS_NAME>v4sf3 (tmp0, tmp1, tmp2));

  /* Convert V4SF result back to scalar mode.  */
  emit_insn (gen_xvcvspbf16_bf (op0, tmp0));
  DONE;
}
  [(set_attr "type" "vecperm")
   (set_attr "length" "24,24,32")])

;; Duplicate a BF value so it can be used for xvcvbf16spn.  Because
;; xvcvbf16spn only uses the even elements, we can use xxspltw instead
;; of vspltw.

(define_insn "xxspltw_bf"
  [(set (match_operand:V4SF 0 "vsx_register_operand" "=wa")
	(unspec:V4SF [(match_operand:BF 1 "vsx_register_operand" "wa")]
		     UNSPEC_XXSPLTW_BF))]
  "TARGET_BFLOAT16_HW"
  "xxspltw %x0,%x1,1"
  [(set_attr "type" "vecperm")])

;; Convert a bfloat16 floating point scalar that has been splatted to
;; V4SFmode.

(define_insn "xvcvbf16spn_bf"
  [(set (match_operand:V4SF 0 "vsx_register_operand" "=wa")
	(unspec:V4SF [(match_operand:V4SF 1 "vsx_register_operand" "wa")]
		     UNSPEC_XVCVBF16SPN_BF))]
  "TARGET_BFLOAT16_HW"
  "xvcvbf16spn %x0,%x1"
  [(set_attr "type" "vecperm")])

;; Convert a V4SFmode vector back to 16-bit floating point scalar.  We
;; only care about the 2nd V4SFmode element, which is the element we
;; converted the 16-bit scalar (4th element) to V4SFmode to do the
;; operation, and converted it back.

(define_insn "xvcvspbf16_bf"
  [(set (match_operand:BF 0 "vsx_register_operand" "=wa")
	(unspec:BF [(match_operand:V4SF 1 "vsx_register_operand" "wa")]
		   UNSPEC_XVCVSPBF16_BF))]
  "TARGET_BFLOAT16_HW"
  "xvcvspbf16 %x0,%x1"
  [(set_attr "type" "vecperm")])


;; Negate 16-bit floating point by XOR with -0.0.  We only do this on
;; power10, since we can easily load up -0.0 via XXSPLTIW.

(define_insn_and_split "neg<mode>2"
  [(set (match_operand:FP16 0 "register_operand" "=wa,wr")
	(neg:FP16 (match_operand:FP16 1 "register_operand" "wa,wr")))
   (clobber (match_scratch:FP16 2 "=&wa,&r"))]
  "TARGET_POWER10 && TARGET_PREFIXED"
  "#"
  "&& 1"
  [(set (match_dup 2)
	(match_dup 3))
   (set (match_dup 0)
	(xor:FP16 (match_dup 1)
		  (match_dup 2)))]
{
  REAL_VALUE_TYPE dconst;

  gcc_assert (real_from_string (&dconst, "-0.0") == 0);

  if (GET_CODE (operands[2]) == SCRATCH)
    operands[2] = gen_reg_rtx (<MODE>mode);

  operands[3] = const_double_from_real_value (dconst, <MODE>mode);
}
  [(set_attr "type" "veclogical,integer")
   (set_attr "length" "16")])

;; XOR used to negate a 16-bit floating point type

(define_insn "xor<mode>3"
  [(set (match_operand:FP16 0 "register_operand" "=wa,wr")
	(xor:FP16 (match_operand:FP16 1 "register_operand" "wa,wr")
		  (match_operand:FP16 2 "register_operand" "wa,wr")))]
  "TARGET_POWER10 && TARGET_PREFIXED"
  "@
   xxlxor %x0,%x1,%x2
   xor %0,%1,%2"
  [(set_attr "type" "veclogical,integer")])


;; If the user used -Ofast, eliminate back to back extend/trunc
;; operations

(define_insn_and_split "*no_extend_trunc_<SFDF:mode>_<FP16_HW:mode>"
  [(set (match_operand:FP16_HW 0 "vsx_register_operand" "=wa")
	(float_truncate:FP16_HW
	 (float_extend:SFDF
	  (match_operand:FP16_HW 1 "vsx_register_operand" "wa"))))]
  "optimize_fast"
  "#"
  "&& 1"
  [(set (match_dup 0) (match_dup 1))])
