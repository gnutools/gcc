;; Matrix-Multiply Assist (MMA) patterns.
;; Copyright (C) 2020-2025 Free Software Foundation, Inc.
;; Contributed by Peter Bergner <bergner@linux.ibm.com> and
;;		  Michael Meissner <meissner@linux.ibm.com>
;;
;; This file is part of GCC.
;;
;; GCC is free software; you can redistribute it and/or modify it
;; under the terms of the GNU General Public License as published
;; by the Free Software Foundation; either version 3, or (at your
;; option) any later version.
;;
;; GCC is distributed in the hope that it will be useful, but WITHOUT
;; ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
;; or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
;; License for more details.
;;
;; You should have received a copy of the GNU General Public License
;; along with GCC; see the file COPYING3.  If not see
;; <http://www.gnu.org/licenses/>.

;; The MMA patterns use the multi-register XOmode and OOmode opaque
;; modes to implement the target specific __vector_quad and
;; __vector_pair types that the MMA built-in functions reference.  We
;; use OPAQUE_MODE to prevent anything from trying to open them up.

(define_constants [(MAX_MMA_OPERANDS 9)])

;; Constants for creating unspecs

(define_c_enum "unspec"
  [UNSPEC_VSX_ASSEMBLE
   UNSPEC_MMA_PMXVBF16GER2
   UNSPEC_MMA_PMXVBF16GER2NN
   UNSPEC_MMA_PMXVBF16GER2NP
   UNSPEC_MMA_PMXVBF16GER2PN
   UNSPEC_MMA_PMXVBF16GER2PP
   UNSPEC_MMA_PMXVF16GER2
   UNSPEC_MMA_PMXVF16GER2NN
   UNSPEC_MMA_PMXVF16GER2NP
   UNSPEC_MMA_PMXVF16GER2PN
   UNSPEC_MMA_PMXVF16GER2PP
   UNSPEC_MMA_PMXVF32GER
   UNSPEC_MMA_PMXVF32GERNN
   UNSPEC_MMA_PMXVF32GERNP
   UNSPEC_MMA_PMXVF32GERPN
   UNSPEC_MMA_PMXVF32GERPP
   UNSPEC_MMA_PMXVF64GER
   UNSPEC_MMA_PMXVF64GERNN
   UNSPEC_MMA_PMXVF64GERNP
   UNSPEC_MMA_PMXVF64GERPN
   UNSPEC_MMA_PMXVF64GERPP
   UNSPEC_MMA_PMXVI16GER2
   UNSPEC_MMA_PMXVI16GER2PP
   UNSPEC_MMA_PMXVI16GER2S
   UNSPEC_MMA_PMXVI16GER2SPP
   UNSPEC_MMA_PMXVI4GER8
   UNSPEC_MMA_PMXVI4GER8PP
   UNSPEC_MMA_PMXVI8GER4
   UNSPEC_MMA_PMXVI8GER4PP
   UNSPEC_MMA_PMXVI8GER4SPP
   UNSPEC_MMA_XVBF16GER2
   UNSPEC_MMA_XVBF16GER2NN
   UNSPEC_MMA_XVBF16GER2NP
   UNSPEC_MMA_XVBF16GER2PN
   UNSPEC_MMA_XVBF16GER2PP
   UNSPEC_MMA_XVF16GER2
   UNSPEC_MMA_XVF16GER2NN
   UNSPEC_MMA_XVF16GER2NP
   UNSPEC_MMA_XVF16GER2PN
   UNSPEC_MMA_XVF16GER2PP
   UNSPEC_MMA_XVF32GER
   UNSPEC_MMA_XVF32GERNN
   UNSPEC_MMA_XVF32GERNP
   UNSPEC_MMA_XVF32GERPN
   UNSPEC_MMA_XVF32GERPP
   UNSPEC_MMA_XVF64GER
   UNSPEC_MMA_XVF64GERNN
   UNSPEC_MMA_XVF64GERNP
   UNSPEC_MMA_XVF64GERPN
   UNSPEC_MMA_XVF64GERPP
   UNSPEC_MMA_XVI16GER2
   UNSPEC_MMA_XVI16GER2PP
   UNSPEC_MMA_XVI16GER2S
   UNSPEC_MMA_XVI16GER2SPP
   UNSPEC_MMA_XVI4GER8
   UNSPEC_MMA_XVI4GER8PP
   UNSPEC_MMA_XVI8GER4
   UNSPEC_MMA_XVI8GER4PP
   UNSPEC_MMA_XVI8GER4SPP
   UNSPEC_MMA_XXMFACC
   UNSPEC_MMA_XXMTACC
   UNSPEC_MMA_DMSETDMRZ
   UNSPEC_DM_INSERT512_UPPER
   UNSPEC_DM_INSERT512_LOWER
   UNSPEC_DM_INSERT1024
   UNSPEC_DM_EXTRACT512
   UNSPEC_DMR_RELOAD_FROM_MEMORY
   UNSPEC_DMR_RELOAD_TO_MEMORY
   UNSPEC_DMF_DMXOR
   UNSPEC_DMF_DMXVI8GERX4
   UNSPEC_DMF_DMXVI8GERX4PP
   UNSPEC_DMF_DMXVI8GERX4SPP
   UNSPEC_DMF_PMDMXVI8GERX4
   UNSPEC_DMF_PMDMXVI8GERX4PP
   UNSPEC_DMF_PMDMXVI8GERX4SPP
   UNSPEC_DMF_DMXVBF16GERX2
   UNSPEC_DMF_DMXVBF16GERX2PP
   UNSPEC_DMF_DMXVBF16GERX2PN
   UNSPEC_DMF_DMXVBF16GERX2NP
   UNSPEC_DMF_DMXVBF16GERX2NN
   UNSPEC_DMF_PMDMXVBF16GERX2
   UNSPEC_DMF_PMDMXVBF16GERX2PP
   UNSPEC_DMF_PMDMXVBF16GERX2PN
   UNSPEC_DMF_PMDMXVBF16GERX2NP
   UNSPEC_DMF_PMDMXVBF16GERX2NN
  ])

(define_c_enum "unspecv"
  [UNSPECV_MMA_ASSEMBLE
   UNSPECV_MMA_XXSETACCZ
  ])

;; MMA instructions with 1 accumulator argument
(define_int_iterator MMA_ACC		[UNSPEC_MMA_XXMFACC
					 UNSPEC_MMA_XXMTACC])

;; MMA instructions with 2 vector arguments
(define_int_iterator MMA_VV		[UNSPEC_MMA_XVI4GER8
					 UNSPEC_MMA_XVI8GER4
					 UNSPEC_MMA_XVI16GER2
					 UNSPEC_MMA_XVI16GER2S
					 UNSPEC_MMA_XVF16GER2
					 UNSPEC_MMA_XVBF16GER2
					 UNSPEC_MMA_XVF32GER])

