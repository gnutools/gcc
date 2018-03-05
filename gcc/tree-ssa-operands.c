/* SSA operands management for trees.
   Copyright (C) 2003-2018 Free Software Foundation, Inc.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3, or (at your option)
any later version.

GCC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "tree.h"
#include "gimple.h"
#include "timevar.h"
#include "ssa.h"
#include "gimple-pretty-print.h"
#include "diagnostic-core.h"
#include "stmt.h"
#include "print-tree.h"
#include "dumpfile.h"


/* This file contains the code required to manage the operands cache of the
   SSA optimizer.  For every stmt, we maintain an operand cache in the stmt
   annotation.  This cache contains operands that will be of interest to
   optimizers and other passes wishing to manipulate the IL.

   The operand type are broken up into REAL and VIRTUAL operands.  The real
   operands are represented as pointers into the stmt's operand tree.  Thus
   any manipulation of the real operands will be reflected in the actual tree.
   Virtual operands are represented solely in the cache, although the base
   variable for the SSA_NAME may, or may not occur in the stmt's tree.
   Manipulation of the virtual operands will not be reflected in the stmt tree.

   The routines in this file are concerned with creating this operand cache
   from a stmt tree.

   The operand tree is the parsed by the various get_* routines which look
   through the stmt tree for the occurrence of operands which may be of
   interest, and calls are made to the append_* routines whenever one is
   found.  There are 4 of these routines, each representing one of the
   4 types of operands. Defs, Uses, Virtual Uses, and Virtual May Defs.

   The append_* routines check for duplication, and simply keep a list of
   unique objects for each operand type in the build_* extendable vectors.

   Once the stmt tree is completely parsed, the finalize_ssa_operands()
   routine is called, which proceeds to perform the finalization routine
   on each of the 4 operand vectors which have been built up.

   If the stmt had a previous operand cache, the finalization routines
   attempt to match up the new operands with the old ones.  If it's a perfect
   match, the old vector is simply reused.  If it isn't a perfect match, then
   a new vector is created and the new operands are placed there.  For
   virtual operands, if the previous cache had SSA_NAME version of a
   variable, and that same variable occurs in the same operands cache, then
   the new cache vector will also get the same SSA_NAME.

   i.e., if a stmt had a VUSE of 'a_5', and 'a' occurs in the new
   operand vector for VUSE, then the new vector will also be modified
   such that it contains 'a_5' rather than 'a'.  */


/* Flags to describe operand properties in helpers.  */

/* By default, operands are loaded.  */
#define opf_use		0

/* Operand is the target of an assignment expression or a
   call-clobbered variable.  */
#define opf_def 	(1 << 0)

/* No virtual operands should be created in the expression.  This is used
   when traversing ADDR_EXPR nodes which have different semantics than
   other expressions.  Inside an ADDR_EXPR node, the only operands that we
   need to consider are indices into arrays.  For instance, &a.b[i] should
   generate a USE of 'i' but it should not generate a VUSE for 'a' nor a
   VUSE for 'b'.  */
#define opf_no_vops 	(1 << 1)

/* Operand is having its address taken.  */
#define opf_address_taken (1 << 2)

/* Array for building all the use operands.  */
static vec<tree *> build_uses;

#define BF_RENAME     1
#define BF_RENAME_VOP 2
#define BF_VUSE       4
#define BF_VDEF       8
#define BF_VOLATILE  16
static int build_flags;

static void get_expr_operands (gimple *, tree *, int);

/* Number of functions with initialized ssa_operands.  */
static int n_initialized = 0;

/* Accessor to tree-ssa-operands.c caches.  */
static inline struct ssa_operands *
gimple_ssa_operands (const struct function *fun)
{
  return &fun->gimple_df->ssa_operands;
}


/*  Return true if the SSA operands cache is active.  */

bool
ssa_operands_active (struct function *fun)
{
  if (fun == NULL)
    return false;

  return fun->gimple_df && gimple_ssa_operands (fun)->ops_active;
}


/* Create the VOP variable, an artificial global variable to act as a
   representative of all of the virtual operands FUD chain.  */

static void
create_vop_var (struct function *fn)
{
  tree global_var;

  gcc_assert (fn->gimple_df->vop == NULL_TREE);

  global_var = build_decl (BUILTINS_LOCATION, VAR_DECL,
			   get_identifier (".MEM"),
			   void_type_node);
  DECL_ARTIFICIAL (global_var) = 1;
  DECL_IGNORED_P (global_var) = 1;
  TREE_READONLY (global_var) = 0;
  DECL_EXTERNAL (global_var) = 1;
  TREE_STATIC (global_var) = 1;
  TREE_USED (global_var) = 1;
  DECL_CONTEXT (global_var) = NULL_TREE;
  TREE_THIS_VOLATILE (global_var) = 0;
  TREE_ADDRESSABLE (global_var) = 0;
  VAR_DECL_IS_VIRTUAL_OPERAND (global_var) = 1;

  fn->gimple_df->vop = global_var;
}

/* These are the sizes of the operand memory buffer in bytes which gets
   allocated each time more operands space is required.  The final value is
   the amount that is allocated every time after that.
   In 1k we can fit 25 use operands (or 63 def operands) on a host with
   8 byte pointers, that would be 10 statements each with 1 def and 2
   uses.  */

#define OP_SIZE_INIT	0
#define OP_SIZE_1	(1024 - sizeof (void *))
#define OP_SIZE_2	(1024 * 4 - sizeof (void *))
#define OP_SIZE_3	(1024 * 16 - sizeof (void *))

/* Initialize the operand cache routines.  */

void
init_ssa_operands (struct function *fn)
{
  if (!n_initialized++)
    {
      build_uses.create (10);
      build_flags = 0;
    }

  gcc_assert (gimple_ssa_operands (fn)->operand_memory == NULL);
  gimple_ssa_operands (fn)->operand_memory_index
     = gimple_ssa_operands (fn)->ssa_operand_mem_size;
  gimple_ssa_operands (fn)->ops_active = true;
  gimple_ssa_operands (fn)->ssa_operand_mem_size = OP_SIZE_INIT;
  create_vop_var (fn);
}


/* Dispose of anything required by the operand routines.  */

void
fini_ssa_operands (struct function *fn)
{
  struct ssa_operand_memory_d *ptr;

  if (!--n_initialized)
    {
      build_flags = 0;
      build_uses.release ();
    }

  gimple_ssa_operands (fn)->free_uses = NULL;

  while ((ptr = gimple_ssa_operands (fn)->operand_memory) != NULL)
    {
      gimple_ssa_operands (fn)->operand_memory
	= gimple_ssa_operands (fn)->operand_memory->next;
      ggc_free (ptr);
    }

  gimple_ssa_operands (fn)->ops_active = false;

  fn->gimple_df->vop = NULL_TREE;
}


