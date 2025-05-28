/* RTL dead code elimination.
   Copyright (C) 2005-2025 Free Software Foundation, Inc.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include <iostream>
#include <ostream>
#define INCLUDE_ALGORITHM
#define INCLUDE_FUNCTIONAL
#define INCLUDE_ARRAY
#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "rtl.h"
#include "tree.h"
#include "predict.h"
#include "df.h"
#include "rtl-ssa.h"
#include "memmodel.h"
#include "tm_p.h"
#include "emit-rtl.h" /* FIXME: Can go away once crtl is moved to rtl.h.  */
#include "cfgrtl.h"
#include "cfgbuild.h"
#include "cfgcleanup.h"
#include "dce.h"
#include "valtrack.h"
#include "tree-pass.h"
#include "dbgcnt.h"
#include "rtl-iter.h"
#include <unordered_set>

using namespace rtl_ssa;


/* -------------------------------------------------------------------------
   Core mark/delete routines
   ------------------------------------------------------------------------- */

/* True if we are invoked while the df engine is running; in this case,
   we don't want to reenter it.  */
static bool df_in_progress = false;

/* True if we are allowed to alter the CFG in this pass.  */
static bool can_alter_cfg = false;

/* Instructions that have been marked but whose dependencies have not
   yet been processed.  */
static vec<rtx_insn *> worklist;

/* Bitmap of instructions marked as needed indexed by INSN_UID.  */
static sbitmap marked;

/* Bitmap obstacks used for block processing by the fast algorithm.  */
static bitmap_obstack dce_blocks_bitmap_obstack;
static bitmap_obstack dce_tmp_bitmap_obstack;

static bool find_call_stack_args (rtx_call_insn *, bool, bool, bitmap);

/* A subroutine for which BODY is part of the instruction being tested;
   either the top-level pattern, or an element of a PARALLEL.  The
   instruction is known not to be a bare USE or CLOBBER.  */

static bool
deletable_insn_p_1 (rtx body)
{
  switch (GET_CODE (body))
    {
    case PREFETCH:
    case TRAP_IF:
      /* The UNSPEC case was added here because the ia-64 claims that
	 USEs do not work after reload and generates UNSPECS rather
	 than USEs.  Since dce is run after reload we need to avoid
	 deleting these even if they are dead.  If it turns out that
	 USEs really do work after reload, the ia-64 should be
	 changed, and the UNSPEC case can be removed.  */
    case UNSPEC:
      return false;

    default:
      return !volatile_refs_p (body);
    }
}

/* Don't delete calls that may throw if we cannot do so.  */

static bool
can_delete_call (rtx_insn *insn)
{
  if (cfun->can_delete_dead_exceptions && can_alter_cfg)
    return true;
  if (!insn_nothrow_p (insn))
    return false;
  if (can_alter_cfg)
    return true;
  /* If we can't alter cfg, even when the call can't throw exceptions, it
     might have EDGE_ABNORMAL_CALL edges and so we shouldn't delete such
     calls.  */
  gcc_assert (CALL_P (insn));
  if (BLOCK_FOR_INSN (insn) && BB_END (BLOCK_FOR_INSN (insn)) == insn)
    {
      edge e;
      edge_iterator ei;

      FOR_EACH_EDGE (e, ei, BLOCK_FOR_INSN (insn)->succs)
	if ((e->flags & EDGE_ABNORMAL_CALL) != 0)
	  return false;
    }
  return true;
}

static bool inside_ud = false;

/* Return true if INSN is a normal instruction that can be deleted by
   the DCE pass.  */

static bool
deletable_insn_p (rtx_insn *insn, bool fast, bitmap arg_stores)
{
  rtx body, x;
  int i;
  df_ref def;

  if (CALL_P (insn)
      /* We cannot delete calls inside of the recursive dce because
	 this may cause basic blocks to be deleted and this messes up
	 the rest of the stack of optimization passes.  */
      && (!df_in_progress)
      /* We cannot delete pure or const sibling calls because it is
	 hard to see the result.  */
      && (!SIBLING_CALL_P (insn))
      /* We can delete dead const or pure calls as long as they do not
         infinite loop.  */
      && (RTL_CONST_OR_PURE_CALL_P (insn)
	  && !RTL_LOOPING_CONST_OR_PURE_CALL_P (insn))
      /* Don't delete calls that may throw if we cannot do so.  */
      && can_delete_call (insn))
    return find_call_stack_args (as_a <rtx_call_insn *> (insn), false,
				 fast, arg_stores);

  /* Don't delete jumps, notes and the like.  */
  if (!NONJUMP_INSN_P (insn))
    return false;

  /* Don't delete insns that may throw if we cannot do so.  */
  if (!(cfun->can_delete_dead_exceptions && can_alter_cfg)
      && !insn_nothrow_p (insn))
    return false;

  /* If INSN sets a global_reg, leave it untouched.  */
  FOR_EACH_INSN_DEF (def, insn)
    if (HARD_REGISTER_NUM_P (DF_REF_REGNO (def))
	&& global_regs[DF_REF_REGNO (def)])
      return false;
    /* Initialization of pseudo PIC register should never be removed.  */
    else if (DF_REF_REG (def) == pic_offset_table_rtx
	     && REGNO (pic_offset_table_rtx) >= FIRST_PSEUDO_REGISTER)
      return false;

  /* Callee-save restores are needed.  */
  if (RTX_FRAME_RELATED_P (insn)
      && crtl->shrink_wrapped_separate
      && find_reg_note (insn, REG_CFA_RESTORE, NULL))
    return false;

  body = PATTERN (insn);
  switch (GET_CODE (body))
    {
    case USE:
    case VAR_LOCATION:
      return false;

    case CLOBBER:
      if (fast)
	{
	  /* A CLOBBER of a dead pseudo register serves no purpose.
	     That is not necessarily true for hard registers until
	     after reload.  */
	  x = XEXP (body, 0);
	  return REG_P (x) && (!HARD_REGISTER_P (x) || reload_completed);
	}
      else
	/* Because of the way that use-def chains are built, it is not
	   possible to tell if the clobber is dead because it can
	   never be the target of a use-def chain.  */
	return false;

    case PARALLEL:
      for (i = XVECLEN (body, 0) - 1; i >= 0; i--)
	if (!deletable_insn_p_1 (XVECEXP (body, 0, i)))
	  return false;
      return true;

    default:
      return deletable_insn_p_1 (body);
    }
}


/* Return true if INSN has been marked as needed.  */

static inline int
marked_insn_p (rtx_insn *insn)
{
  /* Artificial defs are always needed and they do not have an insn.
     We should never see them here.  */
  gcc_assert (insn);
  return bitmap_bit_p (marked, INSN_UID (insn));
}


/* If INSN has not yet been marked as needed, mark it now, and add it to
   the worklist.  */

static void
mark_insn (rtx_insn *insn, bool fast)
{
  if (!marked_insn_p (insn))
    {
      if (!fast)
	worklist.safe_push (insn);
      bitmap_set_bit (marked, INSN_UID (insn));
      if (dump_file)
	fprintf (dump_file, "  Adding insn %d to worklist\n", INSN_UID (insn));
      if (CALL_P (insn)
	  && !df_in_progress
	  && !SIBLING_CALL_P (insn)
	  && (RTL_CONST_OR_PURE_CALL_P (insn)
	      && !RTL_LOOPING_CONST_OR_PURE_CALL_P (insn))
	  && can_delete_call (insn))
	find_call_stack_args (as_a <rtx_call_insn *> (insn), true, fast, NULL);
    }
}


/* A note_stores callback used by mark_nonreg_stores.  DATA is the
   instruction containing DEST.  */

static void
mark_nonreg_stores_1 (rtx dest, const_rtx pattern, void *data)
{
  if (GET_CODE (pattern) != CLOBBER && !REG_P (dest))
    mark_insn ((rtx_insn *) data, true);
}


/* A note_stores callback used by mark_nonreg_stores.  DATA is the
   instruction containing DEST.  */

static void
mark_nonreg_stores_2 (rtx dest, const_rtx pattern, void *data)
{
  if (GET_CODE (pattern) != CLOBBER && !REG_P (dest))
    mark_insn ((rtx_insn *) data, false);
}


/* Mark INSN if it stores to a non-register destination.  */

static void
mark_nonreg_stores (rtx_insn *insn, bool fast)
{
  if (fast)
    note_stores (insn, mark_nonreg_stores_1, insn);
  else
    note_stores (insn, mark_nonreg_stores_2, insn);
}


/* Return true if a store to SIZE bytes, starting OFF bytes from stack pointer,
   is a call argument store, and clear corresponding bits from SP_BYTES
   bitmap if it is.  */

static bool
check_argument_store (HOST_WIDE_INT size, HOST_WIDE_INT off,
		      HOST_WIDE_INT min_sp_off, HOST_WIDE_INT max_sp_off,
		      bitmap sp_bytes)
{
  HOST_WIDE_INT byte;
  for (byte = off; byte < off + size; byte++)
    {
      if (byte < min_sp_off
	  || byte >= max_sp_off
	  || !bitmap_clear_bit (sp_bytes, byte - min_sp_off))
	return false;
    }
  return true;
}