;; MMA instructions with 1 accumulator and 2 vector arguments
(define_int_iterator MMA_AVV		[UNSPEC_MMA_XVI4GER8PP
					 UNSPEC_MMA_XVI8GER4PP
					 UNSPEC_MMA_XVI8GER4SPP
					 UNSPEC_MMA_XVI16GER2PP
					 UNSPEC_MMA_XVI16GER2SPP
					 UNSPEC_MMA_XVF16GER2PP
					 UNSPEC_MMA_XVF16GER2PN
					 UNSPEC_MMA_XVF16GER2NP
					 UNSPEC_MMA_XVF16GER2NN
					 UNSPEC_MMA_XVBF16GER2PP
					 UNSPEC_MMA_XVBF16GER2PN
					 UNSPEC_MMA_XVBF16GER2NP
					 UNSPEC_MMA_XVBF16GER2NN
					 UNSPEC_MMA_XVF32GERPP
					 UNSPEC_MMA_XVF32GERPN
					 UNSPEC_MMA_XVF32GERNP
					 UNSPEC_MMA_XVF32GERNN])

;; MMA instructions with 1 vector pair and 1 vector arguments
(define_int_iterator MMA_PV		[UNSPEC_MMA_XVF64GER])

;; DMF instructions with 1 vector pair and 1 vector arguments
(define_int_iterator DMF_PV		[UNSPEC_DMF_DMXVI8GERX4
					 UNSPEC_DMF_DMXVBF16GERX2])

;; MMA instructions with 1 accumulator, 1 vector pair and 1 vector arguments
(define_int_iterator MMA_APV		[UNSPEC_MMA_XVF64GERPP
					 UNSPEC_MMA_XVF64GERPN
					 UNSPEC_MMA_XVF64GERNP
					 UNSPEC_MMA_XVF64GERNN])

;; DMF instructions with 1 dmr, 1 vector pair and 1 vector arguments
(define_int_iterator DMF_DPV		[UNSPEC_DMF_DMXVI8GERX4PP
					 UNSPEC_DMF_DMXVI8GERX4SPP
					 UNSPEC_DMF_DMXVBF16GERX2PP
					 UNSPEC_DMF_DMXVBF16GERX2PN
					 UNSPEC_DMF_DMXVBF16GERX2NP
					 UNSPEC_DMF_DMXVBF16GERX2NN])

;; MMA instructions with 2 vector, 2 4-bit and 1 8-bit arguments
(define_int_iterator MMA_VVI4I4I8	[UNSPEC_MMA_PMXVI4GER8])

;; MMA instructions with 1 accumulator, 2 vector, 2 4-bit and 1 8-bit arguments
(define_int_iterator MMA_AVVI4I4I8	[UNSPEC_MMA_PMXVI4GER8PP])

;; MMA instructions with 2 vector, 2 4-bit and 1 2-bit arguments
(define_int_iterator MMA_VVI4I4I2	[UNSPEC_MMA_PMXVI16GER2
					 UNSPEC_MMA_PMXVI16GER2S
					 UNSPEC_MMA_PMXVF16GER2
					 UNSPEC_MMA_PMXVBF16GER2])

;; MMA instructions with 1 accumulator, 2 vector, 2 4-bit and 1 2-bit arguments
(define_int_iterator MMA_AVVI4I4I2	[UNSPEC_MMA_PMXVI16GER2PP
					 UNSPEC_MMA_PMXVI16GER2SPP
					 UNSPEC_MMA_PMXVF16GER2PP
					 UNSPEC_MMA_PMXVF16GER2PN
					 UNSPEC_MMA_PMXVF16GER2NP
					 UNSPEC_MMA_PMXVF16GER2NN
					 UNSPEC_MMA_PMXVBF16GER2PP
					 UNSPEC_MMA_PMXVBF16GER2PN
					 UNSPEC_MMA_PMXVBF16GER2NP
					 UNSPEC_MMA_PMXVBF16GER2NN])

;; MMA instructions with 2 vector and 2 4-bit arguments
(define_int_iterator MMA_VVI4I4		[UNSPEC_MMA_PMXVF32GER])

;; MMA instructions with 1 accumulator, 2 vector and 2 4-bit arguments
(define_int_iterator MMA_AVVI4I4	[UNSPEC_MMA_PMXVF32GERPP
					 UNSPEC_MMA_PMXVF32GERPN
					 UNSPEC_MMA_PMXVF32GERNP
					 UNSPEC_MMA_PMXVF32GERNN])

;; MMA instructions with 2 vector, 1 4-bit and 1 2-bit arguments
(define_int_iterator MMA_PVI4I2		[UNSPEC_MMA_PMXVF64GER])

;; MMA instructions with 1 accumulator, 2 vector, 1 4-bit and 1 2-bit arguments
(define_int_iterator MMA_APVI4I2	[UNSPEC_MMA_PMXVF64GERPP
					 UNSPEC_MMA_PMXVF64GERPN
					 UNSPEC_MMA_PMXVF64GERNP
					 UNSPEC_MMA_PMXVF64GERNN])

;; MMA instructions with 2 vector and 3 4-bit arguments
(define_int_iterator MMA_VVI4I4I4	[UNSPEC_MMA_PMXVI8GER4])

;; MMA instructions with 1 accumulator, 2 vector and 3 4-bit arguments
(define_int_iterator MMA_AVVI4I4I4	[UNSPEC_MMA_PMXVI8GER4PP
					 UNSPEC_MMA_PMXVI8GER4SPP])

;; DMF instructions with 1 vector pair, 1 vector and 1 8-bit and 2 4-bit
;; arguments
(define_int_iterator DMF_PVI8I4I4	[UNSPEC_DMF_PMDMXVI8GERX4])

;; DMF instructions with 1dmr, 1 vector pair, 1 vector and 1 8-bit and
;; 2 4-bit arguments
(define_int_iterator DMF_DPVI8I4I4	[UNSPEC_DMF_PMDMXVI8GERX4PP
					 UNSPEC_DMF_PMDMXVI8GERX4SPP])

;; DMF instructions with 1 vector pair, 1 vector, 1 8-bit, 1 4-bit
;; and 1 2-bit arguments
(define_int_iterator DMF_PVI8I4I2	[UNSPEC_DMF_PMDMXVBF16GERX2])

;; DMF instructions with 1dmr, 1 vector pair, 1 vector, 1 8-bit,
;; 1 4-bit and 1 2-bit arguments
(define_int_iterator DMF_DPVI8I4I2	[UNSPEC_DMF_PMDMXVBF16GERX2PP
					 UNSPEC_DMF_PMDMXVBF16GERX2PN
					 UNSPEC_DMF_PMDMXVBF16GERX2NP
					 UNSPEC_DMF_PMDMXVBF16GERX2NN])

(define_int_attr acc		[(UNSPEC_MMA_XXMFACC		"xxmfacc")
				 (UNSPEC_MMA_XXMTACC		"xxmtacc")])

(define_int_attr vv		[(UNSPEC_MMA_XVI4GER8		"xvi4ger8")
				 (UNSPEC_MMA_XVI8GER4		"xvi8ger4")
				 (UNSPEC_MMA_XVI16GER2		"xvi16ger2")
				 (UNSPEC_MMA_XVI16GER2S		"xvi16ger2s")
				 (UNSPEC_MMA_XVF16GER2		"xvf16ger2")
				 (UNSPEC_MMA_XVBF16GER2		"xvbf16ger2")
				 (UNSPEC_MMA_XVF32GER		"xvf32ger")])