/* Return memory for an operand of size SIZE.  */

static inline void *
ssa_operand_alloc (struct function *fn, unsigned size)
{
  char *ptr;

  gcc_assert (size == sizeof (struct use_optype_d));

  if (gimple_ssa_operands (fn)->operand_memory_index + size
      >= gimple_ssa_operands (fn)->ssa_operand_mem_size)
    {
      struct ssa_operand_memory_d *ptr;

      switch (gimple_ssa_operands (fn)->ssa_operand_mem_size)
	{
	case OP_SIZE_INIT:
	  gimple_ssa_operands (fn)->ssa_operand_mem_size = OP_SIZE_1;
	  break;
	case OP_SIZE_1:
	  gimple_ssa_operands (fn)->ssa_operand_mem_size = OP_SIZE_2;
	  break;
	case OP_SIZE_2:
	case OP_SIZE_3:
	  gimple_ssa_operands (fn)->ssa_operand_mem_size = OP_SIZE_3;
	  break;
	default:
	  gcc_unreachable ();
	}


      ptr = (ssa_operand_memory_d *) ggc_internal_alloc
	(sizeof (void *) + gimple_ssa_operands (fn)->ssa_operand_mem_size);

      ptr->next = gimple_ssa_operands (fn)->operand_memory;
      gimple_ssa_operands (fn)->operand_memory = ptr;
      gimple_ssa_operands (fn)->operand_memory_index = 0;
    }

  ptr = &(gimple_ssa_operands (fn)->operand_memory
	  ->mem[gimple_ssa_operands (fn)->operand_memory_index]);
  gimple_ssa_operands (fn)->operand_memory_index += size;
  return ptr;
}


/* Allocate a USE operand.  */

static inline struct use_optype_d *
alloc_use (struct function *fn)
{
  struct use_optype_d *ret;
  if (gimple_ssa_operands (fn)->free_uses)
    {
      ret = gimple_ssa_operands (fn)->free_uses;
      gimple_ssa_operands (fn)->free_uses
	= gimple_ssa_operands (fn)->free_uses->next;
    }
  else
    ret = (struct use_optype_d *)
          ssa_operand_alloc (fn, sizeof (struct use_optype_d));
  return ret;
}


/* Adds OP to the list of uses of statement STMT after LAST.  */

static inline use_optype_p
add_use_op (struct function *fn, gimple *stmt, tree *op, use_optype_p last)
{
  use_optype_p new_use;

  new_use = alloc_use (fn);
  USE_OP_PTR (new_use)->use = op;
  link_imm_use_stmt (USE_OP_PTR (new_use), *op, stmt);
  last->next = new_use;
  new_use->next = NULL;
  return new_use;
}


/* Takes elements from build_defs and turns them into def operands of STMT.  */

static inline void
finalize_ssa_defs (struct function *fn, gimple *stmt)
{
  /* Pre-pend the vdef we may have built.  */
  if (build_flags & BF_VDEF
      && !gimple_vdef (stmt))
    gimple_set_vdef (stmt, gimple_vop (fn));

  /* Clear and unlink a no longer necessary VDEF.  */
  if (!(build_flags & BF_VDEF)
      && gimple_vdef (stmt) != NULL_TREE)
    {
      if (TREE_CODE (gimple_vdef (stmt)) == SSA_NAME)
	{
	  unlink_stmt_vdef (stmt);
	  release_ssa_name_fn (fn, gimple_vdef (stmt));
	}
      gimple_set_vdef (stmt, NULL_TREE);
    }

  /* If we have a non-SSA_NAME VDEF, mark it for renaming.  */
  if (gimple_vdef (stmt)
      && TREE_CODE (gimple_vdef (stmt)) != SSA_NAME)
    {
      build_flags |= BF_RENAME_VOP | BF_RENAME;
    }
}


/* Takes elements from build_uses and turns them into use operands of STMT.  */

static inline void
finalize_ssa_uses (struct function *fn, gimple *stmt)
{
  unsigned new_i;
  struct use_optype_d new_list;
  use_optype_p old_ops, ptr, last;

  /* Pre-pend the VUSE we may have built.  */
  if (build_flags & BF_VUSE)
      build_uses.safe_insert (0, gimple_vuse_ptr (stmt));

  /* Clear a no longer necessary VUSE.  */
  if (!(build_flags & BF_VUSE)
      && gimple_vuse (stmt) != NULL_TREE)
    gimple_set_vuse (stmt, NULL_TREE);

  new_list.next = NULL;
  last = &new_list;

  old_ops = gimple_use_ops (stmt);

  /* If there is anything in the old list, free it.  */
  if (old_ops)
    {
      for (ptr = old_ops; ptr->next; ptr = ptr->next)
	delink_imm_use (USE_OP_PTR (ptr));
      delink_imm_use (USE_OP_PTR (ptr));
      ptr->next = gimple_ssa_operands (fn)->free_uses;
      gimple_ssa_operands (fn)->free_uses = old_ops;
    }

  /* If we added a VUSE, make sure to set the operand if it is not already
     present and mark it for renaming.  */
  if ((build_flags & BF_VUSE)
      && gimple_vuse (stmt) == NULL_TREE)
    {
      gimple_set_vuse (stmt, gimple_vop (fn));
      build_flags |= BF_RENAME_VOP | BF_RENAME;
    }

  /* Now create nodes for all the new nodes.  */
  for (new_i = 0; new_i < build_uses.length (); new_i++)
    {
      tree *op = build_uses[new_i];
      last = add_use_op (fn, stmt, op, last);
    }

  /* Now set the stmt's operands.  */
  gimple_set_use_ops (stmt, new_list.next);
}


/* Clear the in_list bits and empty the build array for VDEFs and
   VUSEs.  */

static inline void
cleanup_build_arrays (void)
{
  build_uses.truncate (0);
  build_flags = 0;
}


/* Finalize all the build vectors, fill the new ones into INFO.  */

static inline void
finalize_ssa_stmt_operands (struct function *fn, gimple *stmt)
{
  finalize_ssa_defs (fn, stmt);
  finalize_ssa_uses (fn, stmt);

  if (build_flags & BF_VOLATILE)
    gimple_set_has_volatile_ops (stmt, true);
  if (build_flags & BF_RENAME)
    fn->gimple_df->ssa_renaming_needed = 1;
  if (build_flags & BF_RENAME_VOP)
    fn->gimple_df->rename_vops = 1;

  cleanup_build_arrays ();
}


/* Start the process of building up operands vectors in INFO.  */

static inline void
start_ssa_stmt_operands (void)
{
  gcc_assert (build_uses.length () == 0);
  gcc_assert (build_flags == 0);
}