/* If MEM has sp address, return 0, if it has sp + const address,
   return that const, if it has reg address where reg is set to sp + const
   and FAST is false, return const, otherwise return
   INTTYPE_MINUMUM (HOST_WIDE_INT).  */

static HOST_WIDE_INT
sp_based_mem_offset (rtx_call_insn *call_insn, const_rtx mem, bool fast)
{
  HOST_WIDE_INT off = 0;
  rtx addr = XEXP (mem, 0);
  if (GET_CODE (addr) == PLUS
      && REG_P (XEXP (addr, 0))
      && CONST_INT_P (XEXP (addr, 1)))
    {
      off = INTVAL (XEXP (addr, 1));
      addr = XEXP (addr, 0);
    }
  if (addr == stack_pointer_rtx)
    return off;

  if (!REG_P (addr) || fast)
    return INTTYPE_MINIMUM (HOST_WIDE_INT);

  /* If not fast, use chains to see if addr wasn't set to sp + offset.  */
  df_ref use;
  FOR_EACH_INSN_USE (use, call_insn)
  if (rtx_equal_p (addr, DF_REF_REG (use)))
    break;

  if (use == NULL)
    return INTTYPE_MINIMUM (HOST_WIDE_INT);

  struct df_link *defs;
  for (defs = DF_REF_CHAIN (use); defs; defs = defs->next)
    if (! DF_REF_IS_ARTIFICIAL (defs->ref))
      break;

  if (defs == NULL)
    return INTTYPE_MINIMUM (HOST_WIDE_INT);

  rtx set = single_set (DF_REF_INSN (defs->ref));
  if (!set)
    return INTTYPE_MINIMUM (HOST_WIDE_INT);

  if (GET_CODE (SET_SRC (set)) != PLUS
      || XEXP (SET_SRC (set), 0) != stack_pointer_rtx
      || !CONST_INT_P (XEXP (SET_SRC (set), 1)))
    return INTTYPE_MINIMUM (HOST_WIDE_INT);

  off += INTVAL (XEXP (SET_SRC (set), 1));
  return off;
}

/* Data for check_argument_load called via note_uses.  */
struct check_argument_load_data {
  bitmap sp_bytes;
  HOST_WIDE_INT min_sp_off, max_sp_off;
  rtx_call_insn *call_insn;
  bool fast;
  bool load_found;
};

/* Helper function for find_call_stack_args.  Check if there are
   any loads from the argument slots in between the const/pure call
   and store to the argument slot, set LOAD_FOUND if any is found.  */

static void
check_argument_load (rtx *loc, void *data)
{
  struct check_argument_load_data *d
    = (struct check_argument_load_data *) data;
  subrtx_iterator::array_type array;
  FOR_EACH_SUBRTX (iter, array, *loc, NONCONST)
    {
      const_rtx mem = *iter;
      HOST_WIDE_INT size;
      if (MEM_P (mem)
	  && MEM_SIZE_KNOWN_P (mem)
	  && MEM_SIZE (mem).is_constant (&size))
	{
	  HOST_WIDE_INT off = sp_based_mem_offset (d->call_insn, mem, d->fast);
	  if (off != INTTYPE_MINIMUM (HOST_WIDE_INT)
	      && off < d->max_sp_off
	      && off + size > d->min_sp_off)
	    for (HOST_WIDE_INT byte = MAX (off, d->min_sp_off);
		 byte < MIN (off + size, d->max_sp_off); byte++)
	      if (bitmap_bit_p (d->sp_bytes, byte - d->min_sp_off))
		{
		  d->load_found = true;
		  return;
		}
	}
    }
}

/* Try to find all stack stores of CALL_INSN arguments if
   ACCUMULATE_OUTGOING_ARGS.  If all stack stores have been found
   and it is therefore safe to eliminate the call, return true,
   otherwise return false.  This function should be first called
   with DO_MARK false, and only when the CALL_INSN is actually
   going to be marked called again with DO_MARK true.  */

static bool
find_call_stack_args (rtx_call_insn *call_insn, bool do_mark, bool fast,
		      bitmap arg_stores)
{
  rtx p;
  rtx_insn *insn, *prev_insn;
  bool ret;
  HOST_WIDE_INT min_sp_off, max_sp_off;
  bitmap sp_bytes;

  gcc_assert (CALL_P (call_insn));
  if (!ACCUMULATE_OUTGOING_ARGS)
    return true;

  if (!do_mark)
    {
      gcc_assert (arg_stores);
      bitmap_clear (arg_stores);
    }

  min_sp_off = INTTYPE_MAXIMUM (HOST_WIDE_INT);
  max_sp_off = 0;

  /* First determine the minimum and maximum offset from sp for
     stored arguments.  */
  for (p = CALL_INSN_FUNCTION_USAGE (call_insn); p; p = XEXP (p, 1))
    if (GET_CODE (XEXP (p, 0)) == USE
	&& MEM_P (XEXP (XEXP (p, 0), 0)))
      {
	rtx mem = XEXP (XEXP (p, 0), 0);
	HOST_WIDE_INT size;
	if (!MEM_SIZE_KNOWN_P (mem) || !MEM_SIZE (mem).is_constant (&size))
	  return false;
	HOST_WIDE_INT off = sp_based_mem_offset (call_insn, mem, fast);
	if (off == INTTYPE_MINIMUM (HOST_WIDE_INT))
	  return false;
	min_sp_off = MIN (min_sp_off, off);
	max_sp_off = MAX (max_sp_off, off + size);
      }

  if (min_sp_off >= max_sp_off)
    return true;
  sp_bytes = BITMAP_ALLOC (NULL);

  /* Set bits in SP_BYTES bitmap for bytes relative to sp + min_sp_off
     which contain arguments.  Checking has been done in the previous
     loop.  */
  for (p = CALL_INSN_FUNCTION_USAGE (call_insn); p; p = XEXP (p, 1))
    if (GET_CODE (XEXP (p, 0)) == USE
	&& MEM_P (XEXP (XEXP (p, 0), 0)))
      {
	rtx mem = XEXP (XEXP (p, 0), 0);
	/* Checked in the previous iteration.  */
	HOST_WIDE_INT size = MEM_SIZE (mem).to_constant ();
	HOST_WIDE_INT off = sp_based_mem_offset (call_insn, mem, fast);
	gcc_checking_assert (off != INTTYPE_MINIMUM (HOST_WIDE_INT));
	for (HOST_WIDE_INT byte = off; byte < off + size; byte++)
	  if (!bitmap_set_bit (sp_bytes, byte - min_sp_off))
	    gcc_unreachable ();
      }

  /* Walk backwards, looking for argument stores.  The search stops
     when seeing another call, sp adjustment, memory store other than
     argument store or a read from an argument stack slot.  */
  struct check_argument_load_data data
    = { sp_bytes, min_sp_off, max_sp_off, call_insn, fast, false };
  ret = false;
  for (insn = PREV_INSN (call_insn); insn; insn = prev_insn)
    {
      if (insn == BB_HEAD (BLOCK_FOR_INSN (call_insn)))
	prev_insn = NULL;
      else
	prev_insn = PREV_INSN (insn);

      if (CALL_P (insn))
	break;

      if (!NONDEBUG_INSN_P (insn))
	continue;

      rtx set = single_set (insn);
      if (!set || SET_DEST (set) == stack_pointer_rtx)
	break;

      note_uses (&PATTERN (insn), check_argument_load, &data);
      if (data.load_found)
	break;

      if (!MEM_P (SET_DEST (set)))
	continue;

      rtx mem = SET_DEST (set);
      HOST_WIDE_INT off = sp_based_mem_offset (call_insn, mem, fast);
      if (off == INTTYPE_MINIMUM (HOST_WIDE_INT))
	break;

      HOST_WIDE_INT size;
      if (!MEM_SIZE_KNOWN_P (mem)
	  || !MEM_SIZE (mem).is_constant (&size)
	  || !check_argument_store (size, off, min_sp_off,
				    max_sp_off, sp_bytes))
	break;

      if (!deletable_insn_p (insn, fast, NULL))
	break;

      if (do_mark)
	mark_insn (insn, fast);
      else
	bitmap_set_bit (arg_stores, INSN_UID (insn));

      if (bitmap_empty_p (sp_bytes))
	{
	  ret = true;
	  break;
	}
    }

  BITMAP_FREE (sp_bytes);
  if (!ret && arg_stores)
    bitmap_clear (arg_stores);

  return ret;
}


/* Remove all REG_EQUAL and REG_EQUIV notes referring to the registers INSN
   writes to.  */

static void
remove_reg_equal_equiv_notes_for_defs (rtx_insn *insn)
{
  df_ref def;

  FOR_EACH_INSN_DEF (def, insn)
    remove_reg_equal_equiv_notes_for_regno (DF_REF_REGNO (def));
}

/* Scan all BBs for debug insns and reset those that reference values
   defined in unmarked insns.  */