(define_int_attr avv		[(UNSPEC_MMA_XVI4GER8PP		"xvi4ger8pp")
				 (UNSPEC_MMA_XVI8GER4PP		"xvi8ger4pp")
				 (UNSPEC_MMA_XVI8GER4SPP	"xvi8ger4spp")
				 (UNSPEC_MMA_XVI16GER2PP	"xvi16ger2pp")
				 (UNSPEC_MMA_XVI16GER2SPP	"xvi16ger2spp")
				 (UNSPEC_MMA_XVF16GER2PP	"xvf16ger2pp")
				 (UNSPEC_MMA_XVF16GER2PN	"xvf16ger2pn")
				 (UNSPEC_MMA_XVF16GER2NP	"xvf16ger2np")
				 (UNSPEC_MMA_XVF16GER2NN	"xvf16ger2nn")
				 (UNSPEC_MMA_XVBF16GER2PP	"xvbf16ger2pp")
				 (UNSPEC_MMA_XVBF16GER2PN	"xvbf16ger2pn")
				 (UNSPEC_MMA_XVBF16GER2NP	"xvbf16ger2np")
				 (UNSPEC_MMA_XVBF16GER2NN	"xvbf16ger2nn")
				 (UNSPEC_MMA_XVF32GERPP		"xvf32gerpp")
				 (UNSPEC_MMA_XVF32GERPN		"xvf32gerpn")
				 (UNSPEC_MMA_XVF32GERNP		"xvf32gernp")
				 (UNSPEC_MMA_XVF32GERNN		"xvf32gernn")])

(define_int_attr pv		[(UNSPEC_MMA_XVF64GER		"xvf64ger")
				 (UNSPEC_DMF_DMXVI8GERX4	"dmxvi8gerx4")
				 (UNSPEC_DMF_DMXVBF16GERX2	"dmxvbf16gerx2")])

(define_int_attr apv		[(UNSPEC_MMA_XVF64GERPP		"xvf64gerpp")
				 (UNSPEC_MMA_XVF64GERPN		"xvf64gerpn")
				 (UNSPEC_MMA_XVF64GERNP		"xvf64gernp")
				 (UNSPEC_MMA_XVF64GERNN		"xvf64gernn")])

(define_int_attr dpv		[(UNSPEC_DMF_DMXVI8GERX4PP	"dmxvi8gerx4pp")
				 (UNSPEC_DMF_DMXVI8GERX4SPP	"dmxvi8gerx4spp")
				 (UNSPEC_DMF_DMXVBF16GERX2PP	"dmxvbf16gerx2pp")
				 (UNSPEC_DMF_DMXVBF16GERX2PN	"dmxvbf16gerx2pn")
				 (UNSPEC_DMF_DMXVBF16GERX2NP	"dmxvbf16gerx2np")
				 (UNSPEC_DMF_DMXVBF16GERX2NN	"dmxvbf16gerx2nn")])

;; The "pm" prefix is not in these expansions, so that we can generate
;; pmdmxvi4ger8 on systems with dense math registers and xvi4ger8 on systems
;; without dense math registers.
(define_int_attr vvi4i4i8	[(UNSPEC_MMA_PMXVI4GER8		"xvi4ger8")])

(define_int_attr avvi4i4i8	[(UNSPEC_MMA_PMXVI4GER8PP	"xvi4ger8pp")])

(define_int_attr vvi4i4i2	[(UNSPEC_MMA_PMXVI16GER2	"xvi16ger2")
				 (UNSPEC_MMA_PMXVI16GER2S	"xvi16ger2s")
				 (UNSPEC_MMA_PMXVF16GER2	"xvf16ger2")
				 (UNSPEC_MMA_PMXVBF16GER2	"xvbf16ger2")])

(define_int_attr avvi4i4i2	[(UNSPEC_MMA_PMXVI16GER2PP	"xvi16ger2pp")
				 (UNSPEC_MMA_PMXVI16GER2SPP	"xvi16ger2spp")
				 (UNSPEC_MMA_PMXVF16GER2PP	"xvf16ger2pp")
				 (UNSPEC_MMA_PMXVF16GER2PN	"xvf16ger2pn")
				 (UNSPEC_MMA_PMXVF16GER2NP	"xvf16ger2np")
				 (UNSPEC_MMA_PMXVF16GER2NN	"xvf16ger2nn")
				 (UNSPEC_MMA_PMXVBF16GER2PP	"xvbf16ger2pp")
				 (UNSPEC_MMA_PMXVBF16GER2PN	"xvbf16ger2pn")
				 (UNSPEC_MMA_PMXVBF16GER2NP	"xvbf16ger2np")
				 (UNSPEC_MMA_PMXVBF16GER2NN	"xvbf16ger2nn")])

(define_int_attr vvi4i4		[(UNSPEC_MMA_PMXVF32GER		"xvf32ger")])

(define_int_attr avvi4i4	[(UNSPEC_MMA_PMXVF32GERPP	"xvf32gerpp")
				 (UNSPEC_MMA_PMXVF32GERPN	"xvf32gerpn")
				 (UNSPEC_MMA_PMXVF32GERNP	"xvf32gernp")
				 (UNSPEC_MMA_PMXVF32GERNN	"xvf32gernn")])

(define_int_attr pvi4i2		[(UNSPEC_MMA_PMXVF64GER		"xvf64ger")])

(define_int_attr apvi4i2	[(UNSPEC_MMA_PMXVF64GERPP	"xvf64gerpp")
				 (UNSPEC_MMA_PMXVF64GERPN	"xvf64gerpn")
				 (UNSPEC_MMA_PMXVF64GERNP	"xvf64gernp")
				 (UNSPEC_MMA_PMXVF64GERNN	"xvf64gernn")])

(define_int_attr vvi4i4i4	[(UNSPEC_MMA_PMXVI8GER4		"xvi8ger4")])

(define_int_attr avvi4i4i4	[(UNSPEC_MMA_PMXVI8GER4PP	"xvi8ger4pp")
				 (UNSPEC_MMA_PMXVI8GER4SPP	"xvi8ger4spp")])

(define_int_attr pvi8i4i4	[(UNSPEC_DMF_PMDMXVI8GERX4	"pmdmxvi8gerx4")])

(define_int_attr dpvi8i4i4	[(UNSPEC_DMF_PMDMXVI8GERX4PP	"pmdmxvi8gerx4pp")
				 (UNSPEC_DMF_PMDMXVI8GERX4SPP	"pmdmxvi8gerx4spp")])

(define_int_attr pvi8i4i2       [(UNSPEC_DMF_PMDMXVBF16GERX2	"pmdmxvbf16gerx2")])

(define_int_attr dpvi8i4i2      [(UNSPEC_DMF_PMDMXVBF16GERX2PP	"pmdmxvbf16gerx2pp")
				 (UNSPEC_DMF_PMDMXVBF16GERX2PN	"pmdmxvbf16gerx2pn")
				 (UNSPEC_DMF_PMDMXVBF16GERX2NP	"pmdmxvbf16gerx2np")
				 (UNSPEC_DMF_PMDMXVBF16GERX2NN	"pmdmxvbf16gerx2nn")])