/* Add USE_P to the list of pointers to operands.  */

static inline void
append_use (tree *use_p)
{
  build_uses.safe_push (use_p);
}


/* Add VAR to the set of variables that require a VDEF operator.  */

static inline void
append_vdef (void)
{
  build_flags |= BF_VDEF | BF_VUSE;
}


/* Add VAR to the set of variables that require a VUSE operator.  */

static inline void
append_vuse (void)
{
  build_flags |= BF_VUSE;
}

/* Add virtual operands for STMT.  FLAGS is as in get_expr_operands.  */

static void
add_virtual_operand (gimple *stmt ATTRIBUTE_UNUSED, int flags)
{
  /* Add virtual operands to the stmt, unless the caller has specifically
     requested not to do that (used when adding operands inside an
     ADDR_EXPR expression).  */
  if (flags & opf_no_vops)
    return;

  gcc_assert (!is_gimple_debug (stmt));

  if (flags & opf_def)
    append_vdef ();
  else
    append_vuse ();
}


/* Add *VAR_P to the appropriate operand array for statement STMT.
   FLAGS is as in get_expr_operands.  If *VAR_P is a GIMPLE register,
   it will be added to the statement's real operands, otherwise it is
   added to virtual operands.  */

static void
add_stmt_operand (tree *var_p, gimple *stmt, int flags)
{
  tree var = *var_p;

  gcc_assert (SSA_VAR_P (*var_p));

  if (is_gimple_reg (var))
    {
      /* The variable is a GIMPLE register.  Add it to real operands.  */
      if (flags & opf_def)
	;
      else
	append_use (var_p);
      if (DECL_P (*var_p))
	build_flags |= BF_RENAME;
    }
  else
    {
      /* Mark statements with volatile operands.  */
      if (!(flags & opf_no_vops)
	  && TREE_THIS_VOLATILE (var))
	build_flags |= BF_VOLATILE;

      /* The variable is a memory access.  Add virtual operands.  */
      add_virtual_operand (stmt, flags);
    }
}


/* A subroutine of get_expr_operands to handle MEM_REF.

   STMT is the statement being processed, EXPR is the MEM_REF
      that got us here.

   FLAGS is as in get_expr_operands.  */

static void
get_mem_ref_operands (gimple *stmt, tree expr, int flags)
{
  tree *pptr = &TREE_OPERAND (expr, 0);

  if (!(flags & opf_no_vops)
      && TREE_THIS_VOLATILE (expr))
    build_flags |= BF_VOLATILE;

  /* Add the VOP.  */
  add_virtual_operand (stmt, flags);

  /* If requested, add a USE operand for the base pointer.  */
  get_expr_operands (stmt, pptr, opf_use | (flags & opf_no_vops));
}


/* A subroutine of get_expr_operands to handle TARGET_MEM_REF.  */

static void
get_tmr_operands (gimple *stmt, tree expr, int flags)
{
  if (!(flags & opf_no_vops)
      && TREE_THIS_VOLATILE (expr))
    build_flags |= BF_VOLATILE;

  /* First record the real operands.  */
  get_expr_operands (stmt,
		     &TMR_BASE (expr), opf_use | (flags & opf_no_vops));
  get_expr_operands (stmt,
		     &TMR_INDEX (expr), opf_use | (flags & opf_no_vops));
  get_expr_operands (stmt,
		     &TMR_INDEX2 (expr), opf_use | (flags & opf_no_vops));

  add_virtual_operand (stmt, flags);
}


/* If STMT is a call that may clobber globals and other symbols that
   escape, return an appropriate flag mask for the necessary VDEF/VUSEs.
   Otherwise return zero.  */

static int
get_call_vop_flags (gimple *stmt)
{
  int call_flags = gimple_call_flags (stmt);
  if (!(call_flags & ECF_NOVOPS))
    {
      if (!(call_flags & (ECF_PURE | ECF_CONST)))
	return BF_VDEF | BF_VUSE;
      else if (!(call_flags & ECF_CONST))
	return BF_VUSE;
    }
  return 0;
}


/* If STMT is a call that may clobber globals and other symbols that
   escape, add them to the VDEF/VUSE lists for it.  */

static void
maybe_add_call_vops (gcall *stmt)
{
  build_flags |= get_call_vop_flags (stmt);
}


/* Scan operands in the ASM_EXPR stmt referred to in INFO.  */

static void
get_asm_stmt_operands (gasm *stmt)
{
  size_t i, noutputs;
  const char **oconstraints;
  const char *constraint;
  bool allows_mem, allows_reg, is_inout;

  noutputs = gimple_asm_noutputs (stmt);
  oconstraints = (const char **) alloca ((noutputs) * sizeof (const char *));

  /* Gather all output operands.  */
  for (i = 0; i < gimple_asm_noutputs (stmt); i++)
    {
      tree link = gimple_asm_output_op (stmt, i);
      constraint = TREE_STRING_POINTER (TREE_VALUE (TREE_PURPOSE (link)));
      oconstraints[i] = constraint;
      parse_output_constraint (&constraint, i, 0, 0, &allows_mem,
	                       &allows_reg, &is_inout);

      /* This should have been split in gimplify_asm_expr.  */
      gcc_assert (!allows_reg || !is_inout);

      get_expr_operands (stmt, &TREE_VALUE (link), opf_def);
    }

  /* Gather all input operands.  */
  for (i = 0; i < gimple_asm_ninputs (stmt); i++)
    {
      tree link = gimple_asm_input_op (stmt, i);
      constraint = TREE_STRING_POINTER (TREE_VALUE (TREE_PURPOSE (link)));
      parse_input_constraint (&constraint, 0, 0, noutputs, 0, oconstraints,
	                      &allows_mem, &allows_reg);

      get_expr_operands (stmt, &TREE_VALUE (link), opf_use);
    }

  /* Clobber all memory and addressable symbols for asm ("" : : : "memory");  */
  if (gimple_asm_clobbers_memory_p (stmt))
    add_virtual_operand (stmt, opf_def);
}


/* Recursively scan the expression pointed to by EXPR_P in statement
   STMT.  FLAGS is one of the OPF_* constants modifying how to
   interpret the operands found.  */