static void
reset_unmarked_insns_debug_uses (void)
{
  basic_block bb;
  rtx_insn *insn, *next;

  FOR_EACH_BB_REVERSE_FN (bb, cfun)
    FOR_BB_INSNS_REVERSE_SAFE (bb, insn, next)
      if (DEBUG_INSN_P (insn))
	{
	  df_ref use;

	  FOR_EACH_INSN_USE (use, insn)
	    {
	      struct df_link *defs;
	      for (defs = DF_REF_CHAIN (use); defs; defs = defs->next)
		{
		  rtx_insn *ref_insn;
		  if (DF_REF_IS_ARTIFICIAL (defs->ref))
		    continue;
		  ref_insn = DF_REF_INSN (defs->ref);
		  if (!marked_insn_p (ref_insn))
		    break;
		}
	      if (!defs)
		continue;
	      /* ??? FIXME could we propagate the values assigned to
		 each of the DEFs?  */
	      INSN_VAR_LOCATION_LOC (insn) = gen_rtx_UNKNOWN_VAR_LOC ();
	      df_insn_rescan_debug_internal (insn);
	      break;
	    }
	}
}

/* Delete every instruction that hasn't been marked.  */
static void
delete_unmarked_insns (void)
{
  basic_block bb;
  rtx_insn *insn, *next;
  bool must_clean = false;
  bool any = false;

  FOR_EACH_BB_REVERSE_FN (bb, cfun)
    FOR_BB_INSNS_REVERSE_SAFE (bb, insn, next)
      if (NONDEBUG_INSN_P (insn))
	{
	  rtx turn_into_use = NULL_RTX;

	  /* Always delete no-op moves.  */
	  if (noop_move_p (insn)
	      /* Unless the no-op move can throw and we are not allowed
		 to alter cfg.  */
	      && (!cfun->can_throw_non_call_exceptions
		  || (cfun->can_delete_dead_exceptions && can_alter_cfg)
		  || insn_nothrow_p (insn)))
	    {
	      if (RTX_FRAME_RELATED_P (insn))
		turn_into_use
		  = find_reg_note (insn, REG_CFA_RESTORE, NULL);
	      if (turn_into_use && REG_P (XEXP (turn_into_use, 0)))
		turn_into_use = XEXP (turn_into_use, 0);
	      else
		turn_into_use = NULL_RTX;
	    }

	  /* Otherwise rely only on the DCE algorithm.  */
	  else if (marked_insn_p (insn))
	    continue;

	  /* Beware that reaching a dbg counter limit here can result
	     in miscompiled file.  This occurs when a group of insns
	     must be deleted together, typically because the kept insn
	     depends on the output from the deleted insn.  Deleting
	     this insns in reverse order (both at the bb level and
	     when looking at the blocks) minimizes this, but does not
	     eliminate it, since it is possible for the using insn to
	     be top of a block and the producer to be at the bottom of
	     the block.  However, in most cases this will only result
	     in an uninitialized use of an insn that is dead anyway.

	     However, there is one rare case that will cause a
	     miscompile: deletion of non-looping pure and constant
	     calls on a machine where ACCUMULATE_OUTGOING_ARGS is true.
	     In this case it is possible to remove the call, but leave
	     the argument pushes to the stack.  Because of the changes
	     to the stack pointer, this will almost always lead to a
	     miscompile.  */
	  if (!dbg_cnt (dce))
	    continue;

	  if (dump_file)
	    fprintf (dump_file, "DCE: Deleting insn %d\n", INSN_UID (insn));

	  /* Before we delete the insn we have to remove the REG_EQUAL notes
	     for the destination regs in order to avoid dangling notes.  */
	  remove_reg_equal_equiv_notes_for_defs (insn);

	  if (turn_into_use)
	    {
	      /* Don't remove frame related noop moves if they cary
		 REG_CFA_RESTORE note, while we don't need to emit any code,
		 we need it to emit the CFI restore note.  */
	      PATTERN (insn)
		= gen_rtx_USE (GET_MODE (turn_into_use), turn_into_use);
	      INSN_CODE (insn) = -1;
	      df_insn_rescan (insn);
	    }
	  else
	    /* Now delete the insn.  */
      if (inside_ud) {
        debug(insn);
      }
      any = true;
	    must_clean |= delete_insn_and_edges (insn);
	}

  if (any && inside_ud) {
    std::cerr << "\033[31m" << "ud was able to delete more insns..." << "\033[0m \n";
  }

  /* Deleted a pure or const call.  */
  if (must_clean)
    {
      gcc_assert (can_alter_cfg);
      delete_unreachable_blocks ();
      free_dominance_info (CDI_DOMINATORS);
    }
}


/* Go through the instructions and mark those whose necessity is not
   dependent on inter-instruction information.  Make sure all other
   instructions are not marked.  */

static void
prescan_insns_for_dce (bool fast)
{
  basic_block bb;
  rtx_insn *insn, *prev;
  bitmap arg_stores = NULL;

  if (dump_file)
    fprintf (dump_file, "Finding needed instructions:\n");

  if (!df_in_progress && ACCUMULATE_OUTGOING_ARGS)
    arg_stores = BITMAP_ALLOC (NULL);

  FOR_EACH_BB_FN (bb, cfun)
    {
      FOR_BB_INSNS_REVERSE_SAFE (bb, insn, prev)
	if (NONDEBUG_INSN_P (insn))
	  {
	    /* Don't mark argument stores now.  They will be marked
	       if needed when the associated CALL is marked.  */
	    if (arg_stores && bitmap_bit_p (arg_stores, INSN_UID (insn)))
	      continue;
	    if (deletable_insn_p (insn, fast, arg_stores))
	      mark_nonreg_stores (insn, fast);
	    else
	      mark_insn (insn, fast);
	  }
      /* find_call_stack_args only looks at argument stores in the
	 same bb.  */
      if (arg_stores)
	bitmap_clear (arg_stores);
    }

  if (arg_stores)
    BITMAP_FREE (arg_stores);

  if (dump_file)
    fprintf (dump_file, "Finished finding needed instructions:\n");
}


/* UD-based DSE routines. */

/* Mark instructions that define artificially-used registers, such as
   the frame pointer and the stack pointer.  */

static void
mark_artificial_uses (void)
{
  basic_block bb;
  struct df_link *defs;
  df_ref use;

  FOR_ALL_BB_FN (bb, cfun)
    FOR_EACH_ARTIFICIAL_USE (use, bb->index)
      for (defs = DF_REF_CHAIN (use); defs; defs = defs->next)
	if (!DF_REF_IS_ARTIFICIAL (defs->ref))
	  mark_insn (DF_REF_INSN (defs->ref), false);
}


/* Mark every instruction that defines a register value that INSN uses.  */

static void
mark_reg_dependencies (rtx_insn *insn)
{
  struct df_link *defs;
  df_ref use;

  if (DEBUG_INSN_P (insn))
    return;

  FOR_EACH_INSN_USE (use, insn)
    {
      if (dump_file)
	{
	  fprintf (dump_file, "Processing use of ");
	  print_simple_rtl (dump_file, DF_REF_REG (use));
	  fprintf (dump_file, " in insn %d:\n", INSN_UID (insn));
	}
      for (defs = DF_REF_CHAIN (use); defs; defs = defs->next)
	if (! DF_REF_IS_ARTIFICIAL (defs->ref))
	  mark_insn (DF_REF_INSN (defs->ref), false);
    }
}


/* Initialize global variables for a new DCE pass.  */

static void
init_dce (bool fast)
{
  if (!df_in_progress)
    {
      if (!fast)
	{
	  df_set_flags (DF_RD_PRUNE_DEAD_DEFS);
	  df_chain_add_problem (DF_UD_CHAIN);
	}
      df_analyze ();
    }

  if (dump_file)
    df_dump (dump_file);

  if (fast)
    {
      bitmap_obstack_initialize (&dce_blocks_bitmap_obstack);
      bitmap_obstack_initialize (&dce_tmp_bitmap_obstack);
      can_alter_cfg = false;
    }
  else
    can_alter_cfg = true;

  marked = sbitmap_alloc (get_max_uid () + 1);
  bitmap_clear (marked);
}


/* Free the data allocated by init_dce.  */

static void
fini_dce (bool fast)
{
  sbitmap_free (marked);

  if (fast)
    {
      bitmap_obstack_release (&dce_blocks_bitmap_obstack);
      bitmap_obstack_release (&dce_tmp_bitmap_obstack);
    }
}


/* UD-chain based DCE.  */

static unsigned int
rest_of_handle_ud_dce (void)
{
  rtx_insn *insn;

  init_dce (false);

  prescan_insns_for_dce (false);
  mark_artificial_uses ();
  while (worklist.length () > 0)
    {
      insn = worklist.pop ();
      mark_reg_dependencies (insn);
    }
  worklist.release ();

  inside_ud = true;
  if (MAY_HAVE_DEBUG_BIND_INSNS)
    reset_unmarked_insns_debug_uses ();

  /* Before any insns are deleted, we must remove the chains since
     they are not bidirectional.  */
  df_remove_problem (df_chain);
  delete_unmarked_insns ();
  inside_ud = false;

  fini_dce (false);
  return 0;
}