;; Vector pair support.  OOmode can only live in VSRs.
(define_expand "movoo"
  [(set (match_operand:OO 0 "nonimmediate_operand")
	(match_operand:OO 1 "input_operand"))]
  ""
{
  if (TARGET_MMA)
    {
      rs6000_emit_move (operands[0], operands[1], OOmode);
      DONE;
    }
  else if (currently_expanding_to_rtl && seen_error ())
    {
      /* PR103353 shows we may want to continue to expand the __builtin_vsx_lxvp
	 built-in function, even if we have already emitted error messages about
	 some missing required conditions.  As shown in that PR, without one
	 explicit mov optab on OOmode provided, it would call emit_move_insn
	 recursively.  So we allow this pattern to be generated when we are
	 expanding to RTL and have seen errors.  It would not cause further ICEs
	 as the compilation would stop soon after expanding.  */
    }
  else if (rs6000_opaque_type_invalid_use_p (currently_expanding_gimple_stmt))
    ;
  else
    /* Catch unexpected cases.  */
    gcc_assert (false);
})

(define_insn_and_split "*movoo"
  [(set (match_operand:OO 0 "nonimmediate_operand" "=wa,ZwO,wa")
	(match_operand:OO 1 "input_operand" "ZwO,wa,wa"))]
  "TARGET_MMA
   && (gpc_reg_operand (operands[0], OOmode)
       || gpc_reg_operand (operands[1], OOmode))"
  "@
   lxvp%X1 %x0,%1
   stxvp%X0 %x1,%0
   #"
  "&& reload_completed
   && (!MEM_P (operands[0]) && !MEM_P (operands[1]))"
  [(const_int 0)]
{
  rs6000_split_multireg_move (operands[0], operands[1]);
  DONE;
}
  [(set_attr "type" "vecload,vecstore,veclogical")
   (set_attr "size" "256")
   (set_attr "length" "*,*,8")])


;; Vector quad support.  Under the original MMA, XOmode can only live in VSX
;; registers 0..31.  With dense math, XOmode can live in either VSX registers
;; (0..63) or DMR registers.
(define_expand "movxo"
  [(set (match_operand:XO 0 "nonimmediate_operand")
	(match_operand:XO 1 "input_operand"))]
  ""
{
  if (TARGET_MMA)
    {
      rs6000_emit_move (operands[0], operands[1], XOmode);
      DONE;
    }
  else if (currently_expanding_to_rtl && seen_error ())
    {
      /* PR103353 shows we may want to continue to expand the __builtin_vsx_lxvp
	 built-in function, even if we have already emitted error messages about
	 some missing required conditions.  So do the same handlings for XOmode
	 as OOmode here.  */
    }
  else if (rs6000_opaque_type_invalid_use_p (currently_expanding_gimple_stmt))
    ;
  else
    /* Catch unexpected cases.  */
    gcc_assert (false);
})

(define_insn_and_split "*movxo_nodm"
  [(set (match_operand:XO 0 "nonimmediate_operand" "=wD,ZwO,wD")
	(match_operand:XO 1 "input_operand" "ZwO,wD,wD"))]
  "TARGET_MMA && !TARGET_DENSE_MATH
   && (gpc_reg_operand (operands[0], XOmode)
       || gpc_reg_operand (operands[1], XOmode))"
  "@
   #
   #
   #"
  "&& reload_completed"
  [(const_int 0)]
{
  rs6000_split_multireg_move (operands[0], operands[1]);
  DONE;
}
  [(set_attr "type" "vecload,vecstore,veclogical")
   (set_attr "length" "*,*,16")
   (set_attr "max_prefixed_insns" "2,2,*")])

(define_insn_and_split "*movxo_dm"
  [(set (match_operand:XO 0 "nonimmediate_operand" "=wa,ZwO,wa,wD,wD,wa")
	(match_operand:XO 1 "input_operand"        "ZwO,wa, wa,wa,wD,wD"))]
  "TARGET_DENSE_MATH
   && (gpc_reg_operand (operands[0], XOmode)
       || gpc_reg_operand (operands[1], XOmode))"
  "@
   #
   #
   #
   dmxxinstdmr512 %0,%1,%Y1,0
   dmmr %0,%1
   dmxxextfdmr512 %0,%Y0,%1,0"
  "&& reload_completed
   && !dmr_operand (operands[0], XOmode)
   && !dmr_operand (operands[1], XOmode)"
  [(const_int 0)]
{
  rs6000_split_multireg_move (operands[0], operands[1]);
  DONE;
}
  [(set_attr "type" "vecload,vecstore,veclogical,mma,mma,mma")
   (set_attr "length" "*,*,16,*,*,*")
   (set_attr "max_prefixed_insns" "2,2,*,*,*,*")])

;; We cannot update the two output registers atomically, so mark the output
;; as an early clobber so we don't accidentally clobber the input operands.  */

(define_insn_and_split "vsx_assemble_pair"
  [(set (match_operand:OO 0 "vsx_register_operand" "=&wa")
	(unspec:OO [(match_operand:V16QI 1 "mma_assemble_input_operand" "mwa")
		    (match_operand:V16QI 2 "mma_assemble_input_operand" "mwa")]
		   UNSPEC_VSX_ASSEMBLE))]
  "TARGET_MMA"
  "#"
  "&& reload_completed"
  [(const_int 0)]
{
  rtx src = gen_rtx_UNSPEC (OOmode,
			    gen_rtvec (2, operands[1], operands[2]),
			    UNSPEC_VSX_ASSEMBLE);
  rs6000_split_multireg_move (operands[0], src);
  DONE;
})

(define_expand "vsx_disassemble_pair"
  [(match_operand:V16QI 0 "mma_disassemble_output_operand")
   (match_operand:OO 1 "vsx_register_operand")
   (match_operand 2 "const_0_to_1_operand")]
  "TARGET_MMA"
{
  int regoff = INTVAL (operands[2]) * 16;
  rtx src = simplify_gen_subreg (V16QImode, operands[1], OOmode, regoff);
  emit_move_insn (operands[0], src);
  DONE;
})

;; Move from VSX registers to DMR registers via two insert 512 bit
;; instructions.
(define_insn "dm_insert512"
  [(set (match_operand:XO 0 "dmr_operand" "=wD")
	(unspec:XO [(match_operand:OO 1 "vsx_register_operand" "wa")
		    (match_operand:OO 2 "vsx_register_operand" "wa")
		    (match_operand 3 "const_0_to_1_operand")]
		   UNSPEC_DM_INSERT512_UPPER))]
  "TARGET_DENSE_MATH"
  "dmxxinstdmr512 %0,%x1,%x2,%3"
  [(set_attr "type" "mma")])

(define_insn "dm_insert1024"
  [(set (match_operand:TDO 0 "dmr_operand" "=wD")
	(unspec:TDO [(match_operand:OO 1 "vsx_register_operand" "wa")
		     (match_operand:OO 2 "vsx_register_operand" "wa")
		     (match_operand:OO 3 "vsx_register_operand" "wa")
		     (match_operand:OO 4 "vsx_register_operand" "wa")]
		    UNSPEC_DM_INSERT1024))]
  "TARGET_DENSE_MATH"
  "dmxxinstdmr512 %0,%x1,%x2,0\n\tdmxxinstdmr512 %0,%x3,%x4,1"
  [(set_attr "type" "mma")])