static void
get_expr_operands (gimple *stmt, tree *expr_p, int flags)
{
  enum tree_code code;
  enum tree_code_class codeclass;
  tree expr = *expr_p;
  int uflags = opf_use;

  if (expr == NULL)
    return;

  if (is_gimple_debug (stmt))
    uflags |= (flags & opf_no_vops);

  code = TREE_CODE (expr);
  codeclass = TREE_CODE_CLASS (code);

  switch (code)
    {
    case ADDR_EXPR:
      /* There may be variables referenced inside but there
	 should be no VUSEs created, since the referenced objects are
	 not really accessed.  The only operands that we should find
	 here are ARRAY_REF indices which will always be real operands
	 (GIMPLE does not allow non-registers as array indices).  */
      flags |= opf_no_vops;
      get_expr_operands (stmt, &TREE_OPERAND (expr, 0),
			 flags | opf_address_taken);
      return;

    case SSA_NAME:
    case VAR_DECL:
    case PARM_DECL:
    case RESULT_DECL:
      if (!(flags & opf_address_taken))
	add_stmt_operand (expr_p, stmt, flags);
      return;

    case DEBUG_EXPR_DECL:
      gcc_assert (gimple_debug_bind_p (stmt));
      return;

    case MEM_REF:
      get_mem_ref_operands (stmt, expr, flags);
      return;

    case TARGET_MEM_REF:
      get_tmr_operands (stmt, expr, flags);
      return;

    case ARRAY_REF:
    case ARRAY_RANGE_REF:
    case COMPONENT_REF:
    case REALPART_EXPR:
    case IMAGPART_EXPR:
      {
	if (!(flags & opf_no_vops)
	    && TREE_THIS_VOLATILE (expr))
	  build_flags |= BF_VOLATILE;

	get_expr_operands (stmt, &TREE_OPERAND (expr, 0), flags);

	if (code == COMPONENT_REF)
	  {
	    if (!(flags & opf_no_vops)
		&& TREE_THIS_VOLATILE (TREE_OPERAND (expr, 1)))
	      build_flags |= BF_VOLATILE;
	    get_expr_operands (stmt, &TREE_OPERAND (expr, 2), uflags);
	  }
	else if (code == ARRAY_REF || code == ARRAY_RANGE_REF)
	  {
            get_expr_operands (stmt, &TREE_OPERAND (expr, 1), uflags);
            get_expr_operands (stmt, &TREE_OPERAND (expr, 2), uflags);
            get_expr_operands (stmt, &TREE_OPERAND (expr, 3), uflags);
	  }

	return;
      }

    case WITH_SIZE_EXPR:
      /* WITH_SIZE_EXPR is a pass-through reference to its first argument,
	 and an rvalue reference to its second argument.  */
      get_expr_operands (stmt, &TREE_OPERAND (expr, 1), uflags);
      get_expr_operands (stmt, &TREE_OPERAND (expr, 0), flags);
      return;

    case COND_EXPR:
    case VEC_COND_EXPR:
    case VEC_PERM_EXPR:
      get_expr_operands (stmt, &TREE_OPERAND (expr, 0), uflags);
      get_expr_operands (stmt, &TREE_OPERAND (expr, 1), uflags);
      get_expr_operands (stmt, &TREE_OPERAND (expr, 2), uflags);
      return;

    case CONSTRUCTOR:
      {
	/* General aggregate CONSTRUCTORs have been decomposed, but they
	   are still in use as the COMPLEX_EXPR equivalent for vectors.  */
	constructor_elt *ce;
	unsigned HOST_WIDE_INT idx;

	/* A volatile constructor is actually TREE_CLOBBER_P, transfer
	   the volatility to the statement, don't use TREE_CLOBBER_P for
	   mirroring the other uses of THIS_VOLATILE in this file.  */
	if (!(flags & opf_no_vops)
	    && TREE_THIS_VOLATILE (expr))
	  build_flags |= BF_VOLATILE;

	for (idx = 0;
	     vec_safe_iterate (CONSTRUCTOR_ELTS (expr), idx, &ce);
	     idx++)
	  get_expr_operands (stmt, &ce->value, uflags);

	return;
      }

    case BIT_FIELD_REF:
      if (!(flags & opf_no_vops)
	  && TREE_THIS_VOLATILE (expr))
	build_flags |= BF_VOLATILE;
      /* FALLTHRU */

    case VIEW_CONVERT_EXPR:
    do_unary:
      get_expr_operands (stmt, &TREE_OPERAND (expr, 0), flags);
      return;

    case BIT_INSERT_EXPR:
    case COMPOUND_EXPR:
      //abort(); // happens in debug insns
      /* FALLTHRU */

    case OBJ_TYPE_REF:
    case ASSERT_EXPR:
    do_binary:
      {
	get_expr_operands (stmt, &TREE_OPERAND (expr, 0), flags);
	get_expr_operands (stmt, &TREE_OPERAND (expr, 1), flags);
	return;
      }

    case DOT_PROD_EXPR:
    case SAD_EXPR:
    case REALIGN_LOAD_EXPR:
    case WIDEN_MULT_PLUS_EXPR:
    case WIDEN_MULT_MINUS_EXPR:
    case FMA_EXPR:
      {
	abort(); // hmm, but might exist hidden down in debug stmts? doesn't seem so at present!
	get_expr_operands (stmt, &TREE_OPERAND (expr, 0), flags);
	get_expr_operands (stmt, &TREE_OPERAND (expr, 1), flags);
	get_expr_operands (stmt, &TREE_OPERAND (expr, 2), flags);
	return;
      }

    case TREE_LIST:
      /* Operands of GIMPLE_ASM, real operand is in TREE_VALUE.  */
      get_expr_operands (stmt, &TREE_VALUE (expr), flags);
      return;

    case FUNCTION_DECL:
    case LABEL_DECL:
    case CONST_DECL:
    case CASE_LABEL_EXPR:
      /* Expressions that make no memory references.  */
      return;

    default:
      if (codeclass == tcc_constant || codeclass == tcc_type)
	return;
      /*fprintf(stderr, "XXX");
      debug_tree (expr);
      fputs ("\n", stderr);*/
      if (codeclass == tcc_unary)
	goto do_unary;
      if (codeclass == tcc_binary || codeclass == tcc_comparison)
	goto do_binary;
    }

  /* If we get here, something has gone wrong.  */
  if (flag_checking)
    {
      fprintf (stderr, "unhandled expression in get_expr_operands():\n");
      debug_tree (expr);
      fputs ("\n", stderr);
      gcc_unreachable ();
    }
}


/* Parse STMT looking for operands.  When finished, the various
   build_* operand vectors will have potential operands in them.  */