namespace {

const pass_data pass_data_ud_rtl_dce =
{
  RTL_PASS, /* type */
  "ud_dce", /* name */
  OPTGROUP_NONE, /* optinfo_flags */
  TV_DCE, /* tv_id */
  0, /* properties_required */
  0, /* properties_provided */
  0, /* properties_destroyed */
  0, /* todo_flags_start */
  TODO_df_finish, /* todo_flags_finish */
};

class pass_ud_rtl_dce : public rtl_opt_pass
{
public:
  pass_ud_rtl_dce (gcc::context *ctxt)
    : rtl_opt_pass (pass_data_ud_rtl_dce, ctxt)
  {}

  /* opt_pass methods: */
  bool gate (function *) final override
    {
      return optimize > 1 && flag_dce && dbg_cnt (dce_ud);
    }

  unsigned int execute (function *) final override
    {
      return rest_of_handle_ud_dce ();
    }

}; // class pass_ud_rtl_dce

} // anon namespace

rtl_opt_pass *
make_pass_ud_rtl_dce (gcc::context *ctxt)
{
  return new pass_ud_rtl_dce (ctxt);
}


/* -------------------------------------------------------------------------
   Fast DCE functions
   ------------------------------------------------------------------------- */

/* Process basic block BB.  Return true if the live_in set has
   changed. REDO_OUT is true if the info at the bottom of the block
   needs to be recalculated before starting.  AU is the proper set of
   artificial uses.  Track global substitution of uses of dead pseudos
   in debug insns using GLOBAL_DEBUG.  */

static bool
word_dce_process_block (basic_block bb, bool redo_out,
			struct dead_debug_global *global_debug)
{
  bitmap local_live = BITMAP_ALLOC (&dce_tmp_bitmap_obstack);
  rtx_insn *insn;
  bool block_changed;
  struct dead_debug_local debug;

  if (redo_out)
    {
      /* Need to redo the live_out set of this block if when one of
	 the succs of this block has had a change in it live in
	 set.  */
      edge e;
      edge_iterator ei;
      df_confluence_function_n con_fun_n = df_word_lr->problem->con_fun_n;
      bitmap_clear (DF_WORD_LR_OUT (bb));
      FOR_EACH_EDGE (e, ei, bb->succs)
	(*con_fun_n) (e);
    }

  if (dump_file)
    {
      fprintf (dump_file, "processing block %d live out = ", bb->index);
      df_print_word_regset (dump_file, DF_WORD_LR_OUT (bb));
    }

  bitmap_copy (local_live, DF_WORD_LR_OUT (bb));
  dead_debug_local_init (&debug, NULL, global_debug);

  FOR_BB_INSNS_REVERSE (bb, insn)
    if (DEBUG_INSN_P (insn))
      {
	df_ref use;
	FOR_EACH_INSN_USE (use, insn)
	  if (DF_REF_REGNO (use) >= FIRST_PSEUDO_REGISTER
	      && known_eq (GET_MODE_SIZE (GET_MODE (DF_REF_REAL_REG (use))),
			   2 * UNITS_PER_WORD)
	      && !bitmap_bit_p (local_live, 2 * DF_REF_REGNO (use))
	      && !bitmap_bit_p (local_live, 2 * DF_REF_REGNO (use) + 1))
	    dead_debug_add (&debug, use, DF_REF_REGNO (use));
      }
    else if (INSN_P (insn))
      {
	bool any_changed;

	/* No matter if the instruction is needed or not, we remove
	   any regno in the defs from the live set.  */
	any_changed = df_word_lr_simulate_defs (insn, local_live);
	if (any_changed)
	  mark_insn (insn, true);

	/* On the other hand, we do not allow the dead uses to set
	   anything in local_live.  */
	if (marked_insn_p (insn))
	  df_word_lr_simulate_uses (insn, local_live);

	/* Insert debug temps for dead REGs used in subsequent debug
	   insns.  We may have to emit a debug temp even if the insn
	   was marked, in case the debug use was after the point of
	   death.  */
	if (debug.used && !bitmap_empty_p (debug.used))
	  {
	    df_ref def;

	    FOR_EACH_INSN_DEF (def, insn)
	      dead_debug_insert_temp (&debug, DF_REF_REGNO (def), insn,
				      marked_insn_p (insn)
				      && !control_flow_insn_p (insn)
				      ? DEBUG_TEMP_AFTER_WITH_REG_FORCE
				      : DEBUG_TEMP_BEFORE_WITH_VALUE);
	  }

	if (dump_file)
	  {
	    fprintf (dump_file, "finished processing insn %d live out = ",
		     INSN_UID (insn));
	    df_print_word_regset (dump_file, local_live);
	  }
      }

  block_changed = !bitmap_equal_p (local_live, DF_WORD_LR_IN (bb));
  if (block_changed)
    bitmap_copy (DF_WORD_LR_IN (bb), local_live);

  dead_debug_local_finish (&debug, NULL);
  BITMAP_FREE (local_live);
  return block_changed;
}


/* Process basic block BB.  Return true if the live_in set has
   changed. REDO_OUT is true if the info at the bottom of the block
   needs to be recalculated before starting.  AU is the proper set of
   artificial uses.  Track global substitution of uses of dead pseudos
   in debug insns using GLOBAL_DEBUG.  */

static bool
dce_process_block (basic_block bb, bool redo_out, bitmap au,
		   struct dead_debug_global *global_debug)
{
  bitmap local_live = BITMAP_ALLOC (&dce_tmp_bitmap_obstack);
  rtx_insn *insn;
  bool block_changed;
  df_ref def;
  struct dead_debug_local debug;

  if (redo_out)
    {
      /* Need to redo the live_out set of this block if when one of
	 the succs of this block has had a change in it live in
	 set.  */
      edge e;
      edge_iterator ei;
      df_confluence_function_n con_fun_n = df_lr->problem->con_fun_n;
      bitmap_clear (DF_LR_OUT (bb));
      FOR_EACH_EDGE (e, ei, bb->succs)
	(*con_fun_n) (e);
    }

  if (dump_file)
    {
      fprintf (dump_file, "processing block %d lr out = ", bb->index);
      df_print_regset (dump_file, DF_LR_OUT (bb));
    }

  bitmap_copy (local_live, DF_LR_OUT (bb));

  df_simulate_initialize_backwards (bb, local_live);
  dead_debug_local_init (&debug, NULL, global_debug);

  FOR_BB_INSNS_REVERSE (bb, insn)
    if (DEBUG_INSN_P (insn))
      {
	df_ref use;
	FOR_EACH_INSN_USE (use, insn)
	  if (!bitmap_bit_p (local_live, DF_REF_REGNO (use))
	      && !bitmap_bit_p (au, DF_REF_REGNO (use)))
	    dead_debug_add (&debug, use, DF_REF_REGNO (use));
      }
    else if (INSN_P (insn))
      {
	bool needed = marked_insn_p (insn);

	/* The insn is needed if there is someone who uses the output.  */
	if (!needed)
	  FOR_EACH_INSN_DEF (def, insn)
	    if (bitmap_bit_p (local_live, DF_REF_REGNO (def))
		|| bitmap_bit_p (au, DF_REF_REGNO (def)))
	      {
		needed = true;
		mark_insn (insn, true);
		break;
	      }

	/* No matter if the instruction is needed or not, we remove
	   any regno in the defs from the live set.  */
	df_simulate_defs (insn, local_live);

	/* On the other hand, we do not allow the dead uses to set
	   anything in local_live.  */
	if (needed)
	  df_simulate_uses (insn, local_live);

	/* Insert debug temps for dead REGs used in subsequent debug
	   insns.  We may have to emit a debug temp even if the insn
	   was marked, in case the debug use was after the point of
	   death.  */
	if (debug.used && !bitmap_empty_p (debug.used))
	  FOR_EACH_INSN_DEF (def, insn)
	    dead_debug_insert_temp (&debug, DF_REF_REGNO (def), insn,
				    needed && !control_flow_insn_p (insn)
				    ? DEBUG_TEMP_AFTER_WITH_REG_FORCE
				    : DEBUG_TEMP_BEFORE_WITH_VALUE);
      }

  dead_debug_local_finish (&debug, NULL);
  df_simulate_finalize_backwards (bb, local_live);

  block_changed = !bitmap_equal_p (local_live, DF_LR_IN (bb));
  if (block_changed)
    bitmap_copy (DF_LR_IN (bb), local_live);

  BITMAP_FREE (local_live);
  return block_changed;
}


/* Perform fast DCE once initialization is done.  If WORD_LEVEL is
   true, use the word level dce, otherwise do it at the pseudo
   level.  */