(define_expand "mma_assemble_acc"
  [(match_operand:XO 0 "accumulator_operand")
   (match_operand:V16QI 1 "mma_assemble_input_operand")
   (match_operand:V16QI 2 "mma_assemble_input_operand")
   (match_operand:V16QI 3 "mma_assemble_input_operand")
   (match_operand:V16QI 4 "mma_assemble_input_operand")]
  "TARGET_MMA"
{
  if (TARGET_DENSE_MATH)
    {
      rtx vp0 = gen_reg_rtx (OOmode);
      rtx vp1 = gen_reg_rtx (OOmode);
      emit_insn (gen_vsx_assemble_pair (vp0, operands[1], operands[2]));
      emit_insn (gen_vsx_assemble_pair (vp1, operands[3], operands[4]));
      emit_insn (gen_dm_insert512 (operands[0], vp0, vp1, const0_rtx));
    }
  else
    {
      rtx src = gen_rtx_UNSPEC_VOLATILE (XOmode,
			    	    	 gen_rtvec (4, operands[1], operands[2],
				       		    operands[3], operands[4]),
			    	     	 UNSPECV_MMA_ASSEMBLE);
      emit_move_insn (operands[0], src);
    }
  DONE;
})

;; We cannot update the four output registers atomically, so mark the output
;; as an early clobber so we don't accidentally clobber the input operands.  */

(define_insn_and_split "*mma_assemble_acc"
  [(set (match_operand:XO 0 "accumulator_operand" "=&wD")
	(unspec_volatile:XO
	  [(match_operand:V16QI 1 "mma_assemble_input_operand" "mwa")
	   (match_operand:V16QI 2 "mma_assemble_input_operand" "mwa")
	   (match_operand:V16QI 3 "mma_assemble_input_operand" "mwa")
	   (match_operand:V16QI 4 "mma_assemble_input_operand" "mwa")]
	  UNSPECV_MMA_ASSEMBLE))]
  "TARGET_MMA
   && accumulator_operand (operands[0], XOmode)"
  "#"
  "&& reload_completed"
  [(const_int 0)]
{
  rtx src = gen_rtx_UNSPEC_VOLATILE (XOmode,
			    	     gen_rtvec (4, operands[1], operands[2],
				       		operands[3], operands[4]),
			    	     UNSPECV_MMA_ASSEMBLE);
  rs6000_split_multireg_move (operands[0], src);
  DONE;
})

(define_expand "mma_disassemble_acc"
  [(match_operand:V16QI 0 "mma_disassemble_output_operand")
   (match_operand:XO 1 "accumulator_operand")
   (match_operand 2 "const_0_to_3_operand")]
  "TARGET_MMA"
{
  int regoff = INTVAL (operands[2]) * 16;
  rtx src = simplify_gen_subreg (V16QImode, operands[1], XOmode, regoff);
  emit_move_insn (operands[0], src);
  DONE;
})

(define_expand "mma_build_dmr"
  [(match_operand:TDO 0 "dmr_operand")
   (match_operand:V16QI 1 "mma_assemble_input_operand")
   (match_operand:V16QI 2 "mma_assemble_input_operand")
   (match_operand:V16QI 3 "mma_assemble_input_operand")
   (match_operand:V16QI 4 "mma_assemble_input_operand")
   (match_operand:V16QI 5 "mma_assemble_input_operand")
   (match_operand:V16QI 6 "mma_assemble_input_operand")
   (match_operand:V16QI 7 "mma_assemble_input_operand")
   (match_operand:V16QI 8 "mma_assemble_input_operand")]
  "TARGET_DENSE_MATH"
{
  rtx vp0 = gen_reg_rtx (OOmode);
  rtx vp1 = gen_reg_rtx (OOmode);
  rtx vp2 = gen_reg_rtx (OOmode);
  rtx vp3 = gen_reg_rtx (OOmode);
  emit_insn (gen_vsx_assemble_pair (vp0, operands[1], operands[2]));
  emit_insn (gen_vsx_assemble_pair (vp1, operands[3], operands[4]));
  emit_insn (gen_vsx_assemble_pair (vp2, operands[5], operands[6]));
  emit_insn (gen_vsx_assemble_pair (vp3, operands[7], operands[8]));
  emit_insn (gen_dm_insert1024 (operands[0], vp0, vp1, vp2, vp3));
  DONE;
})

;; MMA instructions that do not use their accumulators as an input, still must
;; not allow their vector operands to overlap the registers used by the
;; accumulator.  We enforce this by marking the output as early clobber.  The
;; prime and de-prime instructions are not needed on systems with dense math
;; registers.

(define_insn "mma_<acc>"
  [(set (match_operand:XO 0 "accumulator_operand" "=&wD")
	(unspec:XO [(match_operand:XO 1 "accumulator_operand" "0")]
		    MMA_ACC))]
  "TARGET_MMA && !TARGET_DENSE_MATH"
  "<acc> %A0"
  [(set_attr "type" "mma")])

;; We can't have integer constants in XOmode so we wrap this in an
;; UNSPEC_VOLATILE.  If we have dense math registers, we can just use a normal
;; UNSPEC instead of UNSPEC_VOLATILE.

(define_insn "mma_xxsetaccz"
  [(set (match_operand:XO 0 "accumulator_operand" "=wD")
	(unspec_volatile:XO [(const_int 0)]
			    UNSPECV_MMA_XXSETACCZ))]
  "TARGET_MMA"
{
  return TARGET_DENSE_MATH ? "dmsetdmrz %A0" : "xxsetaccz %A0";
}
  [(set_attr "type" "mma")])

(define_insn "mma_dmsetdmrz"
  [(set (match_operand:TDO 0 "accumulator_operand" "=wD")
	(unspec [(const_int 0)]
		UNSPEC_MMA_DMSETDMRZ))]
  "TARGET_DENSE_MATH"
  "dmsetdmrz %A0"
  [(set_attr "type" "mma")])

(define_insn "mma_<vv>"
  [(set (match_operand:XO 0 "accumulator_operand" "=&wD,&wD")
	(unspec:XO [(match_operand:V16QI 1 "vsx_register_operand" "v,?wa")
		    (match_operand:V16QI 2 "vsx_register_operand" "v,?wa")]
		    MMA_VV))]
  "TARGET_MMA"
{
  return TARGET_DENSE_MATH ? "dm<vv> %A0,%x1,%x2" : "<vv> %A0,%x1,%x2";
}
  [(set_attr "type" "mma")])

(define_insn "mma_<avv>"
  [(set (match_operand:XO 0 "accumulator_operand" "=&wD,&wD")
	(unspec:XO [(match_operand:XO 1 "accumulator_operand" "0,0")
		    (match_operand:V16QI 2 "vsx_register_operand" "v,?wa")
		    (match_operand:V16QI 3 "vsx_register_operand" "v,?wa")]
		    MMA_AVV))]
  "TARGET_MMA"
{
  return TARGET_DENSE_MATH ? "dm<avv> %A0,%x2,%x3" : "<avv> %A0,%x2,%x3";
}
  [(set_attr "type" "mma")])