static void
parse_ssa_operands (gimple *stmt)
{
  enum gimple_code code = gimple_code (stmt);
  size_t i, n, start = 0;

  switch (code)
    {
    case GIMPLE_ASM:
      get_asm_stmt_operands (as_a <gasm *> (stmt));
      break;

    case GIMPLE_TRANSACTION:
      /* The start of a transaction is a memory barrier.  */
      add_virtual_operand (stmt, opf_def | opf_use);
      break;

    case GIMPLE_DEBUG:
      if (gimple_debug_bind_p (stmt)
	  && gimple_debug_bind_has_value_p (stmt))
	get_expr_operands (stmt, gimple_debug_bind_get_value_ptr (stmt),
			   opf_use | opf_no_vops);
      break;

    case GIMPLE_RETURN:
      append_vuse ();
      goto do_default;

    case GIMPLE_CALL:
      /* Add call-clobbered operands, if needed.  */
      maybe_add_call_vops (as_a <gcall *> (stmt));
      /* FALLTHRU */

    case GIMPLE_ASSIGN:
      get_expr_operands (stmt, gimple_op_ptr (stmt, 0), opf_def);
      start = 1;
      /* FALLTHRU */

    default:
    do_default:
      n = gimple_num_ops (stmt);
      for (i = start; i < n; i++)
	get_expr_operands (stmt, gimple_op_ptr (stmt, i), opf_use);
      break;
    }
}


/* Create an operands cache for STMT.  */

static void
build_ssa_operands (struct function *fn, gimple *stmt)
{
  /* Initially assume that the statement has no volatile operands.  */
  gimple_set_has_volatile_ops (stmt, false);

  start_ssa_stmt_operands ();
  parse_ssa_operands (stmt);
  finalize_ssa_stmt_operands (fn, stmt);
}

/* Verifies SSA statement operands.  */

DEBUG_FUNCTION bool
verify_ssa_operands (gimple *stmt)
{
  use_operand_p use_p;
  def_operand_p def_p;
  ssa_op_iter iter;
  unsigned i;
  tree def;

  /* build_ssa_operands w/o finalizing them.  */
  start_ssa_stmt_operands ();
  parse_ssa_operands (stmt);

  /* Now verify the built operands are the same as present in STMT.  */
  def = gimple_vdef (stmt);
  if (!!(build_flags & BF_VDEF) != !!def)
    {
      error ("virtual definition of statement not up-to-date");
      return true;
    }
  if (gimple_vdef (stmt)
      && ((def_p = gimple_vdef_op (stmt)) == NULL_DEF_OPERAND_P
	  || DEF_FROM_PTR (def_p) != gimple_vdef (stmt)))
    {
      error ("virtual def operand missing for stmt");
      return true;
    }

  tree use = gimple_vuse (stmt);
  if (!!(build_flags & BF_VUSE) != !!use)
    {
      error ("virtual use of statement not up-to-date");
      return true;
    }
  if (gimple_vuse (stmt)
      && ((use_p = gimple_vuse_op (stmt)) == NULL_USE_OPERAND_P
	  || USE_FROM_PTR (use_p) != gimple_vuse (stmt)))
    {
      error ("virtual use operand missing for stmt");
      return true;
    }

  FOR_EACH_SSA_USE_OPERAND (use_p, stmt, iter, SSA_OP_USE)
    {
      tree *op;
      FOR_EACH_VEC_ELT (build_uses, i, op)
	{
	  if (use_p->use == op)
	    {
	      build_uses[i] = NULL;
	      break;
	    }
	}
      if (i == build_uses.length ())
	{
	  error ("excess use operand for stmt");
	  debug_generic_expr (USE_FROM_PTR (use_p));
	  return true;
	}
    }

  tree *op;
  FOR_EACH_VEC_ELT (build_uses, i, op)
    if (op != NULL)
      {
	error ("use operand missing for stmt");
	debug_generic_expr (*op);
	return true;
      }

  if (gimple_has_volatile_ops (stmt) != !!(build_flags & BF_VOLATILE))
    {
      error ("stmt volatile flag not up-to-date");
      return true;
    }

  cleanup_build_arrays ();
  return false;
}


/* Releases the operands of STMT back to their freelists, and clears
   the stmt operand lists.  */

void
free_stmt_operands (struct function *fn, gimple *stmt)
{
  use_optype_p uses = gimple_use_ops (stmt), last_use;

  if (uses)
    {
      for (last_use = uses; last_use->next; last_use = last_use->next)
	delink_imm_use (USE_OP_PTR (last_use));
      delink_imm_use (USE_OP_PTR (last_use));
      last_use->next = gimple_ssa_operands (fn)->free_uses;
      gimple_ssa_operands (fn)->free_uses = uses;
      gimple_set_use_ops (stmt, NULL);
    }

  if (gimple_has_mem_ops (stmt))
    {
      gimple_set_vuse (stmt, NULL_TREE);
      gimple_set_vdef (stmt, NULL_TREE);
    }
}


/* Get the operands of statement STMT.  */

void
update_stmt_operands (struct function *fn, gimple *stmt)
{
  /* If update_stmt_operands is called before SSA is initialized, do
     nothing.  */
  if (!ssa_operands_active (fn))
    return;

  timevar_push (TV_TREE_OPS);

  gcc_assert (gimple_modified_p (stmt));
  build_ssa_operands (fn, stmt);
  gimple_set_modified (stmt, false);

  timevar_pop (TV_TREE_OPS);
}

void
update_stmt (gimple *s)
{
  if (gimple_has_ops (s) && ssa_operands_active (cfun))
    {
      if (!flag_try_patch)
	update_stmt_for_real (s);
      else
	{
	  /* Cleanup stale per-statement operands.  Those happen
	     when non-SSA-names are placed into operands via SET_USE. */
	  /*gimple_statement_with_ops *ops_stmt =
	      dyn_cast <gimple_statement_with_ops *> (s);
	  use_optype_p *ptr = &ops_stmt->use_ops;
	  while (*ptr)
	    {
	      if (!USE_OP_PTR(*ptr)->prev
		  && (!USE_OP (*ptr) || !SSA_VAR_P (USE_OP (*ptr))))
		*ptr = (*ptr)->next;
	      else
		ptr = &((*ptr)->next);
	    }*/
	  if (verify_ssa_operands (s))
	    {
	      print_gimple_stmt (stderr, s, 0, TDF_VOPS);
	      abort ();
	    }
	}
    }
}

static use_optype_p *
find_delink_use_op (gimple *stmt, tree *pop)
{
  gimple_statement_with_ops *ops_stmt =
      dyn_cast <gimple_statement_with_ops *> (stmt);
  use_optype_p *puse;
  for (puse = &ops_stmt->use_ops; *puse; puse = &((*puse)->next))
    if ((*puse)->use_ptr.use == pop)
      {
	delink_imm_use (&((*puse)->use_ptr));
	return puse;
      }
  return NULL;
}

static void
free_stmt_use_op (use_optype_p *puse)
{
  use_optype_p use = *puse;
  *puse = use->next;
  use->next = gimple_ssa_operands (cfun)->free_uses;
  gimple_ssa_operands (cfun)->free_uses = use;
}