static void
fast_dce (bool word_level)
{
  int *postorder = df_get_postorder (DF_BACKWARD);
  int n_blocks = df_get_n_blocks (DF_BACKWARD);
  /* The set of blocks that have been seen on this iteration.  */
  bitmap processed = BITMAP_ALLOC (&dce_blocks_bitmap_obstack);
  /* The set of blocks that need to have the out vectors reset because
     the in of one of their successors has changed.  */
  bitmap redo_out = BITMAP_ALLOC (&dce_blocks_bitmap_obstack);
  bitmap all_blocks = BITMAP_ALLOC (&dce_blocks_bitmap_obstack);
  bool global_changed = true;

  /* These regs are considered always live so if they end up dying
     because of some def, we need to bring the back again.  Calling
     df_simulate_fixup_sets has the disadvantage of calling
     bb_has_eh_pred once per insn, so we cache the information
     here.  */
  bitmap au = &df->regular_block_artificial_uses;
  bitmap au_eh = &df->eh_block_artificial_uses;
  int i;
  struct dead_debug_global global_debug;

  prescan_insns_for_dce (true);

  for (i = 0; i < n_blocks; i++)
    bitmap_set_bit (all_blocks, postorder[i]);

  dead_debug_global_init (&global_debug, NULL);

  while (global_changed)
    {
      global_changed = false;

      for (i = 0; i < n_blocks; i++)
	{
	  int index = postorder[i];
	  basic_block bb = BASIC_BLOCK_FOR_FN (cfun, index);
	  bool local_changed;

	  if (index < NUM_FIXED_BLOCKS)
	    {
	      bitmap_set_bit (processed, index);
	      continue;
	    }

	  if (word_level)
	    local_changed
	      = word_dce_process_block (bb, bitmap_bit_p (redo_out, index),
					&global_debug);
	  else
	    local_changed
	      = dce_process_block (bb, bitmap_bit_p (redo_out, index),
				   bb_has_eh_pred (bb) ? au_eh : au,
				   &global_debug);
	  bitmap_set_bit (processed, index);

	  if (local_changed)
	    {
	      edge e;
	      edge_iterator ei;
	      FOR_EACH_EDGE (e, ei, bb->preds)
		if (bitmap_bit_p (processed, e->src->index))
		  /* Be tricky about when we need to iterate the
		     analysis.  We only have redo the analysis if the
		     bitmaps change at the top of a block that is the
		     entry to a loop.  */
		  global_changed = true;
		else
		  bitmap_set_bit (redo_out, e->src->index);
	    }
	}

      if (global_changed)
	{
	  /* Turn off the RUN_DCE flag to prevent recursive calls to
	     dce.  */
	  int old_flag = df_clear_flags (DF_LR_RUN_DCE);

	  /* So something was deleted that requires a redo.  Do it on
	     the cheap.  */
	  delete_unmarked_insns ();
	  bitmap_clear (marked);
	  bitmap_clear (processed);
	  bitmap_clear (redo_out);

	  /* We do not need to rescan any instructions.  We only need
	     to redo the dataflow equations for the blocks that had a
	     change at the top of the block.  Then we need to redo the
	     iteration.  */
	  if (word_level)
	    df_analyze_problem (df_word_lr, all_blocks, postorder, n_blocks);
	  else
	    df_analyze_problem (df_lr, all_blocks, postorder, n_blocks);

	  if (old_flag & DF_LR_RUN_DCE)
	    df_set_flags (DF_LR_RUN_DCE);

	  prescan_insns_for_dce (true);
	}
    }

  dead_debug_global_finish (&global_debug, NULL);

  delete_unmarked_insns ();

  BITMAP_FREE (processed);
  BITMAP_FREE (redo_out);
  BITMAP_FREE (all_blocks);

  /* Both forms of DCE should make further DCE unnecessary.  */
  df_lr_dce->solutions_dirty = false;
}


/* Fast register level DCE.  */

static unsigned int
rest_of_handle_fast_dce (void)
{
  init_dce (true);
  fast_dce (false);
  fini_dce (true);
  return 0;
}


/* Fast byte level DCE.  */

void
run_word_dce (void)
{
  int old_flags;

  if (!flag_dce)
    return;

  timevar_push (TV_DCE);
  old_flags = df_clear_flags (DF_DEFER_INSN_RESCAN + DF_NO_INSN_RESCAN);
  df_word_lr_add_problem ();
  init_dce (true);
  fast_dce (true);
  fini_dce (true);
  df_set_flags (old_flags);
  timevar_pop (TV_DCE);
}


/* This is an internal call that is used by the df live register
   problem to run fast dce as a side effect of creating the live
   information.  The stack is organized so that the lr problem is run,
   this pass is run, which updates the live info and the df scanning
   info, and then returns to allow the rest of the problems to be run.

   This can be called by elsewhere but it will not update the bit
   vectors for any other problems than LR.  */

void
run_fast_df_dce (void)
{
  if (flag_dce)
    {
      /* If dce is able to delete something, it has to happen
	 immediately.  Otherwise there will be problems handling the
	 eq_notes.  */
      int old_flags =
	df_clear_flags (DF_DEFER_INSN_RESCAN + DF_NO_INSN_RESCAN);

      df_in_progress = true;
      rest_of_handle_fast_dce ();
      df_in_progress = false;

      df_set_flags (old_flags);
    }
}


/* Run a fast DCE pass.  */

void
run_fast_dce (void)
{
  if (flag_dce)
    rest_of_handle_fast_dce ();
}


namespace {

const pass_data pass_data_fast_rtl_dce =
{
  RTL_PASS, /* type */
  "rtl_dce", /* name */
  OPTGROUP_NONE, /* optinfo_flags */
  TV_DCE, /* tv_id */
  0, /* properties_required */
  0, /* properties_provided */
  0, /* properties_destroyed */
  0, /* todo_flags_start */
  TODO_df_finish, /* todo_flags_finish */
};

class pass_fast_rtl_dce : public rtl_opt_pass
{
public:
  pass_fast_rtl_dce (gcc::context *ctxt)
    : rtl_opt_pass (pass_data_fast_rtl_dce, ctxt)
  {}

  /* opt_pass methods: */
  bool gate (function *) final override
    {
      return optimize > 0 && flag_dce && dbg_cnt (dce_fast);
    }

  unsigned int execute (function *) final override
    {
      return rest_of_handle_fast_dce ();
    }

}; // class pass_fast_rtl_dce

} // namespace

// TODO: naalokovat prazdnou a pak resize
// -flto_partition=none
// -ftime_report a v build adresari zkusit insn*.cc (emit, match, recog)
struct offset_bitmap
{
private:
  int m_offset;
  sbitmap m_bitmap;

public:
  offset_bitmap ()
    : m_offset{0}, m_bitmap{sbitmap_alloc(0)}
  {}

  offset_bitmap (size_t size, int offset)
    : m_offset{offset}, m_bitmap{sbitmap_alloc (size)}
  {}

  offset_bitmap (int min_index, int max_index)
    : offset_bitmap (size_t (max_index - min_index + 1), -min_index)
  {}

  void resize(size_t size, int offset)
  {
    m_bitmap = sbitmap_resize(m_bitmap, (unsigned int)size, 0); 
    m_offset = offset; 
  }

  void resize(int min_index, int max_index)
  {
    resize(size_t (max_index - min_index + 1), -min_index);
  }

  void clear_bit (int index) { bitmap_clear_bit (m_bitmap, index + m_offset); }

  void set_bit (int index) { bitmap_set_bit (m_bitmap, index + m_offset); }

  bool get_bit (int index) { return bitmap_bit_p (m_bitmap, index + m_offset); }

  ~offset_bitmap () { sbitmap_free (m_bitmap); }
};

class rtl_ssa_dce
{
public:
  unsigned int execute (function *);

private:
  bool is_rtx_pattern_prelive (const_rtx);
  bool can_delete_call (const_rtx);
  bool is_rtx_prelive (const_rtx);
  bool is_prelive (insn_info *);
  void mark_prelive_insn (insn_info *, auto_vec<set_info *> &);
  auto_vec<set_info *> mark_prelive ();
  void mark ();
  std::unordered_set<insn_info *> propagate_dead_phis ();
  void reset_dead_debug_insn (insn_info *);
  void reset_dead_debug ();
  void sweep ();

  void debugize_insn (insn_info *);

  void unmark_debugizable(insn_info *, sbitmap);
  sbitmap find_debugizable(const std::unordered_set<insn_info *> &);
  void debugize_insns (const sbitmap);

  offset_bitmap m_marked;
  sbitmap mm_marked_phis;
};

bool
rtl_ssa_dce::is_rtx_pattern_prelive (const_rtx insn)
{
  switch (GET_CODE (insn))
    {
    case PREFETCH:
    case UNSPEC:
    case TRAP_IF: /* testsuite/gcc.c-torture/execute/20020418-1.c */
      return true;

    default:
      return side_effects_p (insn);
    }
}

// odebrat auto