(define_insn "mma_<pv>"
  [(set (match_operand:XO 0 "accumulator_operand" "=&wD,&wD")
	(unspec:XO [(match_operand:OO 1 "vsx_register_operand" "v,?wa")
		    (match_operand:V16QI 2 "vsx_register_operand" "v,?wa")]
		    MMA_PV))]
  "TARGET_MMA"
{
  return TARGET_DENSE_MATH ? "dm<pv> %A0,%x1,%x2" : "<pv> %A0,%x1,%x2";
}
  [(set_attr "type" "mma")])

(define_insn "mma_<apv>"
  [(set (match_operand:XO 0 "accumulator_operand" "=&wD,&wD")
	(unspec:XO [(match_operand:XO 1 "accumulator_operand" "0,0")
		    (match_operand:OO 2 "vsx_register_operand" "v,?wa")
		    (match_operand:V16QI 3 "vsx_register_operand" "v,?wa")]
		    MMA_APV))]
  "TARGET_MMA"
{
  return TARGET_DENSE_MATH ? "dm<apv> %A0,%x2,%x3" : "<apv> %A0,%x2,%x3";
}
  [(set_attr "type" "mma")])

(define_insn "mma_pm<vvi4i4i8>"
  [(set (match_operand:XO 0 "accumulator_operand" "=&wD,&wD")
	(unspec:XO [(match_operand:V16QI 1 "vsx_register_operand" "v,?wa")
		    (match_operand:V16QI 2 "vsx_register_operand" "v,?wa")
		    (match_operand:SI 3 "const_0_to_15_operand" "n,n")
		    (match_operand:SI 4 "const_0_to_15_operand" "n,n")
		    (match_operand:SI 5 "u8bit_cint_operand" "n,n")]
		    MMA_VVI4I4I8))]
  "TARGET_MMA"
{
  return (TARGET_DENSE_MATH
	  ? "pmdm<vvi4i4i8> %A0,%x1,%x2,%3,%4,%5"
	  : "pm<vvi4i4i8> %A0,%x1,%x2,%3,%4,%5");
}
  [(set_attr "type" "mma")
   (set_attr "prefixed" "yes")])

(define_insn "mma_pm<avvi4i4i8>"
  [(set (match_operand:XO 0 "accumulator_operand" "=&wD,&wD")
	(unspec:XO [(match_operand:XO 1 "accumulator_operand" "0,0")
		    (match_operand:V16QI 2 "vsx_register_operand" "v,?wa")
		    (match_operand:V16QI 3 "vsx_register_operand" "v,?wa")
		    (match_operand:SI 4 "const_0_to_15_operand" "n,n")
		    (match_operand:SI 5 "const_0_to_15_operand" "n,n")
		    (match_operand:SI 6 "u8bit_cint_operand" "n,n")]
		    MMA_AVVI4I4I8))]
  "TARGET_MMA"
{
  return (TARGET_DENSE_MATH
	  ? "pmdm<avvi4i4i8> %A0,%x2,%x3,%4,%5,%6"
	  : "pm<avvi4i4i8> %A0,%x2,%x3,%4,%5,%6");
}
  [(set_attr "type" "mma")
   (set_attr "prefixed" "yes")])

(define_insn "mma_pm<vvi4i4i2>"
  [(set (match_operand:XO 0 "accumulator_operand" "=&wD,&wD")
	(unspec:XO [(match_operand:V16QI 1 "vsx_register_operand" "v,?wa")
		    (match_operand:V16QI 2 "vsx_register_operand" "v,?wa")
		    (match_operand:SI 3 "const_0_to_15_operand" "n,n")
		    (match_operand:SI 4 "const_0_to_15_operand" "n,n")
		    (match_operand:SI 5 "const_0_to_3_operand" "n,n")]
		    MMA_VVI4I4I2))]
  "TARGET_MMA"
{
  return (TARGET_DENSE_MATH
	  ? "pmdm<vvi4i4i2> %A0,%x1,%x2,%3,%4,%5"
	  : "pm<vvi4i4i2> %A0,%x1,%x2,%3,%4,%5");
}
  [(set_attr "type" "mma")
   (set_attr "prefixed" "yes")])

(define_insn "mma_pm<avvi4i4i2>"
  [(set (match_operand:XO 0 "accumulator_operand" "=&wD,&wD")
	(unspec:XO [(match_operand:XO 1 "accumulator_operand" "0,0")
		    (match_operand:V16QI 2 "vsx_register_operand" "v,?wa")
		    (match_operand:V16QI 3 "vsx_register_operand" "v,?wa")
		    (match_operand:SI 4 "const_0_to_15_operand" "n,n")
		    (match_operand:SI 5 "const_0_to_15_operand" "n,n")
		    (match_operand:SI 6 "const_0_to_3_operand" "n,n")]
		    MMA_AVVI4I4I2))]
  "TARGET_MMA"
{
  return (TARGET_DENSE_MATH
	  ? "pmdm<avvi4i4i2> %A0,%x2,%x3,%4,%5,%6"
	  : "pm<avvi4i4i2> %A0,%x2,%x3,%4,%5,%6");
}
  [(set_attr "type" "mma")
   (set_attr "prefixed" "yes")])

(define_insn "mma_pm<vvi4i4>"
  [(set (match_operand:XO 0 "accumulator_operand" "=&wD,&wD")
	(unspec:XO [(match_operand:V16QI 1 "vsx_register_operand" "v,?wa")
		    (match_operand:V16QI 2 "vsx_register_operand" "v,?wa")
		    (match_operand:SI 3 "const_0_to_15_operand" "n,n")
		    (match_operand:SI 4 "const_0_to_15_operand" "n,n")]
		    MMA_VVI4I4))]
  "TARGET_MMA"
{
  return (TARGET_DENSE_MATH
	  ? "pmdm<vvi4i4> %A0,%x1,%x2,%3,%4"
	  : "pm<vvi4i4> %A0,%x1,%x2,%3,%4");
}
  [(set_attr "type" "mma")
   (set_attr "prefixed" "yes")])

(define_insn "mma_pm<avvi4i4>"
  [(set (match_operand:XO 0 "accumulator_operand" "=&wD,&wD")
	(unspec:XO [(match_operand:XO 1 "accumulator_operand" "0,0")
		    (match_operand:V16QI 2 "vsx_register_operand" "v,?wa")
		    (match_operand:V16QI 3 "vsx_register_operand" "v,?wa")
		    (match_operand:SI 4 "const_0_to_15_operand" "n,n")
		    (match_operand:SI 5 "const_0_to_15_operand" "n,n")]
		    MMA_AVVI4I4))]
  "TARGET_MMA"
{
  return (TARGET_DENSE_MATH
	  ? "pmdm<avvi4i4> %A0,%x2,%x3,%4,%5"
	  : "pm<avvi4i4> %A0,%x2,%x3,%4,%5");
}
  [(set_attr "type" "mma")
   (set_attr "prefixed" "yes")])