static void
prepend_use_op (gimple *stmt, tree *op)
{
  gimple_statement_with_ops *ops_stmt =
      dyn_cast <gimple_statement_with_ops *> (stmt);
  use_optype_p *insert_point, new_use;

  new_use = alloc_use (cfun);
  USE_OP_PTR (new_use)->use = op;
  link_imm_use_stmt (USE_OP_PTR (new_use), *op, stmt);
  insert_point = &ops_stmt->use_ops;
  /* Ensure vop use is in front. */
  if (!virtual_operand_p (*op) && *insert_point)
    insert_point = &((*insert_point)->next);
  new_use->next = *insert_point;
  *insert_point = new_use;
}

void
update_stmt_use (use_operand_p use)
{
  /* XXX Should we be asserting the following:?  */
  if (USE_FROM_PTR (use) && SSA_VAR_P (USE_FROM_PTR (use)))
    return;

  gimple *stmt = USE_STMT (use);
  gimple_statement_with_ops *ops_stmt =
      dyn_cast <gimple_statement_with_ops *> (stmt);

  if (!ops_stmt)
    {
      gcc_assert (gimple_code (stmt) == GIMPLE_PHI);
      return;
    }
  tree *pnewval = use->use;
  gcc_assert (is_gimple_debug (stmt)
	      || !*pnewval
	      || is_gimple_min_invariant (*pnewval));

  use_optype_p *puse;
  for (puse = &ops_stmt->use_ops; USE_OP_PTR (*puse) != use; puse = &((*puse)->next))
    ;
  free_stmt_use_op (puse);

  if (gimple_debug_bind_p (stmt)
      && pnewval == gimple_debug_bind_get_value_ptr (stmt)
      && *pnewval
      && !is_gimple_min_invariant (*pnewval))
    {
      start_ssa_stmt_operands ();
      get_expr_operands (stmt, pnewval, opf_no_vops | opf_use);
      for (unsigned i = 0; i < build_uses.length (); i++)
	prepend_use_op (stmt, build_uses[i]);

      if (build_flags & BF_RENAME)
	cfun->gimple_df->ssa_renaming_needed = 1;
      cleanup_build_arrays ();
    }
}

/* Check if it's easy to determine if STMT needs a vuse
   or vdef if we ignore operand I.  Returns -1 if it's not easy
   (and hence it's unknown), 0 if it won't need a vop, BF_VUSE if it
   needs a vuse and BF_VDEF|BF_VUSE if it needs a vdef.  */
static int
easy_stmt_p (gimple *stmt, unsigned i)
{
  switch (gimple_code (stmt))
    {
    case GIMPLE_ASSIGN:
      if (!gimple_assign_single_p (stmt))
	return 0;
      gcc_assert (i == 0 || i == 1);
      if (i == 0 && gimple_assign_load_p (stmt))
	/* LHS ignored, but RHS is load.  */
	return BF_VUSE;
      if (i == 1 && gimple_store_p (stmt))
	/* RHS ignored, but LHS a store.  */
	return BF_VDEF | BF_VUSE;
      /* All other cases don't create VOPs (except perhaps for operand I).  */
      return 0;

    case GIMPLE_CALL:
      {
	int call_flags = gimple_call_flags (stmt);
	/* For a call if we don't ignore the fndecl and that already
	   requires a vdef, it's easy.  */
	if (i != 1 && !(call_flags & (ECF_NOVOPS | ECF_PURE | ECF_CONST)))
	  return BF_VDEF | BF_VUSE;
	/* Also, if we ignore the LHS or have none there can only be a
	   vdef if the function isn't pure (or const).  */
	if ((i == 0 || !gimple_call_lhs (stmt)) && !(call_flags & ECF_NOVOPS))
	    {
	      /* If it's pure we definitely need a vuse.  */
	      if ((call_flags & (ECF_PURE | ECF_CONST)) == ECF_PURE)
		return BF_VUSE;
	      /* If it's const and we have no arguments, or we ignore
	         the single argument (and hence have no LHS), then
		 there won't be a vop.  */
	      if ((call_flags & ECF_CONST)
		  && (!gimple_call_num_args (stmt)
		      || (gimple_call_num_args (stmt) == 1 && i == 3)))
		return 0;
	    }
	return -1;
      }

    case GIMPLE_TRANSACTION:
      return BF_VDEF | BF_VUSE;

    case GIMPLE_RETURN:
      return BF_VUSE;

    default:
      return -1;
    }
}

static int
diddle_vops (gimple *stmt, int oldvop, int newvop, unsigned nop)
{
  int stmtvop = gimple_vdef (stmt) ? BF_VDEF | BF_VUSE : gimple_vuse (stmt) ? BF_VUSE : 0;
  /* ??? The following might seem like a good test:
       gcc_assert (stmtvop >= oldvop)
     but our callers might have already set VOP to NULL in anticipation
     that the replacement will indeed get rid of it.  Until nothing does
     this anymore we can't assert this.  */

  /* If old VOPs weren't determined by the removed operand, new
     VOPs can only become more.  */
  if (stmtvop > oldvop)
    newvop = newvop > stmtvop ? newvop : stmtvop;
  /* Otherwise old operand might have mattered for VOPs.  */
  else if (newvop >= oldvop)
    /* So if new operand has more VOPs, it can overall only become more.  */
    newvop = newvop > stmtvop ? newvop : stmtvop;
  else if (newvop < oldvop)
    /* New operand has less VOPs than old operand and that old operand
       potentially was the only one still forcing these VOPs.  We need
       to revisit everything.  */
    {
      /* But for some cases we can easily check completely.  */
      int knownvop = easy_stmt_p (stmt, nop);
      if (knownvop == -1)
	return -1;
      else if (newvop < knownvop)
	newvop = knownvop;
    }
  else
    gcc_unreachable ();

  if (newvop < BF_VDEF && gimple_vdef (stmt))
    {
      if (TREE_CODE (gimple_vdef (stmt)) == SSA_NAME)
	{
	  unlink_stmt_vdef (stmt);
	  release_ssa_name_fn (cfun, gimple_vdef (stmt));
	}
      gimple_set_vdef (stmt, NULL_TREE);
    }
  else if (newvop >= BF_VDEF && !gimple_vdef (stmt))
    gimple_set_vdef (stmt, gimple_vop (cfun));

  if (newvop >= BF_VUSE)
    {
      gcc_assert (!is_gimple_debug (stmt));
      if (!gimple_vuse (stmt))
	gimple_set_vuse (stmt, gimple_vop (cfun));
    }
  else if (gimple_vuse (stmt))
    gimple_set_vuse (stmt, NULL_TREE);

  if ((gimple_vdef (stmt) && TREE_CODE (gimple_vdef (stmt)) != SSA_NAME)
      || (gimple_vuse (stmt) && TREE_CODE (gimple_vuse (stmt)) != SSA_NAME))
    {
      cfun->gimple_df->rename_vops = 1;
      cfun->gimple_df->ssa_renaming_needed = 1;
    }

  return 0;
}