bool
rtl_ssa_dce::can_delete_call (const_rtx insn)
{
  gcc_checking_assert (CALL_P (insn));

  // We cannot delete pure or const sibling calls because it is
  // hard to see the result.
  // if (!SIBLING_CALL_P (insn))
  //     // We can delete dead const or pure calls as long as they do not
  //     // infinite loop.
  //     && (RTL_CONST_OR_PURE_CALL_P (insn)
	//   && !RTL_LOOPING_CONST_OR_PURE_CALL_P (insn)

  //   && cfun->can_delete_dead_exceptions
  // )
  //   return true;
      // Don't delete calls that may throw if we cannot do so.

  // pridat assert na const nebo pure
  //  pouzivat gcc_checking_assert();
  if (cfun->can_delete_dead_exceptions)
    return true;
  // if (!insn_nothrow_p (insn))
    // return false;
  return insn_nothrow_p (insn);

  // UD_DCE ignores this and works...
  /* If we can't alter cfg, even when the call can't throw exceptions, it
     might have EDGE_ABNORMAL_CALL edges and so we shouldn't delete such
     calls.  */
  // gcc_assert (CALL_P (insn));
  // if (BLOCK_FOR_INSN (insn) && BB_END (BLOCK_FOR_INSN (insn)) == insn)
  //   {
  //     edge e;
  //     edge_iterator ei;

  //     FOR_EACH_EDGE (e, ei, BLOCK_FOR_INSN (insn)->succs)
  // if ((e->flags & EDGE_ABNORMAL_CALL) != 0)
  //   return false;
  //   }
  // return true;
}

bool
rtl_ssa_dce::is_rtx_prelive (const_rtx insn)
{
  gcc_assert (insn != nullptr);

  if (CALL_P (insn)
      // We cannot delete pure or const sibling calls because it is
      // hard to see the result.
      && (!SIBLING_CALL_P (insn))
      // We can delete dead const or pure calls as long as they do not
      // infinite loop.
      && (RTL_CONST_OR_PURE_CALL_P (insn)
	  && !RTL_LOOPING_CONST_OR_PURE_CALL_P (insn))
      // Don't delete calls that may throw if we cannot do so.
      && can_delete_call (insn))
    return false;

  if (!NONJUMP_INSN_P (insn))
    // This handles jumps, notes, call_insns, debug_insns, ...
    return true;

  // Only rtx_insn should be handled here
  gcc_assert (GET_CODE (insn) == INSN);

  // Don't delete insns that may throw if we cannot do so.
  if (!cfun->can_delete_dead_exceptions && !insn_nothrow_p (insn))
    return true;

  // Callee-save restores are needed.
  if (RTX_FRAME_RELATED_P (insn) && crtl->shrink_wrapped_separate
      && find_reg_note (insn, REG_CFA_RESTORE, NULL))
    return true;

  rtx pat = PATTERN (insn);
  switch (GET_CODE (pat))
    {
    case CLOBBER:
    case USE:
    case VAR_LOCATION:
      return true;

    case PARALLEL:
      for (int i = XVECLEN (pat, 0) - 1; i >= 0; i--)
	if (is_rtx_pattern_prelive (XVECEXP (pat, 0, i)))
	  return true;
      return false;

    default:
      return is_rtx_pattern_prelive (pat);
    }
}

bool
rtl_ssa_dce::is_prelive (insn_info *insn)
{
  // bb head and end contain artificial uses that we need to mark as prelive
  // debug instructions are also prelive, however, they are not added to the
  // worklist
  if (insn->is_bb_head () || insn->is_bb_end () || insn->is_debug_insn ())
    return true;

  // Phi instructions are never prelive
  if (insn->is_artificial ())
    return false;

  gcc_assert (insn->is_real ());
  for (def_info *def : insn->defs ())
    {
      // The purpose of this pass is not to eliminate memory stores...
      if (def->is_mem ())
	return true;

      gcc_assert (def->is_reg ());

      // we should not delete the frame pointer because of the dward2frame pass
      if (frame_pointer_needed && def->regno () == HARD_FRAME_POINTER_REGNUM)
	return true;

      // skip clobbers, they are handled inside is_rtx_prelive
      if (def->kind () == access_kind::CLOBBER)
	continue;

      gcc_assert (def->kind () == access_kind::SET);

      // needed by gcc.c-torture/execute/pr51447.c
      if (HARD_REGISTER_NUM_P (def->regno ()) && global_regs[def->regno ()])
	return true;

  // what about reload? check cse.cc
      unsigned int picreg = PIC_OFFSET_TABLE_REGNUM;
      if (picreg != INVALID_REGNUM && fixed_regs[picreg]
	  && def->regno () == picreg)
	return true;
    }

  return is_rtx_prelive (insn->rtl ());
}

void
rtl_ssa_dce::mark_prelive_insn (insn_info *insn, auto_vec<set_info *> &worklist)
{
  if (dump_file)
    fprintf (dump_file, "Insn %d marked as prelive\n", insn->uid ());

  m_marked.set_bit(insn->uid ());
  // debug instruction are not added to worklist not to wake up possibly dead
  // instructions
  if (insn->is_debug_insn ())
    return;

  for (use_info *use : insn->uses ())
    {
      set_info *set = use->def ();
      if (set)
	worklist.safe_push (set);
    }
}

auto_vec<set_info *>
rtl_ssa_dce::mark_prelive ()
{
  if (dump_file)
    fprintf (dump_file, "DCE: prelive phase\n");

  auto_vec<set_info *> worklist;
  for (insn_info *insn : crtl->ssa->all_insns ())
    {
      if (is_prelive (insn))
	mark_prelive_insn (insn, worklist);
    }

  return worklist;
}

// kometare + PARAMETRY kapitalkami
void
rtl_ssa_dce::mark ()
{
  auto worklist = mark_prelive ();
  if (dump_file)
    fprintf (dump_file, "DCE: marking phase\n");

  while (!worklist.is_empty ())
    {
      set_info *set = worklist.pop ();
      insn_info *insn = set->insn ();
      if (!insn) // TODO: is this possible?
	continue;

      // Skip already visited visited instructions.
      auto uid = insn->uid ();
      if (m_marked.get_bit(uid) && !insn->is_phi ())
	continue;

      m_marked.set_bit (uid);


      use_array uses = insn->uses ();
      if (insn->is_phi ())
	{
	  phi_info *phi = static_cast<phi_info *> (set);
	  auto phi_uid = phi->uid ();
	  // Skip already visited phi node.
	  if (bitmap_bit_p(mm_marked_phis, phi_uid))
	    continue;

    bitmap_set_bit (mm_marked_phis, phi_uid);
	  uses = phi->inputs ();
	}

      if (dump_file)
	fprintf (dump_file, "Adding insn %d to worklist\n", insn->uid ());

      for (use_info *use : uses)
	{
	  set_info *parent_set = use->def ();
	  if (!parent_set)
	    continue;

	  worklist.safe_push (parent_set);
	}
    }
}

// Clear debug_insn uses and set gen_rtx_UNKNOWN_VAR_LOC
void
rtl_ssa_dce::reset_dead_debug_insn (insn_info *insn)
{
  gcc_assert (insn->is_debug_insn ());

  if (dump_file)
    fprintf (dump_file, "Resetting debug insn: %d\n", insn->uid ());

  m_marked.clear_bit (insn->uid ());
  insn_change change (insn);
  change.new_uses = {};
  INSN_VAR_LOCATION_LOC (insn->rtl ()) = gen_rtx_UNKNOWN_VAR_LOC ();
  crtl->ssa->change_insn (change);
}

// Iterate over all debug_insns and reset those for which at least one of the
// uses isn't marked.
void
rtl_ssa_dce::reset_dead_debug ()
{
  for (insn_info *insn : crtl->ssa->all_insns ())
    {
      if (!insn->is_debug_insn ())
	continue;

      // When propagate_dead_phis is ready, check if insn is in marked. If true,
      // then try to create debug instructions RTL SSA does not contain debug
      // insntructions... we have to make sure that uses are correct

      // TODO : use iterate_safely?
      for (use_info *use : insn->uses ())
	{
    def_info *def = use->def ();
	  insn_info *parent_insn = def->insn ();
    // Is this check needed?
    if (parent_insn == nullptr)
      continue;
    // If we depend on a dead instruction, clear current instruction uses.
    bool shouldReset = false;
    if (parent_insn->is_phi ()) {
      shouldReset = bitmap_bit_p(mm_marked_phis, static_cast<phi_info *> (def)->uid ());
    } else {
      shouldReset = m_marked.get_bit(parent_insn->uid ());
    }

    if (shouldReset) {
      reset_dead_debug_insn (insn);
	    break;
    }

    // sbitmap marked = parent_insn->is_phi () ? mm_marked_phis : mm_marked;
    // int uid = parent_insn->is_phi () ? static_cast<phi_info *> (def)->uid () : parent_insn->uid () + offset;
    // if (!bitmap_bit_p (marked, uid)) {
    //   reset_dead_debug_insn (insn);
	  //   break;
    // }
	}
    }
}

