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
(define_mode_iterator FP16	[BF HF])
(define_mode_iterator VS_FP16	[BF HF V8BF V8HF])
(define_mode_iterator VFP16	[V8BF V8HF])

;; Mode attribute giving the vector mode for a 16-bit floating point
;; scalar in both upper and lower case.
(define_mode_attr FP16_VECTOR8 [(BF "V8BF")
				(HF "V8HF")])

(define_mode_attr fp16_vector8 [(BF "v8bf")
				(HF "v8hf")])

;; _Float16 and __bfloat16 moves
(define_expand "mov<mode>"
  [(set (match_operand:FP16 0 "nonimmediate_operand")
	(match_operand:FP16 1 "any_operand"))]
  "TARGET_FLOAT16"
{
  if (MEM_P (operands[0]) && !REG_P (operands[1]))
    operands[1] = force_reg (<MODE>mode, operands[1]);
})

;; On power10, we can load up HFmode and BFmode constants with xxspltiw
;; or pli.
(define_insn "*mov<mode>_xxspltiw"
  [(set (match_operand:FP16 0 "gpc_reg_operand"        "=wa,wa,?r,?r")
	(match_operand:FP16 1 "fp16_xxspltiw_constant"   "j,eP,j, eP"))]
  "TARGET_FLOAT16
   && (TARGET_PREFIXED || operands[1] == CONST0_RTX (<MODE>mode))"
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
	  : "li %0,%2");
}
  [(set_attr "type"     "veclogical, vecsimple, *,  *")
   (set_attr "prefixed" "no,         yes,       no, yes")])

;; Handle creating -0.0 if we don't have XXSPLTIW.  For the scalar
;; modes, we can't do the gen_lowpart call until after register
;; allocation.
(define_split
  [(set (match_operand:VS_FP16 0 "altivec_register_operand")
	(match_operand:VS_FP16 1 "minus_zero_constant"))]
  "TARGET_FLOAT16 && reload_completed"
  [(const_int 0)]
{
  int dest_r = reg_or_subregno (operands[0]);
  rtx dest = gen_rtx_REG (V8HImode, dest_r);
  size_t nunits = GET_MODE_NUNITS (V8HFmode);

  rtvec v = rtvec_alloc (nunits);
  for (size_t i = 0; i < nunits; i++)
    RTVEC_ELT (v, i) = constm1_rtx;

  rs6000_expand_vector_init (dest, gen_rtx_PARALLEL (V8HImode, v));
  emit_insn (gen_rtx_SET (dest, gen_rtx_ASHIFT (V8HImode, dest, dest)));
  DONE;
})


(define_insn "*mov<mode>_internal"
  [(set (match_operand:FP16 0 "nonimmediate_operand"
                      "=wa,       wa,       Z,         r,          r,
                        m,        r,        wa,        wa,         r,
                        v")

	(match_operand:FP16 1 "any_operand"
                      "wa,        Z,        wa,        r,          m,
                       r,         wa,       r,         j,          j,
                       eZ"))]
  "TARGET_FLOAT16
   && (gpc_reg_operand (operands[0], <MODE>mode)
       || gpc_reg_operand (operands[1], <MODE>mode))"
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
   li %0,0
   #"
  [(set_attr "type"   "vecsimple, fpload,    fpstore,   *,          load,
                       store,     mtvsr,     mfvsr,     veclogical, *,
                       vecperm")
   (set_attr "isa"    "*,         p9v,       p9v,       *,          *,
                       *,         p8v,       p8v,       p9v,        *,
                       *")
   (set_attr "length" "*,         *,         *,         *,          *,
                       *,         *,         *,         *,          *,
                       8")])

;; Vector duplicate
(define_insn "*vecdup<mode>_reg"
  [(set (match_operand:<FP16_VECTOR8> 0 "altivec_register_operand" "=v")
	(vec_duplicate:<FP16_VECTOR8>
	 (match_operand:FP16 1 "altivec_register_operand" "v")))]
  "TARGET_FLOAT16"
  "vsplth %0,%1,3"
  [(set_attr "type" "vecperm")])

(define_insn "*vecdup<mode>_const"
  [(set (match_operand:<FP16_VECTOR8> 0 "vsx_register_operand" "=wa,wa")
	(vec_duplicate:<FP16_VECTOR8>
	 (match_operand:FP16 1 "fp16_xxspltiw_constant" "j,eP")))]
  "TARGET_FLOAT16
   && (TARGET_PREFIXED || operands[1] == CONST0_RTX (<MODE>mode))"
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