static int
exchange_complex_op (gimple *stmt, tree *pop, tree val, unsigned nop, int flags)
{
  bool was_volatile;
  unsigned i;
  int oldvop, newvop;
  use_optype_p *puse = NULL, use = NULL;

  start_ssa_stmt_operands ();
  get_expr_operands (stmt, gimple_op_ptr (stmt, nop), flags);
  if (nop == 1 && gimple_code (stmt) == GIMPLE_CALL)
    maybe_add_call_vops (as_a <gcall *> (stmt));
  was_volatile = !!(build_flags & BF_VOLATILE);
  oldvop = build_flags & (BF_VDEF | BF_VUSE);

  /* Remove all use ops for things in op[nop].  */
  for (i = build_uses.length (); i-- > 0;)
    {
      tree *op = build_uses[i];
      puse = find_delink_use_op (stmt, op);
      use = puse ? *puse : NULL;
      if (!puse)
	/* Normally we should have found the old useop cache.  But debug
	   statements might change from SOURCE_BIND (without opcache)
	   to DEBUG_BIND (with opcache), so accept that here.  */
	gcc_assert(is_gimple_debug (stmt));
      else if (i)
	free_stmt_use_op (puse);
    }
  cleanup_build_arrays ();

  *pop = val;

  /* Now inspect the new value.  */
  start_ssa_stmt_operands ();
  get_expr_operands (stmt, gimple_op_ptr (stmt, nop), flags);
  if (nop == 1 && gimple_code (stmt) == GIMPLE_CALL)
    maybe_add_call_vops (as_a <gcall *> (stmt));
  newvop = build_flags & (BF_VDEF | BF_VUSE);

  /* If we want to reuse an useop, do it now before diddle_vops,
     as that one might free useops, including the one where *puse
     points into.  This is useless work if it turns out that
     we need to revisit all operands, but that doesn't occur very
     often.  */
  i = 0;
  if (use)
    {
      if (i < build_uses.length ())
	{
	  use->use_ptr.use = build_uses[i];
	  link_imm_use_stmt (&(use->use_ptr), *build_uses[i], stmt);
	  i++;
	}
      else
	free_stmt_use_op (puse);
    }

  /* If the op was volatile and now isn't we need to recheck everything.  */
  if (was_volatile && !(build_flags & BF_VOLATILE))
    {
      cleanup_build_arrays ();
      return 1;
    }

  if (diddle_vops (stmt, oldvop, newvop, nop) < 0)
    {
      cleanup_build_arrays ();
      return 1;
    }

  for (; i < build_uses.length (); i++)
    {
      tree *op = build_uses[i];
      puse = find_delink_use_op (stmt, op);
      /* All ops should be new (or removed above), otherwise
         we'd create strange sharing.  */
      gcc_assert (!puse);
      prepend_use_op (stmt, op);
    }

  if (build_flags & BF_VOLATILE)
    gimple_set_has_volatile_ops (stmt, true);
  if (build_flags & BF_RENAME)
    cfun->gimple_df->ssa_renaming_needed = 1;
  if (build_flags & BF_RENAME_VOP)
    cfun->gimple_df->rename_vops = 1;

  cleanup_build_arrays ();
  return 0;
}

void
gimple_set_vuse (gimple *stmt, tree vuse)
{
  gimple_statement_with_memory_ops *mem_ops_stmt =
    as_a <gimple_statement_with_memory_ops *> (stmt);
  if (!flag_try_patch || !stmt->bb || !ssa_operands_active (cfun))
    mem_ops_stmt->vuse = vuse;
  else
    {
      tree *pop = &mem_ops_stmt->vuse;

      use_optype_p *puse, use;
      if (*pop && gimple_use_ops (stmt))
	{
	  puse = &mem_ops_stmt->use_ops;
	  delink_imm_use (&((*puse)->use_ptr));
	  use = *puse;
	}
      else
	use = NULL;

      *pop = vuse;

      if (vuse)
	{
	  if (!use)
	    prepend_use_op (stmt, pop);
	  else
	    link_imm_use_stmt (&(use->use_ptr), *pop, stmt);
	}
      else if (use)
	free_stmt_use_op (puse);
    }
}

static void
do_change_in_op (gimple *gs, tree *op_ptr, tree *pop, tree val)
{
  unsigned i = op_ptr - gimple_op_ptr (gs, 0);
  gcc_checking_assert (i < gimple_num_ops (gs));
  if (gimple_code (gs) != GIMPLE_DEBUG
      /* GIMPLE_DEBUG only has opcache for debug_bind and operand 1! .*/
      || (gimple_debug_bind_p (gs) && i == 1))
    {
      //tree old = *pop;
      int flags = opf_use;
      gasm *asmstmt;
      if ((i == 0 && (is_gimple_assign (gs) || is_gimple_call (gs)))
	  || ((asmstmt = dyn_cast<gasm *>(gs))
	      && i < gimple_asm_noutputs (asmstmt)))
	flags = opf_def;
      if (gimple_code (gs) == GIMPLE_DEBUG)
	flags |= opf_no_vops;
      if (exchange_complex_op (gs, pop, val, i, flags))
	{
	  /*fprintf (stderr, " XXX replace ");
	    print_generic_expr (stderr, old, TDF_VOPS|TDF_MEMSYMS);
	    fprintf (stderr, " with ");
	    print_generic_expr (stderr, val, TDF_VOPS|TDF_MEMSYMS);
	    fprintf (stderr, " in ");
	    print_gimple_stmt (stderr, gs, 0, 0);*/
	  update_stmt_for_real (gs);
	}
    }
  else
    *pop = val;
}

void
gimple_change_in_op (gimple *gs, tree *op_ptr, tree *pop, tree val)
{
  if (!flag_try_patch || !gs->bb || !ssa_operands_active (cfun))
    *pop = val;
  else
    do_change_in_op (gs, op_ptr, pop, val);
}

void
gimple_set_op_update (gimple *gs, unsigned i, tree val)
{
  tree *pop = gimple_op_ptr (gs, i);
  if (!flag_try_patch || !gs->bb || !ssa_operands_active (cfun))
    *pop = val;
  else
    do_change_in_op (gs, pop, pop, val);
}

/* Swap operands EXP0 and EXP1 in statement STMT.  No attempt is done
   to test the validity of the swap operation.  */