// Iterate over non-debug instructions in RPO and remove all that aren't marked.
void
rtl_ssa_dce::sweep ()
{
  if (dump_file)
    fprintf (dump_file, "DCE: Sweep phase\n");

  auto_vec<insn_change> to_delete;

  // Previously created debug instructions won't be visited here
  for (insn_info *insn : crtl->ssa->nondebug_insns ())
    {
      // Artificial or marked insns should not be deleted.
      if (insn->is_artificial () || m_marked.get_bit (insn->uid ()))
	continue;

      if (dump_file)
	fprintf (dump_file, "Sweeping insn: %d\n", insn->uid ());

      auto change = insn_change::delete_insn (insn);
      to_delete.safe_push (std::move (change));

      // If we use following way with reverse_nondebug_insns, not all insns seem
      // to be deleted...
      // crtl->ssa->change_insn(change);
    }

  auto_vec<insn_change *> changes (to_delete.length ());
  for (size_t i = 0; i != to_delete.length (); ++i)
    changes.safe_push (&to_delete[i]);

  gcc_assert (crtl->ssa->verify_insn_changes (changes));
  crtl->ssa->change_insns (changes);

  if (dump_file)
    fprintf (dump_file, "DCE: finish sweep phase\n");
}

#include <iostream>

unsigned int
rtl_ssa_dce::execute (function *fn)
{
  calculate_dominance_info (CDI_DOMINATORS);
  // internal compiler error: gcc.c-torture/execute/20040811-1.c -
  // rtl_ssa::function_info::add_phi_nodes
  crtl->ssa = new rtl_ssa::function_info (fn);
  int real_max = 0, artificial_min = 0;
  std::size_t count = 0;
  for (insn_info *insn : crtl->ssa->all_insns ()) 
  {
    artificial_min = std::min (artificial_min, insn->uid ());
    real_max = std::max (real_max, insn->uid ());
    count++;
  }

  m_marked.resize(artificial_min, real_max + 1);
  // std::cout << "real_max: " << real_max << '\n';
  // std::cout << "artificial_min: " << artificial_min << '\n';
  // std::cout << "total: " << real_max - artificial_min + 3 << '\n';
  // std::cout << "count: " << count << '\n';

  unsigned int phi_node_max = 0;
  for (ebb_info *ebb : crtl->ssa->ebbs ())
    for (phi_info *phi : ebb->phis ())
    phi_node_max = std::max (phi_node_max, phi->uid ());

  // std::cout << "phi_node_max: " << phi_node_max << '\n';
  mm_marked_phis = sbitmap_alloc(phi_node_max + 1);
  bitmap_clear(mm_marked_phis);

  if (dump_file)
    dump (dump_file, crtl->ssa);

  // std::cout << '\n' << function_name(cfun) << '\n';
  // std::cout << "HARD_FRAME_POINTER_IS_ARG_POINTER: " <<
  // HARD_FRAME_POINTER_IS_ARG_POINTER << '\n'; std::cout <<
  // "HARD_FRAME_POINTER_IS_FRAME_POINTER: " <<
  // HARD_FRAME_POINTER_IS_FRAME_POINTER << '\n'; std::cout <<
  // "HARD_FRAME_POINTER_REGNUM: " << HARD_FRAME_POINTER_REGNUM << '\n';
  // std::cout << "FRAME_POINTER_REGNUM: " << FRAME_POINTER_REGNUM << '\n';
  // std::cout << "fixed_regs[HARD_FRAME_POINTER_REGNUM]: " <<
  // (fixed_regs[HARD_FRAME_POINTER_REGNUM] == 1) << '\n'; std::cout <<
  // "fixed_regs[FRAME_POINTER_REGNUM]: " << (fixed_regs[FRAME_POINTER_REGNUM]
  // == 1) << '\n'; std::cout << "global_regs[HARD_FRAME_POINTER_REGNUM]: " <<
  // (global_regs[HARD_FRAME_POINTER_REGNUM] == 1) << '\n'; std::cout <<
  // "global_regs[FRAME_POINTER_REGNUM]: " << (global_regs[FRAME_POINTER_REGNUM]
  // == 1) << '\n'; std::cout << "frame_pointer_needed: " <<
  // frame_pointer_needed << '\n';

  mark ();
  if (MAY_HAVE_DEBUG_BIND_INSNS)
   {
    auto dead_phis = propagate_dead_phis();
    auto debugizable = find_debugizable(dead_phis);
    debugize_insns(debugizable);
    
    reset_dead_debug ();
   }
  sweep ();

  free_dominance_info (CDI_DOMINATORS);
  if (crtl->ssa->perform_pending_updates ())
    cleanup_cfg (0);

  sbitmap_free(mm_marked_phis);
  delete crtl->ssa;
  crtl->ssa = nullptr;
  return 0;
}

// mark instructions that depend on a dead phi
std::unordered_set<insn_info *>
rtl_ssa_dce::propagate_dead_phis ()
{
  std::unordered_set<phi_info *> visited_dead_phis;
  std::unordered_set<insn_info *> depends_on_dead_phi;
  auto_vec<set_info *> worklist;

  // add dead phis to worklist
  for (ebb_info *ebb : crtl->ssa->ebbs ())
    {
      for (phi_info *phi : ebb->phis ())
	{
	  if (bitmap_bit_p (mm_marked_phis, phi->uid ()))
	    continue;

	  worklist.safe_push (phi);
	}
    }

  // suppose that debug insns are marked - non marked will be removed later
  // propagate dead phis via du chains and unmark reachable debug instructions
  while (!worklist.is_empty ())
    {
      set_info *set = worklist.pop ();
      insn_info *insn = set->insn ();

      if (insn->is_debug_insn ())
	{
	  if (dump_file)
	    fprintf (dump_file, "Debug insns %d depends on dead phi.\n",
		     insn->uid ());

    m_marked.clear_bit (insn->uid ());
	  // debug instructions dont have chains
	  continue;
	}

      // mark
      if (insn->is_phi ())
	{
	  gcc_checking_assert (!bitmap_bit_p(mm_marked_phis, static_cast<phi_info *> (set)->uid ()));
	  visited_dead_phis.emplace (static_cast<phi_info *> (set));
	}
      else
	{
	  gcc_checking_assert (!m_marked.get_bit (insn->uid ()));
	  depends_on_dead_phi.emplace (insn);
	}

      for (use_info *use : set->all_uses ())
	{
	  if (use->is_in_phi ())
	    {
	      // do not add already visited dead phis
	      if (visited_dead_phis.count (use->phi ()) == 0)
		worklist.safe_push (use->phi ());
	    }
	  else
	    {
	      gcc_assert (use->is_in_any_insn ());
	      // add all defs from insn to worklist
	      for (def_info *def : use->insn ()->defs ())
		{
		  if (def->kind () != access_kind::SET)
		    continue;

		  worklist.safe_push (static_cast<set_info *> (def));
		}
	    }
	}
    }

  return depends_on_dead_phi;
}

void
rtl_ssa_dce::debugize_insn (insn_info *insn)
{
  
}

struct register_replacement {
  unsigned int regno;
  rtx expr;
};

static rtx
replace_dead_reg(rtx x, const_rtx old_rtx ATTRIBUTE_UNUSED, void *data)
{
  auto replacement = static_cast<register_replacement *>(data);
  
 if (REG_P (x) && REGNO (x) >= FIRST_VIRTUAL_REGISTER && replacement->regno == REGNO (x))
 {
  if (GET_MODE (x) == GET_MODE (replacement->expr))
	  return replacement->expr;
  return lowpart_subreg (GET_MODE (x), replacement->expr, GET_MODE (replacement->expr));
 }

 return NULL_RTX;
}

// visit every marked instruction in INSN dependency tree and unmark it
void
rtl_ssa_dce::unmark_debugizable (insn_info *insn, sbitmap debugizable) 
{
  auto_vec<insn_info *> worklist;
  gcc_checking_assert (!insn->is_artificial ());

  bitmap_set_bit (debugizable, insn->uid ());
  worklist.safe_push (insn);

  // process all marked dependencies and unmark them
  while (!worklist.is_empty ()) {
    insn_info *current = worklist.pop ();
    int current_uid = current->uid ();

    // skip instruction that are not marked
    if (!bitmap_bit_p(debugizable, current_uid))
      continue;

    bitmap_clear_bit(debugizable, current_uid);

    // add all marked dependencies to the worklist
    for (def_info *def : current->defs())
    {
      if (def->kind() != access_kind::SET) // skip clobbers
        continue;
  
      auto *set = static_cast<set_info *>(def);
      for (use_info *use : set->all_uses()) 
      {
        // this phi node might not be dead
        if (use->is_in_phi ())
          continue;

        insn_info *use_insn = use->insn();

        // artificial instruction will never be debugizable
        if (use_insn->is_artificial ())
          continue;

        if (bitmap_bit_p(debugizable, use_insn->uid()))
          worklist.safe_push (use_insn);
      }
    }
  }
}

