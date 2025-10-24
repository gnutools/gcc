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

;; Mode iterator for 16-bit floating point modes both as a scalar and
;; as a vector.
(define_mode_iterator FP16 [(BF "TARGET_BFLOAT16")
			    (HF "TARGET_FLOAT16")])

(define_mode_iterator VFP16 [(V8BF "TARGET_BFLOAT16")
			     (V8HF "TARGET_FLOAT16")])

;; Mode iterator for 16-bit floating point modes on machines with
;; hardware support both as a scalar and as a vector.
(define_mode_iterator FP16_HW [(BF "TARGET_BFLOAT16_HW")
			       (HF "TARGET_FLOAT16_HW")])

(define_mode_iterator VFP16_HW [(V8BF "TARGET_BFLOAT16_HW")
				(V8HF "TARGET_FLOAT16_HW")])

;; Mode iterator for floating point modes other than SF/DFmode that we
;; convert to/from _Float16 (HFmode) via DFmode.
(define_mode_iterator fp16_float_convert [TF KF IF SD DD TD])

;; Mode attribute giving the instruction to convert the even
;; V8HFmode or V8BFmode elements to V4SFmode
(define_mode_attr cvt_fp16_to_v4sf_insn [(BF   "xvcvbf16spn")
					 (HF   "xvcvhpsp")
					 (V8BF "xvcvbf16spn")
					 (V8HF "xvcvhpsp")])

;; Mode attribute giving the vector mode for a 16-bit floating point
;; scalar in both upper and lower case.
(define_mode_attr FP16_VECTOR8 [(BF "V8BF")
				(HF "V8HF")])

(define_mode_attr fp16_vector8 [(BF "v8bf")
				(HF "v8hf")])

;; Mode attribute giving the vector mode with 4 16-bit floating point
;; elements given a scalar or 8 element vector.
(define_mode_attr FP16_VECTOR4 [(BF   "V4BF")
				(HF   "V4HF")
				(V8BF "V4BF")
				(V8HF "V4HF")])

;; Binary operators for bfloat16/float16 vectorization.
(define_code_iterator FP16_BINARY_OP [plus minus mult smax smin])

;; Standard names for the unary/binary/ternary operators
(define_code_attr fp16_names [(abs   "abs")
			      (fma   "fma")
			      (plus  "add")
			      (minus "sub")
			      (mult  "mul")
			      (neg   "neg")
			      (smax  "smax")
			      (smin  "smin")])