(define_insn "mma_pm<pvi4i2>"
  [(set (match_operand:XO 0 "accumulator_operand" "=&wD,&wD")
	(unspec:XO [(match_operand:OO 1 "vsx_register_operand" "v,?wa")
		    (match_operand:V16QI 2 "vsx_register_operand" "v,?wa")
		    (match_operand:SI 3 "const_0_to_15_operand" "n,n")
		    (match_operand:SI 4 "const_0_to_3_operand" "n,n")]
		    MMA_PVI4I2))]
  "TARGET_MMA"
{
  return (TARGET_DENSE_MATH
	  ? "pmdm<pvi4i2> %A0,%x1,%x2,%3,%4"
	  : "pm<pvi4i2> %A0,%x1,%x2,%3,%4");
}
  [(set_attr "type" "mma")
   (set_attr "prefixed" "yes")])

(define_insn "mma_pm<apvi4i2>"
  [(set (match_operand:XO 0 "accumulator_operand" "=&wD,&wD")
	(unspec:XO [(match_operand:XO 1 "accumulator_operand" "0,0")
		    (match_operand:OO 2 "vsx_register_operand" "v,?wa")
		    (match_operand:V16QI 3 "vsx_register_operand" "v,?wa")
		    (match_operand:SI 4 "const_0_to_15_operand" "n,n")
		    (match_operand:SI 5 "const_0_to_3_operand" "n,n")]
		    MMA_APVI4I2))]
  "TARGET_MMA"
{
  return (TARGET_DENSE_MATH
	  ? "pmdm<apvi4i2> %A0,%x2,%x3,%4,%5"
	  : "pm<apvi4i2> %A0,%x2,%x3,%4,%5");
}
  [(set_attr "type" "mma")
   (set_attr "prefixed" "yes")])

(define_insn "mma_pm<vvi4i4i4>"
  [(set (match_operand:XO 0 "accumulator_operand" "=&wD,&wD")
	(unspec:XO [(match_operand:V16QI 1 "vsx_register_operand" "v,?wa")
		    (match_operand:V16QI 2 "vsx_register_operand" "v,?wa")
		    (match_operand:SI 3 "const_0_to_15_operand" "n,n")
		    (match_operand:SI 4 "const_0_to_15_operand" "n,n")
		    (match_operand:SI 5 "const_0_to_15_operand" "n,n")]
		    MMA_VVI4I4I4))]
  "TARGET_MMA"
{
  return (TARGET_DENSE_MATH
	  ? "pmdm<vvi4i4i4> %A0,%x1,%x2,%3,%4,%5"
	  : "pm<vvi4i4i4> %A0,%x1,%x2,%3,%4,%5");
}
  [(set_attr "type" "mma")
   (set_attr "prefixed" "yes")])

(define_insn "mma_pm<avvi4i4i4>"
  [(set (match_operand:XO 0 "accumulator_operand" "=&wD,&wD")
	(unspec:XO [(match_operand:XO 1 "accumulator_operand" "0,0")
		    (match_operand:V16QI 2 "vsx_register_operand" "v,?wa")
		    (match_operand:V16QI 3 "vsx_register_operand" "v,?wa")
		    (match_operand:SI 4 "const_0_to_15_operand" "n,n")
		    (match_operand:SI 5 "const_0_to_15_operand" "n,n")
		    (match_operand:SI 6 "const_0_to_15_operand" "n,n")]
		    MMA_AVVI4I4I4))]
  "TARGET_MMA"
{
  return (TARGET_DENSE_MATH
	  ? "pmdm<avvi4i4i4> %A0,%x2,%x3,%4,%5,%6"
	  : "pm<avvi4i4i4> %A0,%x2,%x3,%4,%5,%6");
}
  [(set_attr "type" "mma")
   (set_attr "prefixed" "yes")])

;; TDOmode (__dmr keyword for 1,024 bit registers).
(define_expand "movtdo"
  [(set (match_operand:TDO 0 "nonimmediate_operand")
	(match_operand:TDO 1 "input_operand"))]
  "TARGET_DENSE_MATH"
{
  rs6000_emit_move (operands[0], operands[1], TDOmode);
  DONE;
})

(define_insn_and_split "*movtdo"
  [(set (match_operand:TDO 0 "nonimmediate_operand" "=wa,m,wa,wD,wD,wa")
	(match_operand:TDO 1 "input_operand" "m,wa,wa,wa,wD,wD"))]
  "TARGET_DENSE_MATH
   && (gpc_reg_operand (operands[0], TDOmode)
       || gpc_reg_operand (operands[1], TDOmode))"
  "@
   #
   #
   #
   #
   dmmr %0,%1
   #"
  "&& reload_completed
   && (!dmr_operand (operands[0], TDOmode) || !dmr_operand (operands[1], TDOmode))"
  [(const_int 0)]
{
  rtx op0 = operands[0];
  rtx op1 = operands[1];

  if (REG_P (op0) && REG_P (op1))
    {
      int regno0 = REGNO (op0);
      int regno1 = REGNO (op1);

      if (DMR_REGNO_P (regno0) && VSX_REGNO_P (regno1))
	{
	  rtx pair0 = gen_rtx_REG (OOmode, regno1);
	  rtx pair1 = gen_rtx_REG (OOmode, regno1 + 2);
	  rtx pair2 = gen_rtx_REG (OOmode, regno1 + 4);
	  rtx pair3 = gen_rtx_REG (OOmode, regno1 + 6);
	  emit_insn (gen_dm_insert1024 (op0, pair0, pair1, pair2, pair3));
	  DONE;
	}

      else if (VSX_REGNO_P (regno0) && DMR_REGNO_P (regno1))
	{
	  rtx op0_upper = gen_rtx_REG (XOmode, regno0);
	  rtx op0_lower = gen_rtx_REG (XOmode, regno0 + 4);
	  emit_insn (gen_movtdo_extract512 (op0_upper, op1, const0_rtx));
	  emit_insn (gen_movtdo_extract512 (op0_lower, op1, const1_rtx));
	  DONE;
	}

     else
	gcc_assert (VSX_REGNO_P (regno0) && VSX_REGNO_P (regno1));
    }

  rs6000_split_multireg_move (operands[0], operands[1]);
  DONE;
}
  [(set_attr "type" "vecload,vecstore,vecmove,vecmove,vecmove,vecmove")
   (set_attr "length" "*,*,32,8,*,8")
   (set_attr "max_prefixed_insns" "4,4,*,*,*,*")])

(define_insn "dmf_dmxor"
  [(set (match_operand:TDO 0 "dmr_operand" "=wD")
	(unspec:TDO [(match_operand:TDO 1 "dmr_operand" "0")
		     (match_operand:TDO 2 "dmr_operand" "wD")]
		    UNSPEC_DMF_DMXOR))]
  "TARGET_DENSE_MATH"
  "dmxor %0,%1,%2"
  [(set_attr "type" "mma")])

;; Move from DMR registers to VSX registers via two extract 512 bit
;; instructions.
(define_insn "movtdo_extract512"
  [(set (match_operand:XO 0 "vsx_register_operand" "=wa")
	(unspec:XO [(match_operand:TDO 1 "dmr_operand" "wD")
		    (match_operand 2 "const_0_to_1_operand" "n")]
		   UNSPEC_DM_EXTRACT512))]
  "TARGET_DENSE_MATH"
  "dmxxextfdmr512 %0,%Y0,%1,%2"
  [(set_attr "type" "mma")])