// return a bitmap which describes whether an instruction can be debugized
// - that means replaced with a debug instruction
sbitmap
rtl_ssa_dce::find_debugizable(const std::unordered_set<insn_info *> &depends_on_dead_phi) 
{
  // only real instructions can be turned to debug instructions
  sbitmap debugizable = sbitmap_alloc (get_max_uid () + 1);
  bitmap_clear(debugizable);

  for (insn_info *insn : crtl->ssa->reverse_all_insns ()) {
    // Skip live nondebug instrunctions. Debug instructions are by default live 
    // and we cannot skip them here - they have to be marked as debugizable
    if (insn->is_artificial () || 
      (m_marked.get_bit (insn->uid ()) && !insn->is_debug_insn()))
      continue;

    // instructions that depend on a dead phi node cannot be debugized
    if (depends_on_dead_phi.count (insn) > 0) {
      if (insn->is_debug_insn ())
        reset_dead_debug_insn (insn);

      // we don't have to call unmark_debugizable, because a dead nondebug
      // instructions that depends on a dead phi won't be turned into a 
      // debug instrunction
      continue;
    }

    // handle debug instrunctions - mark them and skip
    if (insn->is_debug_insn ()) {
      bitmap_set_bit (debugizable, insn->uid ());
      continue;
    }

    // this insn may have some debugizable dependencies and if we find that
    // current insn is not debugizable, we have to reset those dependencies

    rtx_insn *rtl = insn->rtl ();
    def_array defs = insn->defs ();
    rtx rtx_set;

    // If insn has debug uses and will be deleted, signalize it
    if (!MAY_HAVE_DEBUG_BIND_INSNS ||
      (rtx_set = single_set (rtl)) == NULL_RTX ||
      side_effects_p (SET_SRC (rtx_set)) ||
      asm_noperands (PATTERN (rtl)) >= 0)
      {
        std::cerr << "FAILED TO CREATE DEBUG\n";
        unmark_debugizable(insn, debugizable);
        continue;
      }

    // insn is definitely a single_set, following if statement is useless:
    if (insn->num_defs () != 1)
    {
      gcc_assert (false);
      if (insn->num_defs() > 1)
        unmark_debugizable(insn, debugizable);
      std::cerr << "FAILED TO CREATE DEBUG\n";
      continue;
    }

    def_info *def = *defs.begin ();
    if (def->kind () != access_kind::SET) // Skip clobbers
      continue;

    set_info *set = static_cast<set_info *> (def);

    // if some dependent is a debugizable
    bool has_debug_uses = false;
    for (use_info *use : set->all_uses()) 
    {
      // skip phi nodes
      if (use->is_in_phi ())
        continue;

      insn_info *use_insn = use->insn();
      if (use_insn->is_artificial ())
        continue;

      if (bitmap_bit_p(debugizable, use_insn->uid ())) {
        has_debug_uses = true;
        break;
      }
    }
    // if (!set->has_nondebug_insn_uses ())
    if (!has_debug_uses)
      continue;

    bitmap_set_bit (debugizable, insn->uid ());
  }

  return debugizable;
}

// create new debug instructions according to the DEBUGIZABLE sbitmap
void
rtl_ssa_dce::debugize_insns (const sbitmap debugizable)
{
  for (insn_info *insn : crtl->ssa->reverse_all_insns ())
  {
    if (insn->is_artificial () || 
      insn->is_debug_insn() ||
      !bitmap_bit_p(debugizable, insn->uid ()))
      continue;

    rtx_insn *rtl = insn->rtl ();
    def_array defs = insn->defs ();
    rtx rtx_set = single_set (rtl);
    def_info *def = *defs.begin ();
    gcc_checking_assert (def->kind () != access_kind::SET);

    set_info *set = static_cast<set_info *> (def);

    // turn instruction into debug
    rtx dval = make_debug_expr_from_rtl (SET_DEST (rtx_set));

    /* Emit a debug bind insn before the insn in which
        reg dies.  */
    rtx bind_var_loc =
      gen_rtx_VAR_LOCATION (GET_MODE (SET_DEST (rtx_set)),
          DEBUG_EXPR_TREE_DECL (dval),
          SET_SRC (rtx_set),
          VAR_INIT_STATUS_INITIALIZED);

    obstack_watermark watermark = crtl->ssa->new_change_attempt();
    insn_info *debug_insn = crtl->ssa->create_insn(watermark, DEBUG_INSN, bind_var_loc);
    insn_change change(debug_insn);
    change.new_uses = insn->uses();
    change.move_range = insn_range_info(insn);
    debug (change);

    if (!rtl_ssa::restrict_movement (change))
      std::cerr << "change move range location does not exists\n";
    crtl->ssa->change_insn(change);
    // rtx_insn *bind = emit_debug_insn_before (bind_var_loc, rtl);

    register_replacement replacement;
    for (use_info *u : set->all_uses ()) {
      // TODO: transform dependent insns
      if (u->is_artificial())
        continue;

      replacement.regno = u->regno();
      replacement.expr = dval;

      // if debugizable -> replace

      simplify_replace_fn_rtx(INSN_VAR_LOCATION_LOC(rtl), NULL_RTX, replace_dead_reg, &replacement);
    }

    // pr113089.c
    if (insn->uses().size() > 0) {
      // std::cerr << "Insn has uses...\n";
    }

    // debug (bind_var_loc);
    // debug (insn);
    // debug (change.insn ());
    // debug (crtl->ssa);

    // note:
    // 1. Walk over all insns from last to first. If an insntruction can be
    //    debugized, update a bitmap. If the instruction is dead, walk over
    //    its dependencies with worklist and reset the bitmap for visited 
    //    instructions.
    // 2. Do the actual debugizing.

    // rtx set;
    // // debugize_insns should be called only if MAY_HAVE_DEBUG_BIND_INSNS
    // if (MAY_HAVE_DEBUG_BIND_INSNS
		//   && (set = single_set (rtl)) != NULL_RTX
    //   && !(*defs.begin ())->has_nondebug_insn_uses()
		//   // && is_dead_reg (SET_DEST (set), counts)
    //   // Proc tam byla tato podminka?
    //   // - debug statementy se sypou za kazdou definici ve zdrojaku, tedy 
    //   // proto se chci ptat na to, kdyz existuje debug pouziti, tak je to 
    //   // zajimava promenna
		//   /* Used at least once in some DEBUG_INSN.  */ 
		//   // && counts[REGNO (SET_DEST (set)) + nreg] > 0
    //   // Tohle je ten bind nebo cast debug instrukce?
		//   /* And set exactly once.  */
		//   // && counts[REGNO (SET_DEST (set)) + nreg * 2] == 1
		//   && !side_effects_p (SET_SRC (set))
		//   && asm_noperands (PATTERN (rtl)) < 0)
		// {

    // }
  }

  sbitmap_free (debugizable);
  // TODO: check that all of the debug insn uses are live,
  // otherwise reset the instruction
}

static void
test (insn_info *insn)
{
  // Radeji bych zde videl pridani dval do rtl ssa
  // crtl->ssa->create_insn(watermark, DEBUG_INSN, rtx pat);

  // TODO : change ssa form - we have to keep uses in the first debug insn and
  // delete other instructions
  // make dval available in the following iteration
}

// static auto_vec<insn_change>
// wip (std::unordered_set<insn_info *> &marked,
//      std::unordered_set<phi_info *> &marked_phis)
// {
//   std::unordered_set<insn_info *> depends_on_dead_phi = propagate_dead_phis ();
//   for (insn_info *insn : crtl->ssa->all_insns ())
//     {
//       if (insn->is_phi ()) // insn->is_artificial() ??
// 	continue;

//       // skip marked instructions
//       if (marked.count (insn) > 0) // debug instruction are still alive
// 	continue;

//       // we cannot create debug instruction if we depend on a dead phi
//       if (depends_on_dead_phi.count (insn) > 0)
// 	continue;

//       // insn is nondebug and dead
//     }
// }

rtl_opt_pass *
make_pass_fast_rtl_dce (gcc::context *ctxt)
{
  return new pass_fast_rtl_dce (ctxt);
}

namespace {

const pass_data pass_data_rtl_ssa_dce = {
  RTL_PASS,	  /* type */
  "rtl_ssa_dce",  /* name */
  OPTGROUP_NONE,  /* optinfo_flags */
  TV_DCE,	  /* tv_id */
  0,		  /* properties_required */
  0,		  /* properties_provided */
  0,		  /* properties_destroyed */
  0,		  /* todo_flags_start */
  TODO_df_finish, /* todo_flags_finish */
};

class pass_rtl_ssa_dce : public rtl_opt_pass
{
public:
  pass_rtl_ssa_dce (gcc::context *ctxt)
    : rtl_opt_pass (pass_data_rtl_ssa_dce, ctxt)
  {}

  opt_pass *clone () final override { return new pass_rtl_ssa_dce (m_ctxt); }

  /* opt_pass methods: */
  bool gate (function *) final override { return optimize > 0 && flag_dce; }

  unsigned int execute (function *fn) final override
  {
    return rtl_ssa_dce ().execute (fn);
  }

}; // class pass_rtl_ssa_dce

} // namespace

rtl_opt_pass *
make_pass_rtl_ssa_dce (gcc::context *ctxt)
{
  return new pass_rtl_ssa_dce (ctxt);
}