;; UNSPEC constants
(define_c_enum "unspec"
  [UNSPEC_VSLD_BF
   UNSPEC_VSRD_BF
   UNSPEC_CVT_FP16_TO_V4SF
   UNSPEC_XXSPLTW_FP16
   UNSPEC_XVCVSPBF16_BF
   UNSPEC_XVCVSPHP_V8HF
   UNSPEC_XVCVSPBF16_V8BF])

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
  "TARGET_PREFIXED || operands[1] == CONST0_RTX (<MODE>mode)"
{
  rtx op1 = operands[1];
  const REAL_VALUE_TYPE *rtype = CONST_DOUBLE_REAL_VALUE (op1);
  long real_words[1];

  if (op1 == CONST0_RTX (<MODE>mode))
    return (!vsx_register_operand (operands[0], <MODE>mode)
	    ? "li %0,0"
	    : "xxlxor %x0,%x0,%x0");

  real_to_target (real_words, rtype, <MODE>mode);
  operands[2] = GEN_INT (real_words[0]);
  return (vsx_register_operand (operands[0], <MODE>mode)
	  ? "xxspltiw %x0,%2"
	  : "pli %0,%2");
}
  [(set_attr "type"     "veclogical, vecsimple, *,  *")
   (set_attr "prefixed" "no,         yes,       no, yes")])

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
   xxlxor %x0,%x0,%x0
   li %0,0"
  [(set_attr "type" "vecsimple, fpload,    fpstore,   *,          load,
                     store,     mtvsr,     mfvsr,     veclogical, *")
   (set_attr "isa"  "*,         p9v,       p9v,       *,          *,
                     *,         p8v,       p8v,       p9v,        *")])

;; Vector duplicate
(define_insn "*vecdup<mode>_reg"
  [(set (match_operand:<FP16_VECTOR8> 0 "altivec_register_operand" "=v")
	(vec_duplicate:<FP16_VECTOR8>
	 (match_operand:FP16 1 "altivec_register_operand" "v")))]
  ""
  "vsplth %0,%1,3"
  [(set_attr "type" "vecperm")])

(define_insn "*vecdup<mode>_const"
  [(set (match_operand:<FP16_VECTOR8> 0 "vsx_register_operand" "=wa,wa")
	(vec_duplicate:<FP16_VECTOR8>
	 (match_operand:FP16 1 "fp16_xxspltiw_constant" "j,eP")))]
  "TARGET_PREFIXED || operands[1] == CONST0_RTX (<MODE>mode)"
{
  rtx op1 = operands[1];
  if (op1 == CONST0_RTX (<MODE>mode))
    return "xxlxor %x0,%x0,%x0";

  const REAL_VALUE_TYPE *rtype = CONST_DOUBLE_REAL_VALUE (op1);
  long real_words[1];

  real_to_target (real_words, rtype, <MODE>mode);
  operands[2] = GEN_INT (real_words[0]);
  return "xxspltiw %x0,2";
}
  [(set_attr "type" "veclogical,vecperm")
   (set_attr "prefixed" "*,yes")])

;; Add logical operations for 16-bit floating point types that are
;; used for things like negate, abs, and extracting exponents.
(define_expand "and<mode>3"
  [(set (match_operand:FP16 0 "gpc_reg_operand")
	(and:FP16 (match_operand:FP16 1 "gpc_reg_operand")
		  (match_operand:FP16 2 "gpc_reg_operand")))]
  ""
  "")

(define_expand "ior<mode>3"
  [(set (match_operand:FP16 0 "gpc_reg_operand")
        (ior:FP16 (match_operand:FP16 1 "gpc_reg_operand")
		  (match_operand:FP16 2 "gpc_reg_operand")))]
  ""
  "")

(define_expand "xor<mode>3"
  [(set (match_operand:FP16 0 "gpc_reg_operand")
        (xor:FP16 (match_operand:FP16 1 "gpc_reg_operand")
		  (match_operand:FP16 2 "gpc_reg_operand")))]
  ""
  "")

(define_expand "nor<mode>3"
  [(set (match_operand:FP16 0 "gpc_reg_operand")
	(and:FP16
	 (not:FP16 (match_operand:FP16 1 "gpc_reg_operand"))
	 (not:FP16 (match_operand:FP16 2 "gpc_reg_operand"))))]
  ""
  "")

(define_expand "andn<mode>3"
  [(set (match_operand:FP16 0 "gpc_reg_operand")
        (and:FP16
	 (not:FP16 (match_operand:FP16 2 "gpc_reg_operand"))
	 (match_operand:FP16 1 "gpc_reg_operand")))]
  ""
  "")

(define_expand "eqv<mode>3"
  [(set (match_operand:FP16 0 "gpc_reg_operand")
	(not:FP16
	 (xor:FP16 (match_operand:FP16 1 "gpc_reg_operand")
		   (match_operand:FP16 2 "gpc_reg_operand"))))]
  ""
  "")

;; Rewrite nand into canonical form
(define_expand "nand<mode>3"
  [(set (match_operand:FP16 0 "gpc_reg_operand")
	(ior:FP16
	 (not:FP16 (match_operand:FP16 1 "gpc_reg_operand"))
	 (not:FP16 (match_operand:FP16 2 "gpc_reg_operand"))))]
  ""
  "")

;; The canonical form is to have the negated element first, so we need to
;; reverse arguments.
(define_expand "iorn<mode>3"
  [(set (match_operand:FP16 0 "gpc_reg_operand")
	(ior:FP16
	 (not:FP16 (match_operand:FP16 2 "gpc_reg_operand"))
	 (match_operand:FP16 1 "gpc_reg_operand")))]
  ""
  "")

(define_insn "*bool<mode>3"
  [(set (match_operand:FP16 0 "gpc_reg_operand" "=wa,r")
	(match_operator:FP16 3 "boolean_operator"
	 [(match_operand:FP16 1 "gpc_reg_operand" "wa,r")
	  (match_operand:FP16 2 "gpc_reg_operand" "wa,r")]))]
  ""
  "@
   xxl%q3 %x0,%x1,%x2
   %q3 %0,%1,%2"
  [(set_attr "type" "veclogical,logical")])

(define_insn "*boolc<mode>3"
  [(set (match_operand:GPR 0 "gpc_reg_operand" "=wa,r")
	(match_operator:GPR 3 "boolean_operator"
	 [(not:GPR (match_operand:GPR 2 "gpc_reg_operand" "wa,r"))
	  (match_operand:GPR 1 "gpc_reg_operand" "wa,r")]))]
  ""
  "@
   xxl%q3 %x0,%x1,%x0
   %q3 %0,%1,%2"
  [(set_attr "type" "veclogical,logical")])

(define_insn "*boolcc<mode>3"
  [(set (match_operand:GPR 0 "gpc_reg_operand" "=wa,r")
	(match_operator:GPR 3 "boolean_operator"
	 [(not:GPR (match_operand:GPR 1 "gpc_reg_operand" "wa,r"))
	  (not:GPR (match_operand:GPR 2 "gpc_reg_operand" "wa,r"))]))]
  ""
  "@
   xxl%q3 %x0,%x1,%x2
   %q3 %0,%1,%2"
  [(set_attr "type" "veclogical,logical")])


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
;;	PLXSD		-- load up shift amount
;;	VSLD		-- shift BF left 48 bits
;;	XSCVSPNDP	-- convert memory format of SFmode to DFmode.
(define_insn_and_split "extendbf<mode>2"
  [(set (match_operand:SFDF 0 "vsx_register_operand" "=wa")
	(float_extend:SFDF
	 (match_operand:BF 1 "vsx_register_operand" "v")))
   (clobber (match_scratch:DI 2 "=&v"))
   (clobber (match_scratch:DI 3 "=&v"))]
  "TARGET_BFLOAT16_HW"
  "#"
  "&& 1"
  [(pc)]
{
  rtx op0 = operands[0];
  rtx op1 = operands[1];
  rtx op2 = operands[2];
  rtx op3 = operands[3];

  if (GET_CODE (op2) == SCRATCH)
    op2 = gen_reg_rtx (DImode);

  if (GET_CODE (op3) == SCRATCH)
    op3 = gen_reg_rtx (DImode);

  /* Load up shift amount.  */
  emit_move_insn (op2, GEN_INT (48));

  /* Shift BFmode into the upper 16 bits.  */
  emit_insn (gen_shift_left_bf (op3, op1, op2));

  /* XSCVSPNDP -- convert single V4SFmode element to DFmode.  */
  emit_insn (GET_MODE (op0) == SFmode
	     ? gen_xscvspdpn_sf_bf (op0, op3)
	     : gen_xscvspdpn_df_bf (op0, op3));

  DONE;
}
  [(set_attr "type" "fpsimple")
   (set_attr "length" "12")])

;; Shift BFmode left
(define_insn "shift_left_bf"
  [(set (match_operand:DI 0 "gpc_reg_operand" "=v,?r")
	(unspec:DI [(match_operand:BF 1 "gpc_reg_operand" "v,r")
		    (match_operand:DI 2 "reg_or_cint_operand" "v,rn")]
		   UNSPEC_VSLD_BF))]
  "TARGET_BFLOAT16"
  "@
   vsld %0,%1,%2
   sld%I2 %0,%1,%H2"
  [(set_attr "type" "vecsimple,shift")
   (set_attr "maybe_var_shift" "*,yes")])

;; Convert element 0 of a V4SFmode to scalar SFmode (which on the
;; PowerPC uses the DFmode encoding).
(define_insn "xscvspdpn_<mode>_bf"
  [(set (match_operand:SFDF 0 "vsx_register_operand" "=wa")
	(unspec:SFDF [(match_operand:DI 1 "vsx_register_operand" "wa")]
		     UNSPEC_VSX_CVSPDPN))]
  "TARGET_BFLOAT16"
  "xscvspdpn %x0,%x1"
  [(set_attr "type" "fp")])

;; Convert BFmode to SFmode/DFmode.
;; 3 instructions are generated:
;;	PLXSD		-- load up shift amount
;;	XSCVDPSPN	-- convert DFmode to the memory format of SFmode
;;	VSRD		-- shift BF right 48 bits
(define_insn_and_split "trunc<mode>bf2"
  [(set (match_operand:BF 0 "vsx_register_operand" "=wa")
	(float_truncate:BF
	 (match_operand:SFDF 1 "vsx_register_operand" "wa")))
   (clobber (match_scratch:DI 2 "=&v"))
   (clobber (match_scratch:DI 3 "=&v"))]
  "TARGET_BFLOAT16_HW"
  "#"
  "&& 1"
  [(pc)]
{
  rtx op0 = operands[0];
  rtx op1 = operands[1];
  rtx op2 = operands[2];
  rtx op3 = operands[3];

  if (GET_CODE (op2) == SCRATCH)
    op2 = gen_reg_rtx (DImode);

  if (GET_CODE (op3) == SCRATCH)
    op3 = gen_reg_rtx (DImode);

  /* Load up shift amount.  */
  emit_move_insn (op2, GEN_INT (48));

  /* XSCVSPNDP -- convert DFmode to single V4SFmode element.  */
  emit_insn (GET_MODE (op0) == SFmode
	     ? gen_xscvdpspn_bf_sf (op3, op1)
	     : gen_xscvdpspn_bf_df (op3, op1));


  /* Shift BFmode into the lower 16 bits.  */
  emit_insn (gen_shift_right_bf (op0, op3, op2));
  DONE;
}
  [(set_attr "type" "fpsimple")
   (set_attr "length" "12")])

;; Shift BFmode right
(define_insn "shift_right_bf"
  [(set (match_operand:BF 0 "gpc_reg_operand" "=v,?r")
	(unspec:BF [(match_operand:DI 1 "gpc_reg_operand" "v,r")
		    (match_operand:DI 2 "reg_or_cint_operand" "v,rn")]
		   UNSPEC_VSRD_BF))]
  "TARGET_BFLOAT16"
  "@
   vsrd %0,%1,%2
   srd%I2 %0,%1,%H2"
  [(set_attr "type" "vecsimple,shift")
   (set_attr "maybe_var_shift" "*,yes")])

;; Convert a SFmode scalar represented as DFmode to elements 0 and 1 of
;; V4SFmode.
(define_insn "xscvdpspn_bf_<mode>"
  [(set (match_operand:DI 0 "vsx_register_operand" "=wa")
	(unspec:DI [(match_operand:SFDF 1 "vsx_register_operand" "wa")]
		   UNSPEC_VSX_CVSPDP))]
  "TARGET_BFLOAT16"
  "xscvdpspn %x0,%x1"
  [(set_attr "type" "fp")])


;; Convert the even elements of a vector 16-bit floating point to
;; V4SFmode.  Deal with little endian vs. big endian element ordering
;; in identifying which elements are converted.

(define_expand "cvt_fp16_to_v4sf_<mode>"
  [(set (match_operand:V4SF 0 "vsx_register_operand")
	(float_extend:V4SF
	 (vec_select:<FP16_VECTOR4>
	  (match_operand:VFP16_HW 1 "vsx_register_operand")
	  (parallel [(match_dup 2)
		     (match_dup 3)
		     (match_dup 4)
		     (match_dup 5)]))))]
  ""
{
  int endian_adjust = WORDS_BIG_ENDIAN ? 0 : 1;
  operands[2] = GEN_INT (0 + endian_adjust);
  operands[3] = GEN_INT (2 + endian_adjust);
  operands[4] = GEN_INT (4 + endian_adjust);
  operands[5] = GEN_INT (6 + endian_adjust);
})

(define_insn "*cvt_fp16_to_v4sf_<mode>_le"
  [(set (match_operand:V4SF 0 "vsx_register_operand")
	(float_extend:V4SF
	 (vec_select:<FP16_VECTOR4>
	  (match_operand:VFP16_HW 1 "vsx_register_operand")
	  (parallel [(const_int 1)
		     (const_int 3)
		     (const_int 5)
		     (const_int 7)]))))]
  "!WORDS_BIG_ENDIAN"
  "<cvt_fp16_to_v4sf_insn> %x0,%x1"
  [(set_attr "type" "vecfloat")])

(define_insn "*cvt_fp16_to_v4sf_<mode>_be"
  [(set (match_operand:V4SF 0 "vsx_register_operand")
	(float_extend:V4SF
	 (vec_select:<FP16_VECTOR4>
	  (match_operand:VFP16_HW 1 "vsx_register_operand")
	  (parallel [(const_int 0)
		     (const_int 2)
		     (const_int 4)
		     (const_int 6)]))))]
  "WORDS_BIG_ENDIAN"
  "<cvt_fp16_to_v4sf_insn> %x0,%x1"
  [(set_attr "type" "vecfloat")])

;; Duplicate and convert a 16-bit floating point scalar to V4SFmode.

(define_insn_and_split "*dup_<mode>_to_v4sf"
  [(set (match_operand:V4SF 0 "vsx_register_operand" "=wa")
	(vec_duplicate:V4SF
	 (float_extend:SF
	  (match_operand:FP16_HW 1 "vsx_register_operand" "wa"))))]
  ""
  "#"
  "&& 1"
  [(pc)]
{
  rtx op0 = operands[0];
  rtx op1 = operands[1];
  rtx op0_vfp16 = gen_lowpart (<FP16_VECTOR8>mode, op0);

  emit_insn (gen_xxspltw_<mode> (op0, op1));
  emit_insn (gen_cvt_fp16_to_v4sf_<fp16_vector8> (op0, op0_vfp16));
  DONE;
}
  [(set_attr "length" "8")
   (set_attr "type" "vecperm")])

;; Duplicate a HF/BF value so it can be used for xvcvhpspn/xvcvbf16spn.
;; Because xvcvhpspn/xvcvbf16spn only uses the even elements, we can
;; use xxspltw instead of vspltw.  This has the advantage that the
;; register allocator can use any of the 64 VSX registers instead of
;; being limited to the 32 Altivec registers that VSPLTH would require.

(define_insn "xxspltw_<mode>"
  [(set (match_operand:V4SF 0 "vsx_register_operand" "=wa")
	(unspec:V4SF [(match_operand:FP16_HW 1 "vsx_register_operand" "wa")]
		     UNSPEC_XXSPLTW_FP16))]
  ""
  "xxspltw %x0,%x1,1"
  [(set_attr "type" "vecperm")])

;; Convert a bfloat16 floating point scalar that has been splatted to
;; V4SFmode.

(define_insn "xvcvbf16spn_bf"
  [(set (match_operand:V4SF 0 "vsx_register_operand" "=wa")
	(unspec:V4SF [(match_operand:V4SF 1 "vsx_register_operand" "wa")]
		     UNSPEC_CVT_FP16_TO_V4SF))]
  "TARGET_BFLOAT16_HW"
  "xvcvbf16spn %x0,%x1"
  [(set_attr "type" "vecperm")])

;; Convert a V4SFmode vector to a 16-bit floating point scalar.  We
;; only care about the 2nd V4SFmode element, which is the element we
;; converted the 16-bit scalar (4th element) to V4SFmode to do the
;; operation, and converted it back.

(define_insn "xvcvspbf16_bf"
  [(set (match_operand:BF 0 "vsx_register_operand" "=wa")
	(unspec:BF [(match_operand:V4SF 1 "vsx_register_operand" "wa")]
		   UNSPEC_XVCVSPBF16_BF))]
  "TARGET_BFLOAT16_HW"
  "xvcvspbf16 %x0,%x1"
  [(set_attr "type" "vecfloat")])

;; Convert between HFmode/BFmode and 128-bit binary floating point and
;; decimal floating point types.  We use convert_move since some of the
;; types might not have valid RTX expanders.  We use DFmode as the
;; intermediate conversion destination.

(define_expand "extend<FP16_HW:mode><fp16_float_convert:mode>2"
  [(set (match_operand:fp16_float_convert 0 "vsx_register_operand")
	(float_extend:fp16_float_convert
	 (match_operand:FP16_HW 1 "vsx_register_operand")))]
  ""
{
  rtx df_tmp = gen_reg_rtx (DFmode);
  emit_insn (gen_extend<FP16_HW:mode>df2 (df_tmp, operands[1]));
  convert_move (operands[0], df_tmp, 0);
  DONE;
})

(define_expand "trunc<fp16_float_convert:mode><FP16_HW:mode>2"
  [(set (match_operand:FP16_HW 0 "vsx_register_operand")
	(float_truncate:FP16_HW
	 (match_operand:fp16_float_convert 1 "vsx_register_operand")))]
  ""
{
  rtx df_tmp = gen_reg_rtx (DFmode);

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

;; Optimize __bfloat16 binary operations.  Unlike _Float16 where we
;; have instructions to convert between HFmode and SFmode as scalar
;; values, with BFmode, we only have vector conversions.  Thus to do:
;;
;;      __bfloat16 a, b, c;
;;      a = b + c;
;;
;; the GCC compiler would normally generate:
;;
;;      lxsihzx 0,4,2           // load __bfloat16 value b
;;      lxsihzx 12,5,2          // load __bfloat16 value c
;;      xxsldwi 0,0,0,1         // shift b into bits 16..31
;;      xxsldwi 12,12,12,1      // shift c into bits 16..31
;;      xvcvbf16spn 0,0         // vector convert b into V4SFmode
;;      xvcvbf16spn 12,12       // vector convert c into V4SFmode
;;      xscvspdpn 0,0           // convert b into SFmode scalar
;;      xscvspdpn 12,12         // convert c into SFmode scalar
;;      fadds 0,0,12            // add b+c
;;      xscvdpspn 0,0           // convert b+c into SFmode memory format
;;      xvcvspbf16 0,0          // convert b+c into BFmode memory format
;;      stxsihx 0,3,2           // store b+c
;;
;; Using the following combiner patterns, the code generated would now
;; be:
;;
;;      lxsihzx 12,4,2          // load __bfloat16 value b
;;      lxsihzx 0,5,2           // load __bfloat16 value c
;;      xxspltw 12,12,1         // shift b into bits 16..31
;;      xxspltw 0,0,1           // shift c into bits 16..31
;;      xvcvbf16spn 12,12       // vector convert b into V4SFmode
;;      xvcvbf16spn 0,0         // vector convert c into V4SFmode
;;      xvaddsp 0,0,12          // vector b+c in V4SFmode
;;      xvcvspbf16 0,0          // convert b+c into BFmode memory format
;;      stxsihx 0,3,2           // store b+c
;;
;; We cannot just define insns like 'addbf3' to keep the operation as
;; BFmode because GCC will not generate these patterns unless the user
;; uses -Ofast.  Without -Ofast, it will always convert BFmode into
;; SFmode.

(define_insn_and_split "*bfloat16_binary_op_internal1"
  [(set (match_operand:SF 0 "vsx_register_operand")
	(match_operator:SF 1 "fp16_binary_operator"
			   [(match_operand:SF 2 "bfloat16_v4sf_operand")
			    (match_operand:SF 3 "bfloat16_v4sf_operand")]))]
  "TARGET_BFLOAT16_HW && TARGET_BFLOAT16_COMBINE && can_create_pseudo_p ()
   && (bfloat16_bf_operand (operands[2], SFmode)
       || bfloat16_bf_operand (operands[3], SFmode))"
  "#"
  "&& 1"
  [(pc)]
{
  bfloat16_operation_as_v4sf (GET_CODE (operands[1]), operands[0], operands[2],
			      operands[3], NULL_RTX, FP16_BINARY);
  DONE;
})

(define_insn_and_split "*bfloat16_binary_op_internal2"
  [(set (match_operand:BF 0 "vsx_register_operand")
	(float_truncate:BF
	 (match_operator:SF 1 "fp16_binary_operator"
			    [(match_operand:SF 2 "bfloat16_v4sf_operand")
			     (match_operand:SF 3 "bfloat16_v4sf_operand")])))]
  "TARGET_BFLOAT16_HW && TARGET_BFLOAT16_COMBINE && can_create_pseudo_p ()
   && (bfloat16_bf_operand (operands[2], SFmode)
       || bfloat16_bf_operand (operands[3], SFmode))"
  "#"
  "&& 1"
  [(pc)]
{
  bfloat16_operation_as_v4sf (GET_CODE (operands[1]), operands[0], operands[2],
			      operands[3], NULL_RTX, FP16_BINARY);
  DONE;
})

(define_insn_and_split "*bfloat16_fma_internal1"
  [(set (match_operand:SF 0 "vsx_register_operand")
	(fma:SF
	 (match_operand:SF 1 "bfloat16_v4sf_operand")
	 (match_operand:SF 2 "bfloat16_v4sf_operand")
	 (match_operand:SF 3 "bfloat16_v4sf_operand")))]
  "TARGET_BFLOAT16_HW && TARGET_BFLOAT16_COMBINE && can_create_pseudo_p ()
   && (bfloat16_bf_operand (operands[1], SFmode)
       + bfloat16_bf_operand (operands[2], SFmode)
       + bfloat16_bf_operand (operands[3], SFmode) >= 2)"
  "#"
  "&& 1"
  [(pc)]
{
  bfloat16_operation_as_v4sf (FMA, operands[0], operands[1], operands[2],
			      operands[3], FP16_FMA);
  DONE;
})

(define_insn_and_split "*bfloat16_fma_internal2"
  [(set (match_operand:BF 0 "vsx_register_operand" "=wa")
	(float_truncate:BF
	 (fma:SF
	  (match_operand:SF 1 "bfloat16_v4sf_operand")
	  (match_operand:SF 2 "bfloat16_v4sf_operand")
	  (match_operand:SF 3 "bfloat16_v4sf_operand"))))]
  "TARGET_BFLOAT16_HW && TARGET_BFLOAT16_COMBINE && can_create_pseudo_p ()
   && (bfloat16_bf_operand (operands[1], SFmode)
       + bfloat16_bf_operand (operands[2], SFmode)
       + bfloat16_bf_operand (operands[3], SFmode) >= 2)"
  "#"
  "&& 1"
  [(pc)]
{
  bfloat16_operation_as_v4sf (FMA, operands[0], operands[1], operands[2],
			      operands[3], FP16_FMA);
  DONE;
})

(define_insn_and_split "*bfloat16_fms_internal1"
  [(set (match_operand:SF 0 "vsx_register_operand")
	(fma:SF
	 (match_operand:SF 1 "bfloat16_v4sf_operand")
	 (match_operand:SF 2 "bfloat16_v4sf_operand")
	 (neg:SF
	  (match_operand:SF 3 "bfloat16_v4sf_operand"))))]
  "TARGET_BFLOAT16_HW && TARGET_BFLOAT16_COMBINE && can_create_pseudo_p ()
   && (bfloat16_bf_operand (operands[1], SFmode)
       + bfloat16_bf_operand (operands[2], SFmode)
       + bfloat16_bf_operand (operands[3], SFmode) >= 2)"
  "#"
  "&& 1"
  [(pc)]
{
  bfloat16_operation_as_v4sf (FMA, operands[0], operands[1], operands[2],
			      operands[3], FP16_FMS);
  DONE;
})

(define_insn_and_split "*bfloat16_fms_interna2"
  [(set (match_operand:BF 0 "vsx_register_operand")
	(float_truncate:BF
	 (fma:SF
	  (match_operand:SF 1 "bfloat16_v4sf_operand")
	  (match_operand:SF 2 "bfloat16_v4sf_operand")
	  (neg:SF
	   (match_operand:SF 3 "bfloat16_v4sf_operand")))))]
  "TARGET_BFLOAT16_HW && TARGET_BFLOAT16_COMBINE && can_create_pseudo_p ()
   && (bfloat16_bf_operand (operands[1], SFmode)
       + bfloat16_bf_operand (operands[2], SFmode)
       + bfloat16_bf_operand (operands[3], SFmode) >= 2)"
  "#"
  "&& 1"
  [(pc)]
{
  bfloat16_operation_as_v4sf (FMA, operands[0], operands[1], operands[2],
			      operands[3], FP16_FMS);
  DONE;
})

(define_insn_and_split "*bfloat16_nfma_internal1"
  [(set (match_operand:SF 0 "vsx_register_operand")
	(neg:SF
	 (fma:SF
	  (match_operand:SF 1 "bfloat16_v4sf_operand")
	  (match_operand:SF 2 "bfloat16_v4sf_operand")
	  (match_operand:SF 3 "bfloat16_v4sf_operand"))))]
  "TARGET_BFLOAT16_HW && TARGET_BFLOAT16_COMBINE && can_create_pseudo_p ()
   && (bfloat16_bf_operand (operands[1], SFmode)
       + bfloat16_bf_operand (operands[2], SFmode)
       + bfloat16_bf_operand (operands[3], SFmode) >= 2)"
  "#"
  "&& 1"
  [(pc)]
{
  bfloat16_operation_as_v4sf (FMA, operands[0], operands[1], operands[2],
			      operands[3], FP16_NFMA);
  DONE;
})

(define_insn_and_split "*bfloat16_nfma_internal2"
  [(set (match_operand:BF 0 "vsx_register_operand" "=wa")
	(float_truncate:BF
	 (neg:SF
	  (fma:SF
	   (match_operand:SF 1 "bfloat16_v4sf_operand")
	   (match_operand:SF 2 "bfloat16_v4sf_operand")
	   (match_operand:SF 3 "bfloat16_v4sf_operand")))))]
  "TARGET_BFLOAT16_HW && TARGET_BFLOAT16_COMBINE && can_create_pseudo_p ()
   && (bfloat16_bf_operand (operands[1], SFmode)
       + bfloat16_bf_operand (operands[2], SFmode)
       + bfloat16_bf_operand (operands[3], SFmode) >= 2)"
  "#"
  "&& 1"
  [(pc)]
{
  bfloat16_operation_as_v4sf (FMA, operands[0], operands[1], operands[2],
			      operands[3], FP16_NFMA);
  DONE;
})

(define_insn_and_split "*bfloat16_nfma_internal3"
  [(set (match_operand:BF 0 "vsx_register_operand" "=wa")
	(neg:BF
	 (float_truncate:BF
	  (fma:SF
	   (match_operand:SF 1 "bfloat16_v4sf_operand")
	   (match_operand:SF 2 "bfloat16_v4sf_operand")
	   (match_operand:SF 3 "bfloat16_v4sf_operand")))))]
  "TARGET_BFLOAT16_HW && TARGET_BFLOAT16_COMBINE && can_create_pseudo_p ()
   && (bfloat16_bf_operand (operands[1], SFmode)
       + bfloat16_bf_operand (operands[2], SFmode)
       + bfloat16_bf_operand (operands[3], SFmode) >= 2)"
  "#"
  "&& 1"
  [(pc)]
{
  bfloat16_operation_as_v4sf (FMA, operands[0], operands[1], operands[2],
			      operands[3], FP16_NFMA);
  DONE;
})

(define_insn_and_split "*bfloat16_nfms_internal1"
  [(set (match_operand:SF 0 "vsx_register_operand")
	(neg:SF
	 (fma:SF
	  (match_operand:SF 1 "bfloat16_v4sf_operand")
	  (match_operand:SF 2 "bfloat16_v4sf_operand")
	  (neg:SF
	   (match_operand:SF 3 "bfloat16_v4sf_operand")))))]
  "TARGET_BFLOAT16_HW && TARGET_BFLOAT16_COMBINE && can_create_pseudo_p ()
   && (bfloat16_bf_operand (operands[1], SFmode)
       + bfloat16_bf_operand (operands[2], SFmode)
       + bfloat16_bf_operand (operands[3], SFmode) >= 2)"
  "#"
  "&& 1"
  [(pc)]
{
  bfloat16_operation_as_v4sf (FMA, operands[0], operands[1], operands[2],
			      operands[3], FP16_NFMS);
  DONE;
})

(define_insn_and_split "*bfloat16_nfms_internal2"
  [(set (match_operand:BF 0 "vsx_register_operand")
	(float_truncate:BF
	 (neg:SF
	  (fma:SF
	   (match_operand:SF 1 "bfloat16_v4sf_operand")
	   (match_operand:SF 2 "bfloat16_v4sf_operand")
	   (neg:SF
	    (match_operand:SF 3 "bfloat16_v4sf_operand"))))))]
  "TARGET_BFLOAT16_HW && TARGET_BFLOAT16_COMBINE && can_create_pseudo_p ()
   && (bfloat16_bf_operand (operands[1], SFmode)
       + bfloat16_bf_operand (operands[2], SFmode)
       + bfloat16_bf_operand (operands[3], SFmode) >= 2)"
  "#"
  "&& 1"
  [(pc)]
{
  bfloat16_operation_as_v4sf (FMA, operands[0], operands[1], operands[2],
			      operands[3], FP16_NFMS);
  DONE;
})

(define_insn_and_split "*bfloat16_nfms_internal3"
  [(set (match_operand:BF 0 "vsx_register_operand")
	(neg:BF
	 (float_truncate:BF
	  (fma:SF
	   (match_operand:SF 1 "bfloat16_v4sf_operand")
	   (match_operand:SF 2 "bfloat16_v4sf_operand")
	   (neg:SF
	    (match_operand:SF 3 "bfloat16_v4sf_operand"))))))]
  "TARGET_BFLOAT16_HW && TARGET_BFLOAT16_COMBINE && can_create_pseudo_p ()
   && (bfloat16_bf_operand (operands[1], SFmode)
       + bfloat16_bf_operand (operands[2], SFmode)
       + bfloat16_bf_operand (operands[3], SFmode) >= 2)"
  "#"
  "&& 1"
  [(pc)]
{
  bfloat16_operation_as_v4sf (FMA, operands[0], operands[1], operands[2],
			      operands[3], FP16_NFMS);
  DONE;
})

;; Add vectorization support for 16-bit floating point.

;; Binary operators being vectorized.
(define_insn_and_split "<fp16_names><mode>3"
  [(set (match_operand:VFP16_HW 0 "vsx_register_operand")
	(FP16_BINARY_OP:VFP16_HW
	 (match_operand:VFP16_HW 1 "vsx_register_operand")
	 (match_operand:VFP16_HW 2 "vsx_register_operand")))]
  "can_create_pseudo_p ()"
  "#"
  "&& 1"
  [(pc)]
{
  fp16_vectorization (<CODE>, operands[0], operands[1], operands[2], NULL_RTX,
		      FP16_BINARY);
  DONE;
})

;; FMA operations being vectorized.
(define_insn_and_split "fma<mode>4"
  [(set (match_operand:VFP16_HW 0 "vsx_register_operand")
	(fma:VFP16_HW
	 (match_operand:VFP16_HW 1 "vsx_register_operand")
	 (match_operand:VFP16_HW 2 "vsx_register_operand")
	 (match_operand:VFP16_HW 3 "vsx_register_operand")))]
  "can_create_pseudo_p ()"
  "#"
  "&& 1"
  [(pc)]
{
  fp16_vectorization (FMA, operands[0], operands[1], operands[2],
		      operands[3], FP16_FMA);
  DONE;
})

(define_insn_and_split "*fms<mode>4"
  [(set (match_operand:VFP16_HW 0 "vsx_register_operand")
	(fma:VFP16_HW
	 (match_operand:VFP16_HW 1 "vsx_register_operand")
	 (match_operand:VFP16_HW 2 "vsx_register_operand")
	 (neg:VFP16_HW
	  (match_operand:VFP16_HW 3 "vsx_register_operand"))))]
  "can_create_pseudo_p ()"
  "#"
  "&& 1"
  [(pc)]
{
  fp16_vectorization (FMA, operands[0], operands[1], operands[2],
		      operands[3], FP16_FMS);
  DONE;
})

(define_insn_and_split "*nfma<mode>4"
  [(set (match_operand:VFP16_HW 0 "vsx_register_operand")
	(neg:VFP16_HW
	 (fma:VFP16_HW
	  (match_operand:VFP16_HW 1 "vsx_register_operand")
	  (match_operand:VFP16_HW 2 "vsx_register_operand")
	  (match_operand:VFP16_HW 3 "vsx_register_operand"))))]
  "can_create_pseudo_p ()"
  "#"
  "&& 1"
  [(pc)]
{
  fp16_vectorization (FMA, operands[0], operands[1], operands[2],
		      operands[3], FP16_NFMA);
  DONE;
})

(define_insn_and_split "*nfms<mode>4"
  [(set (match_operand:VFP16_HW 0 "vsx_register_operand")
	(neg:VFP16_HW
	 (fma:VFP16_HW
	  (match_operand:VFP16_HW 1 "vsx_register_operand")
	  (match_operand:VFP16_HW 2 "vsx_register_operand")
	  (neg:VFP16_HW
	   (match_operand:VFP16_HW 3 "vsx_register_operand")))))]
  "can_create_pseudo_p ()"
  "#"
  "&& 1"
  [(pc)]
{
  fp16_vectorization (FMA, operands[0], operands[1], operands[2],
		      operands[3], FP16_NFMS);
  DONE;
})

;; If we do multiple __bfloat16 operations, between the first and
;; second operation, GCC will want to convert the first operation from
;; V4SFmode to SFmode and then reconvert it back to V4SFmode.  On the
;; PowerPC, this is complicated because internally in the vector
;; register, SFmode values are stored as DFmode values.
;;
;; For example, if we have:
;;
;;	__bfloat16 a, b, c, d;
;;	a = b + c + d;
;;
;; We would generate:
;;
;;      lxsihzx 0,4,2           // load b as BFmode
;;      lxsihzx 11,5,2          // load c as BFmode
;;      lxsihzx 12,6,2          // load d as BFmode
;;      xxspltw 0,0,1           // shift b into bits 16..31
;;      xxspltw 11,11,1         // shift c into bits 16..31
;;      xxspltw 12,12,1         // shift d into bits 16..31
;;      xvcvbf16spn 0,0         // convert b into V4SFmode
;;      xvcvbf16spn 11,11       // convert c into V4SFmode
;;      xvcvbf16spn 12,12       // convert d into V4SFmode
;;      xvaddsp 0,0,11          // calculate b+c as V4SFmode
;;      xscvspdp 0,0            // convert b+c into DFmode memory format
;;      xscvdpspn 0,0           // convert b+c into SFmode memory format
;;      xxspltw 0,0,0           // convert b+c into V4SFmode
;;      xvaddsp 12,12,0         // calculate b+c+d as V4SFmode
;;      xvcvspbf16 12,12        // convert b+c+d into BFmode memory format
;;      stxsihx 12,3,2          // store b+c+d
;;
;; With this peephole2, we can eliminate the xscvspdp and xscvdpspn
;; instructions.
;;
;; We keep the xxspltw between the two xvaddsp's in case the user
;; explicitly did a SFmode extract of element 0 and did a splat
;; operation.

(define_peephole2
  [(set (match_operand:SF 0 "vsx_register_operand")
	(unspec:SF
	 [(match_operand:V4SF 1 "vsx_register_operand")]
	 UNSPEC_VSX_CVSPDP))
   (set (match_operand:V4SF 2 "vsx_register_operand")
	(unspec:V4SF [(match_dup 0)] UNSPEC_VSX_CVDPSPN))]
  "REGNO (operands[1]) == REGNO (operands[2])
   || peep2_reg_dead_p (1, operands[1])"
  [(set (match_dup 2) (match_dup 1))])

;; Negate 16-bit floating point by XOR with -0.0.

(define_insn_and_split "neg<mode>2"
  [(set (match_operand:FP16 0 "gpc_reg_operand" "=wa,?wr")
	(neg:FP16 (match_operand:FP16 1 "gpc_reg_operand" "wa,wr")))
   (clobber (match_scratch:FP16 2 "=&wa,&r"))]
  ""
  "#"
  "&& 1"
  [(set (match_dup 2)
	(match_dup 3))
   (set (match_dup 0)
	(xor:FP16 (match_dup 1)
		  (match_dup 2)))]
{
  if (GET_CODE (operands[2]) == SCRATCH)
    operands[2] = gen_reg_rtx (<MODE>mode);

  REAL_VALUE_TYPE dconst;

  gcc_assert (real_from_string (&dconst, "-0.0") == 0);

  rtx rc = const_double_from_real_value (dconst, <MODE>mode);
  if (!TARGET_PREFIXED)
    rc = force_const_mem (<MODE>mode, rc);

  operands[3] = rc;
}
  [(set_attr "type" "veclogical,integer")
   (set_attr "length" "16")])

;; 16-bit floating point absolute value

(define_insn_and_split "abs<mode>2"
  [(set (match_operand:FP16 0 "gpc_reg_operand" "=wa,?wr")
	(abs:FP16
	 (match_operand:FP16 1 "gpc_reg_operand" "wa,wr")))
   (clobber (match_scratch:FP16 2 "=&wa,&r"))]
  ""
  "#"
  "&& 1"
  [(set (match_dup 2)
	(match_dup 3))
   (set (match_dup 0)
	(and:FP16 (match_dup 1)
		  (not:FP16 (match_dup 2))))]
{
  if (GET_CODE (operands[2]) == SCRATCH)
    operands[2] = gen_reg_rtx (<MODE>mode);

  REAL_VALUE_TYPE dconst;

  gcc_assert (real_from_string (&dconst, "-0.0") == 0);

  rtx rc = const_double_from_real_value (dconst, <MODE>mode);

  if (!TARGET_PREFIXED)
    rc = force_const_mem (<MODE>mode, rc);

  operands[3] = rc;
}
  [(set_attr "type" "veclogical,integer")
   (set_attr "length" "16")])

;; 16-bit negative floating point absolute value

(define_insn_and_split "*nabs<mode>2"
  [(set (match_operand:FP16 0 "gpc_reg_operand" "=wa,?wr")
	(neg:FP16
	 (abs:FP16
	  (match_operand:FP16 1 "gpc_reg_operand" "wa,wr"))))
   (clobber (match_scratch:FP16 2 "=&wa,&r"))]
  ""
  "#"
  "&& 1"
  [(set (match_dup 2)
	(match_dup 3))
   (set (match_dup 0)
	(ior:FP16 (match_dup 1)
		  (match_dup 2)))]
{
  if (GET_CODE (operands[2]) == SCRATCH)
    operands[2] = gen_reg_rtx (<MODE>mode);

  REAL_VALUE_TYPE dconst;

  gcc_assert (real_from_string (&dconst, "-0.0") == 0);
  rtx rc = const_double_from_real_value (dconst, <MODE>mode);

  if (!TARGET_PREFIXED)
    rc = force_const_mem (<MODE>mode, rc);

  operands[3] = rc;
}
  [(set_attr "type" "veclogical,integer")
   (set_attr "length" "16")])

;; Vector Pack support.

;; Unfortunately the machine independent code assumes there is only one
;; 16-bit floating point type.  So we have to choose whether to support
;; packing _Float16 or __bfloat16.

(define_expand "vec_pack_trunc_v4sf_v8hf"
  [(match_operand:V8HF 0 "vfloat_operand")
   (match_operand:V4SF 1 "vfloat_operand")
   (match_operand:V4SF 2 "vfloat_operand")]
  "TARGET_FLOAT16_HW"
{
  rtx r1 = gen_reg_rtx (V8HFmode);
  rtx r2 = gen_reg_rtx (V8HFmode);

  emit_insn (gen_xvcvsphp_v8hf (r1, operands[1]));
  emit_insn (gen_xvcvsphp_v8hf (r2, operands[2]));
  rs6000_expand_extract_even (operands[0], r1, r2);
  DONE;
})

(define_expand "vec_pack_trunc_v4sf"
  [(match_operand:V8BF 0 "vfloat_operand")
   (match_operand:V4SF 1 "vfloat_operand")
   (match_operand:V4SF 2 "vfloat_operand")]
  "TARGET_BFLOAT16_HW"
{
  rtx r1 = gen_reg_rtx (V8BFmode);
  rtx r2 = gen_reg_rtx (V8BFmode);

  emit_insn (gen_xvcvspbf16_v8bf (r1, operands[1]));
  emit_insn (gen_xvcvspbf16_v8bf (r2, operands[2]));
  rs6000_expand_extract_even (operands[0], r1, r2);
  DONE;
})

;; Used for vector conversion to _Float16
(define_insn "xvcvsphp_v8hf"
  [(set (match_operand:V8HF 0 "vsx_register_operand" "=wa")
	(unspec:V8HF [(match_operand:V4SF 1 "vsx_register_operand" "wa")]
		     UNSPEC_XVCVSPHP_V8HF))]
  "TARGET_P9_VECTOR"
  "xvcvsphp %x0,%x1"
[(set_attr "type" "vecfloat")])

;; Used for vector conversion to __bloat16
(define_insn "xvcvspbf16_v8bf"
  [(set (match_operand:V8BF 0 "vsx_register_operand" "=wa")
	(unspec:V8BF [(match_operand:V4SF 1 "vsx_register_operand" "wa")]
		     UNSPEC_XVCVSPBF16_V8BF))]
  "TARGET_BFLOAT16_HW"
  "xvcvspbf16 %x0,%x1"
  [(set_attr "type" "vecfloat")])

;; Vector unpack support.  Given the name is for the type being
;; unpacked, we can unpack both __bfloat16 and _Float16.

;; Unpack vector _Float16
(define_expand "vec_unpacks_hi_v8hf"
  [(match_operand:V4SF 0 "vfloat_operand")
   (match_operand:V8HF 1 "vfloat_operand")]
  "TARGET_FLOAT16_HW"
{
  rtx reg = gen_reg_rtx (V8HFmode);

  rs6000_expand_interleave (reg, operands[1], operands[1], BYTES_BIG_ENDIAN);
  emit_insn (gen_xvcvhpsp_v8hf (operands[0], reg));
  DONE;
})

(define_expand "vec_unpacks_lo_v8hf"
  [(match_operand:V4SF 0 "vfloat_operand")
   (match_operand:V8HF 1 "vfloat_operand")]
  "TARGET_FLOAT16_HW"
{
  rtx reg = gen_reg_rtx (V8HFmode);

  rs6000_expand_interleave (reg, operands[1], operands[1], !BYTES_BIG_ENDIAN);
  emit_insn (gen_xvcvhpsp_v8hf (operands[0], reg));
  DONE;
})

;; Used for vector conversion from _Float16
(define_insn "xvcvhpsp_v8hf"
  [(set (match_operand:V4SF 0 "vsx_register_operand" "=wa")
	(unspec:V4SF [(match_operand:V8HF 1 "vsx_register_operand" "wa")]
		     UNSPEC_CVT_FP16_TO_V4SF))]
  "TARGET_BFLOAT16_HW"
  "xvcvhpsp %x0,%x1"
  [(set_attr "type" "vecperm")])

;; Unpack vector __bfloat16
(define_expand "vec_unpacks_hi_v8bf"
  [(match_operand:V4SF 0 "vfloat_operand")
   (match_operand:V8BF 1 "vfloat_operand")]
  "TARGET_BFLOAT16_HW"
{
  rtx reg = gen_reg_rtx (V8BFmode);

  rs6000_expand_interleave (reg, operands[1], operands[1], BYTES_BIG_ENDIAN);
  emit_insn (gen_xvcvbf16spn_v8bf (operands[0], reg));
  DONE;
})

(define_expand "vec_unpacks_lo_v8bf"
  [(match_operand:V4SF 0 "vfloat_operand")
   (match_operand:V8BF 1 "vfloat_operand")]
  "TARGET_BFLOAT16_HW"
{
  rtx reg = gen_reg_rtx (V8BFmode);

  rs6000_expand_interleave (reg, operands[1], operands[1], !BYTES_BIG_ENDIAN);
  emit_insn (gen_xvcvbf16spn_v8bf (operands[0], reg));
  DONE;
})

;; Used for vector conversion from __bfloat16
(define_insn "xvcvbf16spn_v8bf"
  [(set (match_operand:V4SF 0 "vsx_register_operand" "=wa")
	(unspec:V4SF [(match_operand:V8BF 1 "vsx_register_operand" "wa")]
		     UNSPEC_CVT_FP16_TO_V4SF))]
  "TARGET_BFLOAT16_HW"
  "xvcvbf16spn %x0,%x1"
  [(set_attr "type" "vecperm")])