void
swap_ssa_operands (gimple *stmt, tree *exp0, tree *exp1)
{
  tree op0, op1;
  op0 = *exp0;
  op1 = *exp1;

  if (op0 != op1)
    {
      /* Attempt to preserve the relative positions of these two operands in
	 their * respective immediate use lists by adjusting their use pointer
	 to point to the new operand position.  */
      use_optype_p use0, use1, ptr;
      use0 = use1 = NULL;

      /* Find the 2 operands in the cache, if they are there.  */
      for (ptr = gimple_use_ops (stmt); ptr; ptr = ptr->next)
	if (USE_OP_PTR (ptr)->use == exp0)
	  {
	    use0 = ptr;
	    break;
	  }

      for (ptr = gimple_use_ops (stmt); ptr; ptr = ptr->next)
	if (USE_OP_PTR (ptr)->use == exp1)
	  {
	    use1 = ptr;
	    break;
	  }

      /* And adjust their location to point to the new position of the
         operand.  */
      if (use0)
	USE_OP_PTR (use0)->use = exp1;
      if (use1)
	USE_OP_PTR (use1)->use = exp0;

      /* Now swap the data.  */
      *exp0 = op1;
      *exp1 = op0;
    }
}


/* Scan the immediate_use list for VAR making sure its linked properly.
   Return TRUE if there is a problem and emit an error message to F.  */

DEBUG_FUNCTION bool
verify_imm_links (FILE *f, tree var)
{
  use_operand_p ptr, prev, list;
  unsigned int count;

  gcc_assert (TREE_CODE (var) == SSA_NAME);

  list = &(SSA_NAME_IMM_USE_NODE (var));
  gcc_assert (list->use == NULL);

  if (list->prev == NULL)
    {
      gcc_assert (list->next == NULL);
      return false;
    }

  prev = list;
  count = 0;
  for (ptr = list->next; ptr != list; )
    {
      if (prev != ptr->prev)
	{
	  fprintf (f, "prev != ptr->prev\n");
	  goto error;
	}

      if (ptr->use == NULL)
	{
	  fprintf (f, "ptr->use == NULL\n");
	  goto error; /* 2 roots, or SAFE guard node.  */
	}
      else if (*(ptr->use) != var)
	{
	  fprintf (f, "*(ptr->use) != var\n");
	  goto error;
	}

      prev = ptr;
      ptr = ptr->next;

      count++;
      if (count == 0)
	{
	  fprintf (f, "number of immediate uses doesn't fit unsigned int\n");
	  goto error;
	}
    }

  /* Verify list in the other direction.  */
  prev = list;
  for (ptr = list->prev; ptr != list; )
    {
      if (prev != ptr->next)
	{
	  fprintf (f, "prev != ptr->next\n");
	  goto error;
	}
      prev = ptr;
      ptr = ptr->prev;
      if (count == 0)
	{
	  fprintf (f, "count-- < 0\n");
	  goto error;
	}
      count--;
    }

  if (count != 0)
    {
      fprintf (f, "count != 0\n");
      goto error;
    }

  return false;

 error:
  if (ptr->loc.stmt && gimple_modified_p (ptr->loc.stmt))
    {
      fprintf (f, " STMT MODIFIED. - <%p> ", (void *)ptr->loc.stmt);
      print_gimple_stmt (f, ptr->loc.stmt, 0, TDF_SLIM);
    }
  fprintf (f, " IMM ERROR : (use_p : tree - %p:%p)", (void *)ptr,
	   (void *)ptr->use);
  print_generic_expr (f, USE_FROM_PTR (ptr), TDF_SLIM);
  fprintf (f, "\n");
  return true;
}


/* Dump all the immediate uses to FILE.  */

void
dump_immediate_uses_for (FILE *file, tree var)
{
  imm_use_iterator iter;
  use_operand_p use_p;

  gcc_assert (var && TREE_CODE (var) == SSA_NAME);

  print_generic_expr (file, var, TDF_SLIM);
  fprintf (file, " : -->");
  if (has_zero_uses (var))
    fprintf (file, " no uses.\n");
  else
    if (has_single_use (var))
      fprintf (file, " single use.\n");
    else
      fprintf (file, "%d uses.\n", num_imm_uses (var));

  FOR_EACH_IMM_USE_FAST (use_p, iter, var)
    {
      if (use_p->loc.stmt == NULL && use_p->use == NULL)
        fprintf (file, "***end of stmt iterator marker***\n");
      else
	if (!is_gimple_reg (USE_FROM_PTR (use_p)))
	  print_gimple_stmt (file, USE_STMT (use_p), 0, TDF_VOPS|TDF_MEMSYMS);
	else
	  print_gimple_stmt (file, USE_STMT (use_p), 0, TDF_SLIM);
    }
  fprintf (file, "\n");
}


/* Dump all the immediate uses to FILE.  */

void
dump_immediate_uses (FILE *file)
{
  tree var;
  unsigned int x;

  fprintf (file, "Immediate_uses: \n\n");
  FOR_EACH_SSA_NAME (x, var, cfun)
    {
      dump_immediate_uses_for (file, var);
    }
}


/* Dump def-use edges on stderr.  */

DEBUG_FUNCTION void
debug_immediate_uses (void)
{
  dump_immediate_uses (stderr);
}


/* Dump def-use edges on stderr.  */

DEBUG_FUNCTION void
debug_immediate_uses_for (tree var)
{
  dump_immediate_uses_for (stderr, var);
}


/* Unlink STMTs virtual definition from the IL by propagating its use.  */

void
unlink_stmt_vdef (gimple *stmt)
{
  use_operand_p use_p;
  imm_use_iterator iter;
  gimple *use_stmt;
  tree vdef = gimple_vdef (stmt);
  tree vuse = gimple_vuse (stmt);

  if (!vdef
      || TREE_CODE (vdef) != SSA_NAME)
    return;

  FOR_EACH_IMM_USE_STMT (use_stmt, iter, vdef)
    {
      FOR_EACH_IMM_USE_ON_STMT (use_p, iter)
	SET_USE (use_p, vuse);
    }

  if (SSA_NAME_OCCURS_IN_ABNORMAL_PHI (vdef))
    SSA_NAME_OCCURS_IN_ABNORMAL_PHI (vuse) = 1;
}

/* Return true if the var whose chain of uses starts at PTR has a
   single nondebug use.  Set USE_P and STMT to that single nondebug
   use, if so, or to NULL otherwise.  */
bool
single_imm_use_1 (const ssa_use_operand_t *head,
		  use_operand_p *use_p, gimple **stmt)
{
  ssa_use_operand_t *ptr, *single_use = 0;

  for (ptr = head->next; ptr != head; ptr = ptr->next)
    if (USE_STMT(ptr) && !is_gimple_debug (USE_STMT (ptr)))
      {
	if (single_use)
	  {
	    single_use = NULL;
	    break;
	  }
	single_use = ptr;
      }

  if (use_p)
    *use_p = single_use;

  if (stmt)
    *stmt = single_use ? single_use->loc.stmt : NULL;

  return single_use;
}