;; Reload DMR registers from memory
(define_insn_and_split "reload_dmr_from_memory"
  [(set (match_operand:TDO 0 "dmr_operand" "=wD")
	(unspec:TDO [(match_operand:TDO 1 "memory_operand" "m")]
		    UNSPEC_DMR_RELOAD_FROM_MEMORY))
   (clobber (match_operand:OO 2 "vsx_register_operand" "=wa"))
   (clobber (match_operand:OO 3 "vsx_register_operand" "=wa"))
   (clobber (match_operand:OO 4 "vsx_register_operand" "=wa"))
   (clobber (match_operand:OO 5 "vsx_register_operand" "=wa"))]
  "TARGET_DENSE_MATH"
  "#"
  "&& reload_completed"
  [(const_int 0)]
{
  rtx dest = operands[0];
  rtx src = operands[1];
  rtx pair0 = operands[2];
  rtx pair1 = operands[3];
  rtx pair2 = operands[4];
  rtx pair3 = operands[5];

  if (BYTES_BIG_ENDIAN)
    {
      emit_move_insn (pair0, adjust_address (src, OOmode, 0));
      emit_move_insn (pair1, adjust_address (src, OOmode, 32));
      emit_move_insn (pair2, adjust_address (src, OOmode, 64));
      emit_move_insn (pair3, adjust_address (src, OOmode, 96));
    }
  else
    {
      emit_move_insn (pair3, adjust_address (src, OOmode, 0));
      emit_move_insn (pair2, adjust_address (src, OOmode, 32));
      emit_move_insn (pair1, adjust_address (src, OOmode, 64));
      emit_move_insn (pair0, adjust_address (src, OOmode, 96));
    }

  emit_insn (gen_dm_insert1024 (dest, pair0, pair1, pair2, pair3));
  DONE;
}
  [(set_attr "length" "20")
   (set_attr "max_prefixed_insns" "2")
   (set_attr "type" "vecload")])

;; Reload dense math registers to memory
(define_insn_and_split "reload_dmr_to_memory"
  [(set (match_operand:TDO 0 "memory_operand" "=m")
	(unspec:TDO [(match_operand:TDO 1 "dmr_operand" "wD")]
		    UNSPEC_DMR_RELOAD_TO_MEMORY))
   (clobber (match_operand:XO 2 "vsx_register_operand" "=wa"))]
  "TARGET_DENSE_MATH"
  "#"
  "&& reload_completed"
  [(const_int 0)]
{
  rtx dest = operands[0];
  rtx src = operands[1];
  rtx tmp = operands[2];
  rtx mem_upper = adjust_address (dest, XOmode, BYTES_BIG_ENDIAN ? 0 : 64);
  rtx mem_lower = adjust_address (dest, XOmode, BYTES_BIG_ENDIAN ? 64 : 0);

  emit_insn (gen_movtdo_extract512 (tmp, src, const0_rtx));
  emit_move_insn (mem_upper, tmp);

  emit_insn (gen_movtdo_extract512 (tmp, src, const1_rtx));
  emit_move_insn (mem_lower, tmp);
  DONE;
}
  [(set_attr "length" "16")
   (set_attr "max_prefixed_insns" "2")])

(define_insn "dmf_<pv>"
  [(set (match_operand:TDO 0 "accumulator_operand" "=wD")
	(unspec:TDO [(match_operand:OO 1 "vsx_register_operand" "wa")
		    (match_operand:V16QI 2 "vsx_register_operand" "wa")]
		    DMF_PV))]
  "TARGET_DENSE_MATH"
{
  return "<pv> %0,%x1,%x2";
}
  [(set_attr "type" "dmf")])

(define_insn "dmf_<dpv>"
  [(set (match_operand:TDO 0 "accumulator_operand" "=wD")
	(unspec:TDO [(match_operand:TDO 1 "accumulator_operand" "0")
		    (match_operand:OO 2 "vsx_register_operand" "wa")
		    (match_operand:V16QI 3 "vsx_register_operand" "wa")]
		    DMF_DPV))]
  "TARGET_DENSE_MATH"
{
  return "<dpv> %0,%x2,%x3";
}
  [(set_attr "type" "dmf")])

(define_insn "dmf_<pvi8i4i4>"
  [(set (match_operand:TDO 0 "accumulator_operand" "=wD")
	(unspec:TDO [(match_operand:OO 1 "vsx_register_operand" "wa")
		     (match_operand:V16QI 2 "vsx_register_operand" "wa")
		     (match_operand:SI 3 "u8bit_cint_operand" "n")
		     (match_operand:SI 4 "const_0_to_15_operand" "n")
		     (match_operand:SI 5 "const_0_to_15_operand" "n")]
		     DMF_PVI8I4I4))]
  "TARGET_DENSE_MATH"
{
  return "<pvi8i4i4> %0,%x1,%x2,%3,%4,%5";
}
  [(set_attr "type" "dmf")
   (set_attr "prefixed" "yes")])

(define_insn "dmf_<dpvi8i4i4>"
  [(set (match_operand:TDO 0 "accumulator_operand" "=wD")
	(unspec:TDO [(match_operand:TDO 1 "accumulator_operand" "0")
		     (match_operand:OO 2 "vsx_register_operand" "wa")
		     (match_operand:V16QI 3 "vsx_register_operand" "wa")
		     (match_operand:SI 4 "u8bit_cint_operand" "n")
		     (match_operand:SI 5 "const_0_to_15_operand" "n")
		     (match_operand:SI 6 "const_0_to_15_operand" "n")]
		     DMF_DPVI8I4I4))]
  "TARGET_DENSE_MATH"
{
  return "<dpvi8i4i4> %0,%x2,%x3,%4,%5,%6";
}
  [(set_attr "type" "dmf")
   (set_attr "prefixed" "yes")])

(define_insn "dmf_<pvi8i4i2>"
  [(set (match_operand:TDO 0 "accumulator_operand" "=wD")
	(unspec:TDO [(match_operand:OO 1 "vsx_register_operand" "wa")
		     (match_operand:V16QI 2 "vsx_register_operand" "wa")
		     (match_operand:SI 3 "u8bit_cint_operand" "n")
		     (match_operand:SI 4 "const_0_to_15_operand" "n")
		     (match_operand:SI 5 "const_0_to_3_operand" "n")]
		     DMF_PVI8I4I2))]
  "TARGET_DENSE_MATH"
{
  return "<pvi8i4i2> %0,%x1,%x2,%3,%4,%5";
}
  [(set_attr "type" "dmf")
   (set_attr "prefixed" "yes")])

(define_insn "dmf_<dpvi8i4i2>"
  [(set (match_operand:TDO 0 "accumulator_operand" "=wD")
	(unspec:TDO [(match_operand:TDO 1 "accumulator_operand" "0")
		     (match_operand:OO 2 "vsx_register_operand" "wa")
		     (match_operand:V16QI 3 "vsx_register_operand" "wa")
		     (match_operand:SI 4 "u8bit_cint_operand" "n")
		     (match_operand:SI 5 "const_0_to_15_operand" "n")
		     (match_operand:SI 6 "const_0_to_3_operand" "n")]
		     DMF_DPVI8I4I2))]
  "TARGET_DENSE_MATH"
{
  return "<dpvi8i4i2> %0,%x2,%x3,%4,%5,%6";
}
  [(set_attr "type" "dmf")
   (set_attr "prefixed" "yes")])
