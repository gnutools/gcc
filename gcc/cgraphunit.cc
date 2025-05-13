/* Driver of optimization process
   Copyright (C) 2003-2025 Free Software Foundation, Inc.
   Contributed by Jan Hubicka

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

/* This module implements main driver of compilation process.

   The main scope of this file is to act as an interface in between
   tree based frontends and the backend.

   The front-end is supposed to use following functionality:

    - finalize_function

      This function is called once front-end has parsed whole body of function
      and it is certain that the function body nor the declaration will change.

      (There is one exception needed for implementing GCC extern inline
	function.)

    - varpool_finalize_decl

      This function has same behavior as the above but is used for static
      variables.

    - add_asm_node

      Insert new toplevel ASM statement

    - finalize_compilation_unit

      This function is called once (source level) compilation unit is finalized
      and it will no longer change.

      The symbol table is constructed starting from the trivially needed
      symbols finalized by the frontend.  Functions are lowered into
      GIMPLE representation and callgraph/reference lists are constructed.
      Those are used to discover other necessary functions and variables.

      At the end the bodies of unreachable functions are removed.

      The function can be called multiple times when multiple source level
      compilation units are combined.

    - compile

      This passes control to the back-end.  Optimizations are performed and
      final assembler is generated.  This is done in the following way. Note
      that with link time optimization the process is split into three
      stages (compile time, linktime analysis and parallel linktime as
      indicated below).

      Compile time:

	1) Inter-procedural optimization.
	   (ipa_passes)

	   This part is further split into:

	   a) early optimizations. These are local passes executed in
	      the topological order on the callgraph.

	      The purpose of early optimizations is to optimize away simple
	      things that may otherwise confuse IP analysis. Very simple
	      propagation across the callgraph is done i.e. to discover
	      functions without side effects and simple inlining is performed.

	   b) early small interprocedural passes.

	      Those are interprocedural passes executed only at compilation
	      time.  These include, for example, transactional memory lowering,
	      unreachable code removal and other simple transformations.

	   c) IP analysis stage.  All interprocedural passes do their
	      analysis.

	      Interprocedural passes differ from small interprocedural
	      passes by their ability to operate across whole program
	      at linktime.  Their analysis stage is performed early to
	      both reduce linking times and linktime memory usage by
	      not having to represent whole program in memory.

	   d) LTO streaming.  When doing LTO, everything important gets
	      streamed into the object file.

       Compile time and or linktime analysis stage (WPA):

	      At linktime units gets streamed back and symbol table is
	      merged.  Function bodies are not streamed in and not
	      available.
	   e) IP propagation stage.  All IP passes execute their
	      IP propagation. This is done based on the earlier analysis
	      without having function bodies at hand.
	   f) Ltrans streaming.  When doing WHOPR LTO, the program
	      is partitioned and streamed into multiple object files.

       Compile time and/or parallel linktime stage (ltrans)

	      Each of the object files is streamed back and compiled
	      separately.  Now the function bodies becomes available
	      again.

	 2) Virtual clone materialization
	    (cgraph_materialize_clone)

	    IP passes can produce copies of existing functions (such
	    as versioned clones or inline clones) without actually
	    manipulating their bodies by creating virtual clones in
	    the callgraph. At this time the virtual clones are
	    turned into real functions
	 3) IP transformation

	    All IP passes transform function bodies based on earlier
	    decision of the IP propagation.

	 4) late small IP passes

	    Simple IP passes working within single program partition.

	 5) Expansion
	    (expand_all_functions)

	    At this stage functions that needs to be output into
	    assembler are identified and compiled in topological order
	 6) Output of variables and aliases
	    Now it is known what variable references was not optimized
	    out and thus all variables are output to the file.

	    Note that with -fno-toplevel-reorder passes 5 and 6
	    are combined together in cgraph_output_in_order.

   Finally there are functions to manipulate the callgraph from
   backend.
    - cgraph_add_new_function is used to add backend produced
      functions introduced after the unit is finalized.
      The functions are enqueue for later processing and inserted
      into callgraph with cgraph_process_new_functions.

    - cgraph_function_versioning

      produces a copy of function into new one (a version)
      and apply simple transformations
*/

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "target.h"
#include "rtl.h"
#include "tree.h"
#include "gimple.h"
#include "cfghooks.h"
#include "regset.h"     /* FIXME: For reg_obstack.  */
#include "alloc-pool.h"
#include "tree-pass.h"
#include "stringpool.h"
#include "gimple-ssa.h"
#include "cgraph.h"
#include "coverage.h"
#include "lto-streamer.h"
#include "fold-const.h"
#include "varasm.h"
#include "stor-layout.h"
#include "output.h"
#include "cfgcleanup.h"
#include "gimple-iterator.h"
#include "gimple-fold.h"
#include "gimplify.h"
#include "gimplify-me.h"
#include "tree-cfg.h"
#include "tree-into-ssa.h"
#include "tree-ssa.h"
#include "langhooks.h"
#include "toplev.h"
#include "debug.h"
#include "symbol-summary.h"
#include "tree-vrp.h"
#include "sreal.h"
#include "ipa-cp.h"
#include "ipa-prop.h"
#include "gimple-pretty-print.h"
#include "plugin.h"
#include "ipa-fnsummary.h"
#include "ipa-utils.h"
#include "except.h"
#include "cfgloop.h"
#include "context.h"
#include "pass_manager.h"
#include "tree-nested.h"
#include "dbgcnt.h"
#include "lto-section-names.h"
#include "stringpool.h"
#include "attribs.h"
#include "ipa-inline.h"
#include "omp-offload.h"
#include "symtab-thunks.h"
#include "wide-int.h"
#include "selftest.h"
#include "tree-ssanames.h"
#include "tree-dfa.h"

/* Queue of cgraph nodes scheduled to be added into cgraph.  This is a
   secondary queue used during optimization to accommodate passes that
   may generate new functions that need to be optimized and expanded.  */
vec<cgraph_node *> cgraph_new_nodes;

static void expand_all_functions (void);
static void mark_functions_to_output (void);
static void handle_alias_pairs (void);

/* Return true if this symbol is a function from the C frontend specified
   directly in RTL form (with "__RTL").  */

bool
symtab_node::native_rtl_p () const
{
  if (TREE_CODE (decl) != FUNCTION_DECL)
    return false;
  if (!DECL_STRUCT_FUNCTION (decl))
    return false;
  return DECL_STRUCT_FUNCTION (decl)->curr_properties & PROP_rtl;
}

/* Determine if symbol declaration is needed.  That is, visible to something
   either outside this translation unit, something magic in the system
   configury */
bool
symtab_node::needed_p (void)
{
  /* Double check that no one output the function into assembly file
     early.  */
  if (!native_rtl_p ())
      gcc_checking_assert
	(!DECL_ASSEMBLER_NAME_SET_P (decl)
	 || !TREE_SYMBOL_REFERENCED (DECL_ASSEMBLER_NAME (decl)));

  if (!definition)
    return false;

  if (DECL_EXTERNAL (decl))
    return false;

  /* If the user told us it is used, then it must be so.  */
  if (force_output)
    return true;

  /* ABI forced symbols are needed when they are external.  */
  if (forced_by_abi && TREE_PUBLIC (decl))
    return true;

  /* Keep constructors, destructors and virtual functions.  */
   if (TREE_CODE (decl) == FUNCTION_DECL
       && (DECL_STATIC_CONSTRUCTOR (decl) || DECL_STATIC_DESTRUCTOR (decl)))
    return true;

  /* Externally visible variables must be output.  The exception is
     COMDAT variables that must be output only when they are needed.  */
  if (TREE_PUBLIC (decl) && !DECL_COMDAT (decl))
    return true;

  return false;
}

/* Head and terminator of the queue of nodes to be processed while building
   callgraph.  */

static symtab_node symtab_terminator (SYMTAB_SYMBOL);
static symtab_node *queued_nodes = &symtab_terminator;

/* Add NODE to queue starting at QUEUED_NODES.
   The queue is linked via AUX pointers and terminated by pointer to 1.  */

static void
enqueue_node (symtab_node *node)
{
  if (node->aux)
    return;
  gcc_checking_assert (queued_nodes);
  node->aux = queued_nodes;
  queued_nodes = node;
}

/* Process CGRAPH_NEW_FUNCTIONS and perform actions necessary to add these
   functions into callgraph in a way so they look like ordinary reachable
   functions inserted into callgraph already at construction time.  */

void
symbol_table::process_new_functions (void)
{
  tree fndecl;

  if (!cgraph_new_nodes.exists ())
    return;

  handle_alias_pairs ();
  /*  Note that this queue may grow as its being processed, as the new
      functions may generate new ones.  */
  for (unsigned i = 0; i < cgraph_new_nodes.length (); i++)
    {
      cgraph_node *node = cgraph_new_nodes[i];
      fndecl = node->decl;
      bitmap_obstack_initialize (NULL);
      switch (state)
	{
	case CONSTRUCTION:
	  /* At construction time we just need to finalize function and move
	     it into reachable functions list.  */

	  cgraph_node::finalize_function (fndecl, false);
	  call_cgraph_insertion_hooks (node);
	  enqueue_node (node);
	  break;

	case IPA:
	case IPA_SSA:
	case IPA_SSA_AFTER_INLINING:
	  /* When IPA optimization already started, do all essential
	     transformations that has been already performed on the whole
	     cgraph but not on this function.  */

	  gimple_register_cfg_hooks ();
	  if (!node->analyzed)
	    node->analyze ();
	  push_cfun (DECL_STRUCT_FUNCTION (fndecl));
	  if ((state == IPA_SSA || state == IPA_SSA_AFTER_INLINING)
	      && !gimple_in_ssa_p (DECL_STRUCT_FUNCTION (fndecl)))
	    {
	      bool summaried_computed = ipa_fn_summaries != NULL;
	      g->get_passes ()->execute_early_local_passes ();
	      /* Early passes compute inline parameters to do inlining
		 and splitting.  This is redundant for functions added late.
		 Just throw away whatever it did.  */
	      if (!summaried_computed)
		{
		  ipa_free_fn_summary ();
		  ipa_free_size_summary ();
		}
	    }
	  else if (ipa_fn_summaries != NULL)
	    compute_fn_summary (node, true);
	  free_dominance_info (CDI_POST_DOMINATORS);
	  free_dominance_info (CDI_DOMINATORS);
	  pop_cfun ();
	  call_cgraph_insertion_hooks (node);
	  break;

	case EXPANSION:
	  /* Functions created during expansion shall be compiled
	     directly.  */
	  node->process = 0;
	  call_cgraph_insertion_hooks (node);
	  node->expand ();
	  break;

	default:
	  gcc_unreachable ();
	  break;
	}
      bitmap_obstack_release (NULL);
    }

  cgraph_new_nodes.release ();
}

/* As an GCC extension we allow redefinition of the function.  The
   semantics when both copies of bodies differ is not well defined.
   We replace the old body with new body so in unit at a time mode
   we always use new body, while in normal mode we may end up with
   old body inlined into some functions and new body expanded and
   inlined in others.

   ??? It may make more sense to use one body for inlining and other
   body for expanding the function but this is difficult to do.

   This is also used to cancel C++ mangling aliases, which can be for
   functions or variables.  */

void
symtab_node::reset (bool preserve_comdat_group)
{
  /* Reset our data structures so we can analyze the function again.  */
  analyzed = false;
  definition = false;
  alias = false;
  transparent_alias = false;
  weakref = false;
  cpp_implicit_alias = false;

  remove_all_references ();
  if (!preserve_comdat_group)
    remove_from_same_comdat_group ();

  if (cgraph_node *cn = dyn_cast <cgraph_node *> (this))
    {
      /* If process is set, then we have already begun whole-unit analysis.
	 This is *not* testing for whether we've already emitted the function.
	 That case can be sort-of legitimately seen with real function
	 redefinition errors.  I would argue that the front end should never
	 present us with such a case, but don't enforce that for now.  */
      gcc_assert (!cn->process);

      memset (&cn->rtl, 0, sizeof (cn->rtl));
      cn->inlined_to = NULL;
      cn->remove_callees ();
    }
}

/* Return true when there are references to the node.  INCLUDE_SELF is
   true if a self reference counts as a reference.  */

bool
symtab_node::referred_to_p (bool include_self)
{
  ipa_ref *ref = NULL;

  /* See if there are any references at all.  */
  if (iterate_referring (0, ref))
    return true;
  /* For functions check also calls.  */
  cgraph_node *cn = dyn_cast <cgraph_node *> (this);
  if (cn && cn->callers)
    {
      if (include_self)
	return true;
      for (cgraph_edge *e = cn->callers; e; e = e->next_caller)
	if (e->caller != this)
	  return true;
    }
  return false;
}

/* DECL has been parsed.  Take it, queue it, compile it at the whim of the
   logic in effect.  If NO_COLLECT is true, then our caller cannot stand to have
   the garbage collector run at the moment.  We would need to either create
   a new GC context, or just not compile right now.  */

void
cgraph_node::finalize_function (tree decl, bool no_collect)
{
  cgraph_node *node = cgraph_node::get_create (decl);

  if (node->definition)
    {
      /* Nested functions should only be defined once.  */
      gcc_assert (!DECL_CONTEXT (decl)
		  || TREE_CODE (DECL_CONTEXT (decl)) !=	FUNCTION_DECL);
      node->reset ();
      node->redefined_extern_inline = true;
    }

  /* Set definition first before calling notice_global_symbol so that
     it is available to notice_global_symbol.  */
  node->definition = true;
  notice_global_symbol (decl);
  node->lowered = DECL_STRUCT_FUNCTION (decl)->cfg != NULL;
  node->semantic_interposition = opt_for_fn (decl, flag_semantic_interposition);
  if (!flag_toplevel_reorder)
    node->no_reorder = true;

  /* With -fkeep-inline-functions we are keeping all inline functions except
     for extern inline ones.  */
  if (flag_keep_inline_functions
      && DECL_DECLARED_INLINE_P (decl)
      && !DECL_EXTERNAL (decl)
      && !DECL_DISREGARD_INLINE_LIMITS (decl))
    node->force_output = 1;

  /* __RTL functions were already output as soon as they were parsed (due
     to the large amount of global state in the backend).
     Mark such functions as "force_output" to reflect the fact that they
     will be in the asm file when considering the symbols they reference.
     The attempt to output them later on will bail out immediately.  */
  if (node->native_rtl_p ())
    node->force_output = 1;

  /* When not optimizing, also output the static functions. (see
     PR24561), but don't do so for always_inline functions, functions
     declared inline and nested functions.  These were optimized out
     in the original implementation and it is unclear whether we want
     to change the behavior here.  */
  if (((!opt_for_fn (decl, optimize) || flag_keep_static_functions
	|| node->no_reorder)
       && !node->cpp_implicit_alias
       && !DECL_DISREGARD_INLINE_LIMITS (decl)
       && !DECL_DECLARED_INLINE_P (decl)
       && !(DECL_CONTEXT (decl)
	    && TREE_CODE (DECL_CONTEXT (decl)) == FUNCTION_DECL))
      && !DECL_COMDAT (decl) && !DECL_EXTERNAL (decl))
    node->force_output = 1;

  /* If we've not yet emitted decl, tell the debug info about it.  */
  if (!TREE_ASM_WRITTEN (decl))
    (*debug_hooks->deferred_inline_function) (decl);

  if (!no_collect)
    ggc_collect ();

  if (symtab->state == CONSTRUCTION
      && (node->needed_p () || node->referred_to_p ()))
    enqueue_node (node);
}

/* Add the function FNDECL to the call graph.
   Unlike finalize_function, this function is intended to be used
   by middle end and allows insertion of new function at arbitrary point
   of compilation.  The function can be either in high, low or SSA form
   GIMPLE.

   The function is assumed to be reachable and have address taken (so no
   API breaking optimizations are performed on it).

   Main work done by this function is to enqueue the function for later
   processing to avoid need the passes to be re-entrant.  */

void
cgraph_node::add_new_function (tree fndecl, bool lowered)
{
  gcc::pass_manager *passes = g->get_passes ();
  cgraph_node *node;

  if (dump_file)
    {
      struct function *fn = DECL_STRUCT_FUNCTION (fndecl);
      const char *function_type = ((gimple_has_body_p (fndecl))
				   ? (lowered
				      ? (gimple_in_ssa_p (fn)
					 ? "ssa gimple"
					 : "low gimple")
				      : "high gimple")
				   : "to-be-gimplified");
      fprintf (dump_file,
	       "Added new %s function %s to callgraph\n",
	       function_type,
	       fndecl_name (fndecl));
    }

  switch (symtab->state)
    {
      case PARSING:
	cgraph_node::finalize_function (fndecl, false);
	break;
      case CONSTRUCTION:
	/* Just enqueue function to be processed at nearest occurrence.  */
	node = cgraph_node::get_create (fndecl);
	if (lowered)
	  node->lowered = true;
	cgraph_new_nodes.safe_push (node);
        break;

      case IPA:
      case IPA_SSA:
      case IPA_SSA_AFTER_INLINING:
      case EXPANSION:
	/* Bring the function into finalized state and enqueue for later
	   analyzing and compilation.  */
	node = cgraph_node::get_create (fndecl);
	node->local = false;
	node->definition = true;
	node->semantic_interposition = opt_for_fn (fndecl,
						   flag_semantic_interposition);
	node->force_output = true;
	if (TREE_PUBLIC (fndecl))
	  node->externally_visible = true;
	if (!lowered && symtab->state == EXPANSION)
	  {
	    push_cfun (DECL_STRUCT_FUNCTION (fndecl));
	    gimple_register_cfg_hooks ();
	    bitmap_obstack_initialize (NULL);
	    execute_pass_list (cfun, passes->all_lowering_passes);
	    passes->execute_early_local_passes ();
	    bitmap_obstack_release (NULL);
	    pop_cfun ();

	    lowered = true;
	  }
	if (lowered)
	  node->lowered = true;
	cgraph_new_nodes.safe_push (node);
        break;

      case FINISHED:
	/* At the very end of compilation we have to do all the work up
	   to expansion.  */
	node = cgraph_node::create (fndecl);
	if (lowered)
	  node->lowered = true;
	node->definition = true;
	node->semantic_interposition = opt_for_fn (fndecl,
						   flag_semantic_interposition);
	node->analyze ();
	push_cfun (DECL_STRUCT_FUNCTION (fndecl));
	gimple_register_cfg_hooks ();
	bitmap_obstack_initialize (NULL);
	if (!gimple_in_ssa_p (DECL_STRUCT_FUNCTION (fndecl)))
	  g->get_passes ()->execute_early_local_passes ();
	bitmap_obstack_release (NULL);
	pop_cfun ();
	node->expand ();
	break;

      default:
	gcc_unreachable ();
    }

  /* Set a personality if required and we already passed EH lowering.  */
  if (lowered
      && (function_needs_eh_personality (DECL_STRUCT_FUNCTION (fndecl))
	  == eh_personality_lang))
    DECL_FUNCTION_PERSONALITY (fndecl) = lang_hooks.eh_personality ();
}

/* Analyze the function scheduled to be output.  */
void
cgraph_node::analyze (void)
{
  if (native_rtl_p ())
    {
      analyzed = true;
      return;
    }

  tree decl = this->decl;
  location_t saved_loc = input_location;
  input_location = DECL_SOURCE_LOCATION (decl);
  semantic_interposition = opt_for_fn (decl, flag_semantic_interposition);

  if (thunk)
    {
      thunk_info *info = thunk_info::get (this);
      cgraph_node *t = cgraph_node::get (info->alias);

      create_edge (t, NULL, t->count);
      callees->can_throw_external = !TREE_NOTHROW (t->decl);
      /* Target code in expand_thunk may need the thunk's target
	 to be analyzed, so recurse here.  */
      if (!t->analyzed && t->definition)
	t->analyze ();
      if (t->alias)
	{
	  t = t->get_alias_target ();
	  if (!t->analyzed && t->definition)
	    t->analyze ();
	}
      bool ret = expand_thunk (this, false, false);
      thunk_info::get (this)->alias = NULL;
      if (!ret)
	return;
    }
  if (alias)
    resolve_alias (cgraph_node::get (alias_target), transparent_alias);
  else if (dispatcher_function)
    {
      /* Generate the dispatcher body of multi-versioned functions.  */
      cgraph_function_version_info *dispatcher_version_info
	= function_version ();
      if (dispatcher_version_info != NULL
          && (dispatcher_version_info->dispatcher_resolver
	      == NULL_TREE))
	{
	  tree resolver = NULL_TREE;
	  gcc_assert (targetm.generate_version_dispatcher_body);
	  resolver = targetm.generate_version_dispatcher_body (this);
	  gcc_assert (resolver != NULL_TREE);
	}
    }
  else
    {
      push_cfun (DECL_STRUCT_FUNCTION (decl));

      assign_assembler_name_if_needed (decl);

      /* Make sure to gimplify bodies only once.  During analyzing a
	 function we lower it, which will require gimplified nested
	 functions, so we can end up here with an already gimplified
	 body.  */
      if (!gimple_has_body_p (decl))
	gimplify_function_tree (decl);

      /* Lower the function.  */
      if (!lowered)
	{
	  if (first_nested_function (this))
	    lower_nested_functions (decl);

	  gimple_register_cfg_hooks ();
	  bitmap_obstack_initialize (NULL);
	  execute_pass_list (cfun, g->get_passes ()->all_lowering_passes);
	  compact_blocks ();
	  bitmap_obstack_release (NULL);
	  lowered = true;
	}

      pop_cfun ();
    }
  analyzed = true;

  input_location = saved_loc;
}

/* C++ frontend produce same body aliases all over the place, even before PCH
   gets streamed out. It relies on us linking the aliases with their function
   in order to do the fixups, but ipa-ref is not PCH safe.  Consequently we
   first produce aliases without links, but once C++ FE is sure he won't stream
   PCH we build the links via this function.  */

void
symbol_table::process_same_body_aliases (void)
{
  symtab_node *node;
  FOR_EACH_SYMBOL (node)
    if (node->cpp_implicit_alias && !node->analyzed)
      node->resolve_alias
	(VAR_P (node->alias_target)
	 ? (symtab_node *)varpool_node::get_create (node->alias_target)
	 : (symtab_node *)cgraph_node::get_create (node->alias_target));
  cpp_implicit_aliases_done = true;
}

/* Process a symver attribute.  */

static void
process_symver_attribute (symtab_node *n)
{
  tree value = lookup_attribute ("symver", DECL_ATTRIBUTES (n->decl));

  for (; value != NULL; value = TREE_CHAIN (value))
    {
      /* Starting from bintuils 2.35 gas supports:
	  # Assign foo to bar@V1 and baz@V2.
	  .symver foo, bar@V1
	  .symver foo, baz@V2
      */
      const char *purpose = IDENTIFIER_POINTER (TREE_PURPOSE (value));
      if (strcmp (purpose, "symver") != 0)
	continue;

      tree symver = get_identifier_with_length
	(TREE_STRING_POINTER (TREE_VALUE (TREE_VALUE (value))),
	 TREE_STRING_LENGTH (TREE_VALUE (TREE_VALUE (value))));
      symtab_node *def = symtab_node::get_for_asmname (symver);

      if (def)
	{
	  error_at (DECL_SOURCE_LOCATION (n->decl),
		    "duplicate definition of a symbol version");
	  inform (DECL_SOURCE_LOCATION (def->decl),
		  "same version was previously defined here");
	  return;
	}
      if (!n->definition)
	{
	  error_at (DECL_SOURCE_LOCATION (n->decl),
		    "symbol needs to be defined to have a version");
	  return;
	}
      if (DECL_COMMON (n->decl))
	{
	  error_at (DECL_SOURCE_LOCATION (n->decl),
		    "common symbol cannot be versioned");
	  return;
	}
      if (DECL_COMDAT (n->decl))
	{
	  error_at (DECL_SOURCE_LOCATION (n->decl),
		    "comdat symbol cannot be versioned");
	  return;
	}
      if (n->weakref)
	{
	  error_at (DECL_SOURCE_LOCATION (n->decl),
		    "%<weakref%> cannot be versioned");
	  return;
	}
      if (!TREE_PUBLIC (n->decl))
	{
	  error_at (DECL_SOURCE_LOCATION (n->decl),
		    "versioned symbol must be public");
	  return;
	}
      if (DECL_VISIBILITY (n->decl) != VISIBILITY_DEFAULT)
	{
	  error_at (DECL_SOURCE_LOCATION (n->decl),
		    "versioned symbol must have default visibility");
	  return;
	}

      /* Create new symbol table entry representing the version.  */
      tree new_decl = copy_node (n->decl);

      DECL_INITIAL (new_decl) = NULL_TREE;
      if (TREE_CODE (new_decl) == FUNCTION_DECL)
	DECL_STRUCT_FUNCTION (new_decl) = NULL;
      SET_DECL_ASSEMBLER_NAME (new_decl, symver);
      TREE_PUBLIC (new_decl) = 1;
      DECL_ATTRIBUTES (new_decl) = NULL;

      symtab_node *symver_node = symtab_node::get_create (new_decl);
      symver_node->alias = true;
      symver_node->definition = true;
      symver_node->symver = true;
      symver_node->create_reference (n, IPA_REF_ALIAS, NULL);
      symver_node->analyzed = true;
    }
}

/* Process attributes common for vars and functions.  */

static void
process_common_attributes (symtab_node *node, tree decl)
{
  tree weakref = lookup_attribute ("weakref", DECL_ATTRIBUTES (decl));

  if (weakref && !lookup_attribute ("alias", DECL_ATTRIBUTES (decl)))
    {
      warning_at (DECL_SOURCE_LOCATION (decl), OPT_Wattributes,
		  "%<weakref%> attribute should be accompanied with"
		  " an %<alias%> attribute");
      DECL_WEAK (decl) = 0;
      DECL_ATTRIBUTES (decl) = remove_attribute ("weakref",
						 DECL_ATTRIBUTES (decl));
    }

  if (lookup_attribute ("no_reorder", DECL_ATTRIBUTES (decl)))
    node->no_reorder = 1;
  process_symver_attribute (node);
}

/* Look for externally_visible and used attributes and mark cgraph nodes
   accordingly.

   We cannot mark the nodes at the point the attributes are processed (in
   handle_*_attribute) because the copy of the declarations available at that
   point may not be canonical.  For example, in:

    void f();
    void f() __attribute__((used));

   the declaration we see in handle_used_attribute will be the second
   declaration -- but the front end will subsequently merge that declaration
   with the original declaration and discard the second declaration.

   Furthermore, we can't mark these nodes in finalize_function because:

    void f() {}
    void f() __attribute__((externally_visible));

   is valid.

   So, we walk the nodes at the end of the translation unit, applying the
   attributes at that point.  */

static void
process_function_and_variable_attributes (cgraph_node *first,
                                          varpool_node *first_var)
{
  cgraph_node *node;
  varpool_node *vnode;

  for (node = symtab->first_function (); node != first;
       node = symtab->next_function (node))
    {
      tree decl = node->decl;

      if (node->alias
	  && lookup_attribute ("flatten", DECL_ATTRIBUTES (decl)))
	{
	  tree tdecl = node->get_alias_target_tree ();
	  if (!tdecl || !DECL_P (tdecl)
	      || !lookup_attribute ("flatten", DECL_ATTRIBUTES (tdecl)))
	    warning_at (DECL_SOURCE_LOCATION (decl), OPT_Wattributes,
			"%<flatten%> attribute is ignored on aliases");
	}
      if (DECL_PRESERVE_P (decl))
	node->mark_force_output ();
      else if (lookup_attribute ("externally_visible", DECL_ATTRIBUTES (decl)))
	{
	  if (! TREE_PUBLIC (node->decl))
	    warning_at (DECL_SOURCE_LOCATION (node->decl), OPT_Wattributes,
			"%<externally_visible%>"
			" attribute have effect only on public objects");
	}
      if (lookup_attribute ("weakref", DECL_ATTRIBUTES (decl))
	  && node->definition
	  && (!node->alias || DECL_INITIAL (decl) != error_mark_node))
	{
	  /* NODE->DEFINITION && NODE->ALIAS is nonzero for valid weakref
	     function declarations; DECL_INITIAL is non-null for invalid
	     weakref functions that are also defined.  */
	  warning_at (DECL_SOURCE_LOCATION (decl), OPT_Wattributes,
		      "%<weakref%> attribute ignored"
		      " because function is defined");
	  DECL_WEAK (decl) = 0;
	  DECL_ATTRIBUTES (decl) = remove_attribute ("weakref",
						     DECL_ATTRIBUTES (decl));
	  DECL_ATTRIBUTES (decl) = remove_attribute ("alias",
						     DECL_ATTRIBUTES (decl));
	  node->alias = false;
	  node->weakref = false;
	  node->transparent_alias = false;
	}
      else if (lookup_attribute ("alias", DECL_ATTRIBUTES (decl))
	  && node->definition
	  && !node->alias)
	warning_at (DECL_SOURCE_LOCATION (node->decl), OPT_Wattributes,
		    "%<alias%> attribute ignored"
		    " because function is defined");

      if (lookup_attribute ("always_inline", DECL_ATTRIBUTES (decl))
	  && !DECL_DECLARED_INLINE_P (decl)
	  /* redefining extern inline function makes it DECL_UNINLINABLE.  */
	  && !DECL_UNINLINABLE (decl))
	warning_at (DECL_SOURCE_LOCATION (decl), OPT_Wattributes,
		    "%<always_inline%> function might not be inlinable"
		    " unless also declared %<inline%>");

      process_common_attributes (node, decl);
    }
  for (vnode = symtab->first_variable (); vnode != first_var;
       vnode = symtab->next_variable (vnode))
    {
      tree decl = vnode->decl;
      if (DECL_EXTERNAL (decl)
	  && DECL_INITIAL (decl))
	varpool_node::finalize_decl (decl);
      if (DECL_PRESERVE_P (decl))
	vnode->force_output = true;
      else if (lookup_attribute ("externally_visible", DECL_ATTRIBUTES (decl)))
	{
	  if (! TREE_PUBLIC (vnode->decl))
	    warning_at (DECL_SOURCE_LOCATION (vnode->decl), OPT_Wattributes,
			"%<externally_visible%>"
			" attribute have effect only on public objects");
	}
      if (lookup_attribute ("weakref", DECL_ATTRIBUTES (decl))
	  && vnode->definition
	  && DECL_INITIAL (decl))
	{
	  warning_at (DECL_SOURCE_LOCATION (vnode->decl), OPT_Wattributes,
		      "%<weakref%> attribute ignored"
		      " because variable is initialized");
	  DECL_WEAK (decl) = 0;
	  DECL_ATTRIBUTES (decl) = remove_attribute ("weakref",
						      DECL_ATTRIBUTES (decl));
	}
      process_common_attributes (vnode, decl);
    }
}

/* Mark DECL as finalized.  By finalizing the declaration, frontend instruct the
   middle end to output the variable to asm file, if needed or externally
   visible.  */

void
varpool_node::finalize_decl (tree decl)
{
  varpool_node *node = varpool_node::get_create (decl);

  gcc_assert (TREE_STATIC (decl) || DECL_EXTERNAL (decl));

  if (node->definition)
    return;
  /* Set definition first before calling notice_global_symbol so that
     it is available to notice_global_symbol.  */
  node->definition = true;
  node->semantic_interposition = flag_semantic_interposition;
  notice_global_symbol (decl);
  if (!flag_toplevel_reorder)
    node->no_reorder = true;
  if (TREE_THIS_VOLATILE (decl) || DECL_PRESERVE_P (decl)
      /* Traditionally we do not eliminate static variables when not
	 optimizing and when not doing toplevel reorder.  */
      || (node->no_reorder && !DECL_COMDAT (node->decl)
	  && !DECL_ARTIFICIAL (node->decl)))
    node->force_output = true;

  if (flag_openmp)
    {
      tree attr = lookup_attribute ("omp allocate", DECL_ATTRIBUTES (decl));
      if (attr)
	{
	  tree align = TREE_VALUE (TREE_VALUE (attr));
	  if (align)
	    SET_DECL_ALIGN (decl, MAX (tree_to_uhwi (align) * BITS_PER_UNIT,
				       DECL_ALIGN (decl)));
	}
    }

  if (symtab->state == CONSTRUCTION
      && (node->needed_p () || node->referred_to_p ()))
    enqueue_node (node);
  if (symtab->state >= IPA_SSA)
    node->analyze ();
  /* Some frontends produce various interface variables after compilation
     finished.  */
  if (symtab->state == FINISHED
      || (node->no_reorder
	  && symtab->state == EXPANSION))
    node->assemble_decl ();
}

/* EDGE is an polymorphic call.  Mark all possible targets as reachable
   and if there is only one target, perform trivial devirtualization.
   REACHABLE_CALL_TARGETS collects target lists we already walked to
   avoid duplicate work.  */

static void
walk_polymorphic_call_targets (hash_set<void *> *reachable_call_targets,
			       cgraph_edge *edge)
{
  unsigned int i;
  void *cache_token;
  bool final;
  vec <cgraph_node *>targets
    = possible_polymorphic_call_targets
	(edge, &final, &cache_token);

  if (cache_token != NULL && !reachable_call_targets->add (cache_token))
    {
      if (symtab->dump_file)
	dump_possible_polymorphic_call_targets
	  (symtab->dump_file, edge);

      for (i = 0; i < targets.length (); i++)
	{
	  /* Do not bother to mark virtual methods in anonymous namespace;
	     either we will find use of virtual table defining it, or it is
	     unused.  */
	  if (targets[i]->definition
	      && TREE_CODE
		  (TREE_TYPE (targets[i]->decl))
		   == METHOD_TYPE
	      && !type_in_anonymous_namespace_p
		   (TYPE_METHOD_BASETYPE (TREE_TYPE (targets[i]->decl))))
	    enqueue_node (targets[i]);
	}
    }

  /* Very trivial devirtualization; when the type is
     final or anonymous (so we know all its derivation)
     and there is only one possible virtual call target,
     make the edge direct.  */
  if (final)
    {
      if (targets.length () <= 1 && dbg_cnt (devirt))
	{
	  cgraph_node *target;
	  if (targets.length () == 1)
	    target = targets[0];
	  else
	    target = cgraph_node::create (builtin_decl_unreachable ());

	  if (symtab->dump_file)
	    {
	      fprintf (symtab->dump_file,
		       "Devirtualizing call: ");
	      print_gimple_stmt (symtab->dump_file,
				 edge->call_stmt, 0,
				 TDF_SLIM);
	    }
          if (dump_enabled_p ())
            {
	      dump_printf_loc (MSG_OPTIMIZED_LOCATIONS, edge->call_stmt,
			       "devirtualizing call in %s to %s\n",
			       edge->caller->dump_name (),
			       target->dump_name ());
	    }

	  edge = cgraph_edge::make_direct (edge, target);
	  gimple *new_call = cgraph_edge::redirect_call_stmt_to_callee (edge);

	  if (symtab->dump_file)
	    {
	      fprintf (symtab->dump_file, "Devirtualized as: ");
	      print_gimple_stmt (symtab->dump_file, new_call, 0, TDF_SLIM);
	    }
	}
    }
}

/* Issue appropriate warnings for the global declaration DECL.  */

static void
check_global_declaration (symtab_node *snode)
{
  const char *decl_file;
  tree decl = snode->decl;

  /* Warn about any function declared static but not defined.  We don't
     warn about variables, because many programs have static variables
     that exist only to get some text into the object file.  */
  if (TREE_CODE (decl) == FUNCTION_DECL
      && DECL_INITIAL (decl) == 0
      && DECL_EXTERNAL (decl)
      && ! DECL_ARTIFICIAL (decl)
      && ! TREE_PUBLIC (decl))
    {
      if (warning_suppressed_p (decl, OPT_Wunused))
	;
      else if (snode->referred_to_p (/*include_self=*/false))
	pedwarn (input_location, 0, "%q+F used but never defined", decl);
      else
	warning (OPT_Wunused_function, "%q+F declared %<static%> but never "
				       "defined", decl);
    }

  /* Warn about static fns or vars defined but not used.  */
  if (((warn_unused_function && TREE_CODE (decl) == FUNCTION_DECL)
       || (((warn_unused_variable && ! TREE_READONLY (decl))
	    || (warn_unused_const_variable > 0 && TREE_READONLY (decl)
		&& (warn_unused_const_variable == 2
		    || (main_input_filename != NULL
			&& (decl_file = DECL_SOURCE_FILE (decl)) != NULL
			&& filename_cmp (main_input_filename,
					 decl_file) == 0))))
	   && VAR_P (decl)))
      && ! DECL_IN_SYSTEM_HEADER (decl)
      && ! snode->referred_to_p (/*include_self=*/false)
      /* This TREE_USED check is needed in addition to referred_to_p
	 above, because the `__unused__' attribute is not being
	 considered for referred_to_p.  */
      && ! TREE_USED (decl)
      /* The TREE_USED bit for file-scope decls is kept in the identifier,
	 to handle multiple external decls in different scopes.  */
      && ! (DECL_NAME (decl) && TREE_USED (DECL_NAME (decl)))
      && ! DECL_EXTERNAL (decl)
      && ! DECL_ARTIFICIAL (decl)
      && ! DECL_ABSTRACT_ORIGIN (decl)
      && ! TREE_PUBLIC (decl)
      /* A volatile variable might be used in some non-obvious way.  */
      && (! VAR_P (decl) || ! TREE_THIS_VOLATILE (decl))
      /* Global register variables must be declared to reserve them.  */
      && ! (VAR_P (decl) && DECL_REGISTER (decl))
      /* Global ctors and dtors are called by the runtime.  */
      && (TREE_CODE (decl) != FUNCTION_DECL
	  || (!DECL_STATIC_CONSTRUCTOR (decl)
	      && !DECL_STATIC_DESTRUCTOR (decl)))
      && (! VAR_P (decl) || !warning_suppressed_p (decl, OPT_Wunused_variable))
      /* Otherwise, ask the language.  */
      && lang_hooks.decls.warn_unused_global (decl))
    warning_at (DECL_SOURCE_LOCATION (decl),
		(TREE_CODE (decl) == FUNCTION_DECL)
		? OPT_Wunused_function
		: (TREE_READONLY (decl)
		   ? OPT_Wunused_const_variable_
		   : OPT_Wunused_variable),
		"%qD defined but not used", decl);
}

/* Discover all functions and variables that are trivially needed, analyze
   them as well as all functions and variables referred by them  */
static cgraph_node *first_analyzed;
static varpool_node *first_analyzed_var;

/* FIRST_TIME is set to TRUE for the first time we are called for a
   translation unit from finalize_compilation_unit() or false
   otherwise.  */

static void
analyze_functions (bool first_time)
{
  /* Keep track of already processed nodes when called multiple times for
     intermodule optimization.  */
  cgraph_node *first_handled = first_analyzed;
  varpool_node *first_handled_var = first_analyzed_var;
  hash_set<void *> reachable_call_targets;

  symtab_node *node;
  symtab_node *next;
  int i;
  ipa_ref *ref;
  bool changed = true;
  location_t saved_loc = input_location;

  bitmap_obstack_initialize (NULL);
  symtab->state = CONSTRUCTION;
  input_location = UNKNOWN_LOCATION;

  thunk_info::process_early_thunks ();

  /* Ugly, but the fixup cannot happen at a time same body alias is created;
     C++ FE is confused about the COMDAT groups being right.  */
  if (symtab->cpp_implicit_aliases_done)
    FOR_EACH_SYMBOL (node)
      if (node->cpp_implicit_alias)
	  node->fixup_same_cpp_alias_visibility (node->get_alias_target ());
  build_type_inheritance_graph ();

  if (flag_openmp && first_time)
    omp_discover_implicit_declare_target ();

  /* Analysis adds static variables that in turn adds references to new functions.
     So we need to iterate the process until it stabilize.  */
  while (changed)
    {
      changed = false;
      process_function_and_variable_attributes (first_analyzed,
						first_analyzed_var);

      /* First identify the trivially needed symbols.  */
      for (node = symtab->first_symbol ();
	   node != first_analyzed
	   && node != first_analyzed_var; node = node->next)
	{
	  /* Convert COMDAT group designators to IDENTIFIER_NODEs.  */
	  node->get_comdat_group_id ();
	  if (node->needed_p ())
	    {
	      enqueue_node (node);
	      if (!changed && symtab->dump_file)
		fprintf (symtab->dump_file, "Trivially needed symbols:");
	      changed = true;
	      if (symtab->dump_file)
		fprintf (symtab->dump_file, " %s", node->dump_asm_name ());
	    }
	  if (node == first_analyzed
	      || node == first_analyzed_var)
	    break;
	}
      symtab->process_new_functions ();
      first_analyzed_var = symtab->first_variable ();
      first_analyzed = symtab->first_function ();

      if (changed && symtab->dump_file)
	fprintf (symtab->dump_file, "\n");

      /* Lower representation, build callgraph edges and references for all trivially
         needed symbols and all symbols referred by them.  */
      while (queued_nodes != &symtab_terminator)
	{
	  changed = true;
	  node = queued_nodes;
	  queued_nodes = (symtab_node *)queued_nodes->aux;
	  cgraph_node *cnode = dyn_cast <cgraph_node *> (node);
	  if (cnode && cnode->definition)
	    {
	      cgraph_edge *edge;
	      tree decl = cnode->decl;

	      /* ??? It is possible to create extern inline function
	      and later using weak alias attribute to kill its body.
	      See gcc.c-torture/compile/20011119-1.c  */
	      if (!DECL_STRUCT_FUNCTION (decl)
		  && !cnode->alias
		  && !cnode->thunk
		  && !cnode->dispatcher_function)
		{
		  cnode->reset ();
		  cnode->redefined_extern_inline = true;
		  continue;
		}

	      if (!cnode->analyzed)
		cnode->analyze ();

	      for (edge = cnode->callees; edge; edge = edge->next_callee)
		if (edge->callee->definition
		    && (!DECL_EXTERNAL (edge->callee->decl)
			/* When not optimizing, do not try to analyze extern
			   inline functions.  Doing so is pointless.  */
			|| opt_for_fn (edge->callee->decl, optimize)
			/* Weakrefs needs to be preserved.  */
			|| edge->callee->alias
			/* always_inline functions are inlined even at -O0.  */
		        || lookup_attribute
				 ("always_inline",
			          DECL_ATTRIBUTES (edge->callee->decl))
			/* Multiversioned functions needs the dispatcher to
			   be produced locally even for extern functions.  */
			|| edge->callee->function_version ()))
		   enqueue_node (edge->callee);
	      if (opt_for_fn (cnode->decl, optimize)
		  && opt_for_fn (cnode->decl, flag_devirtualize))
		{
		  cgraph_edge *next;

		  for (edge = cnode->indirect_calls; edge; edge = next)
		    {
		      next = edge->next_callee;
		      if (edge->indirect_info->polymorphic)
			walk_polymorphic_call_targets (&reachable_call_targets,
						       edge);
		    }
		}

	      /* If decl is a clone of an abstract function,
		 mark that abstract function so that we don't release its body.
		 The DECL_INITIAL() of that abstract function declaration
		 will be later needed to output debug info.  */
	      if (DECL_ABSTRACT_ORIGIN (decl))
		{
		  cgraph_node *origin_node
		    = cgraph_node::get_create (DECL_ABSTRACT_ORIGIN (decl));
		  origin_node->used_as_abstract_origin = true;
		}
	      /* Preserve a functions function context node.  It will
		 later be needed to output debug info.  */
	      if (tree fn = decl_function_context (decl))
		{
		  cgraph_node *origin_node = cgraph_node::get_create (fn);
		  enqueue_node (origin_node);
		}
	    }
	  else
	    {
	      varpool_node *vnode = dyn_cast <varpool_node *> (node);
	      if (vnode && vnode->definition && !vnode->analyzed)
		vnode->analyze ();
	    }

	  if (node->same_comdat_group)
	    {
	      symtab_node *next;
	      for (next = node->same_comdat_group;
		   next != node;
		   next = next->same_comdat_group)
		if (!next->comdat_local_p ())
		  enqueue_node (next);
	    }
	  for (i = 0; node->iterate_reference (i, ref); i++)
	    if (ref->referred->definition
		&& (!DECL_EXTERNAL (ref->referred->decl)
		    || ((TREE_CODE (ref->referred->decl) != FUNCTION_DECL
			 && optimize)
			|| (TREE_CODE (ref->referred->decl) == FUNCTION_DECL
			    && opt_for_fn (ref->referred->decl, optimize))
		    || node->alias
		    || ref->referred->alias)))
	      enqueue_node (ref->referred);
	  symtab->process_new_functions ();
	}
    }
  update_type_inheritance_graph ();

  /* Collect entry points to the unit.  */
  if (symtab->dump_file)
    {
      fprintf (symtab->dump_file, "\n\nInitial ");
      symtab->dump (symtab->dump_file);
    }

  if (first_time)
    {
      symtab_node *snode;
      FOR_EACH_SYMBOL (snode)
	check_global_declaration (snode);
    }

  if (symtab->dump_file)
    fprintf (symtab->dump_file, "\nRemoving unused symbols:");

  for (node = symtab->first_symbol ();
       node != first_handled
       && node != first_handled_var; node = next)
    {
      next = node->next;
      /* For symbols declared locally we clear TREE_READONLY when emitting
	 the constructor (if one is needed).  For external declarations we can
	 not safely assume that the type is readonly because we may be called
	 during its construction.  */
      if (TREE_CODE (node->decl) == VAR_DECL
	  && TYPE_P (TREE_TYPE (node->decl))
	  && TYPE_NEEDS_CONSTRUCTING (TREE_TYPE (node->decl))
	  && DECL_EXTERNAL (node->decl))
	TREE_READONLY (node->decl) = 0;
      if (!node->aux && !node->referred_to_p ())
	{
	  if (symtab->dump_file)
	    fprintf (symtab->dump_file, " %s", node->dump_name ());

	  /* See if the debugger can use anything before the DECL
	     passes away.  Perhaps it can notice a DECL that is now a
	     constant and can tag the early DIE with an appropriate
	     attribute.

	     Otherwise, this is the last chance the debug_hooks have
	     at looking at optimized away DECLs, since
	     late_global_decl will subsequently be called from the
	     contents of the now pruned symbol table.  */
	  if (VAR_P (node->decl)
	      && !decl_function_context (node->decl))
	    {
	      /* We are reclaiming totally unreachable code and variables
	         so they effectively appear as readonly.  Show that to
		 the debug machinery.  */
	      TREE_READONLY (node->decl) = 1;
	      node->definition = false;
	      (*debug_hooks->late_global_decl) (node->decl);
	    }

	  node->remove ();
	  continue;
	}
      if (cgraph_node *cnode = dyn_cast <cgraph_node *> (node))
	{
	  tree decl = node->decl;

	  if (cnode->definition && !gimple_has_body_p (decl)
	      && !cnode->alias
	      && !cnode->thunk)
	    cnode->reset ();

	  gcc_assert (!cnode->definition || cnode->thunk
		      || cnode->alias
		      || gimple_has_body_p (decl)
		      || cnode->native_rtl_p ());
	  gcc_assert (cnode->analyzed == cnode->definition);
	}
      node->aux = NULL;
    }
  for (;node; node = node->next)
    node->aux = NULL;
  first_analyzed = symtab->first_function ();
  first_analyzed_var = symtab->first_variable ();
  if (symtab->dump_file)
    {
      fprintf (symtab->dump_file, "\n\nReclaimed ");
      symtab->dump (symtab->dump_file);
    }
  bitmap_obstack_release (NULL);
  ggc_collect ();
  /* Initialize assembler name hash, in particular we want to trigger C++
     mangling and same body alias creation before we free DECL_ARGUMENTS
     used by it.  */
  if (!seen_error ())
    symtab->symtab_initialize_asm_name_hash ();

  input_location = saved_loc;
}

/* Check declaration of the type of ALIAS for compatibility with its TARGET
   (which may be an ifunc resolver) and issue a diagnostic when they are
   not compatible according to language rules (plus a C++ extension for
   non-static member functions).  */

static void
maybe_diag_incompatible_alias (tree alias, tree target)
{
  tree altype = TREE_TYPE (alias);
  tree targtype = TREE_TYPE (target);

  bool ifunc = cgraph_node::get (alias)->ifunc_resolver;
  tree funcptr = altype;

  if (ifunc)
    {
      /* Handle attribute ifunc first.  */
      if (TREE_CODE (altype) == METHOD_TYPE)
	{
	  /* Set FUNCPTR to the type of the alias target.  If the type
	     is a non-static member function of class C, construct a type
	     of an ordinary function taking C* as the first argument,
	     followed by the member function argument list, and use it
	     instead to check for incompatibility.  This conversion is
	     not defined by the language but an extension provided by
	     G++.  */

	  tree rettype = TREE_TYPE (altype);
	  tree args = TYPE_ARG_TYPES (altype);
	  altype = build_function_type (rettype, args);
	  funcptr = altype;
	}

      targtype = TREE_TYPE (targtype);

      if (POINTER_TYPE_P (targtype))
	{
	  targtype = TREE_TYPE (targtype);

	  /* Only issue Wattribute-alias for conversions to void* with
	     -Wextra.  */
	  if (VOID_TYPE_P (targtype) && !extra_warnings)
	    return;

	  /* Proceed to handle incompatible ifunc resolvers below.  */
	}
      else
	{
	  funcptr = build_pointer_type (funcptr);

	  error_at (DECL_SOURCE_LOCATION (target),
		    "%<ifunc%> resolver for %qD must return %qT",
		 alias, funcptr);
	  inform (DECL_SOURCE_LOCATION (alias),
		  "resolver indirect function declared here");
	  return;
	}
    }

  if ((!FUNC_OR_METHOD_TYPE_P (targtype)
       || (prototype_p (altype)
	   && prototype_p (targtype)
	   && !types_compatible_p (altype, targtype))))
    {
      /* Warn for incompatibilities.  Avoid warning for functions
	 without a prototype to make it possible to declare aliases
	 without knowing the exact type, as libstdc++ does.  */
      if (ifunc)
	{
	  funcptr = build_pointer_type (funcptr);

	  auto_diagnostic_group d;
	  if (warning_at (DECL_SOURCE_LOCATION (target),
			  OPT_Wattribute_alias_,
			  "%<ifunc%> resolver for %qD should return %qT",
			  alias, funcptr))
	    inform (DECL_SOURCE_LOCATION (alias),
		    "resolver indirect function declared here");
	}
      else
	{
	  auto_diagnostic_group d;
	  if (warning_at (DECL_SOURCE_LOCATION (alias),
			    OPT_Wattribute_alias_,
			    "%qD alias between functions of incompatible "
			    "types %qT and %qT", alias, altype, targtype))
	    inform (DECL_SOURCE_LOCATION (target),
		    "aliased declaration here");
	}
    }
}

/* Translate the ugly representation of aliases as alias pairs into nice
   representation in callgraph.  We don't handle all cases yet,
   unfortunately.  */

static void
handle_alias_pairs (void)
{
  alias_pair *p;
  unsigned i;

  for (i = 0; alias_pairs && alias_pairs->iterate (i, &p);)
    {
      symtab_node *target_node = symtab_node::get_for_asmname (p->target);

      /* Weakrefs with target not defined in current unit are easy to handle:
	 they behave just as external variables except we need to note the
	 alias flag to later output the weakref pseudo op into asm file.  */
      if (!target_node
	  && lookup_attribute ("weakref", DECL_ATTRIBUTES (p->decl)) != NULL)
	{
	  symtab_node *node = symtab_node::get (p->decl);
	  if (node)
	    {
	      node->alias_target = p->target;
	      node->weakref = true;
	      node->alias = true;
	      node->transparent_alias = true;
	    }
	  alias_pairs->unordered_remove (i);
	  continue;
	}
      else if (!target_node)
	{
	  error ("%q+D aliased to undefined symbol %qE", p->decl, p->target);
	  symtab_node *node = symtab_node::get (p->decl);
	  if (node)
	    node->alias = false;
	  alias_pairs->unordered_remove (i);
	  continue;
	}

      if (DECL_EXTERNAL (target_node->decl)
	  /* We use local aliases for C++ thunks to force the tailcall
	     to bind locally.  This is a hack - to keep it working do
	     the following (which is not strictly correct).  */
	  && (TREE_CODE (target_node->decl) != FUNCTION_DECL
	      || ! DECL_VIRTUAL_P (target_node->decl))
	  && ! lookup_attribute ("weakref", DECL_ATTRIBUTES (p->decl)))
	{
	  error ("%q+D aliased to external symbol %qE",
		 p->decl, p->target);
	}

      if (TREE_CODE (p->decl) == FUNCTION_DECL
          && target_node && is_a <cgraph_node *> (target_node))
	{
	  maybe_diag_incompatible_alias (p->decl, target_node->decl);

	  maybe_diag_alias_attributes (p->decl, target_node->decl);

	  cgraph_node *src_node = cgraph_node::get (p->decl);
	  if (src_node && src_node->definition)
	    src_node->reset ();
	  cgraph_node::create_alias (p->decl, target_node->decl);
	  alias_pairs->unordered_remove (i);
	}
      else if (VAR_P (p->decl)
	       && target_node && is_a <varpool_node *> (target_node))
	{
	  varpool_node::create_alias (p->decl, target_node->decl);
	  alias_pairs->unordered_remove (i);
	}
      else
	{
	  error ("%q+D alias between function and variable is not supported",
		 p->decl);
	  inform (DECL_SOURCE_LOCATION (target_node->decl),
		  "aliased declaration here");

	  alias_pairs->unordered_remove (i);
	}
    }
  vec_free (alias_pairs);
}


/* Figure out what functions we want to assemble.  */

static void
mark_functions_to_output (void)
{
  bool check_same_comdat_groups = false;
  cgraph_node *node;

  if (flag_checking)
    FOR_EACH_FUNCTION (node)
      gcc_assert (!node->process);

  FOR_EACH_FUNCTION (node)
    {
      tree decl = node->decl;

      gcc_assert (!node->process || node->same_comdat_group);
      if (node->process)
	continue;

      /* We need to output all local functions that are used and not
	 always inlined, as well as those that are reachable from
	 outside the current compilation unit.  */
      if (node->analyzed
	  && !node->thunk
	  && !node->alias
	  && !node->inlined_to
	  && !TREE_ASM_WRITTEN (decl)
	  && !DECL_EXTERNAL (decl))
	{
	  node->process = 1;
	  if (node->same_comdat_group)
	    {
	      cgraph_node *next;
	      for (next = dyn_cast<cgraph_node *> (node->same_comdat_group);
		   next != node;
		   next = dyn_cast<cgraph_node *> (next->same_comdat_group))
		if (!next->thunk && !next->alias
		    && !next->comdat_local_p ())
		  next->process = 1;
	    }
	}
      else if (node->same_comdat_group)
	{
	  if (flag_checking)
	    check_same_comdat_groups = true;
	}
      else
	{
	  /* We should've reclaimed all functions that are not needed.  */
	  if (flag_checking
	      && !node->inlined_to
	      && gimple_has_body_p (decl)
	      /* FIXME: in ltrans unit when offline copy is outside partition but inline copies
		 are inside partition, we can end up not removing the body since we no longer
		 have analyzed node pointing to it.  */
	      && !node->in_other_partition
	      && !node->alias
	      && !node->clones
	      && !DECL_EXTERNAL (decl))
	    {
	      node->debug ();
	      internal_error ("failed to reclaim unneeded function");
	    }
	  gcc_assert (node->inlined_to
		      || !gimple_has_body_p (decl)
		      || node->in_other_partition
		      || node->clones
		      || DECL_ARTIFICIAL (decl)
		      || DECL_EXTERNAL (decl));

	}

    }
  if (flag_checking && check_same_comdat_groups)
    FOR_EACH_FUNCTION (node)
      if (node->same_comdat_group && !node->process)
	{
	  tree decl = node->decl;
	  if (!node->inlined_to
	      && gimple_has_body_p (decl)
	      /* FIXME: in an ltrans unit when the offline copy is outside a
		 partition but inline copies are inside a partition, we can
		 end up not removing the body since we no longer have an
		 analyzed node pointing to it.  */
	      && !node->in_other_partition
	      && !node->clones
	      && !DECL_EXTERNAL (decl))
	    {
	      node->debug ();
	      internal_error ("failed to reclaim unneeded function in same "
			      "comdat group");
	    }
	}
}

/* DECL is FUNCTION_DECL.  Initialize datastructures so DECL is a function
   in lowered gimple form.  IN_SSA is true if the gimple is in SSA.

   Set current_function_decl and cfun to newly constructed empty function body.
   return basic block in the function body.  */

basic_block
init_lowered_empty_function (tree decl, bool in_ssa, profile_count count)
{
  basic_block bb;
  edge e;

  current_function_decl = decl;
  allocate_struct_function (decl, false);
  gimple_register_cfg_hooks ();
  init_empty_tree_cfg ();
  init_tree_ssa (cfun);

  if (in_ssa)
    {
      init_ssa_operands (cfun);
      cfun->gimple_df->in_ssa_p = true;
      cfun->curr_properties |= PROP_ssa;
    }

  DECL_INITIAL (decl) = make_node (BLOCK);
  BLOCK_SUPERCONTEXT (DECL_INITIAL (decl)) = decl;

  DECL_SAVED_TREE (decl) = error_mark_node;
  cfun->curr_properties |= (PROP_gimple_lcf | PROP_gimple_leh | PROP_gimple_any
			    | PROP_cfg | PROP_loops);

  set_loops_for_fn (cfun, ggc_cleared_alloc<loops> ());
  init_loops_structure (cfun, loops_for_fn (cfun), 1);
  loops_for_fn (cfun)->state |= LOOPS_MAY_HAVE_MULTIPLE_LATCHES;

  /* Create BB for body of the function and connect it properly.  */
  ENTRY_BLOCK_PTR_FOR_FN (cfun)->count = count;
  EXIT_BLOCK_PTR_FOR_FN (cfun)->count = count;
  bb = create_basic_block (NULL, ENTRY_BLOCK_PTR_FOR_FN (cfun));
  bb->count = count;
  e = make_edge (ENTRY_BLOCK_PTR_FOR_FN (cfun), bb, EDGE_FALLTHRU);
  e->probability = profile_probability::always ();
  e = make_edge (bb, EXIT_BLOCK_PTR_FOR_FN (cfun), 0);
  e->probability = profile_probability::always ();
  add_bb_to_loop (bb, ENTRY_BLOCK_PTR_FOR_FN (cfun)->loop_father);

  return bb;
}

/* Assemble thunks and aliases associated to node.  */

void
cgraph_node::assemble_thunks_and_aliases (void)
{
  cgraph_edge *e;
  ipa_ref *ref;

  for (e = callers; e;)
    if (e->caller->thunk
	&& !e->caller->inlined_to)
      {
	cgraph_node *thunk = e->caller;

	e = e->next_caller;
	expand_thunk (thunk, !rtl_dump_and_exit, false);
	thunk->assemble_thunks_and_aliases ();
      }
    else
      e = e->next_caller;

  FOR_EACH_ALIAS (this, ref)
    {
      cgraph_node *alias = dyn_cast <cgraph_node *> (ref->referring);
      if (!alias->transparent_alias)
	{
	  bool saved_written = TREE_ASM_WRITTEN (decl);

	  /* Force assemble_alias to really output the alias this time instead
	     of buffering it in same alias pairs.  */
	  TREE_ASM_WRITTEN (decl) = 1;
	  if (alias->symver)
	    do_assemble_symver (alias->decl,
				DECL_ASSEMBLER_NAME (decl));
	  else
	    do_assemble_alias (alias->decl,
			       DECL_ASSEMBLER_NAME (decl));
	  alias->assemble_thunks_and_aliases ();
	  TREE_ASM_WRITTEN (decl) = saved_written;
	}
    }
}

/* Expand function specified by node.  */

void
cgraph_node::expand (void)
{
  location_t saved_loc;

  /* We ought to not compile any inline clones.  */
  gcc_assert (!inlined_to);

  /* __RTL functions are compiled as soon as they are parsed, so don't
     do it again.  */
  if (native_rtl_p ())
    return;

  announce_function (decl);
  process = 0;
  gcc_assert (lowered);

  /* Initialize the default bitmap obstack.  */
  bitmap_obstack_initialize (NULL);
  get_untransformed_body ();

  /* Generate RTL for the body of DECL.  */

  timevar_push (TV_REST_OF_COMPILATION);

  gcc_assert (symtab->global_info_ready);

  /* Initialize the RTL code for the function.  */
  saved_loc = input_location;
  input_location = DECL_SOURCE_LOCATION (decl);

  gcc_assert (DECL_STRUCT_FUNCTION (decl));
  push_cfun (DECL_STRUCT_FUNCTION (decl));
  init_function_start (decl);

  gimple_register_cfg_hooks ();

  bitmap_obstack_initialize (&reg_obstack); /* FIXME, only at RTL generation*/

  update_ssa (TODO_update_ssa_only_virtuals);
  if (ipa_transforms_to_apply.exists ())
    execute_all_ipa_transforms (false);

  /* Perform all tree transforms and optimizations.  */

  /* Signal the start of passes.  */
  invoke_plugin_callbacks (PLUGIN_ALL_PASSES_START, NULL);

  execute_pass_list (cfun, g->get_passes ()->all_passes);

  /* Signal the end of passes.  */
  invoke_plugin_callbacks (PLUGIN_ALL_PASSES_END, NULL);

  bitmap_obstack_release (&reg_obstack);

  /* Release the default bitmap obstack.  */
  bitmap_obstack_release (NULL);

  /* If requested, warn about function definitions where the function will
     return a value (usually of some struct or union type) which itself will
     take up a lot of stack space.  */
  if (!DECL_EXTERNAL (decl) && TREE_TYPE (decl))
    {
      tree ret_type = TREE_TYPE (TREE_TYPE (decl));

      if (ret_type && TYPE_SIZE_UNIT (ret_type)
	  && TREE_CODE (TYPE_SIZE_UNIT (ret_type)) == INTEGER_CST
	  && compare_tree_int (TYPE_SIZE_UNIT (ret_type),
			       warn_larger_than_size) > 0)
	{
	  unsigned int size_as_int
	    = TREE_INT_CST_LOW (TYPE_SIZE_UNIT (ret_type));

	  if (compare_tree_int (TYPE_SIZE_UNIT (ret_type), size_as_int) == 0)
	    warning (OPT_Wlarger_than_,
		     "size of return value of %q+D is %u bytes",
                     decl, size_as_int);
	  else
	    warning (OPT_Wlarger_than_,
		     "size of return value of %q+D is larger than %wu bytes",
	             decl, warn_larger_than_size);
	}
    }

  gimple_set_body (decl, NULL);
  if (DECL_STRUCT_FUNCTION (decl) == 0)
    {
      /* Stop pointing to the local nodes about to be freed.
	 But DECL_INITIAL must remain nonzero so we know this
	 was an actual function definition.  */
      if (DECL_INITIAL (decl) != 0)
	DECL_INITIAL (decl) = error_mark_node;
    }

  input_location = saved_loc;

  ggc_collect ();
  timevar_pop (TV_REST_OF_COMPILATION);

  if (DECL_STRUCT_FUNCTION (decl)
      && DECL_STRUCT_FUNCTION (decl)->assume_function)
    {
      /* Assume functions aren't expanded into RTL, on the other side
	 we don't want to release their body.  */
      if (cfun)
	pop_cfun ();
      return;
    }

  /* Make sure that BE didn't give up on compiling.  */
  gcc_assert (TREE_ASM_WRITTEN (decl));
  if (cfun)
    pop_cfun ();

  /* It would make a lot more sense to output thunks before function body to
     get more forward and fewer backward jumps.  This however would need
     solving problem with comdats.  See PR48668.  Also aliases must come after
     function itself to make one pass assemblers, like one on AIX, happy.
     See PR 50689.
     FIXME: Perhaps thunks should be move before function IFF they are not in
     comdat groups.  */
  assemble_thunks_and_aliases ();
  release_body ();
}

/* Node comparator that is responsible for the order that corresponds
   to time when a function was launched for the first time.  */

int
tp_first_run_node_cmp (const void *pa, const void *pb)
{
  const cgraph_node *a = *(const cgraph_node * const *) pa;
  const cgraph_node *b = *(const cgraph_node * const *) pb;
  unsigned int tp_first_run_a = a->tp_first_run;
  unsigned int tp_first_run_b = b->tp_first_run;

  if (!opt_for_fn (a->decl, flag_profile_reorder_functions)
      || a->no_reorder)
    tp_first_run_a = 0;
  if (!opt_for_fn (b->decl, flag_profile_reorder_functions)
      || b->no_reorder)
    tp_first_run_b = 0;

  if (tp_first_run_a == tp_first_run_b)
    return a->order - b->order;

  /* Functions with time profile must be before these without profile.  */
  tp_first_run_a = (tp_first_run_a - 1) & INT_MAX;
  tp_first_run_b = (tp_first_run_b - 1) & INT_MAX;

  return tp_first_run_a - tp_first_run_b;
}

/* Expand all functions that must be output.

   Attempt to topologically sort the nodes so function is output when
   all called functions are already assembled to allow data to be
   propagated across the callgraph.  Use a stack to get smaller distance
   between a function and its callees (later we may choose to use a more
   sophisticated algorithm for function reordering; we will likely want
   to use subsections to make the output functions appear in top-down
   order).  */

static void
expand_all_functions (void)
{
  cgraph_node *node;
  cgraph_node **order = XCNEWVEC (cgraph_node *,
					 symtab->cgraph_count);
  cgraph_node **tp_first_run_order = XCNEWVEC (cgraph_node *,
					 symtab->cgraph_count);
  unsigned int expanded_func_count = 0, profiled_func_count = 0;
  int order_pos, tp_first_run_order_pos = 0, new_order_pos = 0;
  int i;

  order_pos = ipa_reverse_postorder (order);
  gcc_assert (order_pos == symtab->cgraph_count);

  /* Garbage collector may remove inline clones we eliminate during
     optimization.  So we must be sure to not reference them.  */
  for (i = 0; i < order_pos; i++)
    if (order[i]->process)
      {
	if (order[i]->tp_first_run
	    && opt_for_fn (order[i]->decl, flag_profile_reorder_functions))
	  tp_first_run_order[tp_first_run_order_pos++] = order[i];
	else
          order[new_order_pos++] = order[i];
      }

  /* First output functions with time profile in specified order.  */
  qsort (tp_first_run_order, tp_first_run_order_pos,
	 sizeof (cgraph_node *), tp_first_run_node_cmp);
  for (i = 0; i < tp_first_run_order_pos; i++)
    {
      node = tp_first_run_order[i];

      if (node->process)
	{
	  expanded_func_count++;
	  profiled_func_count++;

	  if (symtab->dump_file)
	    fprintf (symtab->dump_file,
		     "Time profile order in expand_all_functions:%s:%d\n",
		     node->dump_asm_name (), node->tp_first_run);
	  node->process = 0;
	  node->expand ();
	}
    }

  /* Output functions in RPO so callees get optimized before callers.  This
     makes ipa-ra and other propagators to work.
     FIXME: This is far from optimal code layout.
     Make multiple passes over the list to defer processing of gc
     candidates until all potential uses are seen.  */
  int gc_candidates = 0;
  int prev_gc_candidates = 0;

  while (1)
    {
      for (i = new_order_pos - 1; i >= 0; i--)
	{
	  node = order[i];

	  if (node->gc_candidate)
	    gc_candidates++;
	  else if (node->process)
	    {
	      expanded_func_count++;
	      node->process = 0;
	      node->expand ();
	    }
	}
      if (!gc_candidates || gc_candidates == prev_gc_candidates)
	break;
      prev_gc_candidates = gc_candidates;
      gc_candidates = 0;
    }

  /* Free any unused gc_candidate functions.  */
  if (gc_candidates)
    for (i = new_order_pos - 1; i >= 0; i--)
      {
	node = order[i];
	if (node->gc_candidate)
	  {
	    struct function *fn = DECL_STRUCT_FUNCTION (node->decl);
	    if (symtab->dump_file)
	      fprintf (symtab->dump_file,
		       "Deleting unused function %s\n",
		       IDENTIFIER_POINTER (DECL_ASSEMBLER_NAME (node->decl)));
	    node->process = false;
	    free_dominance_info (fn, CDI_DOMINATORS);
	    free_dominance_info (fn, CDI_POST_DOMINATORS);
	    node->release_body (false);
	  }
      }

  if (dump_file)
    fprintf (dump_file, "Expanded functions with time profile (%s):%u/%u\n",
	     main_input_filename, profiled_func_count, expanded_func_count);

  if (symtab->dump_file && tp_first_run_order_pos)
    fprintf (symtab->dump_file, "Expanded functions with time profile:%u/%u\n",
             profiled_func_count, expanded_func_count);

  symtab->process_new_functions ();
  free_gimplify_stack ();
  delete ipa_saved_clone_sources;
  ipa_saved_clone_sources = NULL;
  free (order);
  free (tp_first_run_order);
}

/* This is used to sort the node types by the cgraph order number.  */

enum cgraph_order_sort_kind
{
  ORDER_FUNCTION,
  ORDER_VAR,
  ORDER_VAR_UNDEF,
  ORDER_ASM
};

struct cgraph_order_sort
{
  /* Construct from a cgraph_node.  */
  cgraph_order_sort (cgraph_node *node)
  : kind (ORDER_FUNCTION), order (node->order)
  {
    u.f = node;
  }

  /* Construct from a varpool_node.  */
  cgraph_order_sort (varpool_node *node)
  : kind (node->definition ? ORDER_VAR : ORDER_VAR_UNDEF), order (node->order)
  {
    u.v = node;
  }

  /* Construct from a asm_node.  */
  cgraph_order_sort (asm_node *node)
  : kind (ORDER_ASM), order (node->order)
  {
    u.a = node;
  }

  /* Assembly cgraph_order_sort based on its type.  */
  void process ();

  enum cgraph_order_sort_kind kind;
  union
  {
    cgraph_node *f;
    varpool_node *v;
    asm_node *a;
  } u;
  int order;
};

/* Assembly cgraph_order_sort based on its type.  */

void
cgraph_order_sort::process ()
{
  switch (kind)
    {
    case ORDER_FUNCTION:
      u.f->process = 0;
      u.f->expand ();
      break;
    case ORDER_VAR:
      u.v->assemble_decl ();
      break;
    case ORDER_VAR_UNDEF:
      assemble_undefined_decl (u.v->decl);
      break;
    case ORDER_ASM:
      assemble_asm (u.a->asm_str);
      break;
    default:
      gcc_unreachable ();
    }
}

/* Compare cgraph_order_sort by order.  */

static int
cgraph_order_cmp (const void *a_p, const void *b_p)
{
  const cgraph_order_sort *nodea = (const cgraph_order_sort *)a_p;
  const cgraph_order_sort *nodeb = (const cgraph_order_sort *)b_p;

  return nodea->order - nodeb->order;
}

/* Output all functions, variables, and asm statements in the order
   according to their order fields, which is the order in which they
   appeared in the file.  This implements -fno-toplevel-reorder.  In
   this mode we may output functions and variables which don't really
   need to be output.  */

static void
output_in_order (void)
{
  int i;
  cgraph_node *cnode;
  varpool_node *vnode;
  asm_node *anode;
  auto_vec<cgraph_order_sort> nodes;
  cgraph_order_sort *node;

  FOR_EACH_DEFINED_FUNCTION (cnode)
    if (cnode->process && !cnode->thunk
	&& !cnode->alias && cnode->no_reorder)
      nodes.safe_push (cgraph_order_sort (cnode));

  /* There is a similar loop in symbol_table::output_variables.
     Please keep them in sync.  */
  FOR_EACH_VARIABLE (vnode)
    if (vnode->no_reorder
	&& !DECL_HARD_REGISTER (vnode->decl)
	&& !DECL_HAS_VALUE_EXPR_P (vnode->decl))
      nodes.safe_push (cgraph_order_sort (vnode));

  for (anode = symtab->first_asm_symbol (); anode; anode = anode->next)
    nodes.safe_push (cgraph_order_sort (anode));

  /* Sort nodes by order.  */
  nodes.qsort (cgraph_order_cmp);

  /* In toplevel reorder mode we output all statics; mark them as needed.  */
  FOR_EACH_VEC_ELT (nodes, i, node)
    if (node->kind == ORDER_VAR)
      node->u.v->finalize_named_section_flags ();

  FOR_EACH_VEC_ELT (nodes, i, node)
    node->process ();

  symtab->clear_asm_symbols ();
}

static void
ipa_passes (void)
{
  gcc::pass_manager *passes = g->get_passes ();

  set_cfun (NULL);
  current_function_decl = NULL;
  gimple_register_cfg_hooks ();
  bitmap_obstack_initialize (NULL);

  invoke_plugin_callbacks (PLUGIN_ALL_IPA_PASSES_START, NULL);

  if (!in_lto_p)
    {
      execute_ipa_pass_list (passes->all_small_ipa_passes);
      if (seen_error ())
	return;
    }

  /* This extra symtab_remove_unreachable_nodes pass tends to catch some
     devirtualization and other changes where removal iterate.  */
  symtab->remove_unreachable_nodes (symtab->dump_file);

  /* If pass_all_early_optimizations was not scheduled, the state of
     the cgraph will not be properly updated.  Update it now.  */
  if (symtab->state < IPA_SSA)
    symtab->state = IPA_SSA;

  if (!in_lto_p)
    {
      /* Generate coverage variables and constructors.  */
      coverage_finish ();

      /* Process new functions added.  */
      set_cfun (NULL);
      current_function_decl = NULL;
      symtab->process_new_functions ();

      execute_ipa_summary_passes
	((ipa_opt_pass_d *) passes->all_regular_ipa_passes);
    }

  /* Some targets need to handle LTO assembler output specially.  */
  if (flag_generate_lto || flag_generate_offload)
    targetm.asm_out.lto_start ();

  if (!in_lto_p
      || flag_incremental_link == INCREMENTAL_LINK_LTO)
    {
      if (!quiet_flag)
	fprintf (stderr, "Streaming LTO\n");
      if (g->have_offload)
	{
	  section_name_prefix = OFFLOAD_SECTION_NAME_PREFIX;
	  lto_stream_offload_p = true;
	  ipa_write_summaries ();
	  lto_stream_offload_p = false;
	}
      if (flag_lto)
	{
	  section_name_prefix = LTO_SECTION_NAME_PREFIX;
	  lto_stream_offload_p = false;
	  ipa_write_summaries ();
	}
    }

  if (flag_generate_lto || flag_generate_offload)
    targetm.asm_out.lto_end ();

  if (!flag_ltrans
      && ((in_lto_p && flag_incremental_link != INCREMENTAL_LINK_LTO)
	  || !flag_lto || flag_fat_lto_objects))
    execute_ipa_pass_list (passes->all_regular_ipa_passes);
  invoke_plugin_callbacks (PLUGIN_ALL_IPA_PASSES_END, NULL);

  bitmap_obstack_release (NULL);
}


/* Weakrefs may be associated to external decls and thus not output
   at expansion time.  Emit all necessary aliases.  */

void
symbol_table::output_weakrefs (void)
{
  symtab_node *node;
  FOR_EACH_SYMBOL (node)
    if (node->alias
        && !TREE_ASM_WRITTEN (node->decl)
	&& node->weakref)
      {
	tree target;

	/* Weakrefs are special by not requiring target definition in current
	   compilation unit.  It is thus bit hard to work out what we want to
	   alias.
	   When alias target is defined, we need to fetch it from symtab reference,
	   otherwise it is pointed to by alias_target.  */
	if (node->alias_target)
	  target = (DECL_P (node->alias_target)
		    ? DECL_ASSEMBLER_NAME (node->alias_target)
		    : node->alias_target);
	else if (node->analyzed)
	  target = DECL_ASSEMBLER_NAME (node->get_alias_target ()->decl);
	else
	  gcc_unreachable ();
        do_assemble_alias (node->decl, target);
      }
}


class exec_context;
class data_storage;



static bool
get_constant_type_size (tree type, unsigned &result)
{
  if (TREE_CODE (type) == INTEGER_TYPE
      || TREE_CODE (type) == BOOLEAN_TYPE)
    result = TYPE_PRECISION (type);
  else
    {
      tree tree_size = TYPE_SIZE (type);
      gcc_assert (TREE_CODE (tree_size) == INTEGER_CST);
      wide_int wi_size = wi::to_wide (tree_size);

      gcc_assert (wi::fits_uhwi_p (wi_size));
      unsigned HOST_WIDE_INT hwi_size = wi_size.to_uhwi ();

      gcc_assert (hwi_size <= UINT_MAX);
      result = hwi_size;
    }
  return true;
}


static unsigned
get_constant_type_size (tree type)
{
  unsigned result;
  gcc_assert (get_constant_type_size (type, result));
  return result;
}


enum value_type
{
  VAL_NONE,
  VAL_UNDEFINED,
  VAL_ADDRESS,
  VAL_CONSTANT,
  VAL_MIXED
};


enum storage_type
{
  STRG_VARIABLE,
  STRG_ALLOC
};

class heap_memory;

struct storage_ref
{
  enum storage_type type;
  union u
    {
      u (const exec_context & ctx) : var_context (&ctx) {}
      u (const heap_memory & m) : alloc_mem (&m) {}
      const exec_context * var_context;
      const heap_memory * alloc_mem;
    }
  u;
  unsigned storage_index;

  storage_ref (const exec_context & ctx, unsigned idx)
    : type (STRG_VARIABLE), u (ctx), storage_index (idx)
  {}
  storage_ref (const heap_memory & mem, unsigned idx)
    : type (STRG_ALLOC), u (mem), storage_index (idx)
  {}
  storage_ref (const storage_ref & other) = default;
  data_storage & get () const;
};


struct storage_address
{
  storage_ref storage;
  unsigned offset;

  storage_address (const storage_ref & ref, unsigned off)
    : storage (ref), offset (off)
  {}
};


struct stored_address
{
  storage_address address;
  unsigned position;

  stored_address (storage_address addr, unsigned pos)
    : address (addr), position (pos)
  {}
};

namespace selftest
{
  void data_value_classify_tests ();
  void data_value_set_address_tests ();
  void data_value_set_tests ();
  void data_value_set_at_tests ();
  void data_value_set_address_tests ();
  void data_value_print_tests ();
  void context_printer_print_tests ();
  void context_printer_print_first_data_ref_part_tests ();
  void context_printer_print_value_update_tests ();
  void exec_context_evaluate_tests ();
  void exec_context_evaluate_literal_tests ();
  void exec_context_evaluate_unary_tests ();
  void exec_context_evaluate_binary_tests ();
  void exec_context_execute_assign_tests ();
  void exec_context_execute_call_tests ();
  void exec_context_allocate_tests ();
  void exec_context_evaluate_condition_tests ();
}


class data_value
{
  unsigned bit_width;
  wide_int constant_mask;
  wide_int address_mask;
  wide_int constant_value;
  vec<stored_address> addresses;
  void set_cst_at (unsigned dest_offset, unsigned value_width,
		   const wide_int &val, unsigned src_offset);
  stored_address *find_address (unsigned offset) const;
  void set_at (unsigned dest_offset, unsigned value_width,
	       const data_value & value_src, unsigned src_offset);

  friend void selftest::data_value_classify_tests ();
  friend void selftest::data_value_set_address_tests ();
  friend void selftest::data_value_set_tests ();
  friend void selftest::data_value_set_at_tests ();

public:
  data_value (unsigned width)
    : bit_width (width),
    constant_mask (wi::shwi (HOST_WIDE_INT_0, width)),
    address_mask (wi::shwi (HOST_WIDE_INT_0, width)),
    constant_value (wi::shwi (HOST_WIDE_INT_0, width)),
    addresses ()
  {}
  data_value (tree type)
    : data_value (get_constant_type_size (type))
  {}
  data_value (const data_value &);
  data_value & operator= (const data_value &);
  value_type classify () const;
  value_type classify (unsigned offset, unsigned width) const;
  unsigned get_bitwidth () const { return bit_width; }
  void set_undefined_at (unsigned offset, unsigned width);
  void set_address_at (storage_address & address, unsigned offset);
  void set_address (storage_address & address);
  void set_at (const data_value & value, unsigned offset);
  void set (const data_value & value);
  void set_cst_at (const wide_int & val, unsigned offset);
  void set_cst (const wide_int & val);
  wide_int get_cst_at (unsigned offset, unsigned width) const;
  wide_int get_cst () const;
  storage_address *get_address () const;
  storage_address *get_address_at (unsigned offset) const;
  data_value get_at (unsigned offset, unsigned width) const;
  bool is_fully_defined () const { return (~(constant_mask | address_mask)) == 0; }
  tree to_tree (tree type) const;
};


class heap_memory;
class context_printer;


class data_storage
{
  const storage_type type;
  data_value value;

  union u
    {
      u (const exec_context & ctx, tree t) : variable (ctx, t) {}
      u (const heap_memory & mem, unsigned alloc_idx, unsigned alloc_amount)
	: allocated (mem, alloc_idx, alloc_amount)
      {}
      //~u () {}

      struct v
	{
	  v (const exec_context & ctx, tree t) : context (ctx), decl (t) {}
	  const exec_context & context;
	  const tree decl;
	}
      variable;

      const struct a
	{
	  a (const heap_memory & mem, unsigned alloc_idx, unsigned alloc_amount)
	    : alloc_mem (mem), index (alloc_idx), amount_bits (alloc_amount)
	  {}
	  const heap_memory & alloc_mem;
	  const unsigned index;
	  const unsigned amount_bits;
	}
      allocated;
    }
  u;

public:
  data_storage (const exec_context & ctx, tree decl)
    : type (STRG_VARIABLE), value (TREE_TYPE (decl)),
    u (ctx, decl)
  {}
  data_storage (const heap_memory & mem, unsigned alloc_index, unsigned alloc_amount)
    : type (STRG_ALLOC), value (alloc_amount),
    u (mem, alloc_index, alloc_amount)
  {}
  storage_type get_type () const { return type; }
  tree get_variable () const;

  bool matches (tree var) const
  { return type == STRG_VARIABLE && u.variable.decl == var; }

  bool matches_alloc (unsigned index) const
  { return type == STRG_ALLOC && u.allocated.index == index; }

  const data_value & get_value () const { return value; }
  data_storage & operator= (const data_storage& other) = default;
  void set (const data_value & val) { return value.set (val); }
  void set_at (const data_value & val, unsigned offset) { return value.set_at (val, offset); }
  void print (context_printer & printer) const;
  storage_ref get_ref () const;
};


class context_printer
{
  pretty_printer pp;
  dump_flags_t flags;
  unsigned indent;

  friend void selftest::exec_context_evaluate_tests ();
  friend void selftest::data_value_print_tests ();
  friend void selftest::context_printer_print_tests ();
  friend void selftest::context_printer_print_first_data_ref_part_tests ();
  friend void selftest::context_printer_print_value_update_tests ();

public:
  context_printer ();
  context_printer (dump_flags_t f);
  pretty_printer & get_pretty_printer () const { return const_cast <pretty_printer &> (pp); }
  void begin_stmt (gimple *);
  void print (exec_context *, tree);
  void print_newline ();
  void print_function_entry (struct function * func);
  void print_function_exit (struct function * func);
  void print_bb_jump (edge e);
  void print_bb_entry (basic_block bb);
  tree print_first_data_ref_part (exec_context & context, tree data_ref, unsigned offset, int * ignored_bits);
  void print_ignored_stmt ();
  void print_value_update (exec_context & context, tree, const data_value &); 
  void end_stmt (gimple *);
  void print_at (const data_value & value, tree type, unsigned offset, unsigned width);
  void print_at (const data_value & value, tree type, unsigned offset);
  void print (const data_value & value, tree type);
  void print_condition_value (bool value);
};


template <typename T>
class optional 
{
  union u
    {
      u (T arg) : value (arg) {}
      u () : dummy (0) {}
      u (const u & other, bool present) { if (present) { new (&value) T (other.value); } else { new (&dummy) char (0); } }
      ~u () {} // TODO
      T value;
      char dummy;
    }
  u;
  bool present;

public:
  optional () : u (), present (false) {}
  optional (T arg) : u (arg), present (true) {}
  optional (const optional & other) : u (other.u, other.present), present (other.present) {}
  ~optional () {}  // TODO
  optional & operator= (const optional & other) { new (this) optional (other); return *this; }
  T & operator * () const;
  void emplace (T value);
};


template <typename T>
T &
optional<T>::operator* () const
{
  gcc_assert (present);
  return const_cast <T &> (u.value);
}

template <typename T>
void
optional<T>::emplace (T arg)
{
  present = true;
  u.value = arg;
}


static optional <data_value>
execute (struct function *func, exec_context &caller,
	 context_printer & printer, vec<tree> * args);


class heap_memory
{
  vec<data_storage> storages;
  unsigned next_alloc_index;

public:
  heap_memory () : storages (), next_alloc_index (0) {}
  data_storage *find_alloc (unsigned index) const;
  int find (const data_storage & storage) const;
  data_storage & allocate (unsigned amount);
  data_storage & get_storage (unsigned idx) const;
};


class exec_context
{
  exec_context * const parent;
  context_printer & printer;
  heap_memory & alloc_mem;
  vec<data_storage> var_storages;
  data_value evaluate_constructor (tree cstr) const;
  data_value evaluate_unary (enum tree_code code, tree type, tree arg) const;
  data_value evaluate_binary (enum tree_code code, tree type, tree lhs, tree rhs) const;
  bool evaluate_condition (gcond *cond) const;
  template <typename A, typename L>
  void add_variables (vec<tree, A, L> *variables, unsigned vars_count);
  template <typename A>
  void add_variables (vec<tree, A, vl_ptr> *variables);
  template <typename A>
  void add_variables (vec<tree, A, vl_embed> *variables);
  void execute_assign (gassign *g);
  void execute_call (gcall *g);
  data_storage *find_var (tree variable) const;
  data_storage *find_alloc (unsigned index) const;
  data_storage &allocate (unsigned amount);
  void decompose_ref (tree data_ref, data_storage * & storage, int & offset) const;
  void execute_phi (gphi *phi, edge e);
  exec_context (exec_context * caller, context_printer & printer,
		heap_memory & mem, vec<tree> & decls);

  friend void selftest::data_value_print_tests ();
  friend void selftest::data_value_set_address_tests ();
  friend void selftest::data_value_set_tests ();
  friend void selftest::context_printer_print_tests ();
  friend void selftest::exec_context_evaluate_literal_tests ();
  friend void selftest::exec_context_evaluate_unary_tests ();
  friend void selftest::exec_context_evaluate_binary_tests ();
  friend void selftest::exec_context_execute_assign_tests ();
  friend void selftest::exec_context_execute_call_tests ();
  friend void selftest::exec_context_allocate_tests ();
  friend void selftest::exec_context_evaluate_condition_tests ();

public:
  exec_context (exec_context & caller, context_printer & printer,
		vec<tree> & decls);
  exec_context (heap_memory & mem, context_printer & printer, vec<tree> & decls);
  const exec_context & root () const;
  int find (const data_storage &storage) const;
  data_storage *find_reachable_var (tree variable) const;
  void execute (gimple *g);
  gimple * execute (basic_block bb);
  data_storage & get_storage (unsigned idx) const;
  context_printer & get_printer () const { return printer; }
  data_value evaluate (tree expr) const;
  optional <data_value> execute_function (struct function *);
  edge select_leaving_edge (basic_block bb, gimple *last_stmt);
  void jump (edge e);
};


class context_builder
{
  vec<tree> decls;

public:
  context_builder ();
  exec_context build (exec_context & caller, context_printer & printer);
  exec_context build (heap_memory & mem, context_printer & printer);
  template <typename A, typename L>
  void add_decls (vec<tree, A, L> *additional_decls);
  //void add_decls (vec<tree> *additional_decls);
};


context_builder::context_builder ()
  : decls ()
{}


template <typename A, typename L>
void
context_builder::add_decls (vec<tree, A, L> *additional_decls)
{
  if (additional_decls == nullptr)
    return;

  decls.reserve (additional_decls->length ());

  tree *declp;
  unsigned i;
  FOR_EACH_VEC_ELT (*additional_decls, i, declp)
    decls.quick_push (*declp);
}


#if 0
void
context_builder::add_decls (vec<tree> *additional_decls)
{
  if (additional_decls == nullptr)
    return;

  decls.reserve (additional_decls->length ());

  tree *declp;
  unsigned i;
  FOR_EACH_VEC_ELT (*additional_decls, i, declp)
    decls.quick_push (*declp);
}
#endif


exec_context
context_builder::build (exec_context & caller, context_printer & printer)
{
  return exec_context (caller, printer, decls);
}


exec_context
context_builder::build (heap_memory & mem, context_printer & printer)
{
  return exec_context (mem, printer, decls);
}


exec_context::exec_context (exec_context *caller, context_printer & printer,
			    heap_memory & mem, vec<tree> & decls)
  : parent (caller), printer (printer), alloc_mem (mem), var_storages ()
{
  add_variables (&decls);
}

exec_context::exec_context (exec_context & caller, context_printer & printer,
			    vec<tree> & decls)
  : exec_context::exec_context (&caller, printer, caller.alloc_mem, decls)
{}

exec_context::exec_context (heap_memory & mem, context_printer & printer,
			    vec<tree> & decls)
  : exec_context::exec_context (nullptr, printer, mem, decls)
{}


template <typename A, typename L>
void
exec_context::add_variables (vec<tree, A, L> *variables, unsigned vars_count)
{
  if (vars_count == 0)
    return;

  var_storages.reserve (vars_count);

  tree *varp;
  unsigned i;
  FOR_EACH_VEC_ELT (*variables, i, varp)
    if (*varp != NULL_TREE
	&& TYPE_SIZE (TREE_TYPE (*varp)) != NULL_TREE)
      var_storages.quick_push (data_storage (*this, *varp));
}

template <typename A>
void
exec_context::add_variables (vec<tree, A, vl_ptr> *variables)
{
  add_variables (variables, variables->length ());
}

template <typename A>
void
exec_context::add_variables (vec<tree, A, vl_embed> *variables)
{
  add_variables (variables, vec_safe_length (variables));
}

context_printer::context_printer (dump_flags_t f)
  : pp (), flags (f), indent (0)
{
  pp_needs_newline (&pp) = true;
}

context_printer::context_printer ()
  : context_printer (TDF_NONE)
{}


void
context_printer::print_newline ()
{
  int indent = pp_indentation (&pp);
  pp_newline_and_flush (&pp);
  pp_indentation (&pp) = indent;
}


void
context_printer::begin_stmt (gimple *g)
{
  pp_indent (&pp);
  pp_gimple_stmt_1 (&pp, g, pp_indentation (&pp), TDF_NONE);
  print_newline ();
  pp_indentation (&pp) += 2;
}

void
context_printer::print_condition_value (bool value)
{
  pp_indent (&pp);
  pp_string (&pp, "# Condition evaluates to ");
  pp_string (&pp, value ? "true" : "false");
  print_newline ();
}

void
context_printer::print (exec_context * ctx, tree expr)
{
  switch (TREE_CODE (expr))
    {
    case MEM_REF:
      {
	gcc_assert (ctx != nullptr);
	data_value val = ctx->evaluate (TREE_OPERAND (expr, 0));
	gcc_assert (val.classify () == VAL_ADDRESS);
	storage_address *address = val.get_address ();
	gcc_assert (address != nullptr);

	data_value val_off = ctx->evaluate (TREE_OPERAND (expr, 1));
	gcc_assert (val_off.classify () == VAL_CONSTANT);
	wide_int wi_off = val_off.get_cst ();
	wi_off = (wi_off * CHAR_BIT) + address->offset;

	if (!wi::fits_uhwi_p (wi_off))
	  gcc_unreachable ();
	unsigned offset_bits = wi_off.to_uhwi ();
	gcc_assert (offset_bits % CHAR_BIT == 0);
	unsigned offset_bytes = offset_bits / CHAR_BIT;

	unsigned size_bits;
	if (!get_constant_type_size (TREE_TYPE (expr), size_bits))
	  gcc_unreachable ();
	gcc_assert (size_bits % CHAR_BIT == 0);
	unsigned size_bytes = size_bits / CHAR_BIT;

	address->storage.get ().print (*this);
	pp_left_bracket (&pp);
	pp_decimal_int (&pp, offset_bytes);
	pp_character (&pp, 'B');
	pp_colon (&pp);
	pp_plus (&pp);
	pp_decimal_int (&pp, size_bytes);
	pp_character (&pp, 'B');
	pp_right_bracket (&pp);
      }
      break;

    default:
      dump_generic_node (&pp, expr, pp_indentation (&pp), flags, false);
    }
}


static const char *
get_func_name (struct function *func)
{
  tree decl = func->decl;
  tree name = DECL_NAME (decl);
  return IDENTIFIER_POINTER (name);
}

void
context_printer::print_function_entry (struct function *func)
{
  pp_indent (&pp);
  pp_string (&pp, "# Entering function ");
  pp_string (&pp, get_func_name (func));
  print_newline ();
  pp_indentation (&pp) += 2;
}

void
context_printer::print_function_exit (struct function *func)
{
  pp_indentation (&pp) -= 2;
  pp_indent (&pp);
  pp_string (&pp, "# Leaving function ");
  pp_string (&pp, get_func_name (func));
  print_newline ();
}

void
context_printer::print_bb_jump (edge e)
{
  pp_indent (&pp);
  pp_string (&pp, "# Leaving bb ");
  pp_decimal_int (&pp, e->src->index);
  pp_string (&pp, ", preparing to execute bb ");
  pp_decimal_int (&pp, e->dest->index);
  print_newline ();
}


void
context_printer::print_bb_entry (basic_block bb)
{
  pp_indent (&pp);
  pp_string (&pp, "# Executing bb ");
  pp_decimal_int (&pp, bb->index);
  print_newline ();
}


static tree
pick_subref_at (tree var_ref, unsigned offset, int * ignored_bits)
{
  tree ref = var_ref;
  unsigned remaining_offset = offset;
  while (true)
    {
      tree var_type = TREE_TYPE (ref);
      if (TREE_CODE (var_type) == ARRAY_TYPE)
	{
	  tree elt_type = TREE_TYPE (var_type);
	  unsigned elt_width;
	  gcc_assert (get_constant_type_size (elt_type, elt_width));
	  unsigned HOST_WIDE_INT hw_idx = remaining_offset / elt_width;
	  tree t_idx = build_int_cst (integer_type_node, hw_idx);
	  ref = build4 (ARRAY_REF, elt_type, ref,
			t_idx, NULL_TREE, NULL_TREE);
	  remaining_offset -= hw_idx * elt_width;
	}
      else if (TREE_CODE (var_type) == RECORD_TYPE)
	{
	  tree field = NULL_TREE;
	  HOST_WIDE_INT field_position = -1;
	  tree next_field = TYPE_FIELDS (TREE_TYPE (ref));

	  do
	    {
	      HOST_WIDE_INT next_position;
	      next_position = int_bit_position (next_field);
	      if (next_position > remaining_offset)
		{
		  if (field_position >= remaining_offset)
		    break;

		  unsigned field_size;
		  if (!get_constant_type_size (TREE_TYPE (field), field_size))
		    gcc_unreachable ();

		  // Continue if the remaining offset is not within
		  // the current field, even if the next field is
		  // after the remaining offset as in the case of padding.
		  if (field_position + field_size > remaining_offset)
		    break;
		}

	      field = next_field;
	      field_position = next_position;
	      next_field = TREE_CHAIN (field);
	    }
	  while (next_field != NULL_TREE);

	  gcc_assert (field != NULL_TREE
		      && field_position >= 0);

	  ref = build3 (COMPONENT_REF, TREE_TYPE (field),
			ref, field, NULL_TREE);
	  if (field_position > remaining_offset)
	    {
	      gcc_assert (ignored_bits != nullptr);
	      *ignored_bits = field_position - remaining_offset;
	      remaining_offset = 0;
	    }
	  else
	    remaining_offset -= field_position;
	}
      else
	break;
    }

  if (remaining_offset == 0)
    return ref;
  else
    {
      gcc_assert (ignored_bits != nullptr);
      *ignored_bits = remaining_offset;
      return NULL_TREE;
    }
}


static tree
find_mem_ref_replacement (exec_context & context, tree data_ref, 
			  unsigned offset)
{
  tree ptr = TREE_OPERAND (data_ref, 0);
  data_value ptr_val = context.evaluate (ptr);
  if (ptr_val.classify () != VAL_ADDRESS)
    return NULL_TREE;

  storage_address *ptr_address = ptr_val.get_address ();
  data_storage &ptr_target = ptr_address->storage.get ();
  if (ptr_target.get_type () != STRG_VARIABLE)
    return NULL_TREE;

  tree access_type = TREE_TYPE (data_ref);
  tree var_ref = ptr_target.get_variable ();
  tree var_type = TREE_TYPE (var_ref);

  if (var_type == access_type)
    return var_ref;
  else
  {
    tree access_offset = TREE_OPERAND (data_ref, 1);
    gcc_assert (TREE_CONSTANT (access_offset));
    gcc_assert (tree_fits_shwi_p (access_offset));
    HOST_WIDE_INT shwi_offset = tree_to_shwi (access_offset);
    gcc_assert (offset < UINT_MAX - shwi_offset);
    HOST_WIDE_INT remaining_offset = shwi_offset * CHAR_BIT + offset;

    return pick_subref_at (var_ref, remaining_offset, nullptr);
  }
}


tree
context_printer::print_first_data_ref_part (exec_context & context, tree data_ref, unsigned offset, int * ignored_bits)
{
  switch (TREE_CODE (data_ref))
    {
    case MEM_REF:
      {
	tree mem_replacement = find_mem_ref_replacement (context, data_ref,
							 offset);
	if (mem_replacement != NULL_TREE)
	  return print_first_data_ref_part (context, mem_replacement, 0, ignored_bits);
      }

    /* Fall through.  */

    default:
      tree ref = pick_subref_at (data_ref, offset, ignored_bits);
      if (ref == NULL_TREE)
	{
	  gcc_assert (ignored_bits != nullptr
		      && *ignored_bits > 0);
	  return NULL_TREE;
	}

      pp_indent (&pp);
      pp_character (&pp, '#');
      pp_space (&pp);
      print (&context, ref);
      return TREE_TYPE (ref);
    }
}


void
context_printer::print_ignored_stmt ()
{
  pp_indent (&pp);
  pp_string (&pp, "# ignored");
  print_newline ();
}


void
context_printer::print_value_update (exec_context & context, tree lhs, const data_value & value)
{
  unsigned previously_done = 0;
  unsigned width = get_constant_type_size (TREE_TYPE (lhs));
  while (previously_done < width)
    {
      int ignored_bits = 0;
      tree type_done = print_first_data_ref_part (context, lhs,
						  previously_done,
						  &ignored_bits);
      if (type_done == NULL_TREE)
	{
	  gcc_assert (ignored_bits > 0);
	  previously_done += ignored_bits;
	  continue;
	}

      unsigned just_done;
      if (!get_constant_type_size (type_done, just_done))
	gcc_unreachable ();
      gcc_assert (just_done > 0);
      gcc_assert (just_done + ignored_bits <= width - previously_done);
      pp_space (&pp);
      pp_equal (&pp);
      pp_space (&pp);
      print_at (value, type_done, previously_done + ignored_bits, just_done);
      print_newline ();
      previously_done += just_done + ignored_bits;
    }
}


void
context_printer::end_stmt (gimple *g ATTRIBUTE_UNUSED)
{
  pp_indentation (&pp) -= 2;
}


data_storage &
storage_ref::get () const
{
  switch (type)
    {
    case STRG_VARIABLE:
      return u.var_context->get_storage (storage_index);

    case STRG_ALLOC:
      return u.alloc_mem->get_storage (storage_index);

    default:
      gcc_unreachable ();
    }
}


storage_ref
data_storage::get_ref () const
{
  switch (type)
    {
    case STRG_VARIABLE:
      {
	const exec_context & ctx = u.variable.context;
	int idx = ctx.find (*this);
	gcc_assert (idx >= 0);
	return storage_ref (ctx, idx);
      }

    case STRG_ALLOC:
      {
	const heap_memory & mem = u.allocated.alloc_mem;
	int idx = mem.find (*this);
	gcc_assert (idx >= 0);
	return storage_ref (mem, idx);
      }

    default:
      gcc_unreachable ();
    }
}


data_value::data_value (const data_value & other)
  : bit_width (other.bit_width),
  constant_mask (other.constant_mask),
  address_mask (other.address_mask),
  constant_value (other.constant_value),
  addresses (other.addresses)
{}


data_value & data_value::operator= (const data_value & other)
{
  new (this) data_value (other);
  return *this;
}


enum value_type
data_value::classify () const
{
  return classify (0, bit_width);
}

value_type
data_value::classify (unsigned offset, unsigned width) const
{
  wide_int mask = wi::shifted_mask (offset, width, false, bit_width);
  bool has_address = (address_mask & mask) != 0;
  bool has_constant = (constant_mask & mask) != 0;

  int has_count = has_address + has_constant;
  if (has_count > 1)
    return VAL_MIXED;
  else if (has_count == 0)
    return VAL_UNDEFINED;
  else if (has_constant && ((~constant_mask) & mask) == 0)
    return VAL_CONSTANT;
  else if (has_address && ((~address_mask) & mask) == 0)
    return VAL_ADDRESS;
  else
    return VAL_MIXED;
}


stored_address *
data_value::find_address (unsigned offset) const
{
  gcc_assert (offset <= bit_width - HOST_BITS_PER_PTR);

  stored_address *result = nullptr;
  stored_address *strd_address;
  unsigned i;
  FOR_EACH_VEC_ELT (addresses, i, strd_address)
    if (strd_address->position == offset)
      {
	gcc_assert (result == nullptr);
	result = strd_address;
      }

  return result;
}


void
data_value::set_address_at (storage_address & address, unsigned offset)
{
  wide_int mask = wi::shifted_mask (offset, HOST_BITS_PER_PTR, false,
				    bit_width);
  enum value_type type = classify (offset, HOST_BITS_PER_PTR);

  if (type == VAL_ADDRESS)
    {
      stored_address *existing_address = find_address (offset);
      gcc_assert (existing_address != nullptr);
      /* Invalidate existing address.  */
      existing_address->position = -1;
    }

  constant_mask &= ~mask;
  address_mask |= mask;

  stored_address new_address (address, offset);
  addresses.safe_push (new_address);
}


void
data_value::set_address (storage_address & address)
{
  gcc_assert (bit_width == HOST_BITS_PER_PTR);
  set_address_at (address, 0);
}


void
data_value::set_undefined_at (unsigned offset, unsigned width)
{
  wide_int dest_mask = wi::shifted_mask (offset, width, false,
					 bit_width);

  // TODO: invalidate existing address if any
  constant_mask &= ~dest_mask;
  address_mask &= ~dest_mask;
}


void
data_value::set_cst_at (unsigned dest_offset, unsigned value_width,
			const wide_int & value_src, unsigned src_offset)
{
  unsigned src_width = value_src.get_precision ();
  gcc_assert (dest_offset < bit_width);
  gcc_assert (value_width <= bit_width - dest_offset);
  gcc_assert (src_offset < src_width);
  gcc_assert (value_width <= src_width - src_offset);

  enum value_type orig_type = classify (dest_offset, value_width);
  wide_int dest_mask = wi::shifted_mask (dest_offset, value_width, false,
					 bit_width);
  // TODO: invalidate existing address if any
  gcc_assert (orig_type != VAL_ADDRESS);
  if (orig_type != VAL_CONSTANT)
    {
      constant_mask |= dest_mask;
      address_mask &= ~dest_mask;
    }

  wide_int src_mask = wi::shifted_mask (src_offset, value_width, false,
					src_width);
  wide_int value = value_src & src_mask;
  if (src_offset > 0)
    value = wi::lrshift (value, src_offset); 

  wide_int dest_value = wide_int_storage::from (wide_int_ref (value), bit_width, UNSIGNED);
  if (dest_offset > 0)
    dest_value <<= dest_offset; 

  constant_value &= ~dest_mask;
  constant_value |= dest_value;
}

void
data_value::set_at (unsigned dest_offset, unsigned value_width,
		    const data_value & value_src, unsigned src_offset)
{
  gcc_assert (dest_offset < bit_width);
  gcc_assert (value_width <= bit_width - dest_offset);

  enum value_type type = value_src.classify (src_offset, value_width);
  switch (type)
    {
    case VAL_UNDEFINED:
      set_undefined_at (dest_offset, value_width);
      break;

    case VAL_CONSTANT:
      set_cst_at (dest_offset, value_width, value_src.constant_value, src_offset);
      break;

    case VAL_ADDRESS:
      {
	if (value_width == HOST_BITS_PER_PTR)
	  {
	    stored_address *found_address = value_src.find_address (src_offset);
	    gcc_assert (found_address != nullptr);
	    set_address_at (found_address->address, dest_offset);
	  }
	else
	  {
	    /* Several addresses side by side in the same area.  Set them one
	       by one  */
	    gcc_assert (value_width > HOST_BITS_PER_PTR);
	    gcc_assert (value_width % HOST_BITS_PER_PTR == 0);
	    gcc_assert (dest_offset % HOST_BITS_PER_PTR == 0);
	    for (unsigned i = 0; i < value_width / HOST_BITS_PER_PTR; i++)
	      {
		unsigned off = i * HOST_BITS_PER_PTR;
		set_at (dest_offset + off, HOST_BITS_PER_PTR,
			value_src, src_offset + off);
	      }
	  }
      }
      break;

    case VAL_MIXED:
      {
	gcc_assert ((constant_mask & address_mask) == 0);

	wide_int cst_part = value_src.constant_mask;
	wide_int address_part = value_src.address_mask;

	unsigned src_width = value_src.bit_width;

	wide_int mask = wi::shifted_mask (src_offset, value_width, false,
					  value_src.bit_width);

	cst_part &= mask;
	address_part &= mask;

	while (cst_part != 0 || address_part != 0)
	  {
	    int ctz_cst = wi::ctz (cst_part);
	    int ctz_addr = wi::ctz (address_part);

	    int next_offset;
	    wide_int selected_part;
	    if (ctz_cst < ctz_addr)
	      {
		next_offset = ctz_cst;
		selected_part = cst_part;
	      }
	    else
	      {
		next_offset = ctz_addr;
		selected_part = address_part;
	      }

	    int width = wi::ctz (wi::bit_not (wi::lrshift (selected_part,
							   next_offset)));

	    unsigned offset = dest_offset + (next_offset - src_offset);

	    set_at (offset, width, value_src, next_offset);

	    wide_int mask = wi::shifted_mask (next_offset, width, false, src_width);

	    cst_part &= ~mask;
	    address_part &= ~mask;
	  }
	
      }
      break;

    default:
      gcc_unreachable ();
    }
}

void
data_value::set_at (const data_value & value, unsigned offset)
{
  set_at (offset, value.bit_width, value, 0);
}


void
data_value::set (const data_value & value)
{
  gcc_assert (value.get_bitwidth () == bit_width);
  set_at (value, 0);
}

void
data_value::set_cst_at (const wide_int & val, unsigned offset)
{
  set_cst_at (offset, val.get_precision (), val, 0);
}

void
data_value::set_cst (const wide_int & val)
{
  gcc_assert (val.get_precision () == bit_width);
  set_cst_at (val, 0);
}

wide_int
data_value::get_cst_at (unsigned offset, unsigned width) const
{
  gcc_assert (offset < bit_width);
  gcc_assert (width <= bit_width - offset);

  enum value_type val_type = classify (offset, width);
  gcc_assert (val_type == VAL_CONSTANT);
  wide_int tmp = wide_int::from (wide_int_ref (constant_value), bit_width, UNSIGNED);
  if (offset > 0)
    tmp = wi::lrshift (tmp, offset);
  return wide_int::from (tmp, width, UNSIGNED);
}

wide_int
data_value::get_cst () const
{
  return get_cst_at (0, bit_width);
}

storage_address *
data_value::get_address_at (unsigned offset) const
{
  gcc_assert (classify (offset, HOST_BITS_PER_PTR) == VAL_ADDRESS);
  wide_int mask = wi::shifted_mask (offset, HOST_BITS_PER_PTR, false,
				    bit_width);

  stored_address *addr_info = find_address (offset);
  if (addr_info != nullptr)
    return &(addr_info->address);

  return nullptr;
}


storage_address *
data_value::get_address () const
{
  gcc_assert (bit_width == HOST_BITS_PER_PTR);
  return get_address_at (0);
}


data_value
data_value::get_at (unsigned offset, unsigned width) const
{
  data_value result (width);
  switch (classify (offset, width))
    {
    case VAL_CONSTANT:
      result.set_cst (get_cst_at (offset, width));
      break;

    case VAL_ADDRESS:
      {
	gcc_assert (width == HOST_BITS_PER_PTR);
	result.set_address (*get_address_at (offset));
      }
      break;

    case VAL_MIXED:
      result.set_at (0, width, *this, offset);
      break;

    case VAL_UNDEFINED:
      break;

    default:
      gcc_unreachable ();
    }

  return result;
}


tree
data_value::to_tree (tree type) const
{
  gcc_assert (classify () == VAL_CONSTANT);
  wide_int value = get_cst ();
  if (TREE_CODE (type) == INTEGER_TYPE)
    return wide_int_to_tree (type, value);
  else
    {
      tree int_type = build_nonstandard_integer_type (bit_width, false);
      tree int_val = wide_int_to_tree (int_type, value);
      return fold_build1 (VIEW_CONVERT_EXPR, type, int_val);
    }
}


void
context_printer::print_at (const data_value & value, tree type, unsigned offset,
			   unsigned width)
{
  if (TREE_CODE (type) == VECTOR_TYPE)
    {
      gcc_assert (width == value.get_bitwidth ());
      gcc_assert (offset == 0);
      tree elt_type = TREE_TYPE (type);
      unsigned elt_width;
      gcc_assert (get_constant_type_size (elt_type, elt_width));
      gcc_assert (elt_width != 0);
      gcc_assert (width % elt_width == 0);
      pp_left_brace (&pp);
      bool needs_comma = false;
      for (unsigned i = 0; i < width / elt_width; i++)
	{
	  if (needs_comma)
	    pp_comma (&pp);
	  pp_space (&pp);
	  print_at (value, elt_type, i * elt_width);
	  needs_comma = true;
	}
      pp_space (&pp);
      pp_right_brace (&pp);
    }
  else
    {
      enum value_type val_type = value.classify (offset, width);
      switch (val_type)
	{
	case VAL_ADDRESS:
	  {
	    gcc_assert (width == HOST_BITS_PER_PTR);
	    pp_ampersand (&pp);
	    storage_address *address = value.get_address_at (offset);
	    data_storage &target_storage = address->storage.get ();
	    target_storage.print (*this);
	    unsigned off = address->offset;
	    if (off > 0)
	      {
		pp_string (&pp, " + ");
		gcc_assert (off % CHAR_BIT == 0);
		off /= CHAR_BIT;
		pp_decimal_int (&pp, off);
		pp_character (&pp, 'B');
	      }
	  }
	  break;

	case VAL_CONSTANT:
	  {
	    const wide_int wi_val = value.get_cst_at (offset, width);
	    if (TREE_CODE (type) == REAL_TYPE)
	      {
		tree int_type = make_signed_type (width);
		tree cst = wide_int_to_tree (int_type, wi_val);
		tree real = fold_build1 (VIEW_CONVERT_EXPR, type, cst);
		print (nullptr, real);
	      }
	    else
	      pp_wide_int (&pp, wi_val, SIGNED); 
	  }
	  break;

	case VAL_UNDEFINED:
	  pp_string (&pp, "<undef>");
	  break;

	default:
	  gcc_unreachable ();
	}
    }
#if 0
      gcc_assert (TREE_CODE (type) == INTEGER_TYPE);
      gcc_assert (val_type == VAL_CONSTANT);
      signop sign = TYPE_SIGN (type);
      if (sign == SIGNED)
	{
	  gcc_assert (wi::fits_shwi_p (constant_value));
	  HOST_WIDE_INT val = constant_value.to_shwi ();
	  pp_wide_integer (&pp, val);
	}
      else if (sign == UNSIGNED)
	{
	  gcc_assert (wi::fits_uhwi_p (constant_value));
	  unsigned HOST_WIDE_INT val = constant_value.to_uhwi ();
	  pp_unsigned_wide_integer (&pp, val);
	}
      else
	gcc_unreachable ();
#endif
}


void
context_printer::print_at (const data_value & value, tree type, unsigned offset)
{
  unsigned width;
  gcc_assert (get_constant_type_size (type, width));
  print_at (value, type, offset, width);
}

void
context_printer::print (const data_value & value, tree type)
{
  print_at (value, type, 0);
}


tree
data_storage::get_variable () const
{
  gcc_assert (type == STRG_VARIABLE);
  return u.variable.decl;
}

void
data_storage::print (context_printer & printer) const
{
  pretty_printer & pp = printer.get_pretty_printer ();
  switch (type)
    {
    case STRG_VARIABLE:
      {
	tree decl = get_variable ();
	printer.print (nullptr, decl);
      }
      break;

    case STRG_ALLOC:
      {
	pp_less (&pp);
	pp_string (&pp, "alloc");
	pp_scalar (&pp, "%02d", u.allocated.index);
	pp_left_paren (&pp);
	unsigned allocated_amount = u.allocated.amount_bits;
	gcc_assert (allocated_amount % CHAR_BIT == 0);
	pp_decimal_int (&pp, allocated_amount / CHAR_BIT);
	pp_right_paren (&pp);
	pp_greater (&pp);
      }
      break;
    }
}


int
heap_memory::find (const data_storage & storage) const
{
  data_storage *strgp;
  unsigned i;
  FOR_EACH_VEC_ELT (storages, i, strgp)
    if (i > INT_MAX)
      return -2;
    else if (strgp == &storage)
      return i;

  return -1;
}


int
exec_context::find (const data_storage & storage) const
{
  data_storage *strgp;
  unsigned i;
  FOR_EACH_VEC_ELT (var_storages, i, strgp)
    if (i > INT_MAX)
      return -2;
    else if (strgp == &storage)
      return i;

  return alloc_mem.find (storage);
}


const exec_context &
exec_context::root () const
{
  const exec_context * ctx = this;

  while (ctx->parent != nullptr)
    ctx = ctx->parent;

  return *ctx;
}


data_storage *
exec_context::find_var (tree var) const
{
  data_storage *strgp;
  unsigned i;
  FOR_EACH_VEC_ELT (var_storages, i, strgp)
    if (strgp->matches (var))
      return strgp;

  return nullptr;
}


data_storage *
exec_context::find_reachable_var (tree variable) const
{
  data_storage * result = find_var (variable);
  if (result != nullptr)
    return result;

  if (parent != nullptr)
    return root ().find_var (variable);
  
  return nullptr;
}


data_storage *
heap_memory::find_alloc (unsigned index) const
{
  if (index >= storages.length ())
    return nullptr;

  return &const_cast <data_storage &> (storages[index]);
}


data_storage *
exec_context::find_alloc (unsigned index) const
{
  return alloc_mem.find_alloc (index);
}


data_storage &
heap_memory::get_storage (unsigned idx) const
{
  gcc_assert (idx < storages.length ());
  return const_cast <data_storage &> (storages[idx]);
}


data_storage &
heap_memory::allocate (unsigned amount)
{
  unsigned index = next_alloc_index;
  gcc_assert (index == storages.length ());
  storages.safe_push (data_storage (*this, index, amount * CHAR_BIT));
  data_storage & result = get_storage (index);
  next_alloc_index++;
  return result;
}


data_storage &
exec_context::allocate (unsigned amount)
{
  return alloc_mem.allocate (amount);
}


data_storage &
exec_context::get_storage (unsigned idx) const
{
  gcc_assert (idx < var_storages.length ());
  return const_cast <data_storage &> (var_storages[idx]);
}


data_value
exec_context::evaluate (tree expr) const
{
  enum tree_code code = TREE_CODE (expr);
  switch (code)
    {
    case ARRAY_REF:
    case COMPONENT_REF:
      {
	data_storage *storage = nullptr;
	int offset = -1;
	decompose_ref (expr, storage, offset);
	gcc_assert (storage != nullptr && offset >= 0);
	data_value var_value = storage->get_value ();
	unsigned bitwidth;
	bool cst = get_constant_type_size (TREE_TYPE (expr), bitwidth);
	gcc_assert (cst);
	return var_value.get_at (offset, bitwidth);
      }
      break;

    case REAL_CST:
      {
	tree sint = make_signed_type (TYPE_PRECISION (TREE_TYPE (expr)));
	tree t = fold_build1 (VIEW_CONVERT_EXPR, sint, expr);
	return evaluate (t);
      }
      break;

    case MEM_REF:
    case TARGET_MEM_REF:
      {
	tree ptr = TREE_OPERAND (expr, 0);
	data_value val_ptr = evaluate (ptr);
	gcc_assert (val_ptr.classify () == VAL_ADDRESS);
	storage_address *address = val_ptr.get_address ();
	gcc_assert (address != nullptr);
	data_value storage_value = address->storage.get ().get_value ();
	unsigned ptr_offset = address->offset;

	tree offset_bytes = TREE_OPERAND (expr, 1);
	data_value val_off = evaluate (offset_bytes);
	gcc_assert (val_off.classify () == VAL_CONSTANT);
	wide_int wi_off = val_off.get_cst ();
	gcc_assert (wi::fits_uhwi_p (wi_off));
	unsigned offset = wi_off.to_uhwi ();

	unsigned bit_width;
	if (!get_constant_type_size (TREE_TYPE (expr), bit_width))
	  gcc_unreachable ();

	if (code == TARGET_MEM_REF)
	  {
	    tree index = TREE_OPERAND (expr, 2);
	    data_value val_idx = evaluate (index);
	    gcc_assert (val_idx.classify () == VAL_CONSTANT);
	    wide_int wi_idx = val_idx.get_cst ();

	    tree step = TREE_OPERAND (expr, 3);
	    data_value val_step = evaluate (step);
	    gcc_assert (val_step.classify () == VAL_CONSTANT);
	    wide_int wi_step = val_step.get_cst ();

	    wide_int additional_off = wi_idx * wi_step;
	    gcc_assert (wi::fits_uhwi_p (additional_off));
	    offset += additional_off.to_uhwi ();
	  }

	return storage_value.get_at (offset * CHAR_BIT + ptr_offset, bit_width);
      }
      break;

    case INTEGER_CST:
      {
	data_value result (TREE_TYPE (expr));
	wide_int wi_expr = wi::to_wide (expr);
	result.set_cst (wi_expr);
	return result;
      }
      break;

    case SSA_NAME:
	if (SSA_NAME_IS_DEFAULT_DEF (expr))
	  return evaluate (SSA_NAME_VAR (expr));

	/* Fallthrough.  */
    case PARM_DECL:
    case VAR_DECL:
      {
	data_storage *data = find_reachable_var (expr);
	gcc_assert (data != nullptr);
	return data->get_value ();
      }

    case ADDR_EXPR:
      {
	tree decl = TREE_OPERAND (expr, 0);
	data_value result (TREE_TYPE (expr));
	if (TREE_CODE (decl) == FUNCTION_DECL)
	  {
	    // Unimplemented, set to NULL and hope that it won't be used.
	    wide_int zero = wi::zero (result.get_bitwidth ());
	    result.set_cst (zero);
	  }
	else
	  {
	    poly_int64 offset;
	    tree var = get_addr_base_and_unit_offset (TREE_OPERAND (expr, 0),
						      &offset);

	    HOST_WIDE_INT off;
	    bool is_constant = offset.is_constant (&off);
	    gcc_assert (is_constant && off >= 0);
	    unsigned off_bits = off * CHAR_BIT;

	    if (TREE_CODE (var) == VAR_DECL)
	      {
		data_storage *strg = find_reachable_var (var);
		gcc_assert (strg != nullptr);
		storage_address address (strg->get_ref (), off_bits);
		result.set_address (address);
	      }
	    else if (TREE_CODE (var) == INDIRECT_REF
		     || TREE_CODE (var) == MEM_REF)
	      {
		gcc_assert (TREE_CODE (var) == INDIRECT_REF
			    || integer_zerop (TREE_OPERAND (var, 1)));
		data_value root_val = evaluate (TREE_OPERAND (var, 0));
		gcc_assert (root_val.classify () == VAL_ADDRESS);
		storage_address *root_addr = root_val.get_address ();
		gcc_assert (root_addr != nullptr);
		gcc_assert (root_addr->offset == 0);
		storage_address address (root_addr->storage, off_bits);
		result.set_address (address);
	      }
	    else
	      gcc_unreachable ();
	  }
	return result;
      }

    case VECTOR_CST:
      {
	tree expr_type = TREE_TYPE (expr);
	data_value result (expr_type);
	tree elt_type = TREE_TYPE (expr_type);
	unsigned elt_size;
	gcc_assert (get_constant_type_size (elt_type, elt_size));

	unsigned HOST_WIDE_INT nunits;
	gcc_assert (VECTOR_CST_NELTS (expr).is_constant (&nunits));
	for (unsigned i = 0; i < nunits; ++i)
	  {
	    tree elt = VECTOR_CST_ELT (expr, i);
	    if (TREE_CODE (elt) == REAL_CST
		&& TREE_TYPE (elt) != elt_type)
	      {
		REAL_VALUE_TYPE *elt_val = TREE_REAL_CST_PTR (elt);
		machine_mode expected_mode = TYPE_MODE (elt_type);
		gcc_assert (exact_real_truncate (expected_mode, elt_val));
		REAL_VALUE_TYPE r;
		real_convert (&r, expected_mode, elt_val);
		elt = build_real (elt_type, r);;
	      }
	    data_value elt_val = evaluate (elt);
	    result.set_at (elt_val, i * elt_size);
	  }
	return result;
      }

    case CONSTRUCTOR:
      return evaluate_constructor (expr);

    default:
      gcc_unreachable ();
    }
}

data_value
exec_context::evaluate_constructor (tree cstr) const
{
  unsigned bit_width;
  gcc_assert (TREE_CODE (TREE_TYPE (cstr)) == VECTOR_TYPE
	      || TREE_CODE (TREE_TYPE (cstr)) == ARRAY_TYPE
	      || TREE_CODE (TREE_TYPE (cstr)) == RECORD_TYPE);

  gcc_assert (get_constant_type_size (TREE_TYPE (cstr), bit_width));

  data_value result(bit_width);

  if (CONSTRUCTOR_NELTS (cstr) == 0)
    {
      wide_int zero = wi::zero (bit_width);
      result.set_cst (zero);
      return result;
    }

  unsigned i;
  tree idx, elt;
  FOR_EACH_CONSTRUCTOR_ELT (CONSTRUCTOR_ELTS (cstr), i, idx, elt)
    {
      data_value val = evaluate (elt);

      wide_int offset;
      if (idx != NULL_TREE
	  && TREE_CODE (idx) == FIELD_DECL)
	offset = wi::to_wide (bit_position (idx));
      else
	{
	  wide_int wi_idx;
	  if (idx == NULL_TREE)
	    wi_idx = wi::uhwi (i, HOST_BITS_PER_WIDE_INT);
	  else
	    wi_idx = wi::to_wide (idx);

	  unsigned elt_size;
	  gcc_assert (get_constant_type_size (TREE_TYPE (elt), elt_size));

	  wide_int max_idx = wi::uhwi (bit_width / elt_size,
				       wi_idx.get_precision ());
	  gcc_assert (wi::ltu_p (wi_idx, max_idx));
	  offset = wi_idx * elt_size;
	}

      gcc_assert (wi::fits_uhwi_p (offset)
		  && wi::ltu_p (offset, UINT_MAX));

      result.set_at (val, offset.to_uhwi ());
    }

  return result;
}


data_value
exec_context::evaluate_unary (enum tree_code code, tree type, tree arg) const
{
  switch (code)
    {
    case NOP_EXPR:
      {
	data_value value = evaluate (arg);
	unsigned target_width = get_constant_type_size (type);
	unsigned source_width = value.get_bitwidth ();
	if (source_width == target_width)
	  return value;

	gcc_assert (value.classify () == VAL_CONSTANT);
	tree t = value.to_tree (TREE_TYPE (arg));
	tree r = fold_unary (code, type, t);
	gcc_assert (TREE_CODE (r) == INTEGER_CST);
	wide_int wi_r = wi::to_wide (r);

	data_value result (type);
	result.set_cst (wi_r);
	return result;
      }
      break;

    default:
      {
	gcc_assert (TREE_CODE (type) == INTEGER_TYPE
		    || TREE_CODE (type) == BOOLEAN_TYPE
		    || TREE_CODE (type) == REAL_TYPE);
	tree val = evaluate (arg).to_tree (TREE_TYPE (arg));
	tree t = fold_unary (code, type, val);
	gcc_assert (t != NULL_TREE);
	return evaluate (t);
      }
    }
}

data_value
exec_context::evaluate_binary (enum tree_code code, tree type, tree lhs, tree rhs) const
{
  gcc_assert (TREE_CODE (type) == INTEGER_TYPE
	      || TREE_CODE (type) == BOOLEAN_TYPE
	      || TREE_CODE (type) == POINTER_TYPE
	      || TREE_CODE (type) == REAL_TYPE);
  data_value val_lhs = evaluate (lhs);
  data_value val_rhs = evaluate (rhs);
  enum value_type lhs_type = val_lhs.classify ();
  enum value_type rhs_type = val_rhs.classify ();
  if (lhs_type == VAL_CONSTANT && rhs_type == VAL_CONSTANT)
    {
      gcc_assert (TREE_TYPE (lhs) == TREE_TYPE (rhs)
		  || (TREE_CODE (rhs) == INTEGER_CST
		      && TREE_CODE (TREE_TYPE (lhs)) == INTEGER_TYPE
		      && TYPE_PRECISION (TREE_TYPE (lhs))
			 == TYPE_PRECISION (TREE_TYPE (rhs))
		      && TYPE_UNSIGNED (TREE_TYPE (lhs))
			 == TYPE_UNSIGNED (TREE_TYPE (rhs))));
      tree lval = val_lhs.to_tree (TREE_TYPE (lhs));
      tree rval = val_rhs.to_tree (TREE_TYPE (rhs));
      tree t = fold_binary (code, type, lval, rval);
      gcc_assert (t != NULL_TREE);
      return evaluate (t);
    }
  else if ((lhs_type == VAL_ADDRESS && rhs_type == VAL_CONSTANT)
	   || (lhs_type == VAL_CONSTANT && rhs_type == VAL_ADDRESS))
    {
      if (code == PLUS_EXPR || code == POINTER_PLUS_EXPR)
	{
	  data_value *val_address = nullptr, *val_offset = nullptr;
	  if (lhs_type == VAL_ADDRESS && rhs_type == VAL_CONSTANT)
	    {
	      val_address = &val_lhs;
	      val_offset = &val_rhs;
	    }
	  else if (lhs_type == VAL_CONSTANT && rhs_type == VAL_ADDRESS)
	    {
	      val_address = &val_rhs;
	      val_offset = &val_lhs;
	    }
	  else
	    gcc_unreachable ();

	  storage_address *address = val_address->get_address ();
	  gcc_assert (address != nullptr);
	  wide_int offset = val_offset->get_cst ();
	  wide_int bit_offset = offset * CHAR_BIT;
	  wide_int total_offset = address->offset + bit_offset;
	  gcc_assert (wi::fits_uhwi_p (total_offset));
	  storage_address final_address (address->storage,
					 total_offset.to_uhwi ());
	  data_value result (type);
	  result.set_address (final_address);
	  return result;
	}
      else if (code == EQ_EXPR || code == NE_EXPR)
	{
	  data_value *val_null = nullptr;
	  if (lhs_type == VAL_ADDRESS && rhs_type == VAL_CONSTANT)
	    val_null = &val_rhs;
	  else if (lhs_type == VAL_CONSTANT && rhs_type == VAL_ADDRESS)
	    val_null = &val_lhs;
	  else
	    gcc_unreachable ();

	  /* Check that val_null really is the null pointer.  */
	  wide_int wi_null_val = val_null->get_cst ();
	  gcc_assert (wi_null_val == 0);

	  data_value result (type);
	  bool bool_result = code == NE_EXPR;
	  wide_int wi_result = wi::uhwi (bool_result, result.get_bitwidth ());
	  result.set_cst (wi_result);
	  return result;
	}
      else
	gcc_unreachable ();
    }
  else
    gcc_unreachable ();
}

void
exec_context::decompose_ref (tree data_ref, data_storage * & storage, int & offset) const
{
  offset = -1;
  enum tree_code code = TREE_CODE (data_ref);
  switch (code)
    {
    case VAR_DECL:
    case SSA_NAME:
      {
	tree var = data_ref;
	offset = 0;
	storage = find_reachable_var (var);
      }
      break;

    case ARRAY_REF:
      {
	data_storage *parent_storage = nullptr;
	int parent_offset = -1;
	tree parent_ref = TREE_OPERAND (data_ref, 0);
	decompose_ref (parent_ref, parent_storage, parent_offset);
	gcc_assert (parent_offset >= 0);
	gcc_assert (parent_storage != nullptr);

	tree idx = TREE_OPERAND (data_ref, 1);
	data_value val = evaluate (idx);
	gcc_assert (val.classify () == VAL_CONSTANT);
	wide_int wi_idx = val.get_cst ();
	gcc_assert (wi::fits_uhwi_p (wi_idx));
	unsigned HOST_WIDE_INT hw_idx = wi_idx.to_uhwi ();

	gcc_assert (TREE_OPERAND (data_ref, 3) == NULL_TREE);
	tree elt_type = TREE_TYPE (TREE_TYPE (parent_ref));
	unsigned size_bits;
	bool found_size = get_constant_type_size (elt_type, size_bits);
	gcc_assert (found_size);
	unsigned this_offset = hw_idx * size_bits;

	storage = parent_storage;
	offset = parent_offset + this_offset;
      }
      break;

    case COMPONENT_REF:
      {
	data_storage *parent_storage = nullptr;
	int parent_offset = -1;
	decompose_ref (TREE_OPERAND (data_ref, 0), parent_storage, parent_offset);
	gcc_assert (parent_offset >= 0);
	gcc_assert (parent_storage != nullptr);

	int this_offset = int_bit_position (TREE_OPERAND (data_ref, 1));
	gcc_assert (this_offset >= 0);

	storage = parent_storage;
	offset = parent_offset + this_offset;
      }
      break;

    case MEM_REF:
    case TARGET_MEM_REF:
      {
	tree var = TREE_OPERAND (data_ref, 0);
	data_value addr = evaluate (var);
	gcc_assert (addr.classify () == VAL_ADDRESS);
	storage = &(addr.get_address ()->storage.get ());

	tree off = TREE_OPERAND (data_ref, 1);
	data_value off_val = evaluate (off);
	gcc_assert (off_val.classify () == VAL_CONSTANT);
	wide_int wi_off = off_val.get_cst ();
	gcc_assert (wi::fits_uhwi_p (wi_off));
	unsigned HOST_WIDE_INT uhwi_off = wi_off.to_uhwi ();

	if (code == TARGET_MEM_REF)
	  {
	    tree index = TREE_OPERAND (data_ref, 2);
	    data_value idx_val = evaluate (index);
	    gcc_assert (idx_val.classify () == VAL_CONSTANT);
	    wide_int wi_idx = idx_val.get_cst ();

	    tree step = TREE_OPERAND (data_ref, 3);
	    data_value step_val = evaluate (step);
	    gcc_assert (step_val.classify () == VAL_CONSTANT);
	    wide_int wi_step = step_val.get_cst ();

	    wide_int additional_off = wi_idx * wi_step;
	    gcc_assert (wi::fits_uhwi_p (additional_off));
	    uhwi_off += additional_off.to_uhwi ();
	  }

	gcc_assert (uhwi_off <= UINT_MAX / CHAR_BIT);
	offset = uhwi_off * CHAR_BIT;
      }
      break;

    default:
      gcc_unreachable ();
    }

  gcc_assert (storage != nullptr);
  gcc_assert (offset >= 0);
}

void
exec_context::execute_assign (gassign *g)
{
  tree lhs = gimple_assign_lhs (g);
  tree lhs_type = TREE_TYPE (lhs);
  data_value value (lhs_type);

  enum tree_code rhs_code = gimple_assign_rhs_code (g);
  enum gimple_rhs_class rhs_class = get_gimple_rhs_class (rhs_code);
  switch (rhs_class)
    {
    case GIMPLE_SINGLE_RHS:
      value = evaluate (gimple_assign_rhs1 (g));
      break;

    case GIMPLE_UNARY_RHS:
      value = evaluate_unary (rhs_code, lhs_type, gimple_assign_rhs1 (g));
      break;

    case GIMPLE_BINARY_RHS:
      value = evaluate_binary (rhs_code, lhs_type,
			       gimple_assign_rhs1 (g), gimple_assign_rhs2 (g));
      break;

    default:
      gcc_unreachable ();
    }

  printer.print_value_update (*this, lhs, value);

  data_storage *storage = nullptr;
  int offset = -1;
  decompose_ref (lhs, storage, offset);
  gcc_assert (storage != nullptr);
  gcc_assert (offset >= 0);
  storage->set_at (value, offset);
}


static bool
is_ignored_function_call (gcall *g)
{
  if (gimple_call_builtin_p (g, BUILT_IN_FREE))
    /* TODO: Remove allocated storage.  */
    return true;
  else if (gimple_call_builtin_p (g))
    return false;

  tree fn = gimple_call_fn (g);
  if (TREE_CODE (fn) == ADDR_EXPR)
    fn = TREE_OPERAND (fn, 0);
  gcc_assert (TREE_CODE (fn) == FUNCTION_DECL);

  if (DECL_STRUCT_FUNCTION (fn) == nullptr
      && gimple_call_lhs (g) == NULL_TREE)
    {
      /* No known implementation: ignore the call and hope that it has no
	 observable effect.  */
      return true;
    }

  return false;
}



void
exec_context::execute_call (gcall *g)
{
  if (is_ignored_function_call (g))
    {
      printer.print_ignored_stmt ();
      return;
    }

  tree lhs = gimple_call_lhs (g);
  optional <data_value> result;
  if (gimple_call_builtin_p (g, BUILT_IN_MALLOC))
    {
      gcc_assert (lhs != NULL_TREE);
      result.emplace (data_value (TREE_TYPE (lhs)));

      gcc_assert (gimple_call_num_args (g) == 1);
      tree arg = gimple_call_arg (g, 0);
      data_value size = evaluate (arg);
      gcc_assert (size.classify () == VAL_CONSTANT);
      wide_int wi_size = size.get_cst ();
      gcc_assert (wi::fits_uhwi_p (wi_size));
      HOST_WIDE_INT alloc_amount = wi_size.to_uhwi ();
      data_storage &storage = allocate (alloc_amount);

      storage_address address (storage.get_ref (), 0);
      (*result).set_address (address);
    }
  else if (gimple_call_builtin_p (g, BUILT_IN_CALLOC))
    {
      gcc_assert (lhs != NULL_TREE);
      result.emplace (data_value (TREE_TYPE (lhs)));

      gcc_assert (gimple_call_num_args (g) == 2);
      tree arg0 = gimple_call_arg (g, 0);
      tree arg1 = gimple_call_arg (g, 1);
      data_value size0 = evaluate (arg0);
      data_value size1 = evaluate (arg1);
      gcc_assert (size0.classify () == VAL_CONSTANT);
      gcc_assert (size1.classify () == VAL_CONSTANT);
      wide_int wi_size0 = size0.get_cst ();
      wide_int wi_size1 = size1.get_cst ();
      wi::overflow_type ovf;
      wide_int combined_size = umul (wi_size0, wi_size1, &ovf);
      gcc_assert (ovf == wi::OVF_NONE);
      gcc_assert (wi::fits_uhwi_p (combined_size));
      HOST_WIDE_INT alloc_amount = combined_size.to_uhwi ();
      data_storage &storage = allocate (alloc_amount);

      wide_int char_bit = wi::shwi (CHAR_BIT, TYPE_PRECISION (size_type_node));
      wide_int size_bits = umul (combined_size, char_bit, &ovf);
      gcc_assert (ovf == wi::OVF_NONE);
      gcc_assert (wi::fits_uhwi_p (size_bits));
      unsigned HOST_WIDE_INT hwi_size = size_bits.to_uhwi ();
      data_value val (hwi_size);
      val.set_cst (wi::zero (hwi_size));
      storage.set (val);

      storage_address address (storage.get_ref (), 0);
      (*result).set_address (address);
    }
  else if (gimple_call_builtin_p (g, BUILT_IN_MEMCPY))
    {
      gcc_assert (gimple_call_num_args (g) == 3);
      tree arg0 = gimple_call_arg (g, 0);
      tree arg1 = gimple_call_arg (g, 1);
      tree arg2 = gimple_call_arg (g, 2);
      data_value ptr0 = evaluate (arg0);
      data_value ptr1 = evaluate (arg1);
      data_value len2 = evaluate (arg2);
      gcc_assert (ptr0.classify () == VAL_ADDRESS);
      gcc_assert (ptr1.classify () == VAL_ADDRESS);
      gcc_assert (len2.classify () == VAL_CONSTANT);
      storage_address addr0 = *ptr0.get_address ();
      data_storage & storage0 = addr0.storage.get ();
      data_value dest_val = storage0.get_value ();
      storage_address addr1 = *ptr1.get_address ();
      data_storage & storage1 = addr1.storage.get ();
      data_value src = storage1.get_value ();
      wide_int wi_len2 = len2.get_cst ();
      gcc_assert (wi::fits_uhwi_p (wi_len2));
      dest_val.set (src);
      storage0.set (dest_val);
    }
  else
    {
      tree fn = gimple_call_fn (g);
      if (TREE_CODE (fn) == ADDR_EXPR)
	fn = TREE_OPERAND (fn, 0);
      gcc_assert (TREE_CODE (fn) == FUNCTION_DECL);

      unsigned nargs = gimple_call_num_args (g); 
      auto_vec <tree> arguments;
      arguments.reserve (nargs);

      for (unsigned i = 0; i < nargs; i++)
	arguments.quick_push (gimple_call_arg (g, i));

      result = ::execute (DECL_STRUCT_FUNCTION (fn), *this, printer,
			  &arguments);
    }

  if (lhs == NULL_TREE)
    return;

  printer.print_value_update (*this, lhs, *result);
  data_storage *lhs_strg = find_var (lhs);
  gcc_assert (lhs_strg != nullptr);
  lhs_strg->set (*result);
}


void
exec_context::execute (gimple *g)
{
  printer.begin_stmt (g);
  switch (g->code)
    {
    case GIMPLE_ASSIGN:
      execute_assign (as_a <gassign *> (g));
      break;

    case GIMPLE_CALL:
      execute_call (as_a <gcall *> (g));
      break;

    case GIMPLE_LABEL:
      printer.print_ignored_stmt ();
      break;

    default:
      gcc_unreachable ();
    }
  printer.end_stmt (g);
}



gimple *
exec_context::execute (basic_block bb)
{
  printer.print_bb_entry (bb);

  gimple *g = bb->il.gimple.seq;

  if (g == nullptr)
    return nullptr;

  while (g && !is_ctrl_stmt (g))
    {
      execute (g);
      g = g->next;
    }

  gcc_assert (!g || !g->next);
  return g;
}


bool
exec_context::evaluate_condition (gcond *cond) const
{
  enum tree_code code = gimple_cond_code (cond);
  tree lhs = gimple_cond_lhs (cond);
  tree rhs = gimple_cond_rhs (cond);

  data_value val_lhs = evaluate (lhs);
  data_value val_rhs = evaluate (rhs);

  enum value_type lhs_type, rhs_type;
  lhs_type = val_lhs.classify ();
  rhs_type = val_rhs.classify ();
  if (lhs_type == VAL_CONSTANT && rhs_type == VAL_CONSTANT)
    {
      tree lval = val_lhs.to_tree (TREE_TYPE (lhs));
      tree rval = val_rhs.to_tree (TREE_TYPE (rhs));

      tree result = fold_binary (code, boolean_type_node, lval, rval);
      gcc_assert (result != NULL_TREE);

      if (integer_onep (result))
	return true;
      else if (integer_zerop (result))
	return false;
      else
	gcc_unreachable ();
    }
  else if (lhs_type == VAL_ADDRESS && rhs_type == VAL_ADDRESS)
    {
      storage_address *laddr = val_lhs.get_address ();
      storage_address *raddr = val_rhs.get_address ();

      gcc_assert (laddr != nullptr && raddr != nullptr);

      bool equal = &laddr->storage.get () == &raddr->storage.get ()
		   && laddr->offset == raddr->offset;

      switch (code)
	{
	case EQ_EXPR:
	  return equal;

	case NE_EXPR:
	  return !equal;

	default:
	  gcc_unreachable ();
	}
    }
  else if ((lhs_type == VAL_CONSTANT && rhs_type == VAL_ADDRESS)
	   || (lhs_type == VAL_ADDRESS && rhs_type == VAL_CONSTANT))
    {
      /* Comparison of an address against a null pointer.  */
      data_value * null = nullptr;
      if (lhs_type == VAL_CONSTANT)
	null = &val_lhs;
      else if (rhs_type == VAL_CONSTANT)
	null = &val_rhs;
      else
	gcc_unreachable ();

      gcc_assert (null->get_cst () == 0);
      if (code == EQ_EXPR)
	return false;
      else if (code == NE_EXPR)
	return true;
      else
	gcc_unreachable ();
    }
  else
    gcc_unreachable ();
}


edge
exec_context::select_leaving_edge (basic_block bb, gimple *last_stmt)
{
  if (last_stmt != nullptr)
    printer.begin_stmt (last_stmt);

  if (last_stmt == nullptr || is_a <ggoto *> (last_stmt))
    {
      if (last_stmt != nullptr)
	printer.end_stmt (last_stmt);
      return single_succ_edge (bb);
    }

  if (is_a <gcond *> (last_stmt))
    {
      bool cond_result = evaluate_condition (as_a <gcond *> (last_stmt));
      printer.print_condition_value (cond_result);
      int flag;
      if (cond_result)
	flag = EDGE_TRUE_VALUE;
      else
	flag = EDGE_FALSE_VALUE;

      edge e, selected = nullptr;
      edge_iterator ei;
      FOR_EACH_EDGE (e, ei, bb->succs)
	if (e->flags & flag)
	  {
	    gcc_assert (selected == nullptr);
	    selected = e;
	  }

      printer.end_stmt (last_stmt);

      gcc_assert (selected != nullptr);
      return selected;
    }

  gcc_unreachable ();
}


void
exec_context::execute_phi (gphi *phi, edge e)
{
  printer.begin_stmt (phi);
  tree lhs = gimple_phi_result (phi);
  tree phi_val = gimple_phi_arg_def_from_edge (phi, e);
  gassign *assign = gimple_build_assign (lhs, phi_val);
  execute_assign (assign);
  printer.end_stmt (phi);
}


void
exec_context::jump (edge e)
{
  printer.print_bb_jump (e);

  gphi_iterator gsi;
  for (gsi = gsi_start_nonvirtual_phis (e->dest); !gsi_end_p (gsi);
       gsi_next_nonvirtual_phi (&gsi))
    execute_phi (*gsi, e);
}


optional <data_value>
exec_context::execute_function (struct function *func)
{
  printer.print_function_entry (func);
  basic_block bb = ENTRY_BLOCK_PTR_FOR_FN (func);
  greturn *final_stmt = nullptr;

  while (true)
    {
      gimple *last_stmt = execute (bb);

      if (last_stmt != nullptr
	  && is_a <greturn *> (last_stmt))
	{
	  final_stmt = as_a <greturn *> (last_stmt);
	  break;
	}

      if (bb == EXIT_BLOCK_PTR_FOR_FN (func))
	break;

      edge e = select_leaving_edge (bb, last_stmt);
      jump (e);
      bb = e->dest;
    }

  if (final_stmt == nullptr)
    return {};
  tree retexpr = gimple_return_retval (final_stmt);
  if (retexpr == NULL_TREE)
    return {};
  data_value result = evaluate (retexpr);
  printer.print_function_exit (func);
  return result;
}


static optional <data_value>
execute (struct function * func, exec_context & caller,
	 context_printer & printer, vec<tree> * arg_values)
{
  tree fndecl = func->decl;

  auto_vec <tree> arguments;

  tree arg = DECL_ARGUMENTS (fndecl);
  while (arg != NULL_TREE)
    {
      arguments.safe_push (arg);
      arg = TREE_CHAIN (arg);
    }

  context_builder builder {};
  builder.add_decls (func->gimple_df->ssa_names);
  builder.add_decls (func->local_decls);
  builder.add_decls (&arguments);

  exec_context ctx = builder.build (caller, printer);

  tree *valp = nullptr;
  unsigned i = 0;
  arg = DECL_ARGUMENTS (fndecl);
  while (arg != NULL_TREE && arg_values->iterate (i, &valp))
    {
      data_value value = caller.evaluate (*valp);
      data_storage *storage = ctx.find_reachable_var (arg);
      gcc_assert (storage != nullptr);
      storage->set (value);

      arg = TREE_CHAIN (arg);
      i++;
    }

  gcc_assert (arg == NULL_TREE && !arg_values->iterate (i, &valp));

  return ctx.execute_function (func);
}


static struct function *
find_main ()
{
  struct cgraph_node * node;
  FOR_EACH_DEFINED_FUNCTION (node)
    {
      struct function *fun = node->get_fun ();
      tree decl = fun->decl;
      tree name = DECL_NAME (decl);
      const char *id = IDENTIFIER_POINTER (name);
      if (strcmp (id, "main") == 0)
	return fun;
    }
  return nullptr;
}

static void
execute (void)
{
  varpool_node *vnode;

  vec<tree> static_vars{};

  FOR_EACH_VARIABLE (vnode)
    {
      tree decl = vnode->decl;
      if (decl
	  && (TREE_STATIC (decl)
	      || TREE_PUBLIC (decl)
	      || DECL_EXTERNAL (decl)))
	static_vars.safe_push (decl);
    }

  heap_memory alloc_mem;

  context_printer printer;

  context_builder builder {};
  builder.add_decls (&static_vars);
  exec_context root_context = builder.build (alloc_mem, printer);

  tree *varp = nullptr;
  unsigned i = 0;
  FOR_EACH_VEC_ELT (static_vars, i, varp)
    {
      tree var = *varp;
      tree initial = DECL_INITIAL (var);
      if (initial != NULL_TREE)
	{
	  gassign *tmp_assign = gimple_build_assign (var, initial);
	  root_context.execute (tmp_assign);
	}
    }

  struct function *main = find_main ();
  gcc_assert (main != nullptr);
  vec<tree> args{};
  args.safe_push (build_zero_cst (integer_type_node));
  args.safe_push (null_pointer_node);
  execute (main, root_context, printer, &args);
}

/* Perform simple optimizations based on callgraph.  */

void
symbol_table::compile (void)
{
  if (seen_error ())
    return;

  symtab_node::checking_verify_symtab_nodes ();

  symtab_node::check_ifunc_callee_symtab_nodes ();

  timevar_push (TV_CGRAPHOPT);
  if (pre_ipa_mem_report)
    dump_memory_report ("Memory consumption before IPA");
  if (!quiet_flag)
    fprintf (stderr, "Performing interprocedural optimizations\n");
  state = IPA;

  /* If LTO is enabled, initialize the streamer hooks needed by GIMPLE.  */
  if (flag_generate_lto || flag_generate_offload)
    lto_streamer_hooks_init ();

  /* Don't run the IPA passes if there was any error or sorry messages.  */
  if (!seen_error ())
  {
    timevar_start (TV_CGRAPH_IPA_PASSES);
    ipa_passes ();
    timevar_stop (TV_CGRAPH_IPA_PASSES);
  }
  /* Do nothing else if any IPA pass found errors or if we are just streaming LTO.  */
  if (seen_error ()
      || ((!in_lto_p || flag_incremental_link == INCREMENTAL_LINK_LTO)
	  && flag_lto && !flag_fat_lto_objects))
    {
      timevar_pop (TV_CGRAPHOPT);
      return;
    }

  global_info_ready = true;
  if (dump_file)
    {
      fprintf (dump_file, "Optimized ");
      symtab->dump (dump_file);
    }
  if (post_ipa_mem_report)
    dump_memory_report ("Memory consumption after IPA");
  timevar_pop (TV_CGRAPHOPT);

  if (flag_gimple_exec)
    execute ();

  /* Output everything.  */
  switch_to_section (text_section);
  (*debug_hooks->assembly_start) ();
  if (!quiet_flag)
    fprintf (stderr, "Assembling functions:\n");
  symtab_node::checking_verify_symtab_nodes ();

  bitmap_obstack_initialize (NULL);
  execute_ipa_pass_list (g->get_passes ()->all_late_ipa_passes);
  bitmap_obstack_release (NULL);
  mark_functions_to_output ();

  /* When weakref support is missing, we automatically translate all
     references to NODE to references to its ultimate alias target.
     The renaming mechanism uses flag IDENTIFIER_TRANSPARENT_ALIAS and
     TREE_CHAIN.

     Set up this mapping before we output any assembler but once we are sure
     that all symbol renaming is done.

     FIXME: All this ugliness can go away if we just do renaming at gimple
     level by physically rewriting the IL.  At the moment we can only redirect
     calls, so we need infrastructure for renaming references as well.  */
#ifndef ASM_OUTPUT_WEAKREF
  symtab_node *node;

  FOR_EACH_SYMBOL (node)
    if (node->alias
	&& lookup_attribute ("weakref", DECL_ATTRIBUTES (node->decl)))
      {
	tree id = DECL_ASSEMBLER_NAME (node->decl);
	gcc_assert (!IDENTIFIER_INTERNAL_P (id));
	IDENTIFIER_TRANSPARENT_ALIAS (id) = 1;
	TREE_CHAIN (id)
	   = (node->alias_target ? node->alias_target
	      : DECL_ASSEMBLER_NAME (node->get_alias_target ()->decl));
      }
#endif

  state = EXPANSION;

  /* Output first asm statements and anything ordered. The process
     flag is cleared for these nodes, so we skip them later.  */
  output_in_order ();

  timevar_start (TV_CGRAPH_FUNC_EXPANSION);
  expand_all_functions ();
  timevar_stop (TV_CGRAPH_FUNC_EXPANSION);

  output_variables ();

  process_new_functions ();
  state = FINISHED;
  output_weakrefs ();

  if (dump_file)
    {
      fprintf (dump_file, "\nFinal ");
      symtab->dump (dump_file);
    }
  if (!flag_checking)
    return;
  symtab_node::verify_symtab_nodes ();
  /* Double check that all inline clones are gone and that all
     function bodies have been released from memory.  */
  if (!seen_error ())
    {
      cgraph_node *node;
      bool error_found = false;

      FOR_EACH_DEFINED_FUNCTION (node)
	if (node->inlined_to
	    || gimple_has_body_p (node->decl))
	  {
	    if (DECL_STRUCT_FUNCTION (node->decl)
		&& (DECL_STRUCT_FUNCTION (node->decl)->curr_properties
		    & PROP_assumptions_done) != 0)
	      continue;
	    error_found = true;
	    node->debug ();
	  }
      if (error_found)
	internal_error ("nodes with unreleased memory found");
    }
}

/* Earlydebug dump file, flags, and number.  */

static int debuginfo_early_dump_nr;
static FILE *debuginfo_early_dump_file;
static dump_flags_t debuginfo_early_dump_flags;

/* Debug dump file, flags, and number.  */

static int debuginfo_dump_nr;
static FILE *debuginfo_dump_file;
static dump_flags_t debuginfo_dump_flags;

/* Register the debug and earlydebug dump files.  */

void
debuginfo_early_init (void)
{
  gcc::dump_manager *dumps = g->get_dumps ();
  debuginfo_early_dump_nr = dumps->dump_register (".earlydebug", "earlydebug",
						  "earlydebug", DK_tree,
						  OPTGROUP_NONE,
						  false);
  debuginfo_dump_nr = dumps->dump_register (".debug", "debug",
					     "debug", DK_tree,
					     OPTGROUP_NONE,
					     false);
}

/* Initialize the debug and earlydebug dump files.  */

void
debuginfo_init (void)
{
  gcc::dump_manager *dumps = g->get_dumps ();
  debuginfo_dump_file = dump_begin (debuginfo_dump_nr, NULL);
  debuginfo_dump_flags = dumps->get_dump_file_info (debuginfo_dump_nr)->pflags;
  debuginfo_early_dump_file = dump_begin (debuginfo_early_dump_nr, NULL);
  debuginfo_early_dump_flags
    = dumps->get_dump_file_info (debuginfo_early_dump_nr)->pflags;
}

/* Finalize the debug and earlydebug dump files.  */

void
debuginfo_fini (void)
{
  if (debuginfo_dump_file)
    dump_end (debuginfo_dump_nr, debuginfo_dump_file);
  if (debuginfo_early_dump_file)
    dump_end (debuginfo_early_dump_nr, debuginfo_early_dump_file);
}

/* Set dump_file to the debug dump file.  */

void
debuginfo_start (void)
{
  set_dump_file (debuginfo_dump_file);
}

/* Undo setting dump_file to the debug dump file.  */

void
debuginfo_stop (void)
{
  set_dump_file (NULL);
}

/* Set dump_file to the earlydebug dump file.  */

void
debuginfo_early_start (void)
{
  set_dump_file (debuginfo_early_dump_file);
}

/* Undo setting dump_file to the earlydebug dump file.  */

void
debuginfo_early_stop (void)
{
  set_dump_file (NULL);
}

/* Analyze the whole compilation unit once it is parsed completely.  */

void
symbol_table::finalize_compilation_unit (void)
{
  timevar_push (TV_CGRAPH);

  /* If we're here there's no current function anymore.  Some frontends
     are lazy in clearing these.  */
  current_function_decl = NULL;
  set_cfun (NULL);

  /* Do not skip analyzing the functions if there were errors, we
     miss diagnostics for following functions otherwise.  */

  /* Emit size functions we didn't inline.  */
  finalize_size_functions ();

  /* Mark alias targets necessary and emit diagnostics.  */
  handle_alias_pairs ();

  if (!quiet_flag)
    {
      fprintf (stderr, "\nAnalyzing compilation unit\n");
      fflush (stderr);
    }

  if (flag_dump_passes)
    dump_passes ();

  /* Gimplify and lower all functions, compute reachability and
     remove unreachable nodes.  */
  analyze_functions (/*first_time=*/true);

  /* Mark alias targets necessary and emit diagnostics.  */
  handle_alias_pairs ();

  /* Gimplify and lower thunks.  */
  analyze_functions (/*first_time=*/false);

  /* All nested functions should be lowered now.  */
  nested_function_info::release ();

  /* Offloading requires LTO infrastructure.  */
  if (!in_lto_p && g->have_offload)
    flag_generate_offload = 1;

  if (!seen_error ())
    {
      timevar_push (TV_SYMOUT);

      /* Give the frontends the chance to emit early debug based on
	 what is still reachable in the TU.  */
      (*lang_hooks.finalize_early_debug) ();

      /* Clean up anything that needs cleaning up after initial debug
	 generation.  */
      debuginfo_early_start ();
      (*debug_hooks->early_finish) (main_input_filename);
      debuginfo_early_stop ();

      timevar_pop (TV_SYMOUT);
    }

  /* Finally drive the pass manager.  */
  compile ();

  timevar_pop (TV_CGRAPH);
}

/* Reset all state within cgraphunit.cc so that we can rerun the compiler
   within the same process.  For use by toplev::finalize.  */

void
cgraphunit_cc_finalize (void)
{
  gcc_assert (cgraph_new_nodes.length () == 0);
  cgraph_new_nodes.truncate (0);

  queued_nodes = &symtab_terminator;

  first_analyzed = NULL;
  first_analyzed_var = NULL;
}

/* Creates a wrapper from cgraph_node to TARGET node. Thunk is used for this
   kind of wrapper method.  */

void
cgraph_node::create_wrapper (cgraph_node *target)
{
  /* Preserve DECL_RESULT so we get right by reference flag.  */
  tree decl_result = DECL_RESULT (decl);

  /* Remove the function's body but keep arguments to be reused
     for thunk.  */
  release_body (true);
  reset ();

  DECL_UNINLINABLE (decl) = false;
  DECL_RESULT (decl) = decl_result;
  DECL_INITIAL (decl) = NULL;
  allocate_struct_function (decl, false);
  set_cfun (NULL);

  /* Turn alias into thunk and expand it into GIMPLE representation.  */
  definition = true;
  semantic_interposition = opt_for_fn (decl, flag_semantic_interposition);

  /* Create empty thunk, but be sure we did not keep former thunk around.
     In that case we would need to preserve the info.  */
  gcc_checking_assert (!thunk_info::get (this));
  thunk_info::get_create (this);
  thunk = true;
  create_edge (target, NULL, count);
  callees->can_throw_external = !TREE_NOTHROW (target->decl);

  tree arguments = DECL_ARGUMENTS (decl);

  while (arguments)
    {
      TREE_ADDRESSABLE (arguments) = false;
      arguments = TREE_CHAIN (arguments);
    }

  expand_thunk (this, false, true);
  thunk_info::remove (this);

  /* Inline summary set-up.  */
  analyze ();
  inline_analyze_function (this);
}

#if CHECKING_P

namespace selftest
{

void
get_constant_type_size_tests ()
{
  ASSERT_EQ (get_constant_type_size (integer_type_node), HOST_BITS_PER_INT);

  ASSERT_EQ (get_constant_type_size (ptr_type_node), HOST_BITS_PER_PTR);

  tree vec2ptr = build_vector_type (ptr_type_node, 2);
  int val = get_constant_type_size (vec2ptr);
  ASSERT_EQ (val, 2 * HOST_BITS_PER_PTR);

  ASSERT_EQ (get_constant_type_size (boolean_type_node), 1);
}

static tree
create_var (tree type, const char * name)
{
  return build_decl (UNKNOWN_LOCATION, VAR_DECL, get_identifier (name),
		     type);
}


void
data_value_classify_tests ()
{
  heap_memory mem;
  context_printer printer;

  tree a = create_var (integer_type_node,"a");

  vec<tree> decls{};
  decls.safe_push (a);
  vec<tree> empty{};

  context_builder builder {};
  builder.add_decls (&decls);
  exec_context ctx = builder.build (mem, printer);

  data_value val(integer_type_node);

  ASSERT_EQ (val.classify (), VAL_UNDEFINED);

  wide_int i = wi::shwi (17, get_constant_type_size (integer_type_node));

  val.set_cst (i);

  ASSERT_EQ (val.classify (), VAL_CONSTANT);


  data_storage *storage_a = ctx.find_reachable_var (a);
  gcc_assert (storage_a != nullptr);

  storage_address address_a (storage_a->get_ref (), 0);

  data_value ptr(ptr_type_node);

  ASSERT_EQ (ptr.classify (), VAL_UNDEFINED);

  ptr.set_address (address_a);

  ASSERT_EQ (ptr.classify (), VAL_ADDRESS);


  tree vec2int = build_vector_type (integer_type_node, 2);
  data_value val2(vec2int);

  ASSERT_EQ (val2.classify (0, HOST_BITS_PER_INT), VAL_UNDEFINED);
  ASSERT_EQ (val2.classify (HOST_BITS_PER_INT, HOST_BITS_PER_INT),
	     VAL_UNDEFINED);
  ASSERT_EQ (val2.classify (), VAL_UNDEFINED);

  val2.set_cst_at (i, HOST_BITS_PER_INT);

  ASSERT_EQ (val2.classify (0, HOST_BITS_PER_INT), VAL_UNDEFINED);
  ASSERT_EQ (val2.classify (HOST_BITS_PER_INT, HOST_BITS_PER_INT), VAL_CONSTANT);
  ASSERT_EQ (val2.classify (), VAL_MIXED);

  val2.set_cst_at (i, 0);

  ASSERT_EQ (val2.classify (0, HOST_BITS_PER_INT), VAL_CONSTANT);
  ASSERT_EQ (val2.classify (HOST_BITS_PER_INT, HOST_BITS_PER_INT), VAL_CONSTANT);
  ASSERT_EQ (val2.classify (), VAL_CONSTANT);


  tree vec2ptr = build_vector_type (ptr_type_node, 2);
  data_value val3(vec2ptr);

  ASSERT_EQ (val3.classify (0, HOST_BITS_PER_PTR), VAL_UNDEFINED);
  ASSERT_EQ (val3.classify (HOST_BITS_PER_PTR, HOST_BITS_PER_PTR),
	     VAL_UNDEFINED);
  ASSERT_EQ (val3.classify (), VAL_UNDEFINED);

  val3.set_address_at (address_a, HOST_BITS_PER_PTR);

  ASSERT_EQ (val3.classify (0, HOST_BITS_PER_PTR), VAL_UNDEFINED);
  ASSERT_EQ (val3.classify (HOST_BITS_PER_PTR, HOST_BITS_PER_PTR), VAL_ADDRESS);
  ASSERT_EQ (val3.classify (), VAL_MIXED);

  val3.set_address_at (address_a, 0);

  ASSERT_EQ (val3.classify (0, HOST_BITS_PER_PTR), VAL_ADDRESS);
  ASSERT_EQ (val3.classify (HOST_BITS_PER_PTR, HOST_BITS_PER_PTR), VAL_ADDRESS);
  ASSERT_EQ (val3.classify (), VAL_ADDRESS);
}

void
exec_context_find_reachable_var_tests ()
{
  heap_memory mem;
  context_printer printer;
  //printer.pp.set_output_stream (nullptr);

  tree a = create_var (integer_type_node, "a");
  tree b = create_var (integer_type_node, "b");
  tree c = create_var (integer_type_node, "c");

  vec<tree> vars{};
  vars.safe_push (a);
  vec<tree> regs{};
  regs.safe_push (b);

  context_builder builder {};
  builder.add_decls (&vars);
  builder.add_decls (&regs);
  exec_context ctx = builder.build (mem, printer);

  ASSERT_EQ (ctx.find_reachable_var (c), nullptr);
  ASSERT_NE (ctx.find_reachable_var (b), nullptr);
  ASSERT_NE (ctx.find_reachable_var (a), nullptr);

  data_storage *storage_a = ctx.find_reachable_var (a);
  ASSERT_EQ (storage_a->get_variable (), a);
  data_storage *storage_b = ctx.find_reachable_var (b);
  ASSERT_EQ (storage_b->get_variable (), b);

  tree d = create_var (integer_type_node, "d");
  tree e = create_var (integer_type_node, "e");

  vec<tree> vars2{};
  vars2.safe_push (d);
  vec<tree> regs2{};
  regs2.safe_push (e);

  context_builder builder2 {};
  builder.add_decls (&vars2);
  builder.add_decls (&regs2);
  exec_context ctx2 = builder.build (ctx, printer);

  ASSERT_NE (ctx2.find_reachable_var (e), nullptr);
  ASSERT_NE (ctx2.find_reachable_var (d), nullptr);
  ASSERT_NE (ctx2.find_reachable_var (b), nullptr);
  ASSERT_NE (ctx2.find_reachable_var (a), nullptr);

  vec<tree> empty{};

  exec_context ctx3 = context_builder ().build (ctx2, printer);

  ASSERT_NE (ctx3.find_reachable_var (a), nullptr);
  ASSERT_NE (ctx3.find_reachable_var (b), nullptr);
  ASSERT_EQ (ctx3.find_reachable_var (d), nullptr);
  ASSERT_EQ (ctx3.find_reachable_var (e), nullptr);
}

void
data_value_set_address_tests ()
{
  heap_memory mem;
  context_printer printer;

  tree a = create_var (integer_type_node, "a");
  tree b = create_var (integer_type_node, "b");

  vec<tree> decls{};
  decls.safe_push (a);
  decls.safe_push (b);
  vec<tree> empty{};

  context_builder builder {};
  builder.add_decls (&decls);
  exec_context ctx = builder.build (mem, printer);

  data_value val1(ptr_type_node);

  data_storage *storage_a = ctx.find_reachable_var (a);
  storage_address address_a (storage_a->get_ref (), 0);
  val1.set_address (address_a);

  ASSERT_EQ (val1.classify (), VAL_ADDRESS);
  ASSERT_EQ (&val1.get_address ()->storage.get (), storage_a);

  data_storage *storage_b = ctx.find_reachable_var (b);
  storage_address address_b (storage_b->get_ref (), 0);
  val1.set_address (address_b);

  ASSERT_EQ (val1.classify (), VAL_ADDRESS);
  ASSERT_EQ (&val1.get_address ()->storage.get (), storage_b);

  exec_context ctx2 = context_builder ().build (ctx, printer);

  data_value val2(ptr_type_node);

  ASSERT_EQ (ctx2.find_reachable_var (a), storage_a);
}

void
data_value_set_tests ()
{
  heap_memory mem;
  context_printer printer;

  tree a = create_var (integer_type_node, "a");
  tree b = create_var (integer_type_node, "b");

  vec<tree> decls{};
  decls.safe_push (a);
  decls.safe_push (b);
  vec<tree> empty{};

  context_builder builder {};
  builder.add_decls (&decls);
  exec_context ctx = builder.build (mem, printer);

  data_storage *storage_a = ctx.find_var (a);
  storage_address address_a (storage_a->get_ref (), 0);

  data_value val1(ptr_type_node);

  val1.set_address (address_a);

  data_value val2(ptr_type_node);

  val2.set (val1);
  ASSERT_EQ (val2.classify (), VAL_ADDRESS);
  ASSERT_EQ (&val2.get_address ()->storage.get (), storage_a);
}

void
data_value_set_at_tests ()
{
  heap_memory mem;
  context_printer printer;

  tree a = create_var (integer_type_node, "a");
  tree b = create_var (integer_type_node, "b");

  vec<tree> decls{};
  decls.safe_push (a);
  decls.safe_push (b);
  vec<tree> empty{};

  context_builder builder {};
  builder.add_decls (&decls);
  exec_context ctx = builder.build (mem, printer);

  data_storage *storage_a = ctx.find_reachable_var (a);
  data_storage *storage_b = ctx.find_reachable_var (b);

  storage_address address_a (storage_a->get_ref (), 0);
  storage_address address_b (storage_b->get_ref (), 0);

  data_value val1(ptr_type_node);

  val1.set_address (address_a);

  tree vec2ptr = build_vector_type (ptr_type_node, 2);
  data_value val2(vec2ptr);

  val2.set_at (val1, HOST_BITS_PER_PTR);
  ASSERT_EQ (val2.classify (HOST_BITS_PER_PTR, HOST_BITS_PER_PTR), VAL_ADDRESS);
  ASSERT_EQ (&val2.get_address_at (HOST_BITS_PER_PTR)->storage.get (), storage_a);

  val1.set_address (address_b);

  val2.set_at (val1, 0);
  ASSERT_EQ (val2.classify (0, HOST_BITS_PER_PTR), VAL_ADDRESS);
  ASSERT_EQ (&val2.get_address_at (0)->storage.get (), storage_b);

  data_value val3(vec2ptr);
  val3.set_at (val2, 0);

  ASSERT_EQ (val3.classify (0, HOST_BITS_PER_PTR), VAL_ADDRESS);
  ASSERT_EQ (&val3.get_address_at (0)->storage.get (), storage_b);
  ASSERT_EQ (val3.classify (HOST_BITS_PER_PTR, HOST_BITS_PER_PTR), VAL_ADDRESS);
  ASSERT_EQ (&val3.get_address_at (HOST_BITS_PER_PTR)->storage.get (), storage_a);

  tree derived = make_node (RECORD_TYPE);
  tree field2 = build_decl (input_location, FIELD_DECL,
			    get_identifier ("field2"), integer_type_node);
  DECL_CONTEXT (field2) = derived;
  DECL_CHAIN (field2) = NULL_TREE;
  tree field1 = build_decl (input_location, FIELD_DECL,
			    get_identifier ("field1"), integer_type_node);
  DECL_CONTEXT (field1) = derived;
  DECL_CHAIN (field1) = field2;
  TYPE_FIELDS (derived) = field1;
  layout_type (derived);

  tree c = create_var (derived, "c");

  context_builder builder2 {};
  builder2.add_decls (&decls);
  vec<tree> decls2{};
  decls2.safe_push (c);

  data_value val_derived(derived);

  ASSERT_EQ (val_derived.classify (), VAL_UNDEFINED);

  wide_int cst = wi::shwi (13, HOST_BITS_PER_INT);
  val_derived.set_cst_at (cst, 0);

  ASSERT_EQ (val_derived.classify (), VAL_MIXED);
  ASSERT_EQ (val_derived.classify (0, HOST_BITS_PER_INT), VAL_CONSTANT);
  wide_int wi_val = val_derived.get_cst_at (0, HOST_BITS_PER_INT);
  ASSERT_TRUE (wi::fits_shwi_p (wi_val));
  ASSERT_EQ (wi_val.to_shwi (), 13);


  data_value vv (integer_type_node);
  wide_int wi23 = wi::shwi (23, HOST_BITS_PER_INT);
  vv.set_cst (wi23);

  data_value vv2 (derived);
  vv2.set_at (vv, HOST_BITS_PER_INT);

  ASSERT_EQ (vv2.classify (), VAL_MIXED);

  ASSERT_EQ (vv2.classify (0, HOST_BITS_PER_INT), VAL_UNDEFINED);
  ASSERT_EQ (vv2.classify (HOST_BITS_PER_INT, HOST_BITS_PER_INT), VAL_CONSTANT);
  wide_int wi_field2 = vv2.get_cst_at (HOST_BITS_PER_INT, HOST_BITS_PER_INT);
  ASSERT_PRED1 (wi::fits_shwi_p, wi_field2);
  ASSERT_EQ (wi_field2.to_shwi (), 23);


  tree c12 = build_array_type_nelts (char_type_node, 12);

  data_value v (c12);

  wide_int wi33 = wi::shwi (33, CHAR_BIT);
  v.set_cst_at (wi33, 9 * CHAR_BIT);

  data_value v2 (c12);
  v2.set_at (9 * CHAR_BIT, CHAR_BIT, v, 9 * CHAR_BIT);

  ASSERT_EQ (v2.classify (), VAL_MIXED);

  ASSERT_EQ (v2.classify (8 * CHAR_BIT, CHAR_BIT), VAL_UNDEFINED);
  ASSERT_EQ (v2.classify (10 * CHAR_BIT, CHAR_BIT), VAL_UNDEFINED);
  ASSERT_EQ (v2.classify (9 * CHAR_BIT, CHAR_BIT), VAL_CONSTANT);
  wide_int wi_c9 = v2.get_cst_at (9 * CHAR_BIT, CHAR_BIT);
  ASSERT_PRED1 (wi::fits_shwi_p, wi_c9);
  ASSERT_EQ (wi_c9.to_shwi (), 33);

  data_value v3 (c12);
  v3.set (v);

  ASSERT_EQ (v3.classify (), VAL_MIXED);

  ASSERT_EQ (v3.classify (8 * CHAR_BIT, CHAR_BIT), VAL_UNDEFINED);
  ASSERT_EQ (v3.classify (10 * CHAR_BIT, CHAR_BIT), VAL_UNDEFINED);
  ASSERT_EQ (v3.classify (9 * CHAR_BIT, CHAR_BIT), VAL_CONSTANT);
  wide_int wi_c9_bis = v3.get_cst_at (9 * CHAR_BIT, CHAR_BIT);
  ASSERT_PRED1 (wi::fits_shwi_p, wi_c9_bis);
  ASSERT_EQ (wi_c9_bis.to_shwi (), 33);


  tree mixed = make_node (RECORD_TYPE);
  tree i3 = build_decl (input_location, FIELD_DECL,
			get_identifier ("i3"), integer_type_node);
  DECL_CONTEXT (i3) = mixed;
  DECL_CHAIN (i3) = NULL_TREE;
  tree p2 = build_decl (input_location, FIELD_DECL,
			get_identifier ("p2"), ptr_type_node);
  DECL_CONTEXT (p2) = mixed;
  DECL_CHAIN (p2) = i3;
  tree i1 = build_decl (input_location, FIELD_DECL,
			get_identifier ("i1"), long_integer_type_node);
  DECL_CONTEXT (i1) = mixed;
  DECL_CHAIN (i1) = p2;
  TYPE_FIELDS (mixed) = i1;
  layout_type (mixed);

  tree t = create_var (integer_type_node, "t");

  vec<tree> decls4{};
  decls4.safe_push (t);

  context_builder builder4 {};
  builder4.add_decls (&decls4);
  exec_context ctx4 = builder4.build (mem, printer);

  data_value mv (mixed);

  wide_int wi4 = wi::shwi (4, HOST_BITS_PER_LONG);
  mv.set_cst_at (wi4, 0);

  data_storage *storage = ctx4.find_reachable_var (t);
  gcc_assert (storage != nullptr);
  storage_address address (storage->get_ref (), 0);

  mv.set_address_at (address, HOST_BITS_PER_LONG);

  wide_int wi7 = wi::shwi (7, HOST_BITS_PER_INT);
  mv.set_cst_at (wi7, HOST_BITS_PER_LONG + HOST_BITS_PER_PTR);

  data_value mv2 (mixed);
  mv2.set (mv);

  ASSERT_EQ (mv2.classify (), VAL_MIXED);

  ASSERT_EQ (mv2.classify (0, HOST_BITS_PER_LONG), VAL_CONSTANT);
  wide_int wi_i1 = mv2.get_cst_at (0, HOST_BITS_PER_LONG);
  ASSERT_PRED1 (wi::fits_shwi_p, wi_i1);
  ASSERT_EQ (wi_i1.to_shwi (), 4);

  ASSERT_EQ (mv2.classify (HOST_BITS_PER_LONG, HOST_BITS_PER_PTR), VAL_ADDRESS);
  storage_address *address2 = mv2.get_address_at (HOST_BITS_PER_LONG);
  gcc_assert (address2 != nullptr);
  data_storage &storage2 = address2->storage.get ();
  ASSERT_EQ (storage2.get_type (), STRG_VARIABLE);
  ASSERT_EQ (storage2.get_variable (), t);

  ASSERT_EQ (mv2.classify (HOST_BITS_PER_LONG + HOST_BITS_PER_PTR,
			   HOST_BITS_PER_INT),
	     VAL_CONSTANT);
  wide_int wi_i3 = mv2.get_cst_at (HOST_BITS_PER_LONG + HOST_BITS_PER_PTR,
				   HOST_BITS_PER_INT);
  ASSERT_PRED1 (wi::fits_shwi_p, wi_i3);
  ASSERT_EQ (wi_i3.to_shwi (), 7);


  tree range17 = build_range_type (integer_type_node,
				  build_int_cst (integer_type_node, 0),
				  build_int_cst (integer_type_node, 17));
  tree array_char_17 = build_array_type (char_type_node, range17);
  tree c17 = create_var (array_char_17, "c17");
  tree i5 = create_var (integer_type_node, "i5");

  vec<tree> decls5{};
  decls5.safe_push (c17);
  decls5.safe_push (i5);

  context_builder builder5 {};
  builder5.add_decls (&decls5);
  exec_context ctx5 = builder5.build (mem, printer);

  data_value z17 (array_char_17);

  wide_int wi0 = wi::shwi (0, 17 * CHAR_BIT);
  z17.set_cst_at (wi0, 0);

  data_storage *strg_c17 = ctx5.find_reachable_var (c17);
  gcc_assert (strg_c17 != nullptr);
  strg_c17->set (z17);

  data_storage *strg_i5 = ctx5.find_reachable_var (i5);
  gcc_assert (strg_i5 != nullptr);

  storage_address addr_i5(strg_i5->get_ref (), 0);
  data_value val_addr_i5 (ptr_type_node);
  val_addr_i5.set_address (addr_i5);
  strg_c17->set_at (val_addr_i5, HOST_BITS_PER_PTR);

  data_value val17_before = strg_c17->get_value ();

  ASSERT_EQ (val17_before.classify (), VAL_MIXED);
  ASSERT_EQ (val17_before.classify (0, HOST_BITS_PER_PTR), VAL_CONSTANT);
  ASSERT_EQ (val17_before.classify (HOST_BITS_PER_PTR, HOST_BITS_PER_PTR), VAL_ADDRESS);
  ASSERT_EQ (val17_before.classify (2 * HOST_BITS_PER_PTR, CHAR_BIT), VAL_CONSTANT);

  data_value undef (5 * CHAR_BIT + HOST_BITS_PER_PTR);
  strg_c17->set_at (undef, 3 * CHAR_BIT);

  data_value val17_after = strg_c17->get_value ();

  ASSERT_EQ (val17_after.classify (), VAL_MIXED);
  ASSERT_EQ (val17_after.classify (0, 3 * CHAR_BIT), VAL_CONSTANT);
  ASSERT_EQ (val17_after.classify (3 * CHAR_BIT, 5 * CHAR_BIT + HOST_BITS_PER_PTR),
	     VAL_UNDEFINED);
  ASSERT_EQ (val17_after.classify (2 * HOST_BITS_PER_PTR, CHAR_BIT),
	     VAL_CONSTANT);
}

void
data_value_print_tests ()
{
  heap_memory mem;
  context_printer printer;
  pretty_printer & pp = printer.pp;

  tree my_var = create_var (integer_type_node, "my_var");

  vec<tree> decls{};
  decls.safe_push (my_var);
  vec<tree> empty{};

  context_builder builder {};
  builder.add_decls (&decls);
  exec_context ctx = builder.build (mem, printer);

  data_value val1 (ptr_type_node);
  data_storage *storage = ctx.find_reachable_var (my_var);
  storage_address address (storage->get_ref (), 0);

  val1.set_address (address);

  printer.print (val1, ptr_type_node);
  ASSERT_STREQ (pp_formatted_text (&pp), "&my_var");


  context_printer printer2;
  pretty_printer & pp2 = printer2.pp;

  tree y = create_var (ptr_type_node, "y");
  tree my_lhs = create_var (ptr_type_node, "my_lhs");

  vec<tree> decls2{};
  decls2.safe_push (my_var);
  decls2.safe_push (y);
  decls2.safe_push (my_lhs);

  context_builder builder2 {};
  builder2.add_decls (&decls2);
  exec_context ctx2 = builder2.build (mem, printer2);

  tree vec2ptr = build_vector_type (ptr_type_node, 2);
  data_value val2 (vec2ptr);
  data_storage *strg_my_var = ctx2.find_reachable_var (my_var);
  storage_address addr_my_var (strg_my_var->get_ref (), 0);
  val2.set_address_at (addr_my_var, 0);
  data_storage *strg_x = ctx2.find_reachable_var (y);
  storage_address addr_x (strg_x->get_ref (), 0);
  val2.set_address_at (addr_x, HOST_BITS_PER_PTR);

  printer2.print (val2, vec2ptr);
  const char *str2 = pp_formatted_text (&pp2);
  ASSERT_STREQ (str2, "{ &my_var, &y }");

  context_printer printer3;
  pretty_printer & pp3 = printer3.pp;

  data_value val_int (integer_type_node);

  wide_int wi_val = wi::shwi (17, HOST_BITS_PER_INT);
  val_int.set_cst (wi_val);

  printer3.print (val_int, integer_type_node);

  ASSERT_STREQ (pp_formatted_text (&pp3), "17");


  heap_memory mem4;
  context_printer printer4;
  pretty_printer & pp4 = printer4.pp;

  exec_context ctx4 = context_builder ().build (mem4, printer4);

  data_storage & alloc1 = ctx4.allocate (12);
  storage_address address1 (alloc1.get_ref (), 0);

  data_value val_ptr (ptr_type_node);
  val_ptr.set_address (address1);

  printer4.print (val_ptr, ptr_type_node);

  ASSERT_STREQ (pp_formatted_text (&pp4), "&<alloc00(12)>");


  heap_memory mem5;
  context_printer printer5;
  pretty_printer & pp5 = printer5.pp;

  exec_context ctx5 = context_builder ().build (mem5, printer5);

  ctx5.allocate (12);
  data_storage & alloc2_ctx5 = ctx5.allocate (17);
  storage_address address2_ctx5 (alloc2_ctx5.get_ref (), 0);

  data_value val_ptr2 (ptr_type_node);
  val_ptr.set_address (address2_ctx5);

  printer5.print (val_ptr, ptr_type_node);

  ASSERT_STREQ (pp_formatted_text (&pp5), "&<alloc01(17)>");


  context_printer printer6;
  pretty_printer & pp6 = printer6.pp;

  data_value val6_259 (short_integer_type_node);
  wide_int cst259 = wi::shwi (259, HOST_BITS_PER_SHORT);
  val6_259.set_cst (cst259);

  printer6.print (val6_259, char_type_node);

  ASSERT_STREQ (pp_formatted_text (&pp6), "3");


  context_printer printer7;
  pretty_printer & pp7 = printer7.pp;

  data_value val7_259 (short_integer_type_node);
  val7_259.set_cst (cst259);

  printer7.print_at (val7_259, char_type_node, CHAR_BIT);

  ASSERT_STREQ (pp_formatted_text (&pp7), "1");


  heap_memory mem8;
  context_printer printer8;
  pretty_printer & pp8 = printer8.pp;

  exec_context ctx8 = context_builder ().build (mem8, printer8);
  data_storage & strg = ctx8.allocate (10);

  data_value v = strg.get_value ();
  wide_int cst41 = wi::shwi (41, CHAR_BIT);
  v.set_cst_at (cst41, HOST_BITS_PER_PTR);

  printer8.print_at (v, char_type_node, HOST_BITS_PER_PTR, CHAR_BIT);

  ASSERT_STREQ (pp_formatted_text (&pp8), "41");


  context_printer printer9;
  pretty_printer & pp9 = printer9.pp;

  tree real2 = build_real (float_type_node, dconst2);
  tree sint = make_signed_type (TYPE_PRECISION (float_type_node));
  tree int_r2 = fold_build1 (VIEW_CONVERT_EXPR, sint, real2);
  wide_int wi_r2 = wi::to_wide (int_r2);

  data_value v9 (float_type_node);
  v9.set_cst (wi_r2);

  printer9.print (v9, float_type_node);

  ASSERT_STREQ (pp_formatted_text (&pp9), "2.0e+0");


  context_printer printer10;
  pretty_printer & pp10 = printer10.pp;

  data_value v10 (integer_type_node);

  printer10.print (v10, integer_type_node);

  ASSERT_STREQ (pp_formatted_text (&pp10), "<undef>");
}


void
data_value_get_at_tests ()
{
  tree mixed1 = make_node (RECORD_TYPE);
  tree i3 = build_decl (input_location, FIELD_DECL,
			get_identifier ("i3"), integer_type_node);
  DECL_CONTEXT (i3) = mixed1;
  DECL_CHAIN (i3) = NULL_TREE;
  tree p2 = build_decl (input_location, FIELD_DECL,
			get_identifier ("p2"), ptr_type_node);
  DECL_CONTEXT (p2) = mixed1;
  DECL_CHAIN (p2) = i3;
  tree i1 = build_decl (input_location, FIELD_DECL,
			get_identifier ("i1"), long_integer_type_node);
  DECL_CONTEXT (i1) = mixed1;
  DECL_CHAIN (i1) = p2;
  TYPE_FIELDS (mixed1) = i1;
  layout_type (mixed1);

  heap_memory mem1;
  context_printer printer;

  tree m1 = create_var (mixed1, "m1");

  vec<tree> decls1{};
  decls1.safe_push (m1);

  context_builder builder1 {};
  builder1.add_decls (&decls1);
  exec_context ctx1 = builder1.build (mem1, printer);

  data_value val1 (mixed1);
  wide_int wi7 = wi::uhwi (7, HOST_BITS_PER_INT);
  val1.set_cst_at (wi7, tree_to_shwi (bit_position (i1)));

  data_storage * storage1 = ctx1.find_reachable_var (m1);
  storage_address address (storage1->get_ref (), 0);
  val1.set_address_at (address, tree_to_shwi (bit_position (p2)));

  data_value sub_val = val1.get_at (0, tree_to_shwi (bit_position (i3)));

  ASSERT_EQ (sub_val.classify (), VAL_MIXED);
  ASSERT_EQ (sub_val.classify (0, HOST_BITS_PER_INT), VAL_CONSTANT);
  ASSERT_EQ (sub_val.classify (HOST_BITS_PER_INT,
			       tree_to_shwi (bit_position (p2))
			       - HOST_BITS_PER_INT),
	     VAL_UNDEFINED);
  ASSERT_EQ (sub_val.classify (tree_to_shwi (bit_position (p2)),
			       HOST_BITS_PER_PTR),
	     VAL_ADDRESS);
}


void
data_storage_set_at_tests ()
{
  heap_memory mem;
  context_printer printer;

  tree a5i = build_array_type_nelts (integer_type_node, 5);
  tree v5i = create_var (a5i, "v5i");
  tree p = create_var (ptr_type_node, "p");

  vec<tree> decls{};
  decls.safe_push (p);
  decls.safe_push (v5i);
  vec<tree> empty{};

  context_builder builder {};
  builder.add_decls (&decls);
  exec_context ctx = builder.build (mem, printer);

  data_storage *storage_p = ctx.find_reachable_var (p);
  gcc_assert (storage_p != nullptr);
  data_storage *storage_v5 = ctx.find_reachable_var (v5i);
  gcc_assert (storage_v5 != nullptr);

  ASSERT_EQ (storage_p->get_value ().classify (), VAL_UNDEFINED);

  storage_address addr0 (storage_v5->get_ref (), 0);
  data_value val0 (ptr_type_node);
  val0.set_address (addr0);

  storage_p->set (val0);

  data_value valp0 = storage_p->get_value ();
  ASSERT_EQ (valp0.classify (), VAL_ADDRESS);
  storage_address *addrp0 = valp0.get_address ();
  ASSERT_EQ (&addrp0->storage.get (), storage_v5);
  ASSERT_EQ (addrp0->offset, 0);

  storage_address addr3 (storage_v5->get_ref (), 24);
  data_value val3 (ptr_type_node);
  val3.set_address (addr3);

  storage_p->set (val3);

  data_value valp3 = storage_p->get_value ();
  ASSERT_EQ (valp3.classify (), VAL_ADDRESS);
  storage_address *addrp3 = valp3.get_address ();
  ASSERT_EQ (&addrp3->storage.get (), storage_v5);
  ASSERT_EQ (addrp3->offset, 24);
}


void
context_printer_print_tests ()
{
  heap_memory mem1;

  context_printer printer1;
  pretty_printer & pp1 = printer1.pp;

  tree p1 = create_var (ptr_type_node, "p1");

  vec<tree> decls1 {};
  decls1.safe_push (p1);

  context_builder builder1;
  builder1.add_decls (&decls1);
  exec_context ctx1 = builder1.build (mem1, printer1);

  data_storage & alloc1 = ctx1.allocate (12);

  storage_address addr_alloc1 (alloc1.get_ref (), 0);
  data_value val_addr1 (ptr_type_node);
  val_addr1.set_address (addr_alloc1);

  data_storage *strg_p1 = ctx1.find_reachable_var (p1);
  gcc_assert (strg_p1 != nullptr);
  strg_p1->set (val_addr1);

  tree mem_alloc1 = build2 (MEM_REF, char_type_node,
			    p1, build_zero_cst (ptr_type_node));

  printer1.print (&ctx1, mem_alloc1);

  const char * str1 = pp_formatted_text (&pp1);
  ASSERT_STREQ (str1, "<alloc00(12)>[0B:+1B]");


  heap_memory mem2;

  context_printer printer2;
  pretty_printer & pp2 = printer2.pp;

  tree p2 = create_var (ptr_type_node, "p2");

  vec<tree> decls2 {};
  decls2.safe_push (p2);

  context_builder builder2;
  builder2.add_decls (&decls2);
  exec_context ctx2 = builder2.build (mem2, printer2);

  data_storage & alloc2 = ctx2.allocate (17);

  storage_address addr_alloc2 (alloc2.get_ref (), 24);
  data_value val_addr2 (ptr_type_node);
  val_addr2.set_address (addr_alloc2);

  data_storage *strg_p2 = ctx2.find_reachable_var (p2);
  gcc_assert (strg_p2 != nullptr);
  strg_p2->set (val_addr2);

  tree mem_alloc2 = build2 (MEM_REF, char_type_node,
			    p2, build_int_cst (ptr_type_node, 11));

  printer2.print (&ctx2, mem_alloc2);

  const char * str2 = pp_formatted_text (&pp2);
  ASSERT_STREQ (str2, "<alloc00(17)>[14B:+1B]");


  heap_memory mem3;

  context_printer printer3;
  pretty_printer & pp3 = printer3.pp;

  tree p3 = create_var (ptr_type_node, "p3");

  vec<tree> decls3 {};
  decls3.safe_push (p3);

  context_builder builder3;
  builder3.add_decls (&decls3);
  exec_context ctx3 = builder3.build (mem3, printer3);

  data_storage & alloc3 = ctx3.allocate (19);

  storage_address addr_alloc3 (alloc3.get_ref (), 40);
  data_value val_addr3 (ptr_type_node);
  val_addr3.set_address (addr_alloc3);

  data_storage *strg_p3 = ctx3.find_reachable_var (p3);
  gcc_assert (strg_p3 != nullptr);
  strg_p3->set (val_addr3);

  tree mem_alloc3 = build2 (MEM_REF, integer_type_node,
			    p3, build_int_cst (ptr_type_node, 3));

  printer3.print (&ctx3, mem_alloc3);

  const char * str3 = pp_formatted_text (&pp3);
  ASSERT_STREQ (str3, "<alloc00(19)>[8B:+4B]");
}


void
context_printer_print_first_data_ref_part_tests ()
{
  vec<tree> empty{};

  tree der2i = make_node (RECORD_TYPE);
  tree der2i_i2 = build_decl (input_location, FIELD_DECL,
			      get_identifier ("der2i_i2"), integer_type_node);
  DECL_CONTEXT (der2i_i2) = der2i;
  DECL_CHAIN (der2i_i2) = NULL_TREE;
  tree der2i_i1 = build_decl (input_location, FIELD_DECL,
			      get_identifier ("der2i_i1"), integer_type_node);
  DECL_CONTEXT (der2i_i1) = der2i;
  DECL_CHAIN (der2i_i1) = der2i_i2;
  TYPE_FIELDS (der2i) = der2i_i1;
  layout_type (der2i);

  tree var2i = create_var (der2i, "var2i");

  heap_memory mem1;
  context_printer printer1;
  pretty_printer & pp1 = printer1.pp;
  exec_context ctx1 = context_builder ().build (mem1, printer1);

  tree res1 = printer1.print_first_data_ref_part (ctx1, var2i, 0, nullptr);

  ASSERT_EQ (res1, integer_type_node);
  const char * str1 = pp_formatted_text (&pp1);
  ASSERT_STREQ (str1, "# var2i.der2i_i1");


  context_printer printer2;
  pretty_printer & pp2 = printer2.pp;

  vec<tree> decls2{};
  decls2.safe_push (var2i);

  context_builder builder2 {};
  builder2.add_decls (&decls2);
  exec_context ctx2 = builder2.build (mem1, printer2);

  tree mem_var2i = build2 (MEM_REF, der2i,
			   build1 (ADDR_EXPR, ptr_type_node, var2i),
			   build_zero_cst (ptr_type_node));

  tree res2 = printer2.print_first_data_ref_part (ctx2, mem_var2i, 0, nullptr);

  ASSERT_EQ (res2, integer_type_node);
  const char * str2 = pp_formatted_text (&pp2);
  ASSERT_STREQ (str2, "# var2i.der2i_i1");


  context_printer printer3;
  pretty_printer & pp3 = printer3.pp;

  context_builder builder3 {};
  builder3.add_decls (&decls2);
  exec_context ctx3 = builder3.build (mem1, printer3);

  tree long_var2i = build2 (MEM_REF, long_integer_type_node,
			   build1 (ADDR_EXPR, ptr_type_node, var2i),
			   build_zero_cst (ptr_type_node));

  tree res3 = printer3.print_first_data_ref_part (ctx3, long_var2i, 0, nullptr);

  ASSERT_EQ (res3, integer_type_node);
  const char * str3 = pp_formatted_text (&pp3);
  ASSERT_STREQ (str3, "# var2i.der2i_i1");


  tree der2s = make_node (RECORD_TYPE);
  tree der2s_s2 = build_decl (input_location, FIELD_DECL,
			      get_identifier ("der2s_s2"),
			      short_integer_type_node);
  DECL_CONTEXT (der2s_s2) = der2s;
  DECL_CHAIN (der2s_s2) = NULL_TREE;
  tree der2s_s1 = build_decl (input_location, FIELD_DECL,
			      get_identifier ("der2s_s1"),
			      short_integer_type_node);
  DECL_CONTEXT (der2s_s1) = der2s;
  DECL_CHAIN (der2s_s1) = der2s_s2;
  TYPE_FIELDS (der2s) = der2s_s1;
  layout_type (der2s);


  tree der1d1i = make_node (RECORD_TYPE);
  tree der1d1i_i2 = build_decl (input_location, FIELD_DECL,
				get_identifier ("der1d1i_i2"), integer_type_node);
  DECL_CONTEXT (der1d1i_i2) = der1d1i;
  DECL_CHAIN (der1d1i_i2) = NULL_TREE;
  tree der1d1i_d1 = build_decl (input_location, FIELD_DECL,
				get_identifier ("der1d1i_d1"), der2s);
  DECL_CONTEXT (der1d1i_d1) = der1d1i;
  DECL_CHAIN (der1d1i_d1) = der1d1i_i2;
  TYPE_FIELDS (der1d1i) = der1d1i_d1;
  layout_type (der1d1i);

  tree var1d1i = create_var (der1d1i, "var1d1i");

  context_printer printer4;
  pretty_printer & pp4 = printer4.pp;

  vec<tree> decls4{};
  decls4.safe_push (var1d1i);

  context_builder builder4 {};
  builder4.add_decls (&decls4);
  exec_context ctx4 = builder4.build (mem1, printer4);

  tree mem_var1d1i = build2 (MEM_REF, long_integer_type_node,
			     build1 (ADDR_EXPR, ptr_type_node, var1d1i),
			     build_zero_cst (ptr_type_node));

  tree res4 = printer4.print_first_data_ref_part (ctx4, mem_var1d1i, 0, nullptr);

  ASSERT_EQ (res4, short_integer_type_node);
  const char * str4 = pp_formatted_text (&pp4);
  ASSERT_STREQ (str4, "# var1d1i.der1d1i_d1.der2s_s1");


  context_printer printer5;
  pretty_printer & pp5 = printer5.pp;

  context_builder builder5 {};
  builder5.add_decls (&decls4);
  exec_context ctx5 = builder5.build (mem1, printer5);

  tree mem_var1d1i_s2 = build2 (MEM_REF, short_integer_type_node,
				build1 (ADDR_EXPR, ptr_type_node, var1d1i),
				build_int_cst (ptr_type_node,
					       sizeof (short)));

  tree res5 = printer5.print_first_data_ref_part (ctx5, mem_var1d1i_s2, 0, nullptr);

  ASSERT_EQ (res5, short_integer_type_node);
  const char * str5 = pp_formatted_text (&pp5);
  ASSERT_STREQ (str5, "# var1d1i.der1d1i_d1.der2s_s2");


  tree der4c = make_node (RECORD_TYPE);
  tree der4c_c4 = build_decl (input_location, FIELD_DECL,
			      get_identifier ("der4c_c4"),
			      char_type_node);
  DECL_CONTEXT (der4c_c4) = der4c;
  DECL_CHAIN (der4c_c4) = NULL_TREE;
  tree der4c_c3 = build_decl (input_location, FIELD_DECL,
			      get_identifier ("der4c_c3"),
			      char_type_node);
  DECL_CONTEXT (der4c_c3) = der4c;
  DECL_CHAIN (der4c_c3) = der4c_c4;
  tree der4c_c2 = build_decl (input_location, FIELD_DECL,
			      get_identifier ("der4c_c2"),
			      char_type_node);
  DECL_CONTEXT (der4c_c2) = der4c;
  DECL_CHAIN (der4c_c2) = der4c_c3;
  tree der4c_c1 = build_decl (input_location, FIELD_DECL,
			      get_identifier ("der4c_c1"),
			      char_type_node);
  DECL_CONTEXT (der4c_c1) = der4c;
  DECL_CHAIN (der4c_c1) = der4c_c2;
  TYPE_FIELDS (der4c) = der4c_c1;
  layout_type (der4c);

  tree var4c = create_var (der4c, "var4c");

  context_printer printer6;
  pretty_printer & pp6 = printer6.pp;

  vec<tree> decls6{};
  decls6.safe_push (var4c);

  context_builder builder6 {};
  builder6.add_decls (&decls6);
  exec_context ctx6 = builder6.build (mem1, printer6);

  tree mem_var4c = build2 (MEM_REF, long_integer_type_node,
			   build1 (ADDR_EXPR, ptr_type_node, var4c),
			   build_int_cst (ptr_type_node, 2));

  tree res6 = printer6.print_first_data_ref_part (ctx6, mem_var4c, 0, nullptr);

  ASSERT_EQ (res6, char_type_node);
  const char * str6 = pp_formatted_text (&pp6);
  ASSERT_STREQ (str6, "# var4c.der4c_c3");


  tree der1i1d = make_node (RECORD_TYPE);
  tree der1i1d_d2 = build_decl (input_location, FIELD_DECL,
				get_identifier ("der1i1d_d2"),
				der4c);
  DECL_CONTEXT (der1i1d_d2) = der1i1d;
  DECL_CHAIN (der1i1d_d2) = NULL_TREE;
  tree der1i1d_i1 = build_decl (input_location, FIELD_DECL,
				get_identifier ("der1i1d_i1"),
				integer_type_node);
  DECL_CONTEXT (der1i1d_i1) = der1i1d;
  DECL_CHAIN (der1i1d_i1) = der1i1d_d2;
  TYPE_FIELDS (der1i1d) = der1i1d_i1;
  layout_type (der1i1d);

  tree var1i1d = create_var (der1i1d, "var1i1d");

  context_printer printer7;
  pretty_printer & pp7 = printer7.pp;

  vec<tree> decls7{};
  decls7.safe_push (var1i1d);

  context_builder builder7 {};
  builder7.add_decls (&decls7);
  exec_context ctx7 = builder7.build (mem1, printer7);

  tree mem_var1i1d = build2 (MEM_REF, char_type_node,
			     build1 (ADDR_EXPR, ptr_type_node, var1i1d),
			     build_int_cst (ptr_type_node,
					    sizeof (int) + 1));

  tree res7 = printer7.print_first_data_ref_part (ctx7, mem_var1i1d, 0, nullptr);

  ASSERT_EQ (res7, char_type_node);
  const char * str7 = pp_formatted_text (&pp7);
  ASSERT_STREQ (str7, "# var1i1d.der1i1d_d2.der4c_c2");


  tree i5 = build_array_type_nelts (integer_type_node, 5);

  tree var_i5 = create_var (i5, "var_i5");

  context_printer printer8;
  pretty_printer & pp8 = printer8.pp;

  vec<tree> decls8{};
  decls8.safe_push (var_i5);

  context_builder builder8 {};
  builder8.add_decls (&decls8);
  exec_context ctx8 = builder8.build (mem1, printer8);

  tree mem_var_i5 = build2 (MEM_REF, char_type_node,
			    build1 (ADDR_EXPR, ptr_type_node, var_i5),
			    build_int_cst (ptr_type_node,
					   3 * sizeof (int)));

  tree res8 = printer8.print_first_data_ref_part (ctx8, mem_var_i5, 0, nullptr);

  ASSERT_EQ (res8, integer_type_node);
  const char * str8 = pp_formatted_text (&pp8);
  ASSERT_STREQ (str8, "# var_i5[3]");


  context_printer printer9;
  pretty_printer & pp9 = printer9.pp;

  context_builder builder9 {};
  builder9.add_decls (&decls8);
  exec_context ctx9 = builder9.build (mem1, printer9);

  tree mem2_var_i5 = build2 (MEM_REF, char_type_node,
			     build1 (ADDR_EXPR, ptr_type_node, var_i5),
			     build_int_cst (ptr_type_node,
					    sizeof (int)));

  tree res9 = printer9.print_first_data_ref_part (ctx9, mem2_var_i5,
						  HOST_BITS_PER_INT, nullptr);

  ASSERT_EQ (res9, integer_type_node);
  const char * str9 = pp_formatted_text (&pp9);
  ASSERT_STREQ (str9, "# var_i5[2]");


  tree a5c4 = build_array_type_nelts (der4c, 5);

  tree der1i1a5d = make_node (RECORD_TYPE);
  tree der1i1a5d_a5d2 = build_decl (input_location, FIELD_DECL,
				    get_identifier ("der1i1a5d_a5d2"),
				    a5c4);
  DECL_CONTEXT (der1i1a5d_a5d2) = der1i1a5d;
  DECL_CHAIN (der1i1a5d_a5d2) = NULL_TREE;
  tree der1i1a5d_i1 = build_decl (input_location, FIELD_DECL,
				  get_identifier ("der1i1a5d_i1"),
				  integer_type_node);
  DECL_CONTEXT (der1i1a5d_i1) = der1i1a5d;
  DECL_CHAIN (der1i1a5d_i1) = der1i1a5d_a5d2;
  TYPE_FIELDS (der1i1a5d) = der1i1a5d_i1;
  layout_type (der1i1a5d);

  tree var_d1i1a5d = create_var (der1i1a5d, "var_d1i1a5d");

  context_printer printer10;
  pretty_printer & pp10 = printer10.pp;

  vec<tree> decls10{};
  decls10.safe_push (var_d1i1a5d);

  context_builder builder10 {};
  builder10.add_decls (&decls10);
  exec_context ctx10 = builder10.build (mem1, printer10);

  tree mem_var_d1i1a5d = build2 (MEM_REF, char_type_node,
				 build1 (ADDR_EXPR, ptr_type_node, var_d1i1a5d),
				 build_int_cst (ptr_type_node,
						sizeof (int) + 13));

  tree res10 = printer10.print_first_data_ref_part (ctx10, mem_var_d1i1a5d, 0, nullptr);

  ASSERT_EQ (res10, char_type_node);
  const char * str10 = pp_formatted_text (&pp10);
  ASSERT_STREQ (str10, "# var_d1i1a5d.der1i1a5d_a5d2[3].der4c_c2");


  tree var_i = create_var (integer_type_node, "var_i");
  tree ptr = create_var (ptr_type_node, "ptr");

  context_printer printer11;
  pretty_printer & pp11 = printer11.pp;

  vec<tree> decls11{};
  decls11.safe_push (var_i);
  decls11.safe_push (ptr);

  context_builder builder11 {};
  builder11.add_decls (&decls11);
  exec_context ctx11 = builder11.build (mem1, printer11);

  data_storage *var_storage = ctx11.find_reachable_var (var_i);
  storage_address var_address (var_storage->get_ref (), 0);

  data_value ptr_val (ptr_type_node);
  ptr_val.set_address (var_address);

  data_storage *ptr_storage = ctx11.find_reachable_var (ptr);
  ptr_storage->set (ptr_val);

  tree ref_ptr = build2 (MEM_REF, integer_type_node, ptr,
			 build_zero_cst (ptr_type_node));

  tree res11 = printer11.print_first_data_ref_part (ctx11, ref_ptr, 0, nullptr);

  ASSERT_EQ (res11, integer_type_node);
  const char * str11 = pp_formatted_text (&pp11);
  ASSERT_STREQ (str11, "# var_i");
}


void
context_printer_print_value_update_tests ()
{
  heap_memory mem;
  context_printer printer;
  pretty_printer & pp = printer.pp;
  pp_buffer (&pp)->m_flush_p = false;

  tree my_var = create_var (ptr_type_node, "my_var");
  tree y = create_var (ptr_type_node, "y");
  tree my_lhs = create_var (ptr_type_node, "my_lhs");

  vec<tree> decls{};
  decls.safe_push (my_var);
  decls.safe_push (y);
  decls.safe_push (my_lhs);
  vec<tree> empty{};

  context_builder builder {};
  builder.add_decls (&decls);
  exec_context ctx = builder.build (mem, printer);

  data_value val1 (ptr_type_node);
  data_storage *storage = ctx.find_reachable_var (my_var);
  storage_address address (storage->get_ref (), 0);
  val1.set_address (address);

  printer.print_value_update (ctx, my_lhs, val1);
  const char *str = pp_formatted_text (&pp);
  ASSERT_STREQ (str, "# my_lhs = &my_var\n");


  context_printer printer2;
  pretty_printer & pp2 = printer2.pp;
  pp_buffer (&pp2)->m_flush_p = false;

  exec_context ctx2 = builder.build (mem, printer2);

  tree vec2ptr = build_vector_type (ptr_type_node, 2);
  data_value val2 (vec2ptr);
  data_storage *strg_my_var = ctx2.find_reachable_var (my_var);
  storage_address addr_my_var (strg_my_var->get_ref (), 0);
  val2.set_address_at (addr_my_var, 0);
  data_storage *strg_x = ctx2.find_reachable_var (y);
  storage_address addr_x (strg_x->get_ref (), 0);
  val2.set_address_at (addr_x, HOST_BITS_PER_PTR);

  tree vec_lhs = create_var (vec2ptr, "vec_lhs");

  printer2.print_value_update (ctx2, vec_lhs, val2);
  const char *str2 = pp_formatted_text (&pp2);
  ASSERT_STREQ (str2, "# vec_lhs = { &my_var, &y }\n");


  context_printer printer3;
  pretty_printer & pp3 = printer3.pp;
  pp_buffer (&pp3)->m_flush_p = false;

  tree der2c = make_node (RECORD_TYPE);
  tree der2c_c2 = build_decl (input_location, FIELD_DECL,
			      get_identifier ("der2c_c2"),
			      char_type_node);
  DECL_CONTEXT (der2c_c2) = der2c;
  DECL_CHAIN (der2c_c2) = NULL_TREE;
  tree der2c_c1 = build_decl (input_location, FIELD_DECL,
			      get_identifier ("der2c_c1"),
			      char_type_node);
  DECL_CONTEXT (der2c_c1) = der2c;
  DECL_CHAIN (der2c_c1) = der2c_c2;
  TYPE_FIELDS (der2c) = der2c_c1;
  layout_type (der2c);

  tree var2c = create_var (der2c, "var2c");

  vec<tree> decls3{};
  decls3.safe_push (var2c);

  context_builder builder3 {};
  builder3.add_decls (&decls3);
  exec_context ctx3 = builder3.build (mem, printer3);

  tree mem_var2c = build2 (MEM_REF, short_integer_type_node,
			   build1 (ADDR_EXPR, ptr_type_node, var2c),
			   build_int_cst (ptr_type_node, 0));

  data_value val259 (short_integer_type_node);
  wide_int wi259 = wi::shwi (259, HOST_BITS_PER_SHORT);
  val259.set_cst (wi259);

  printer3.print_value_update (ctx3, mem_var2c, val259);

  const char *str3 = pp_formatted_text (&pp3);
  ASSERT_STREQ (str3, "# var2c.der2c_c1 = 3\n# var2c.der2c_c2 = 1\n");


  context_printer printer4;
  pretty_printer & pp4 = printer4.pp;
  pp_buffer (&pp4)->m_flush_p = false;

  tree der2i = make_node (RECORD_TYPE);
  tree der2i_i2 = build_decl (input_location, FIELD_DECL,
			      get_identifier ("der2i_i2"), integer_type_node);
  DECL_CONTEXT (der2i_i2) = der2i;
  DECL_CHAIN (der2i_i2) = NULL_TREE;
  tree der2i_i1 = build_decl (input_location, FIELD_DECL,
			      get_identifier ("der2i_i1"), integer_type_node);
  DECL_CONTEXT (der2i_i1) = der2i;
  DECL_CHAIN (der2i_i1) = der2i_i2;
  TYPE_FIELDS (der2i) = der2i_i1;
  layout_type (der2i);

  tree v2i = create_var (der2i, "v2i");

  vec<tree> decls4{};
  decls4.safe_push (v2i);

  context_builder builder4 {};
  builder4.add_decls (&decls4);
  exec_context ctx4 = builder4.build (mem, printer4);

  tree vec2i = build_vector_type (integer_type_node, 2);

  tree mem_v2i = build2 (MEM_REF, vec2i,
			 build1 (ADDR_EXPR, ptr_type_node, v2i),
			 build_int_cst (ptr_type_node, 0));

  data_value val2i = data_value (vec2i);
  wide_int cst2 = wi::shwi (2, HOST_BITS_PER_INT);
  wide_int cst11 = wi::shwi (11, HOST_BITS_PER_INT);
  val2i.set_cst_at (cst2, 0);
  val2i.set_cst_at (cst11, HOST_BITS_PER_INT);

  printer4.print_value_update (ctx4, mem_v2i, val2i);

  const char *str4 = pp_formatted_text (&pp4);
  ASSERT_STREQ (str4, "# v2i.der2i_i1 = 2\n# v2i.der2i_i2 = 11\n");


  heap_memory mem5;
  context_printer printer5;
  pretty_printer & pp5 = printer5.pp;
  pp_buffer (&pp5)->m_flush_p = false;

  tree a5i = build_array_type_nelts (integer_type_node, 5);
  tree v5i = create_var (a5i, "v5i");
  tree p = create_var (ptr_type_node, "p");

  vec<tree> decls5{};
  decls5.safe_push (v5i);
  decls5.safe_push (p);

  context_builder builder5;
  builder5.add_decls (&decls5);
  exec_context ctx5 = builder5.build (mem5, printer5);

  data_storage *strg_v5i = ctx5.find_reachable_var (v5i);
  storage_address addr_v5i (strg_v5i->get_ref (), 24);
  data_value val5 (ptr_type_node);;
  val5.set_address (addr_v5i);

  printer5.print_value_update (ctx5, p, val5);
  const char *str5 = pp_formatted_text (&pp5);
  ASSERT_STREQ (str5, "# p = &v5i + 3B\n");


  context_printer printer6;
  pretty_printer & pp6 = printer6.pp;
  pp_buffer (&pp6)->m_flush_p = false;

  tree der2i_bis = make_node (RECORD_TYPE);
  tree der2i_i2_bis = build_decl (input_location, FIELD_DECL,
				  get_identifier ("der2i_i2_bis"),
				  integer_type_node);
  DECL_CONTEXT (der2i_i2_bis) = der2i_bis;
  DECL_CHAIN (der2i_i2_bis) = NULL_TREE;
  tree der2i_i1_bis = build_decl (input_location, FIELD_DECL,
				  get_identifier ("der2i_i1_bis"),
				  integer_type_node);
  DECL_CONTEXT (der2i_i1_bis) = der2i_bis;
  DECL_CHAIN (der2i_i1_bis) = der2i_i2_bis;
  TYPE_FIELDS (der2i_bis) = der2i_i1_bis;
  layout_type (der2i_bis);

  tree var2i = create_var (der2i_bis, "var2i");

  vec<tree> decls6{};
  decls6.safe_push (var2i);

  context_builder builder6;
  builder6.add_decls (&decls6);
  exec_context ctx6 = builder6.build (mem, printer6);

  wide_int wi12 = wi::shwi (12, HOST_BITS_PER_INT);
  wide_int wi7 = wi::shwi (7, HOST_BITS_PER_INT);
  data_value cstr_12_7 (der2i_bis);
  cstr_12_7.set_cst_at (wi12, 0);
  cstr_12_7.set_cst_at (wi7, HOST_BITS_PER_INT);

  printer6.print_value_update (ctx6, var2i, cstr_12_7);

  const char *str6 = pp_formatted_text (&pp6);
  ASSERT_STREQ (str6, "# var2i.der2i_i1_bis = 12\n# var2i.der2i_i2_bis = 7\n");


  context_printer printer7;
  pretty_printer & pp7 = printer7.pp;
  pp_buffer (&pp7)->m_flush_p = false;

  tree der1c1i = make_node (RECORD_TYPE);
  tree der1c1i_i2 = build_decl (input_location, FIELD_DECL,
				get_identifier ("der1c1i_i2"),
				integer_type_node);
  DECL_CONTEXT (der1c1i_i2) = der1c1i;
  DECL_CHAIN (der1c1i_i2) = NULL_TREE;
  tree der1c1i_c1 = build_decl (input_location, FIELD_DECL,
				get_identifier ("der1c1i_c1"),
				char_type_node);
  DECL_CONTEXT (der1c1i_c1) = der1c1i;
  DECL_CHAIN (der1c1i_c1) = der1c1i_i2;
  TYPE_FIELDS (der1c1i) = der1c1i_c1;
  layout_type (der1c1i);

  tree var1c1i = create_var (der1c1i, "var1c1i");

  vec<tree> decls7 {};
  decls7.safe_push (var1c1i);

  context_builder builder7;
  builder7.add_decls (&decls7);
  exec_context ctx7 = builder7.build (mem, printer7);

  wide_int wi3 = wi::shwi (3, CHAR_BIT);
  wide_int wi11 = wi::shwi (11, HOST_BITS_PER_INT);
  data_value cstr_3_11 (der1c1i);
  cstr_3_11.set_cst_at (wi3, 0);
  cstr_3_11.set_cst_at (wi11, int_bit_position (der1c1i_i2));

  printer7.print_value_update (ctx7, var1c1i, cstr_3_11);

  const char *str7 = pp_formatted_text (&pp7);
  ASSERT_STREQ (str7, "# var1c1i.der1c1i_c1 = 3\n# var1c1i.der1c1i_i2 = 11\n");
}


void
exec_context_evaluate_tests ()
{
  heap_memory mem;
  context_printer printer;

  tree a = create_var (integer_type_node, "a");
  tree b = create_var (integer_type_node, "b");

  vec<tree> decls{};
  decls.safe_push (a);
  decls.safe_push (b);
  vec<tree> empty{};

  context_builder builder {};
  builder.add_decls (&decls);
  exec_context ctx = builder.build (mem, printer);

  tree int_ptr = build_pointer_type (integer_type_node);
  tree var_addr = build1 (ADDR_EXPR,  int_ptr, a);

  data_value val = ctx.evaluate (var_addr);
  storage_address *ptr_addr = val.get_address ();
  ASSERT_NE (ptr_addr, nullptr);
  data_storage &strg_ptr = ptr_addr->storage.get ();
  ASSERT_PRED1 (strg_ptr.matches, a);

  exec_context ctx2 = context_builder ().build (ctx, printer);

  data_value val2 = ctx2.evaluate (var_addr);
  storage_address *ptr2_addr = val2.get_address ();
  ASSERT_NE (ptr2_addr, nullptr);
  data_storage &strg_ptr2 = ptr2_addr->storage.get ();
  ASSERT_PRED1 (strg_ptr2.matches, a);


  data_storage *strg_a = ctx.find_reachable_var (a);
  gcc_assert (strg_a != nullptr);
  data_value tmp22 (integer_type_node);
  wide_int wi22 = wi::shwi (22, HOST_BITS_PER_INT);
  tmp22.set_cst (wi22);
  strg_a->set (tmp22);

  data_value val_a = ctx.evaluate (a);

  ASSERT_EQ (val_a.classify (), VAL_CONSTANT);
  wide_int wi_a = val_a.get_cst ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi_a);
  ASSERT_EQ (wi_a.to_shwi (), 22);


  tree cst33 = build_int_cst (integer_type_node, 33);

  data_value val_33 = ctx.evaluate (cst33);

  ASSERT_EQ (val_33.get_bitwidth (), HOST_BITS_PER_INT);
  ASSERT_EQ (val_33.classify (), VAL_CONSTANT);
  wide_int wi33 = val_33.get_cst ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi33);
  ASSERT_EQ (wi33.to_shwi (), 33);


  vec<constructor_elt, va_gc> * vec_elts = nullptr;
  tree cst2 = build_int_cst (integer_type_node, 2);
  CONSTRUCTOR_APPEND_ELT (vec_elts, NULL_TREE, cst2);
  tree cst11 = build_int_cst (integer_type_node, 11);
  CONSTRUCTOR_APPEND_ELT (vec_elts, NULL_TREE, cst11);

  tree vec2int = build_vector_type (integer_type_node, 2);
  tree v = build_vector_from_ctor (vec2int, vec_elts);

  data_value val_v = ctx.evaluate (v);

  ASSERT_EQ (val_v.get_bitwidth (), 64);
  ASSERT_EQ (val_v.classify (), VAL_CONSTANT);
  wide_int low_part = val_v.get_cst_at (0, HOST_BITS_PER_INT);
  ASSERT_PRED1 (wi::fits_shwi_p, low_part);
  ASSERT_EQ (low_part.to_shwi (), 2);
  wide_int high_part = val_v.get_cst_at (HOST_BITS_PER_INT, HOST_BITS_PER_INT);
  ASSERT_PRED1 (wi::fits_shwi_p, high_part);
  ASSERT_EQ (high_part.to_shwi (), 11);


  vec<constructor_elt, va_gc> * vec_elts2 = nullptr;
  tree cstr2 = build_real (double_type_node, dconst2);
  CONSTRUCTOR_APPEND_ELT (vec_elts2, NULL_TREE, cstr2);
  tree cstm1 = build_real (double_type_node, dconstm1);
  CONSTRUCTOR_APPEND_ELT (vec_elts2, NULL_TREE, cstm1);

  tree vec2float = build_vector_type (float_type_node, 2);
  tree v2 = build_vector_from_ctor (vec2float, vec_elts2);

  data_value val_v2 = ctx.evaluate (v2);

  ASSERT_EQ (val_v2.get_bitwidth (), 64);
  ASSERT_EQ (val_v2.classify (), VAL_CONSTANT);

  tree sint = make_signed_type (TYPE_PRECISION (float_type_node));

#define HOST_BITS_PER_FLOAT (sizeof (float) * CHAR_BIT)
  wide_int low_wi = val_v2.get_cst_at (0, HOST_BITS_PER_FLOAT);
  tree low_int = wide_int_to_tree (sint, low_wi);
  tree low_float = fold_build1 (VIEW_CONVERT_EXPR, float_type_node, low_int);
  ASSERT_EQ (TREE_CODE (low_float), REAL_CST);
  ASSERT_TRUE (real_identical (TREE_REAL_CST_PTR (low_float), &dconst2));

  wide_int high_wi = val_v2.get_cst_at (HOST_BITS_PER_FLOAT, HOST_BITS_PER_FLOAT);
  tree high_int = wide_int_to_tree (sint, high_wi);
  tree high_float = fold_build1 (VIEW_CONVERT_EXPR, float_type_node, high_int);
  ASSERT_EQ (TREE_CODE (high_float), REAL_CST);
  ASSERT_TRUE (real_identical (TREE_REAL_CST_PTR (high_float), &dconstm1));
#undef HOST_BITS_PER_FLOAT


  tree a5i = build_array_type_nelts (integer_type_node, 5);
  tree v5i = create_var (a5i, "v5i");

  vec<tree> decls2{};
  decls2.safe_push (v5i);

  heap_memory mem3;
  context_builder builder3 {};
  builder3.add_decls (&decls2);
  exec_context ctx3 = builder3.build (mem3, printer);

  wide_int cst18 = wi::shwi (18, HOST_BITS_PER_INT);

  data_value value (a5i);
  value.set_cst_at (cst18, 3 * HOST_BITS_PER_INT);

  data_storage *storage = ctx3.find_reachable_var (v5i);
  gcc_assert (storage != nullptr);
  storage->set (value);

  tree v5ref = build4 (ARRAY_REF, integer_type_node, v5i,
		       build_int_cst (integer_type_node, 3),
		       NULL_TREE, NULL_TREE);

  data_value evaluation = ctx3.evaluate (v5ref);

  ASSERT_EQ (evaluation.get_bitwidth(), HOST_BITS_PER_INT);
  ASSERT_EQ (evaluation.classify (), VAL_CONSTANT);
  wide_int wi_val = evaluation.get_cst ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi_val);
  ASSERT_EQ (wi_val.to_shwi (), 18);


  tree derived = make_node (RECORD_TYPE);
  tree field2 = build_decl (input_location, FIELD_DECL,
			    get_identifier ("field2"), integer_type_node);
  DECL_CONTEXT (field2) = derived;
  DECL_CHAIN (field2) = NULL_TREE;
  tree field1 = build_decl (input_location, FIELD_DECL,
			    get_identifier ("field1"), integer_type_node);
  DECL_CONTEXT (field1) = derived;
  DECL_CHAIN (field1) = field2;
  TYPE_FIELDS (derived) = field1;
  layout_type (derived);

  tree a3d = build_array_type_nelts (derived, 3);
  tree v3d = create_var (a3d, "v3d");

  vec<tree> decls3{};
  decls3.safe_push (v3d);

  context_builder builder4 {};
  builder4.add_decls (&decls3);
  exec_context ctx4 = builder4.build (mem3, printer);

  wide_int cst15 = wi::shwi (15, HOST_BITS_PER_INT);

  data_value tmp (a3d);
  tmp.set_cst_at (cst15, 3 * HOST_BITS_PER_INT);

  data_storage *storage2 = ctx4.find_reachable_var (v3d);
  gcc_assert (storage2 != nullptr);
  storage2->set (tmp);

  tree v3aref = build4 (ARRAY_REF, derived, v3d,
		       build_int_cst (integer_type_node, 1),
		       NULL_TREE, NULL_TREE);
  tree v3cref = build3 (COMPONENT_REF, integer_type_node, v3aref,
			field2, NULL_TREE);

  data_value eval2 = ctx4.evaluate (v3cref);

  ASSERT_EQ (eval2.get_bitwidth(), HOST_BITS_PER_INT);
  ASSERT_EQ (eval2.classify (), VAL_CONSTANT);
  wide_int wi_val2 = eval2.get_cst ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi_val2);
  ASSERT_EQ (wi_val2.to_shwi (), 15);


  tree func_type = build_function_type (void_type_node, NULL_TREE);
  layout_type (func_type);

  tree func = build_decl (input_location, FUNCTION_DECL,
			  get_identifier ("func"),
			  func_type);
  tree result = build_decl (input_location, RESULT_DECL,
			    get_identifier ("result"), void_type_node);
  DECL_CONTEXT (result) = func;
  DECL_RESULT (func) = result;

  init_lowered_empty_function (func, true, profile_count::one ());

  tree def_var = create_var (integer_type_node, "def_var");
  DECL_CONTEXT (def_var) = func;
  tree ssa_var = make_ssa_name_fn (DECL_STRUCT_FUNCTION (func), def_var, nullptr);
  SSA_NAME_IS_DEFAULT_DEF (ssa_var) = 1;

  vec<tree> decls5{};
  decls5.safe_push (def_var);
  decls5.safe_push (ssa_var);

  context_builder builder5 {};
  builder5.add_decls (&decls5);
  exec_context ctx5 = builder5.build (mem3, printer);

  wide_int cst14 = wi::shwi (14, HOST_BITS_PER_INT);

  data_value tmp14 (integer_type_node);
  tmp14.set_cst (cst14);

  data_storage *storage5 = ctx5.find_reachable_var (def_var);
  gcc_assert (storage5 != nullptr);
  storage5->set (tmp14);

  data_value eval5 = ctx5.evaluate (ssa_var);

  ASSERT_EQ (eval5.get_bitwidth(), HOST_BITS_PER_INT);
  ASSERT_EQ (eval5.classify (), VAL_CONSTANT);
  wide_int wi_val5 = eval5.get_cst ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi_val5);
  ASSERT_EQ (wi_val5.to_shwi (), 14);


  tree a5c = build_array_type_nelts (char_type_node, 5);
  tree v5c = create_var (a5c,"v5c");

  vec<tree> decls6{};
  decls6.safe_push (v5c);

  context_builder builder6 {};
  builder6.add_decls (&decls6);
  exec_context ctx6 = builder6.build (mem3, printer);

  wide_int cst8 = wi::shwi (8, CHAR_BIT);

  data_value tmp8 (a5c);
  tmp8.set_cst_at (cst8, 3 * CHAR_BIT);

  data_storage *storage6 = ctx6.find_reachable_var (v5c);
  gcc_assert (storage5 != nullptr);
  storage6->set (tmp8);

  tree ref = build2 (MEM_REF, char_type_node,
		     build1 (ADDR_EXPR, ptr_type_node, v5c),
		     build_int_cst (ptr_type_node, 3));

  data_value eval6 = ctx6.evaluate (ref);

  ASSERT_EQ (eval6.get_bitwidth(), CHAR_BIT);
  ASSERT_EQ (eval6.classify (), VAL_CONSTANT);
  wide_int wi_val6 = eval6.get_cst ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi_val6);
  ASSERT_EQ (wi_val6.to_shwi (), 8);


  tree c29 = build_array_type_nelts (char_type_node, 29);
  tree a29 = create_var (c29, "a29");
  tree ssa8 = make_node (SSA_NAME);
  TREE_TYPE (ssa8) = size_type_node;
  tree ssa9 = make_node (SSA_NAME);
  TREE_TYPE (ssa9) = size_type_node;

  vec<tree> decls8{};
  decls8.safe_push (a29);
  decls8.safe_push (ssa8);
  decls8.safe_push (ssa9);

  context_builder builder8;
  builder8.add_decls (&decls8);
  exec_context ctx8 = builder8.build (mem, printer);

  data_storage * strg29 = ctx8.find_reachable_var (a29);
  gcc_assert (strg29 != nullptr);

  tree ref0 = build5 (TARGET_MEM_REF, char_type_node,
		     build1 (ADDR_EXPR, ptr_type_node, a29),
		     build_zero_cst (ptr_type_node),
		     size_zero_node, size_zero_node, NULL_TREE);

  wide_int cst21 = wi::shwi (21, CHAR_BIT);
  data_value tmp29_0 (CHAR_BIT);
  tmp29_0.set_cst (cst21);
  strg29->set_at (tmp29_0, 0);

  data_value val29_0 = ctx8.evaluate (ref0);

  ASSERT_EQ (val29_0.get_bitwidth (), CHAR_BIT);
  ASSERT_EQ (val29_0.classify (), VAL_CONSTANT);
  wide_int wi29_0 = val29_0.get_cst ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi29_0);
  ASSERT_EQ  (wi29_0.to_uhwi (), 21);

  tree ref3 = build5 (TARGET_MEM_REF, char_type_node,
		     build1 (ADDR_EXPR, ptr_type_node, a29),
		     build_int_cst (ptr_type_node, 3),
		     size_zero_node, size_zero_node, NULL_TREE);

  wide_int cst26 = wi::shwi (26, CHAR_BIT);
  data_value tmp29_3 (CHAR_BIT);
  tmp29_3.set_cst (cst26);
  strg29->set_at (tmp29_3, 24);

  data_value val29_3 = ctx8.evaluate (ref3);

  ASSERT_EQ (val29_3.get_bitwidth (), CHAR_BIT);
  ASSERT_EQ (val29_3.classify (), VAL_CONSTANT);
  wide_int wi29_3 = val29_3.get_cst ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi29_3);
  ASSERT_EQ  (wi29_3.to_uhwi (), 26);

  tree ref12 = build5 (TARGET_MEM_REF, char_type_node,
		       build1 (ADDR_EXPR, ptr_type_node, a29),
		       build_zero_cst (ptr_type_node),
		       ssa8, build_int_cst (size_type_node, 4), NULL_TREE);

  wide_int cst17 = wi::shwi (17, CHAR_BIT);
  data_value tmp29_12 (CHAR_BIT);
  tmp29_12.set_cst (cst17);
  strg29->set_at (tmp29_12, 96);

  wide_int cst3_bis = wi::shwi (3, sizeof (size_t) * CHAR_BIT);
  data_value val3_bis (sizeof (size_t) * CHAR_BIT);
  val3_bis.set_cst (cst3_bis);
  data_storage * ssa8_strg = ctx8.find_reachable_var (ssa8);
  gcc_assert (ssa8_strg != nullptr);
  ssa8_strg->set (val3_bis);

  data_value val29_12 = ctx8.evaluate (ref12);

  ASSERT_EQ (val29_12.get_bitwidth (), CHAR_BIT);
  ASSERT_EQ (val29_12.classify (), VAL_CONSTANT);
  wide_int wi29_12 = val29_12.get_cst ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi29_12);
  ASSERT_EQ  (wi29_12.to_uhwi (), 17);

  tree ref27 = build5 (TARGET_MEM_REF, char_type_node,
		       build1 (ADDR_EXPR, ptr_type_node, a29),
		       build_int_cst (ptr_type_node, 7),
		       ssa9, build_int_cst (size_type_node, 4), NULL_TREE);

  wide_int cst14_bis = wi::shwi (14, CHAR_BIT);
  data_value tmp29_27 (CHAR_BIT);
  tmp29_27.set_cst (cst14_bis);
  strg29->set_at (tmp29_27, 216);

  wide_int cst5 = wi::shwi (5, sizeof (size_t) * CHAR_BIT);
  data_value val5_bis (sizeof (size_t) * CHAR_BIT);
  val5_bis.set_cst (cst5);
  data_storage * ssa9_strg = ctx8.find_reachable_var (ssa9);
  gcc_assert (ssa9_strg != nullptr);
  ssa9_strg->set (val5_bis);

  data_value val29_27 = ctx8.evaluate (ref27);

  ASSERT_EQ (val29_27.get_bitwidth (), CHAR_BIT);
  ASSERT_EQ (val29_27.classify (), VAL_CONSTANT);
  wide_int wi29_27 = val29_27.get_cst ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi29_27);
  ASSERT_EQ  (wi29_27.to_uhwi (), 14);


  tree a9i = build_array_type_nelts (integer_type_node, 9);
  tree v9i = create_var (a9i, "v9i");
  tree p = create_var (ptr_type_node, "p");

  vec<tree> decls9 {};
  decls9.safe_push (v9i);
  decls9.safe_push (p);

  context_builder builder9;
  builder9.add_decls (&decls9);
  exec_context ctx9 = builder9.build (mem, printer);

  data_storage * strg9 = ctx9.find_reachable_var (v9i);
  gcc_assert (strg9 != nullptr);
  wide_int cst10 = wi::shwi (10, HOST_BITS_PER_INT);
  data_value val10 (integer_type_node);
  val10.set_cst (cst10);
  strg9->set_at (val10, HOST_BITS_PER_INT);

  data_storage * strg_p = ctx9.find_reachable_var (p);
  gcc_assert (strg_p != nullptr);
  storage_address addr_a9 (strg9->get_ref (), 0);
  data_value val_addr9 (ptr_type_node);
  val_addr9.set_address (addr_a9);
  strg_p->set (val_addr9);

  tree ref1 = build2 (MEM_REF, integer_type_node,
		      p, build_int_cst (ptr_type_node, sizeof (int)));

  data_value val1 = ctx9.evaluate (ref1);

  ASSERT_EQ (val1.classify (), VAL_CONSTANT);
  wide_int wi1 = val1.get_cst ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi1);
  ASSERT_EQ (wi1.to_uhwi (), 10);

  storage_address addr_a9p3 (strg9->get_ref (), 3 * HOST_BITS_PER_INT);
  data_value val_addr9p3 (ptr_type_node);
  val_addr9p3.set_address (addr_a9p3);
  strg_p->set (val_addr9p3);

  wide_int cst16 = wi::shwi (16, HOST_BITS_PER_INT);
  data_value val16 (integer_type_node);
  val16.set_cst (cst16);
  strg9->set_at (val16, 3 * HOST_BITS_PER_INT);

  tree ref3_bis = build2 (MEM_REF, integer_type_node,
			  p, build_zero_cst (ptr_type_node));

  data_value val3 = ctx9.evaluate (ref3_bis);

  ASSERT_EQ (val3.classify (), VAL_CONSTANT);
  wide_int wi3 = val3.get_cst ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi3);
  ASSERT_EQ (wi3.to_uhwi (), 16);

  wide_int cst19 = wi::shwi (19, HOST_BITS_PER_INT);
  data_value val19 (integer_type_node);
  val19.set_cst (cst19);
  strg9->set_at (val19, 5 * HOST_BITS_PER_INT);

  tree ref5 = build2 (MEM_REF, integer_type_node,
		      p, build_int_cst (ptr_type_node, 2 * sizeof (int)));

  data_value val5 = ctx9.evaluate (ref5);

  ASSERT_EQ (val5.classify (), VAL_CONSTANT);
  wide_int wi5 = val5.get_cst ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi5);
  ASSERT_EQ (wi5.to_uhwi (), 19);
}


void
exec_context_evaluate_literal_tests ()
{
  heap_memory mem;
  context_printer printer;

  vec<tree> empty{};
  vec<tree> empty2{};

  exec_context ctx = context_builder ().build (mem, printer);

  tree cst = build_int_cst (integer_type_node, 13);

  data_value val = ctx.evaluate (cst);
  ASSERT_EQ (val.classify (), VAL_CONSTANT);
  wide_int wi_value = val.get_cst ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi_value);
  int int_value = wi_value.to_shwi ();
  ASSERT_EQ (int_value, 13);

  heap_memory mem2;
  context_printer printer2;

  tree derived = make_node (RECORD_TYPE);
  tree field2 = build_decl (input_location, FIELD_DECL,
			    get_identifier ("field2"), integer_type_node);
  DECL_CONTEXT (field2) = derived;
  DECL_CHAIN (field2) = NULL_TREE;
  tree field1 = build_decl (input_location, FIELD_DECL,
			    get_identifier ("field1"), integer_type_node);
  DECL_CONTEXT (field1) = derived;
  DECL_CHAIN (field1) = field2;
  TYPE_FIELDS (derived) = field1;
  layout_type (derived);

  tree d = create_var (derived, "d");

  vec<tree> decls2{};
  decls2.safe_push (d);

  context_builder builder2 {};
  builder2.add_decls (&decls2);
  exec_context ctx2 = builder2.build (mem2, printer2);

  tree comp = fold_build3_loc (input_location, COMPONENT_REF,
			       integer_type_node, d, field2, NULL_TREE);
  tree int_ptr = build_pointer_type (integer_type_node);
  tree comp_addr = build1 (ADDR_EXPR, int_ptr, comp);

  data_value val2 = ctx2.evaluate (comp_addr);
  ASSERT_EQ (val2.classify (), VAL_ADDRESS);
  storage_address *ptr_addr2 = val2.get_address ();
  ASSERT_NE (ptr_addr2, nullptr);
  data_storage &strg_ptr2 = ptr_addr2->storage.get ();
  ASSERT_PRED1 (strg_ptr2.matches, d);
  ASSERT_EQ (ptr_addr2->offset, HOST_BITS_PER_INT);

  heap_memory mem3;
  context_printer printer3;

  tree derived3 = make_node (RECORD_TYPE);
  tree d3_field2 = build_decl (input_location, FIELD_DECL,
			       get_identifier ("field2"), integer_type_node);
  DECL_CONTEXT (d3_field2) = derived3;
  DECL_CHAIN (d3_field2) = NULL_TREE;
  tree d3_field1 = build_decl (input_location, FIELD_DECL,
			       get_identifier ("field1"), integer_type_node);
  DECL_CONTEXT (d3_field1) = derived3;
  DECL_CHAIN (d3_field1) = d3_field2;
  TYPE_FIELDS (derived3) = d3_field1;
  layout_type (derived3);

  tree d3 = create_var (derived3, "d3");
  tree ptr_d3 = build_pointer_type (derived3);
  tree p3 = create_var (ptr_d3, "p3");

  vec<tree> decls3{};
  decls3.safe_push (d3);
  decls3.safe_push (p3);

  context_builder builder3 {};
  builder3.add_decls (&decls3);
  exec_context ctx3 = builder3.build (mem3, printer3);

  data_storage *strg_d3 = ctx3.find_reachable_var (d3);
  gcc_assert (strg_d3 != nullptr);
  data_storage *strg_p3 = ctx3.find_reachable_var (p3);
  gcc_assert (strg_p3 != nullptr);
  storage_address addr_d3 (strg_d3->get_ref (), 0);
  data_value val_p3 (ptr_d3);
  val_p3.set_address (addr_d3);
  strg_p3->set (val_p3);

  tree deref_p3 = build_fold_indirect_ref_loc (input_location, p3);
  tree comp3 = fold_build3_loc (input_location, COMPONENT_REF,
			        integer_type_node, deref_p3, d3_field2,
				NULL_TREE);
  tree int_ptr3 = build_pointer_type (integer_type_node);
  tree comp_addr3 = build1 (ADDR_EXPR, int_ptr3, comp3);

  data_value val3 = ctx3.evaluate (comp_addr3);
  ASSERT_EQ (val3.classify (), VAL_ADDRESS);
  storage_address *ptr_addr3 = val3.get_address ();
  ASSERT_NE (ptr_addr3, nullptr);
  data_storage &strg_ptr3 = ptr_addr3->storage.get ();
  ASSERT_PRED1 (strg_ptr3.matches, d3);
  ASSERT_EQ (ptr_addr3->offset, HOST_BITS_PER_INT);

  heap_memory mem4;
  context_printer printer4;

  tree derived4 = make_node (RECORD_TYPE);
  tree d4_field2 = build_decl (input_location, FIELD_DECL,
			       get_identifier ("field2"), integer_type_node);
  DECL_CONTEXT (d4_field2) = derived4;
  DECL_CHAIN (d4_field2) = NULL_TREE;
  tree d4_field1 = build_decl (input_location, FIELD_DECL,
			       get_identifier ("field1"), integer_type_node);
  DECL_CONTEXT (d4_field1) = derived4;
  DECL_CHAIN (d4_field1) = d4_field2;
  TYPE_FIELDS (derived4) = d4_field1;
  layout_type (derived4);

  tree d4 = create_var (derived4, "d4");
  tree ptr_d4 = build_pointer_type (derived4);
  tree p4 = create_var (ptr_d4, "p4");

  vec<tree> decls4{};
  decls4.safe_push (d4);
  decls4.safe_push (p4);

  context_builder builder4 {};
  builder4.add_decls (&decls4);
  exec_context ctx4 = builder4.build (mem4, printer4);

  data_storage *strg_d4 = ctx4.find_reachable_var (d4);
  gcc_assert (strg_d4 != nullptr);
  data_storage *strg_p4 = ctx4.find_reachable_var (p4);
  gcc_assert (strg_p4 != nullptr);
  storage_address addr_d4 (strg_d4->get_ref (), 0);
  data_value val_p4 (ptr_d4);
  val_p4.set_address (addr_d4);
  strg_p4->set (val_p4);

  tree mem_p4 = fold_build2_loc (input_location, MEM_REF,
				 derived4, p4, build_int_cst (ptr_d4, 0));
  tree comp4 = fold_build3_loc (input_location, COMPONENT_REF,
			        integer_type_node, mem_p4, d4_field2,
				NULL_TREE);
  tree int_ptr4 = build_pointer_type (integer_type_node);
  tree comp_addr4 = build1 (ADDR_EXPR, int_ptr4, comp4);

  data_value val4 = ctx4.evaluate (comp_addr4);
  ASSERT_EQ (val4.classify (), VAL_ADDRESS);
  storage_address *ptr_addr4 = val4.get_address ();
  ASSERT_NE (ptr_addr4, nullptr);
  data_storage &strg_ptr4 = ptr_addr4->storage.get ();
  ASSERT_PRED1 (strg_ptr4.matches, d4);
  ASSERT_EQ (ptr_addr4->offset, HOST_BITS_PER_INT);
}

void
exec_context_evaluate_constructor_tests ()
{
  heap_memory mem;
  context_printer printer;

  tree a = create_var (integer_type_node, "a");
  tree b = create_var (integer_type_node, "b");

  vec<tree> decls{};
  decls.safe_push (a);
  decls.safe_push (b);
  vec<tree> empty{};

  context_builder builder {};
  builder.add_decls (&decls);
  exec_context ctx = builder.build (mem, printer);

  tree int_ptr = build_pointer_type (integer_type_node);

  tree vec2ptr = build_vector_type (ptr_type_node, 2);
  tree addr1 = build1 (ADDR_EXPR, int_ptr, a);
  tree addr2 = build1 (ADDR_EXPR, int_ptr, b);
  vec<constructor_elt, va_gc> * vec_elts = nullptr;
  CONSTRUCTOR_APPEND_ELT (vec_elts, NULL_TREE, addr1);
  CONSTRUCTOR_APPEND_ELT (vec_elts, NULL_TREE, addr2);
  tree cstr = build_constructor (vec2ptr, vec_elts);

  data_value val_cstr = ctx.evaluate (cstr);
  storage_address *addr1_bis = val_cstr.get_address_at (0);
  ASSERT_NE (addr1_bis, nullptr);
  data_storage &strg1 = addr1_bis->storage.get ();
  ASSERT_PRED1 (strg1.matches, a);
  storage_address *addr2_bis = val_cstr.get_address_at (HOST_BITS_PER_PTR);
  ASSERT_NE (addr2_bis, nullptr);
  data_storage &strg2 = addr2_bis->storage.get ();
  ASSERT_PRED1 (strg2.matches, b);


  tree a2i = build_array_type_nelts (integer_type_node, 2);

  exec_context ctx2 = context_builder ().build (mem, printer);

  tree cst3 = build_int_cst (integer_type_node, 3);
  tree cst7 = build_int_cst (integer_type_node, 7);
  vec<constructor_elt, va_gc> * vec_elts2 = nullptr;
  CONSTRUCTOR_APPEND_ELT (vec_elts2, NULL_TREE, cst3);
  CONSTRUCTOR_APPEND_ELT (vec_elts2, NULL_TREE, cst7);
  tree cstr2 = build_constructor (a2i, vec_elts2);

  data_value val_cstr2 = ctx2.evaluate (cstr2);

  ASSERT_EQ (val_cstr2.classify (), VAL_CONSTANT);
  wide_int wi_3 = val_cstr2.get_cst_at (0, HOST_BITS_PER_INT);
  ASSERT_PRED1 (wi::fits_uhwi_p, wi_3);
  ASSERT_EQ (wi_3.to_uhwi (), 3);
  wide_int wi_7 = val_cstr2.get_cst_at (HOST_BITS_PER_INT, HOST_BITS_PER_INT);
  ASSERT_PRED1 (wi::fits_uhwi_p, wi_7);
  ASSERT_EQ (wi_7.to_uhwi (), 7);

  vec_elts2 = nullptr;
  CONSTRUCTOR_APPEND_ELT (vec_elts2, integer_zero_node, cst3);
  CONSTRUCTOR_APPEND_ELT (vec_elts2, integer_one_node, cst7);
  cstr2 = build_constructor (a2i, vec_elts2);

  val_cstr2 = ctx2.evaluate (cstr2);

  ASSERT_EQ (val_cstr2.classify (), VAL_CONSTANT);
  wi_3 = val_cstr2.get_cst_at (0, HOST_BITS_PER_INT);
  ASSERT_PRED1 (wi::fits_uhwi_p, wi_3);
  ASSERT_EQ (wi_3.to_uhwi (), 3);
  wi_7 = val_cstr2.get_cst_at (HOST_BITS_PER_INT, HOST_BITS_PER_INT);
  ASSERT_PRED1 (wi::fits_uhwi_p, wi_7);
  ASSERT_EQ (wi_7.to_uhwi (), 7);


  tree derived = make_node (RECORD_TYPE);
  tree fi2 = build_decl (input_location, FIELD_DECL,
			 get_identifier ("fi2"), integer_type_node);
  DECL_CONTEXT (fi2) = derived;
  DECL_CHAIN (fi2) = NULL_TREE;
  tree fp1 = build_decl (input_location, FIELD_DECL,
			 get_identifier ("fp1"), ptr_type_node);
  DECL_CONTEXT (fp1) = derived;
  DECL_CHAIN (fp1) = fi2;
  TYPE_FIELDS (derived) = fp1;
  layout_type (derived);

  tree var3 = create_var (integer_type_node, "var3");

  vec<tree> decls3 {};
  decls3.safe_push (var3);

  context_builder builder3;
  builder3.add_decls (&decls3);
  exec_context ctx3 = builder3.build (mem, printer);

  data_storage *strg_var3 = ctx3.find_reachable_var (var3);
  gcc_assert (strg_var3 != nullptr);

  tree pvar3 = build1 (ADDR_EXPR, ptr_type_node, var3);
  tree cst2 = build_int_cst (integer_type_node, 2);
  vec<constructor_elt, va_gc> * vec_elts3 = nullptr;
  CONSTRUCTOR_APPEND_ELT (vec_elts3, fp1, pvar3);
  CONSTRUCTOR_APPEND_ELT (vec_elts3, fi2, cst2);
  tree cstr3 = build_constructor (derived, vec_elts3);

  data_value val_cstr3 = ctx3.evaluate (cstr3);

  ASSERT_EQ (val_cstr3.classify (), VAL_MIXED);

  ASSERT_EQ (val_cstr3.classify (0, HOST_BITS_PER_PTR), VAL_ADDRESS);
  storage_address *cstr3_addr0 = val_cstr3.get_address_at (0);
  ASSERT_NE (cstr3_addr0, nullptr);
  ASSERT_EQ (&cstr3_addr0->storage.get (), strg_var3);

  ASSERT_EQ (val_cstr3.classify (HOST_BITS_PER_PTR, HOST_BITS_PER_INT),
	     VAL_CONSTANT);
  wide_int wi_2 = val_cstr3.get_cst_at (HOST_BITS_PER_PTR, HOST_BITS_PER_INT);
  ASSERT_PRED1 (wi::fits_uhwi_p, wi_2);
  ASSERT_EQ (wi_2.to_uhwi (), 2);


  exec_context ctx4 = context_builder ().build (mem, printer);

  tree cstr4 = build_constructor (a2i, nullptr);

  data_value val_cstr4 = ctx4.evaluate (cstr4);

  ASSERT_EQ (val_cstr4.classify (), VAL_CONSTANT);
  wide_int wi_0 = val_cstr4.get_cst_at (0, HOST_BITS_PER_INT);
  ASSERT_PRED1 (wi::fits_uhwi_p, wi_0);
  ASSERT_EQ (wi_0.to_uhwi (), 0);
  wide_int wi_0_bis = val_cstr4.get_cst_at (HOST_BITS_PER_INT, HOST_BITS_PER_INT);
  ASSERT_PRED1 (wi::fits_uhwi_p, wi_0_bis);
  ASSERT_EQ (wi_0_bis.to_uhwi (), 0);
}


void
exec_context_evaluate_unary_tests ()
{
  heap_memory mem;
  context_printer printer;

  tree c1 = create_var (char_type_node, "c1");

  vec<tree> decls1 {};
  decls1.safe_push (c1);

  context_builder builder1;
  builder1.add_decls (&decls1);
  exec_context ctx1 = builder1.build (mem, printer);

  wide_int wi18 = wi::uhwi (18, CHAR_BIT);
  data_value val18 (char_type_node);
  val18.set_cst (wi18);
  data_storage *strg_c1 = ctx1.find_reachable_var (c1);
  gcc_assert (strg_c1 != nullptr);
  strg_c1->set (val18);
  
  data_value val1 = ctx1.evaluate_unary (NOP_EXPR, integer_type_node, c1);

  ASSERT_EQ (val1.get_bitwidth (), HOST_BITS_PER_INT);
  ASSERT_EQ (val1.classify (), VAL_CONSTANT);
  wide_int wi1 = val1.get_cst ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi1);
  ASSERT_EQ (wi1.to_uhwi (), 18);


  REAL_VALUE_TYPE f25 = REAL_VALUE_ATOF ("2.5", TYPE_MODE (float_type_node));
  tree t25 = build_real (float_type_node, f25);

  data_value valfm25 = ctx1.evaluate_unary (NEGATE_EXPR, float_type_node, t25);

  ASSERT_EQ (valfm25.get_bitwidth (), TYPE_PRECISION (float_type_node));
  ASSERT_EQ (valfm25.classify (), VAL_CONSTANT);
  tree tfm25 = valfm25.to_tree (float_type_node);
  ASSERT_EQ (TREE_CODE (tfm25), REAL_CST);
  REAL_VALUE_TYPE fm25 = REAL_VALUE_ATOF ("-2.5", TYPE_MODE (float_type_node));
  ASSERT_TRUE (real_equal (TREE_REAL_CST_PTR (tfm25), &fm25));
}


void
exec_context_evaluate_binary_tests ()
{
  heap_memory mem;
  context_printer printer;

  tree a = create_var (integer_type_node, "a");
  tree b = create_var (integer_type_node, "b");

  vec<tree> decls{};
  decls.safe_push (a);
  decls.safe_push (b);
  vec<tree> empty{};

  context_builder builder {};
  builder.add_decls (&decls);
  exec_context ctx = builder.build (mem, printer);

  wide_int cst12 = wi::shwi (12, HOST_BITS_PER_INT);
  data_value val12 (HOST_BITS_PER_INT);
  val12.set_cst (cst12);
  data_storage *strg_a = ctx.find_reachable_var (a);
  gcc_assert (strg_a != nullptr);
  strg_a->set (val12);

  wide_int cst7 = wi::shwi (7, HOST_BITS_PER_INT);
  data_value val7 (HOST_BITS_PER_INT);
  val7.set_cst (cst7);
  data_storage *strg_b = ctx.find_reachable_var (b);
  gcc_assert (strg_b != nullptr);
  strg_b->set (val7);

  data_value val = ctx.evaluate_binary (MINUS_EXPR, integer_type_node, a, b);

  ASSERT_EQ (val.classify (), VAL_CONSTANT);
  wide_int wi_val = val.get_cst ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi_val);
  ASSERT_EQ (wi_val.to_shwi (), 5);


  tree a_bis = create_var (integer_type_node, "a");
  tree b_bis = create_var (integer_type_node, "b");

  vec<tree> decls2{};
  decls2.safe_push (a_bis);
  decls2.safe_push (b_bis);

  context_builder builder2 {};
  builder2.add_decls (&decls2);
  exec_context ctx2 = builder2.build (mem, printer);

  data_value val12_bis (HOST_BITS_PER_INT);
  val12_bis.set_cst (wi::shwi (12, HOST_BITS_PER_INT));
  ctx2.find_reachable_var (a_bis)->set (val12_bis);

  data_value val7_bis (HOST_BITS_PER_INT);
  val7_bis.set_cst (wi::shwi (7, HOST_BITS_PER_INT));
  ctx2.find_reachable_var (b_bis)->set (val7_bis);

  data_value val2 = ctx2.evaluate_binary (GT_EXPR, boolean_type_node, a_bis, b_bis);

  ASSERT_EQ (val2.classify (), VAL_CONSTANT);
  ASSERT_EQ (val2.get_bitwidth (), 1);
  wide_int wi_val2 = val2.get_cst ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi_val2);
  ASSERT_EQ (wi_val2.to_uhwi (), 1);


  tree a12c = build_array_type_nelts (char_type_node, 12);
  tree v12c = create_var (a12c, "v12c");
  tree p = create_var (ptr_type_node, "p");
  tree i = create_var (pointer_sized_int_node, "i");
  tree o = create_var (pointer_sized_int_node, "o");

  vec<tree> decls3{};
  decls3.safe_push (v12c);
  decls3.safe_push (p);
  decls3.safe_push (i);
  decls3.safe_push (o);

  context_builder builder3 {};
  builder3.add_decls (&decls3);
  exec_context ctx3 = builder3.build (mem, printer);

  data_storage *v12_storage = ctx3.find_reachable_var (v12c);
  gcc_assert (v12_storage != nullptr);
  storage_address v12_address (v12_storage->get_ref (), 0);

  data_value pv12 (ptr_type_node);
  pv12.set_address (v12_address);

  data_storage *p_storage = ctx3.find_reachable_var (p);
  gcc_assert (p_storage != nullptr);
  p_storage->set (pv12);

  data_storage *i_storage = ctx3.find_reachable_var (i);
  gcc_assert (i_storage != nullptr);
  i_storage->set (pv12);

  tree addr_v12 = build1 (ADDR_EXPR, ptr_type_node, v12c);
  tree cst3 = build_int_cst (size_type_node, 3);

  data_value val_addr_p3 = ctx3.evaluate_binary (POINTER_PLUS_EXPR,
						 ptr_type_node,
						 addr_v12, cst3);

  ASSERT_EQ (val_addr_p3.classify (), VAL_ADDRESS);
  storage_address *addr_p3 = val_addr_p3.get_address ();
  ASSERT_NE (addr_p3, nullptr);
  ASSERT_EQ (&addr_p3->storage.get (), v12_storage);
  ASSERT_EQ (addr_p3->offset, 24);

  data_value val_ptr_p3 = ctx3.evaluate_binary (POINTER_PLUS_EXPR,
						ptr_type_node,
						p, cst3);

  ASSERT_EQ (val_ptr_p3.classify (), VAL_ADDRESS);
  storage_address *ptr_p3 = val_ptr_p3.get_address ();
  ASSERT_EQ (&ptr_p3->storage.get (), v12_storage);
  ASSERT_EQ (ptr_p3->offset, 24);

  tree ip7 = build_int_cst (pointer_sized_int_node, 7);
  data_value val_i_p7 = ctx3.evaluate_binary (PLUS_EXPR,
					      pointer_sized_int_node,
					      i, ip7);

  ASSERT_EQ (val_i_p7.classify (), VAL_ADDRESS);
  storage_address *i_p7 = val_i_p7.get_address ();
  ASSERT_EQ (&i_p7->storage.get (), v12_storage);
  ASSERT_EQ (i_p7->offset, 56);

  storage_address addr_v12_p5 (v12_address.storage, 40);
  data_value v12_p5 (ptr_type_node);
  v12_p5.set_address (addr_v12_p5);

  p_storage->set (v12_p5);
  i_storage->set (v12_p5);

  tree cst4 = build_int_cst (size_type_node, 4);
  data_value val_ptr_p9 = ctx3.evaluate_binary (POINTER_PLUS_EXPR,
						ptr_type_node,
						p, cst4);

  ASSERT_EQ (val_ptr_p9.classify (), VAL_ADDRESS);
  storage_address *ptr_p9 = val_ptr_p9.get_address ();
  ASSERT_EQ (&ptr_p9->storage.get (), v12_storage);
  ASSERT_EQ (ptr_p9->offset, 72);

  tree cst6 = build_int_cst (pointer_sized_int_node, 6);
  data_value val_i_p11 = ctx3.evaluate_binary (PLUS_EXPR,
					       pointer_sized_int_node,
					       i, cst6);

  ASSERT_EQ (val_i_p11.classify (), VAL_ADDRESS);
  storage_address *i_p11 = val_i_p11.get_address ();
  ASSERT_EQ (&i_p11->storage.get (), v12_storage);
  ASSERT_EQ (i_p11->offset, 88);

  wide_int wi1 = wi::shwi (1, HOST_BITS_PER_PTR);
  data_value cst1 (pointer_sized_int_node);
  cst1.set_cst (wi1);
  data_storage *o_storage = ctx3.find_reachable_var (o);
  gcc_assert (o_storage != nullptr);
  o_storage->set (cst1);

  data_value val_i_p6 = ctx3.evaluate_binary (PLUS_EXPR,
					      pointer_sized_int_node,
					      o, i);

  ASSERT_EQ (val_i_p6.classify (), VAL_ADDRESS);
  storage_address *i_p6 = val_i_p6.get_address ();
  ASSERT_EQ (&i_p6->storage.get (), v12_storage);
  ASSERT_EQ (i_p6->offset, 48);


  tree f25 = create_var (float_type_node, "f25");

  vec<tree> decls4{};
  decls4.safe_push (f25);

  context_builder builder4 {};
  builder4.add_decls (&decls4);
  exec_context ctx4 = builder4.build (mem, printer);

  machine_mode float_mode = TYPE_MODE (float_type_node);
  REAL_VALUE_TYPE val25 = REAL_VALUE_ATOF ("2.5", float_mode);
  tree c25 = build_real (float_type_node, val25);
  tree float_size = TYPE_SIZE (float_type_node);
  ASSERT_PRED1 (tree_fits_uhwi_p, float_size);
  tree itype = make_signed_type (tree_to_uhwi (float_size));
  tree i25 = fold_build1 (VIEW_CONVERT_EXPR, itype, c25);

  wide_int wi25 = wi::to_wide (i25);
  data_value data25 (float_type_node);
  data25.set_cst (wi25);

  data_storage *f25_storage = ctx4.find_reachable_var (f25);
  gcc_assert (f25_storage != nullptr);
  f25_storage->set (data25);

  REAL_VALUE_TYPE val325 = REAL_VALUE_ATOF ("3.25", float_mode);
  tree c325 = build_real (float_type_node, val325);

  data_value val_f = ctx4.evaluate_binary (PLUS_EXPR, float_type_node,
					   f25, c325);

  ASSERT_EQ (val_f.classify (), VAL_CONSTANT);
  wide_int wi_f = val_f.get_cst ();
  tree t_f = wide_int_to_tree (itype, wi_f);
  tree f = fold_build1 (VIEW_CONVERT_EXPR, float_type_node, t_f);
  ASSERT_EQ (TREE_CODE (f), REAL_CST);
  REAL_VALUE_TYPE val575 = REAL_VALUE_ATOF ("5.75", float_mode);
  ASSERT_TRUE (real_equal (TREE_REAL_CST_PTR (f), &val575));


  tree i5 = create_var (integer_type_node, "i5");
  tree p5 = create_var (build_pointer_type (integer_type_node), "p5");

  vec<tree> decls5{};
  decls5.safe_push (i5);
  decls5.safe_push (p5);

  context_builder builder5 {};
  builder5.add_decls (&decls5);
  exec_context ctx5 = builder5.build (mem, printer);

  data_storage *strg_i5 = ctx5.find_reachable_var (i5);
  gcc_assert (strg_i5 != nullptr);
  data_value addr_i5 (ptr_type_node);
  storage_address strg_addr_i5 (strg_i5->get_ref (), 0);
  addr_i5.set_address (strg_addr_i5);

  data_storage *strg_p5 = ctx5.find_reachable_var (p5);
  gcc_assert (strg_p5 != nullptr);
  strg_p5->set (addr_i5);

  data_value val_ne5 = ctx5.evaluate_binary (NE_EXPR, boolean_type_node,
					     p5,
					     build_int_cst (ptr_type_node, 0));

  ASSERT_EQ (val_ne5.classify (), VAL_CONSTANT);
  wide_int wi_ne5 = val_ne5.get_cst ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi_ne5);
  ASSERT_EQ (wi_ne5.to_uhwi (), 1);
}


void
exec_context_evaluate_condition_tests ()
{
  heap_memory mem1;
  context_printer printer1;

  tree a = create_var (integer_type_node, "a");
  tree b = create_var (integer_type_node, "b");

  vec<tree> decls1 {};
  decls1.safe_push (a);
  decls1.safe_push (b);
  vec<tree> empty{};

  context_builder builder1 {};
  builder1.add_decls (&decls1);
  exec_context ctx1 = builder1.build (mem1, printer1);

  wide_int cst12 = wi::shwi (12, HOST_BITS_PER_INT);
  data_value val12 (HOST_BITS_PER_INT);
  val12.set_cst (cst12);
  data_storage *strg_a = ctx1.find_reachable_var (a);
  gcc_assert (strg_a != nullptr);
  strg_a->set (val12);

  wide_int cst7 = wi::shwi (7, HOST_BITS_PER_INT);
  data_value val7 (HOST_BITS_PER_INT);
  val7.set_cst (cst7);
  data_storage *strg_b = ctx1.find_reachable_var (b);
  gcc_assert (strg_b != nullptr);
  strg_b->set (val7);

  gcond * cond1 = gimple_build_cond (GT_EXPR, a, b, NULL_TREE, NULL_TREE);

  ASSERT_TRUE (ctx1.evaluate_condition (cond1));


  heap_memory mem2;
  context_printer printer2;

  tree p1 = create_var (ptr_type_node, "p1");
  tree p2 = create_var (ptr_type_node, "p2");
  tree v = create_var (integer_type_node, "v");

  vec<tree> decls2 {};
  decls2.safe_push (p1);
  decls2.safe_push (p2);
  decls2.safe_push (v);

  context_builder builder2 {};
  builder2.add_decls (&decls2);
  exec_context ctx2 = builder2.build (mem2, printer2);

  wide_int wi_null = wi::shwi (0, HOST_BITS_PER_PTR);
  data_value val_null1 (ptr_type_node);
  val_null1.set_cst (wi_null);

  data_storage *strg_p1 = ctx2.find_reachable_var (p1);
  gcc_assert (strg_p1 != nullptr);
  strg_p1->set (val_null1);

  tree tree_null = build_int_cst (ptr_type_node, 0);

  gcond *cond2 = gimple_build_cond (EQ_EXPR, p1, tree_null, NULL_TREE, NULL_TREE);

  ASSERT_TRUE (ctx2.evaluate_condition (cond2));

  data_storage & alloc3 = ctx2.allocate (3);
  storage_address addr3 (alloc3.get_ref (), 0);
  data_value val_addr3 (ptr_type_node);
  val_addr3.set_address (addr3);
  data_storage *strg_p2 = ctx2.find_reachable_var (p2);
  gcc_assert (strg_p2 != nullptr);
  strg_p2->set (val_addr3);

  gcond *cond3 = gimple_build_cond (EQ_EXPR, p2, tree_null, NULL_TREE, NULL_TREE);

  ASSERT_FALSE (ctx2.evaluate_condition (cond3));

  gcond *cond4 = gimple_build_cond (EQ_EXPR, p1, p2, NULL_TREE, NULL_TREE);

  ASSERT_FALSE (ctx2.evaluate_condition (cond4));


  heap_memory mem3;
  context_printer printer3;

  tree x1 = create_var (integer_type_node, "x1");
  tree x2 = create_var (integer_type_node, "x2");
  tree px1 = create_var (ptr_type_node, "px1");
  tree px2 = create_var (ptr_type_node, "px2");

  vec<tree> decls3 {};
  decls3.safe_push (x1);
  decls3.safe_push (x2);
  decls3.safe_push (px1);
  decls3.safe_push (px2);

  context_builder builder3 {};
  builder3.add_decls (&decls3);
  exec_context ctx3 = builder3.build (mem3, printer3);

  data_storage *strg3_x1 = ctx3.find_reachable_var (x1);
  gcc_assert (strg3_x1 != nullptr);
  storage_address addr_x1 (strg3_x1->get_ref (), 0);
  data_value val_addr1 (HOST_BITS_PER_PTR);
  val_addr1.set_address (addr_x1);

  data_storage *strg3_px1 = ctx3.find_reachable_var (px1);
  gcc_assert (strg3_px1 != nullptr);
  strg3_px1->set (val_addr1);

  gcond *cond5 = gimple_build_cond (EQ_EXPR, px1, px1, NULL_TREE, NULL_TREE);

  ASSERT_TRUE (ctx3.evaluate_condition (cond5));

  gcond *cond6 = gimple_build_cond (NE_EXPR, px1, px1, NULL_TREE, NULL_TREE);

  ASSERT_FALSE (ctx3.evaluate_condition (cond6));

  data_storage *strg3_x2 = ctx3.find_reachable_var (x2);
  gcc_assert (strg3_x2 != nullptr);
  storage_address addr_x2 (strg3_x2->get_ref (), 0);
  data_value val_addr2 (HOST_BITS_PER_PTR);
  val_addr2.set_address (addr_x2);

  data_storage *strg3_px2 = ctx3.find_reachable_var (px2);
  gcc_assert (strg3_px2 != nullptr);
  strg3_px2->set (val_addr2);

  gcond *cond7 = gimple_build_cond (EQ_EXPR, px1, px2, NULL_TREE, NULL_TREE);

  ASSERT_FALSE (ctx3.evaluate_condition (cond7));

  gcond *cond8 = gimple_build_cond (NE_EXPR, px2, px1, NULL_TREE, NULL_TREE);

  ASSERT_TRUE (ctx3.evaluate_condition (cond8));


  heap_memory mem4;
  context_printer printer4;

  tree pm1 = create_var (ptr_type_node, "pm1");
  tree pm2 = create_var (ptr_type_node, "pm2");

  vec<tree> decls4 {};
  decls4.safe_push (pm1);
  decls4.safe_push (pm2);

  context_builder builder4 {};
  builder4.add_decls (&decls4);
  exec_context ctx4 = builder4.build (mem4, printer4);

  data_storage & m1 = ctx4.allocate (4);
  storage_address addr_m11 (m1.get_ref (), CHAR_BIT);
  data_value val_addr_m11 (HOST_BITS_PER_PTR);
  val_addr_m11.set_address (addr_m11);

  data_storage *strg4_pm1 = ctx4.find_reachable_var (pm1);
  gcc_assert (strg4_pm1 != nullptr);
  strg4_pm1->set (val_addr_m11);

  storage_address addr_m12 (m1.get_ref (), 2 * CHAR_BIT);
  data_value val_addr_m12 (HOST_BITS_PER_PTR);
  val_addr_m12.set_address (addr_m12);

  data_storage *strg4_pm2 = ctx4.find_reachable_var (pm2);
  gcc_assert (strg4_pm2 != nullptr);
  strg4_pm2->set (val_addr_m12);

  gcond *cond09 = gimple_build_cond (EQ_EXPR, pm1, pm2, NULL_TREE, NULL_TREE);

  ASSERT_FALSE (ctx4.evaluate_condition (cond09));

  gcond *cond10 = gimple_build_cond (NE_EXPR, pm2, pm1, NULL_TREE, NULL_TREE);

  ASSERT_TRUE (ctx4.evaluate_condition (cond10));

  gcond *cond11 = gimple_build_cond (EQ_EXPR, pm1, pm1, NULL_TREE, NULL_TREE);

  ASSERT_TRUE (ctx4.evaluate_condition (cond11));

  gcond *cond12 = gimple_build_cond (NE_EXPR, pm2, pm2, NULL_TREE, NULL_TREE);

  ASSERT_FALSE (ctx4.evaluate_condition (cond12));
}

void
exec_context_execute_assign_tests ()
{
  heap_memory mem;
  context_printer printer;

  tree a = create_var (integer_type_node, "a");

  tree derived = make_node (RECORD_TYPE);
  tree field2 = build_decl (input_location, FIELD_DECL,
			    get_identifier ("field2"), integer_type_node);
  DECL_CONTEXT (field2) = derived;
  DECL_CHAIN (field2) = NULL_TREE;
  tree field1 = build_decl (input_location, FIELD_DECL,
			    get_identifier ("field1"), integer_type_node);
  DECL_CONTEXT (field1) = derived;
  DECL_CHAIN (field1) = field2;
  TYPE_FIELDS (derived) = field1;
  layout_type (derived);

  tree b = create_var (derived, "b");

  vec<tree> decls{};
  decls.safe_push (a);
  decls.safe_push (b);
  vec<tree> empty{};

  context_builder builder {};
  builder.add_decls (&decls);
  exec_context ctx = builder.build (mem, printer);

  data_storage *storage_a = ctx.find_reachable_var (a);
  data_storage *storage_b = ctx.find_reachable_var (b);

  tree cst = build_int_cst (integer_type_node, 13);

  gimple *gassign1 = gimple_build_assign (a, cst);

  data_value val = storage_a->get_value ();
  ASSERT_EQ (val.classify (), VAL_UNDEFINED);

  ctx.execute (gassign1);

  data_value val2 = storage_a->get_value ();
  ASSERT_EQ (val2.classify (), VAL_CONSTANT);
  wide_int wi_val2 = val2.get_cst ();
  ASSERT_TRUE (wi::fits_shwi_p (wi_val2));
  ASSERT_EQ (wi_val2.to_shwi (), 13);


  tree lhs = build3 (COMPONENT_REF, integer_type_node, b, field1, NULL_TREE);

  gimple *gassign2 = gimple_build_assign (lhs, cst);

  data_value val3 = storage_b->get_value ();
  ASSERT_EQ (val3.classify (), VAL_UNDEFINED);

  ctx.execute (gassign2);

  data_value val4 = storage_b->get_value ();
  ASSERT_EQ (val4.classify (), VAL_MIXED);
  ASSERT_EQ (val4.classify (0, HOST_BITS_PER_INT), VAL_CONSTANT);
  wide_int wi_val4 = val4.get_cst_at (0, HOST_BITS_PER_INT);
  ASSERT_TRUE (wi::fits_shwi_p (wi_val4));
  ASSERT_EQ (wi_val4.to_shwi (), 13);

  tree lhs2 = build3 (COMPONENT_REF, integer_type_node, b, field2, NULL_TREE);
  tree cst2 = build_int_cst (integer_type_node, 17);
  gimple *gassign3 = gimple_build_assign (lhs2, cst2);

  data_value val5 = storage_b->get_value ();
  ASSERT_EQ (val5.classify (), VAL_MIXED);
  ASSERT_EQ (val5.classify (HOST_BITS_PER_INT, HOST_BITS_PER_INT), VAL_UNDEFINED);

  ctx.execute (gassign3);

  data_value val6 = storage_b->get_value ();
  ASSERT_EQ (val6.classify (), VAL_CONSTANT);
  wide_int wi_val6 = val6.get_cst_at (HOST_BITS_PER_INT, HOST_BITS_PER_INT);
  ASSERT_TRUE (wi::fits_shwi_p (wi_val4));
  ASSERT_EQ (wi_val6.to_shwi (), 17);


  tree ssa1 = make_node (SSA_NAME);
  TREE_TYPE (ssa1) = integer_type_node;

  vec<tree> ssanames{};
  ssanames.safe_push (ssa1);
  
  context_builder builder2 {};
  builder2.add_decls (&decls);
  builder2.add_decls (&ssanames);
  exec_context ctx2 = builder2.build (mem, printer);

  tree i66 = build_int_cst (integer_type_node, 66);
  gimple *gassign5 = gimple_build_assign (ssa1, i66);

  data_storage *ssa1_strg = ctx2.find_reachable_var (ssa1);
  gcc_assert (ssa1_strg != nullptr);
  data_value ssa1_val = ssa1_strg->get_value ();

  ASSERT_EQ (ssa1_val.classify (), VAL_UNDEFINED);

  ctx2.execute (gassign5);

  data_value ssa1_val2 = ssa1_strg->get_value ();
  ASSERT_EQ (ssa1_val2.classify (), VAL_CONSTANT);
  wide_int wi_ssa1_2 = ssa1_val2.get_cst ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi_ssa1_2);
  ASSERT_EQ (wi_ssa1_2.to_shwi (), 66);


  data_value cst66 (integer_type_node);
  wide_int wi66 = wi::shwi (66, HOST_BITS_PER_INT);
  cst66.set_cst (wi66);

  ssa1_strg->set (cst66);

  gimple *gassign4 = gimple_build_assign (a, ssa1);

  data_storage *strg_a_ctx2 = ctx2.find_reachable_var (a);
  gcc_assert (strg_a_ctx2 != nullptr);
  data_value val_a = strg_a_ctx2->get_value ();

  ASSERT_EQ (val_a.classify (), VAL_UNDEFINED);

  ctx2.execute (gassign4);

  data_value val_a2 = strg_a_ctx2->get_value ();
  ASSERT_EQ (val_a2.classify (), VAL_CONSTANT);
  wide_int wi_a2 = val_a2.get_cst ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi_a2);
  HOST_WIDE_INT hwi_a2 = wi_a2.to_shwi ();
  ASSERT_EQ (hwi_a2, 66);


  tree ptr = create_var (ptr_type_node, "ptr");

  vec<tree> decls2{};
  decls2.safe_push (ptr);

  context_builder builder3 {};
  builder3.add_decls (&decls2);
  exec_context ctx3 = builder3.build (mem, printer);

  data_storage & alloc1 = ctx3.allocate (12);
  storage_address address1 (alloc1.get_ref (), 0);

  data_storage *pointer = ctx3.find_reachable_var (ptr);
  gcc_assert (pointer != nullptr);
  data_value p (ptr_type_node);
  p.set_address (address1);
  pointer->set (p);
  
  tree ref = build2 (MEM_REF, integer_type_node, ptr,
		     build_int_cst (ptr_type_node, 0));
  tree cst3 = build_int_cst (integer_type_node, 123);

  gimple *assign = gimple_build_assign (ref, cst3);

  data_value val_alloc1 = alloc1.get_value ();
  ASSERT_EQ (val_alloc1.classify (), VAL_UNDEFINED);

  ctx3.execute (assign);

  data_value val_alloc2 = alloc1.get_value ();
  ASSERT_EQ (val_alloc2.classify (), VAL_MIXED);
  ASSERT_EQ (val_alloc2.classify (0, HOST_BITS_PER_INT), VAL_CONSTANT);
  wide_int wi_val_alloc2 = val_alloc2.get_cst_at (0, HOST_BITS_PER_INT);
  ASSERT_PRED1 (wi::fits_shwi_p, wi_val_alloc2);
  ASSERT_EQ (wi_val_alloc2.to_shwi (), 123);


  tree u = create_var (integer_type_node, "u");

  vec<tree> decls3{};
  decls3.safe_push (u);

  context_builder builder4 {};
  builder4.add_decls (&decls3);
  exec_context ctx4 = builder4.build (mem, printer);

  data_storage *strg_u = ctx4.find_reachable_var (u);
  gcc_assert (strg_u != nullptr);
  
  tree addr_u = build1 (ADDR_EXPR, ptr_type_node, u);
  tree ref_u = build2 (MEM_REF, integer_type_node, addr_u,
		     build_int_cst (ptr_type_node, 0));
  tree cst44 = build_int_cst (integer_type_node, 44);

  gimple *assign44 = gimple_build_assign (ref_u, cst44);

  data_value val_u = strg_u->get_value ();
  ASSERT_EQ (val_u.classify (), VAL_UNDEFINED);

  ctx4.execute (assign44);

  data_value val_u2 = strg_u->get_value ();
  ASSERT_EQ (val_u2.classify (), VAL_CONSTANT);
  wide_int wi_u2 = val_u2.get_cst ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi_u2);
  ASSERT_EQ (wi_u2.to_shwi (), 44);


  tree var6 = create_var (integer_type_node, "var6");
  tree var2i = create_var (derived, "var2i");

  vec<tree> decls5{};
  decls5.safe_push (var2i);
  decls5.safe_push (var6);

  context_builder builder5 {};
  builder5.add_decls (&decls5);
  exec_context ctx5 = builder5.build (mem, printer);

  wide_int wi8 = wi::shwi (8, HOST_BITS_PER_INT);
  wide_int wi13 = wi::shwi (13, HOST_BITS_PER_INT);

  data_value val7 (derived);
  val7.set_cst_at (wi8, 0);
  val7.set_cst_at (wi13, HOST_BITS_PER_INT);

  data_storage *strg = ctx5.find_reachable_var (var2i);
  gcc_assert (strg != nullptr);
  strg->set (val7);
  
  tree comp = build3 (COMPONENT_REF, integer_type_node, var2i, field2,
		      NULL_TREE);

  gimple *assign5 = gimple_build_assign (var6, comp);

  data_storage *strg5 = ctx5.find_reachable_var (var6);
  gcc_assert (strg5 != nullptr);
  data_value before = strg5->get_value ();
  ASSERT_EQ (before.classify (), VAL_UNDEFINED);

  ctx5.execute (assign5);

  gcc_assert (strg5 != nullptr);
  data_value after = strg5->get_value ();
  ASSERT_EQ (after.classify (), VAL_CONSTANT);
  wide_int wi7 = after.get_cst ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi7);
  ASSERT_EQ (wi7.to_shwi (), 13);


  tree a5c = build_array_type_nelts (char_type_node, 5);
  tree v5c = create_var (a5c,"v5c");

  vec<tree> decls6{};
  decls6.safe_push (v5c);

  context_builder builder6 {};
  builder6.add_decls (&decls6);
  exec_context ctx6 = builder6.build (mem, printer);

  tree c8 = build_int_cst (char_type_node, 8);

  tree ref2 = build2 (MEM_REF, char_type_node,
		     build1 (ADDR_EXPR, ptr_type_node, v5c),
		     build_int_cst (ptr_type_node, 3));

  gassign *assign2 = gimple_build_assign (ref2, c8);

  ctx6.execute (assign2);

  data_storage *storage = ctx6.find_reachable_var (v5c);
  gcc_assert (storage != nullptr);
  data_value c8val = storage->get_value ();

  ASSERT_EQ (c8val.classify(), VAL_MIXED);
  ASSERT_EQ (c8val.classify (3 * CHAR_BIT, CHAR_BIT), VAL_CONSTANT);
  wide_int wi_val = c8val.get_cst_at (3 * CHAR_BIT, CHAR_BIT);
  ASSERT_PRED1 (wi::fits_shwi_p, wi_val);
  ASSERT_EQ (wi_val.to_shwi (), 8);


  tree var_a = create_var (integer_type_node, "var_a");
  tree var_b = create_var (integer_type_node, "var_b");
  tree var_x = create_var (integer_type_node, "var_x");

  vec<tree> decls7{};
  decls7.safe_push (var_a);
  decls7.safe_push (var_b);
  decls7.safe_push (var_x);

  context_builder builder7 {};
  builder7.add_decls (&decls7);
  exec_context ctx7 = builder7.build (mem, printer);

  wide_int cst12 = wi::shwi (12, HOST_BITS_PER_INT);
  data_value val12 (HOST_BITS_PER_INT);
  val12.set_cst (cst12);
  data_storage *strg_a = ctx7.find_reachable_var (var_a);
  gcc_assert (strg_a != nullptr);
  strg_a->set (val12);

  wide_int cst7 = wi::shwi (7, HOST_BITS_PER_INT);
  data_value val7_bis (HOST_BITS_PER_INT);
  val7_bis.set_cst (cst7);
  data_storage *strg_b = ctx7.find_reachable_var (var_b);
  gcc_assert (strg_b != nullptr);
  strg_b->set (val7_bis);

  data_storage *strg_x = ctx7.find_reachable_var (var_x);
  gcc_assert (strg_x != nullptr);

  gassign *assign7 = gimple_build_assign (var_x, MINUS_EXPR, var_a, var_b);

  data_value x_before = strg_x->get_value ();
  ASSERT_EQ (x_before.classify (), VAL_UNDEFINED);

  ctx7.execute (assign7);

  data_value x_after = strg_x->get_value ();
  ASSERT_EQ (x_after.classify (), VAL_CONSTANT);
  wide_int x_val = x_after.get_cst ();
  ASSERT_PRED1 (wi::fits_shwi_p, x_val);
  ASSERT_EQ (x_val.to_shwi (), 5);


  tree c29 = build_array_type_nelts (char_type_node, 29);
  tree a29 = create_var (c29, "a29");
  tree ssa8 = make_node (SSA_NAME);
  TREE_TYPE (ssa8) = size_type_node;
  tree ssa9 = make_node (SSA_NAME);
  TREE_TYPE (ssa9) = size_type_node;

  vec<tree> decls8{};
  decls8.safe_push (a29);
  decls8.safe_push (ssa8);
  decls8.safe_push (ssa9);

  context_builder builder8;
  builder8.add_decls (&decls8);
  exec_context ctx8 = builder8.build (mem, printer);

  data_storage * strg29 = ctx8.find_reachable_var (a29);
  gcc_assert (strg29 != nullptr);

  tree ref0 = build5 (TARGET_MEM_REF, char_type_node,
		     build1 (ADDR_EXPR, ptr_type_node, a29),
		     build_zero_cst (ptr_type_node),
		     size_zero_node, size_zero_node, NULL_TREE);
  tree cst21 = build_int_cst (char_type_node, 21);
  gassign * assign29_0 = gimple_build_assign (ref0, cst21);

  data_value val29_before0 = strg29->get_value ();
  data_value val0_before = val29_before0.get_at (0, CHAR_BIT);
  ASSERT_EQ (val0_before.classify (), VAL_UNDEFINED);

  ctx8.execute (assign29_0);

  data_value val29_after0 = strg29->get_value ();
  data_value val0_after = val29_after0.get_at (0, CHAR_BIT);
  ASSERT_EQ (val0_after.classify (), VAL_CONSTANT);
  wide_int wi29_0 = val0_after.get_cst_at (0, CHAR_BIT);
  ASSERT_PRED1 (wi::fits_uhwi_p, wi29_0);
  ASSERT_EQ  (wi29_0.to_uhwi (), 21);

  tree ref3 = build5 (TARGET_MEM_REF, char_type_node,
		     build1 (ADDR_EXPR, ptr_type_node, a29),
		     build_int_cst (ptr_type_node, 3),
		     size_zero_node, size_zero_node, NULL_TREE);
  tree cst26 = build_int_cst (char_type_node, 26);
  gassign * assign29_3 = gimple_build_assign (ref3, cst26);

  data_value val29_before3 = strg29->get_value ();
  data_value val3_before = val29_before3.get_at (24, CHAR_BIT);
  ASSERT_EQ (val3_before.classify (), VAL_UNDEFINED);

  ctx8.execute (assign29_3);

  data_value val29_after3 = strg29->get_value ();
  data_value val3_after = val29_after3.get_at (24, CHAR_BIT);
  ASSERT_EQ (val3_after.classify (), VAL_CONSTANT);
  wide_int wi29_3 = val3_after.get_cst ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi29_3);
  ASSERT_EQ  (wi29_3.to_uhwi (), 26);

  tree ref12 = build5 (TARGET_MEM_REF, char_type_node,
		       build1 (ADDR_EXPR, ptr_type_node, a29),
		       build_zero_cst (ptr_type_node),
		       ssa8, build_int_cst (size_type_node, 4), NULL_TREE);
  tree cst17 = build_int_cst (char_type_node, 17);
  gassign * assign29_12 = gimple_build_assign (ref12, cst17);

  wide_int cst3_bis = wi::shwi (3, sizeof (size_t) * CHAR_BIT);
  data_value val3_bis (sizeof (size_t) * CHAR_BIT);
  val3_bis.set_cst (cst3_bis);
  data_storage * ssa8_strg = ctx8.find_reachable_var (ssa8);
  gcc_assert (ssa8_strg != nullptr);
  ssa8_strg->set (val3_bis);

  data_value val29_before12 = strg29->get_value ();
  data_value val12_before = val29_before12.get_at (96, CHAR_BIT);
  ASSERT_EQ (val12_before.classify (), VAL_UNDEFINED);

  ctx8.execute (assign29_12);

  data_value val29_after12 = strg29->get_value ();
  data_value val12_after = val29_after12.get_at (96, CHAR_BIT);
  ASSERT_EQ (val12_after.classify (), VAL_CONSTANT);
  wide_int wi29_12 = val12_after.get_cst ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi29_12);
  ASSERT_EQ  (wi29_12.to_uhwi (), 17);

  tree ref27 = build5 (TARGET_MEM_REF, char_type_node,
		       build1 (ADDR_EXPR, ptr_type_node, a29),
		       build_int_cst (ptr_type_node, 7),
		       ssa9, build_int_cst (size_type_node, 4), NULL_TREE);
  tree cst14 = build_int_cst (char_type_node, 14);
  gassign * assign29_27 = gimple_build_assign (ref27, cst14);

  wide_int cst5 = wi::shwi (5, sizeof (size_t) * CHAR_BIT);
  data_value val5_bis (sizeof (size_t) * CHAR_BIT);
  val5_bis.set_cst (cst5);
  data_storage * ssa9_strg = ctx8.find_reachable_var (ssa9);
  gcc_assert (ssa9_strg != nullptr);
  ssa9_strg->set (val5_bis);

  data_value val29_before27 = strg29->get_value ();
  data_value val27_before = val29_before27.get_at (216, CHAR_BIT);
  ASSERT_EQ (val27_before.classify (), VAL_UNDEFINED);

  ctx8.execute (assign29_27);

  data_value val29_after27 = strg29->get_value ();
  data_value val27_after = val29_after27.get_at (216, CHAR_BIT);
  ASSERT_EQ (val27_after.classify (), VAL_CONSTANT);
  wide_int wi29_27 = val27_after.get_cst ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi29_27);
  ASSERT_EQ  (wi29_27.to_uhwi (), 14);
}

void
exec_context_execute_call_tests ()
{
  heap_memory mem;
  context_printer printer;

  vec<tree> empty{};

  tree func_type = build_function_type (void_type_node, NULL_TREE);
  layout_type (func_type);

  exec_context ctx = context_builder ().build (mem, printer);

  tree set_args_fn = build_decl (input_location, FUNCTION_DECL,
				 get_identifier ("_gfortran_set_args"),
				 func_type);
  //DECL_EXTERNAL (set_args_fn) = 1;
  //TREE_PUBLIC (set_args_fn) = 1;

  gcall * set_args_call = gimple_build_call (set_args_fn, 0);

  ctx.execute (set_args_call);

  tree set_options_fn = build_decl (input_location, FUNCTION_DECL,
				    get_identifier ("_gfortran_set_options"),
				    func_type);
  //DECL_EXTERNAL (set_args) = 1;
  //TREE_PUBLIC (set_args) = 0;

  gcall * set_options_call = gimple_build_call (set_options_fn, 0);

  ctx.execute (set_options_call);

  tree p = create_var (ptr_type_node, "p");

  vec<tree> decls{};
  decls.safe_push (p);

  heap_memory mem2;
  context_builder builder2 {};
  builder2.add_decls (&decls);
  exec_context ctx2 = builder2.build (mem2, printer);

  tree malloc_fn = builtin_decl_explicit (BUILT_IN_MALLOC);
  tree cst = build_int_cst (size_type_node, 12);

  gcall * malloc_call = gimple_build_call (malloc_fn, 1, cst);
  gimple_set_lhs (malloc_call, p);

  ASSERT_EQ (ctx2.find_alloc (0), nullptr);

  ctx2.execute (malloc_call);

  data_storage *alloc_strg = ctx2.find_alloc (0);
  ASSERT_NE (alloc_strg, nullptr);
  data_value alloc_val = alloc_strg->get_value ();
  ASSERT_EQ (alloc_val.classify (), VAL_UNDEFINED);
  ASSERT_EQ (alloc_val.get_bitwidth (), 96);

  data_storage *p_strg = ctx2.find_var (p);
  ASSERT_NE (p_strg, nullptr);
  data_value p_val = p_strg->get_value ();
  ASSERT_EQ (p_val.classify (), VAL_ADDRESS);
  ASSERT_EQ (&p_val.get_address ()->storage.get (), alloc_strg);

  tree cst2 = build_int_cst (size_type_node, 10);

  gcall * malloc_call2 = gimple_build_call (malloc_fn, 1, cst2);
  gimple_set_lhs (malloc_call2, p);

  ASSERT_EQ (ctx2.find_alloc (1), nullptr);

  ctx2.execute (malloc_call2);

  data_storage *alloc_strg2 = ctx2.find_alloc (1);
  ASSERT_NE (alloc_strg2, nullptr);
  data_value alloc_val2 = alloc_strg2->get_value ();
  ASSERT_EQ (alloc_val2.classify (), VAL_UNDEFINED);
  ASSERT_EQ (alloc_val2.get_bitwidth (), 80);


  tree cst6 = build_int_cst (integer_type_node, 6);

  tree int_func_type = build_function_type (integer_type_node, NULL_TREE);
  layout_type (int_func_type);

  tree my_int_func = build_decl (input_location, FUNCTION_DECL,
				 get_identifier ("my_int_func"),
				 int_func_type);
  tree result = build_decl (input_location, RESULT_DECL,
			    get_identifier ("result"), integer_type_node);
  DECL_CONTEXT (result) = my_int_func;
  DECL_RESULT (my_int_func) = result;

  basic_block bb = init_lowered_empty_function (my_int_func, true, profile_count::one ());
  gimple_stmt_iterator gsi = gsi_last_bb (bb);
  greturn *ret_stmt = gimple_build_return (cst6);
  gsi_insert_after (&gsi, ret_stmt, GSI_CONTINUE_LINKING);

  tree ivar = create_var (integer_type_node, "ivar");

  gcall *my_call = gimple_build_call (my_int_func, 0);
  gimple_set_lhs (my_call, ivar);

  vec<tree> decls2{};
  decls2.safe_push (ivar);

  heap_memory mem3;
  context_builder builder3 {};
  builder3.add_decls (&decls2);
  exec_context ctx3 = builder3.build (mem3, printer);

  data_value ival = ctx3.evaluate (ivar);
  ASSERT_EQ (ival.classify (), VAL_UNDEFINED);

  ctx3.execute (my_call);

  data_value ival2 = ctx3.evaluate (ivar);
  ASSERT_EQ (ival2.classify (), VAL_CONSTANT);
  wide_int func_result = ival2.get_cst ();
  ASSERT_PRED1 (wi::fits_shwi_p, func_result);
  ASSERT_EQ (func_result.to_shwi (), 6);


  tree int_func_arg_type = build_function_type_list (integer_type_node,
						     integer_type_node,
						     NULL_TREE);
  layout_type (int_func_arg_type);

  tree my_int_func_with_arg = build_decl (input_location, FUNCTION_DECL,
					  get_identifier ("my_int_func_with_arg"),
					  int_func_type);

  tree fn_result = build_decl (input_location, RESULT_DECL,
			       get_identifier ("fn_result"), integer_type_node);
  DECL_CONTEXT (fn_result) = my_int_func_with_arg;
  DECL_RESULT (my_int_func_with_arg) = fn_result;

  tree arg = build_decl (input_location, PARM_DECL,
			 get_identifier ("arg"), integer_type_node);
  DECL_ARG_TYPE (arg) = TREE_VALUE (TYPE_ARG_TYPES (int_func_arg_type));
  DECL_CONTEXT (arg) = my_int_func_with_arg;
  DECL_ARGUMENTS (my_int_func_with_arg) = arg;
  layout_decl (arg, 0);

  basic_block bb2 = init_lowered_empty_function (my_int_func_with_arg, true, profile_count::one ());
  gimple_stmt_iterator gsi2 = gsi_last_bb (bb2);
  greturn *ret_stmt2 = gimple_build_return (arg);
  gsi_insert_after (&gsi2, ret_stmt2, GSI_CONTINUE_LINKING);

  tree i19 = build_int_cst (integer_type_node, 19);
  gcall *my_call2 = gimple_build_call (my_int_func_with_arg, 1, i19);

  tree ivar2 = create_var (integer_type_node, "ivar2");
  gimple_set_lhs (my_call2, ivar2);

  vec<tree> decls3{};
  decls3.safe_push (ivar2);

  heap_memory mem4;
  context_builder builder4 {};
  builder4.add_decls (&decls3);
  exec_context ctx4 = builder4.build (mem4, printer);

  data_value ival3 = ctx4.evaluate (ivar2);
  ASSERT_EQ (ival3.classify (), VAL_UNDEFINED);

  ctx4.execute (my_call2);

  data_value ival4 = ctx4.evaluate (ivar2);
  ASSERT_EQ (ival4.classify (), VAL_CONSTANT);
  wide_int func_result2 = ival4.get_cst ();
  ASSERT_PRED1 (wi::fits_shwi_p, func_result2);
  ASSERT_EQ (func_result2.to_shwi (), 19);


  tree i18 = create_var (size_type_node, "i18");

  vec<tree> decls5{};
  decls5.safe_push (p);
  decls5.safe_push (i18);

  heap_memory mem5;
  context_builder builder5 {};
  builder5.add_decls (&decls5);
  exec_context ctx5 = builder5.build (mem5, printer);

  wide_int cst18 = wi::shwi (18, HOST_BITS_PER_LONG);
  data_value val18 (size_type_node);
  val18.set_cst (cst18);
  data_storage *i18_strg = ctx5.find_reachable_var (i18);
  gcc_assert (i18_strg != nullptr);
  i18_strg->set (val18);

  gcall * malloc_call5 = gimple_build_call (malloc_fn, 1, i18);
  gimple_set_lhs (malloc_call5, p);

  ASSERT_EQ (ctx5.find_alloc (0), nullptr);

  ctx5.execute (malloc_call5);

  data_storage *alloc_strg5 = ctx5.find_alloc (0);
  ASSERT_NE (alloc_strg5, nullptr);
  data_value alloc_val5 = alloc_strg5->get_value ();
  ASSERT_EQ (alloc_val5.classify (), VAL_UNDEFINED);
  ASSERT_EQ (alloc_val5.get_bitwidth (), 144);

  data_storage *p_strg5 = ctx5.find_var (p);
  ASSERT_NE (p_strg5, nullptr);
  data_value p_val5 = p_strg5->get_value ();
  ASSERT_EQ (p_val5.classify (), VAL_ADDRESS);
  ASSERT_EQ (&p_val5.get_address ()->storage.get (), alloc_strg5);


  tree simple_type = build_function_type (void_type_node, NULL_TREE);
  layout_type (simple_type);

  exec_context ctx6 = context_builder ().build (mem, printer);

  tree simple_func = build_decl (input_location, FUNCTION_DECL,
				 get_identifier ("simple_func"),
				 simple_type);
  tree void_result = build_decl (input_location, RESULT_DECL,
				 get_identifier ("void_result"),
				 void_type_node);
  DECL_CONTEXT (void_result) = simple_func;
  DECL_RESULT (simple_func) = void_result;

  init_lowered_empty_function (simple_func, true, profile_count::one ());

  gcall * simple_call = gimple_build_call (simple_func, 0);

  ctx6.execute (simple_call);


  vec<tree> decls7{};
  decls7.safe_push (p);

  heap_memory mem7;
  context_builder builder7 {};
  builder7.add_decls (&decls7);
  exec_context ctx7 = builder7.build (mem7, printer);

  tree s6 = build_int_cst (size_type_node, 6);
  tree s4 = build_int_cst (size_type_node, 4);

  tree calloc_fn = builtin_decl_explicit (BUILT_IN_CALLOC);
  gcall * calloc_call7 = gimple_build_call (calloc_fn, 2, s6, s4);
  gimple_set_lhs (calloc_call7, p);

  ASSERT_EQ (ctx7.find_alloc (0), nullptr);

  ctx7.execute (calloc_call7);

  data_storage *p_strg7 = ctx7.find_reachable_var (p);
  ASSERT_NE (p_strg7, nullptr);
  data_value p_val7 = p_strg7->get_value ();
  ASSERT_EQ (p_val7.classify (), VAL_ADDRESS);
  storage_address *val_addr7 = p_val7.get_address ();

  data_storage *alloc_strg7 = ctx7.find_alloc (0);
  ASSERT_NE (alloc_strg7, nullptr);
  ASSERT_EQ (&val_addr7->storage.get (), alloc_strg7);
  ASSERT_EQ (val_addr7->offset, 0);

  data_value alloc_val7 = alloc_strg7->get_value ();
  ASSERT_EQ (alloc_val7.get_bitwidth (), 192);
  ASSERT_EQ (alloc_val7.classify (), VAL_CONSTANT);
  wide_int wi7 = alloc_val7.get_cst ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi7);
  ASSERT_EQ (wi7.to_shwi (), 0);


  tree v81 = create_var (integer_type_node, "v81");
  tree v82 = create_var (integer_type_node, "v82");

  vec<tree> decls8{};
  decls8.safe_push (v81);
  decls8.safe_push (v82);

  heap_memory mem8;
  context_builder builder8 {};
  builder8.add_decls (&decls8);
  exec_context ctx8 = builder8.build (mem8, printer);

  tree a81 = build1_loc (UNKNOWN_LOCATION, ADDR_EXPR,
			 ptr_type_node, v81);
  tree a82 = build1_loc (UNKNOWN_LOCATION, ADDR_EXPR,
			 ptr_type_node, v82);
  tree s84 = build_int_cst (size_type_node, HOST_BITS_PER_INT / CHAR_BIT);

  tree memcpy_fn = builtin_decl_explicit (BUILT_IN_MEMCPY);
  gcall * memcpy_call8 = gimple_build_call (memcpy_fn, 3, a82, a81, s84);

  data_value val17 (integer_type_node);
  wide_int wi17 = wi::uhwi (17, HOST_BITS_PER_INT);
  val17.set_cst (wi17);

  data_storage * storage_v81 = ctx8.find_reachable_var (v81);
  gcc_assert (storage_v81 != nullptr);
  storage_v81->set (val17);
  
  data_storage *storage_v82 = ctx8.find_reachable_var (v82);
  gcc_assert (storage_v82 != nullptr);

  data_value v82_before = storage_v82->get_value ();

  ASSERT_EQ (v82_before.classify (), VAL_UNDEFINED);

  ctx8.execute (memcpy_call8);

  data_value v82_after = storage_v82->get_value ();

  ASSERT_EQ (v82_after.classify (), VAL_CONSTANT);
  wide_int wi_v82 = v82_after.get_cst ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi_v82);
  ASSERT_EQ (wi_v82.to_shwi (), 17);
}

void
exec_context_allocate_tests ()
{
  heap_memory mem;
  context_printer printer;
  exec_context ctx1 = context_builder ().build (mem, printer);

  data_storage & storage1 = ctx1.allocate (12);

  ASSERT_EQ (storage1.get_type (), STRG_ALLOC);


  data_storage & storage2 = ctx1.allocate (7);
  data_value val2 = storage2.get_value ();

  ASSERT_EQ (val2.get_bitwidth (), 56);
  ASSERT_EQ (val2.classify (), VAL_UNDEFINED);


  heap_memory mem3;
  exec_context ctx3 = context_builder ().build (mem3, printer);

  ASSERT_EQ (ctx3.find_alloc (0), nullptr);

  data_storage & storage3 = ctx3.allocate (9);

  ASSERT_NE (ctx3.find_alloc (0), nullptr);
  ASSERT_EQ (ctx3.find_alloc (0), &storage3);
  ASSERT_EQ (ctx3.find_alloc (1), nullptr);


  exec_context ctx4 = context_builder ().build (ctx3, printer);

  ASSERT_NE (ctx3.find_alloc (0), nullptr);
  ASSERT_EQ (ctx4.find_alloc (0), &storage3);


  heap_memory mem5;
  exec_context ctx5 = context_builder ().build (mem5, printer);
  exec_context ctx6 = context_builder ().build (ctx5, printer);

  data_storage & storage5 = ctx5.allocate (16);

  ASSERT_NE (ctx5.find_alloc (0), nullptr);
  ASSERT_EQ (ctx5.find_alloc (0), &storage5);
  ASSERT_NE (ctx6.find_alloc (0), nullptr);
  ASSERT_EQ (ctx6.find_alloc (0), &storage5);


  heap_memory mem7;
  exec_context ctx7 = context_builder ().build (mem7, printer);
  exec_context ctx8 = context_builder ().build (ctx7, printer);

  data_storage & storage8 = ctx8.allocate (11);

  ASSERT_NE (ctx8.find_alloc (0), nullptr);
  ASSERT_EQ (ctx8.find_alloc (0), &storage8);
  ASSERT_NE (ctx7.find_alloc (0), nullptr);
  ASSERT_EQ (ctx7.find_alloc (0), &storage8);
}


void
gimple_exec_cc_tests ()
{
  get_constant_type_size_tests ();
  data_value_classify_tests ();
  exec_context_find_reachable_var_tests ();
  data_value_set_address_tests ();
  data_value_set_tests ();
  data_value_set_at_tests ();
  data_value_print_tests ();
  data_value_get_at_tests ();
  data_storage_set_at_tests ();
  context_printer_print_tests ();
  context_printer_print_first_data_ref_part_tests ();
  context_printer_print_value_update_tests ();
  exec_context_evaluate_tests ();
  exec_context_evaluate_literal_tests ();
  exec_context_evaluate_constructor_tests ();
  exec_context_evaluate_unary_tests ();
  exec_context_evaluate_binary_tests ();
  exec_context_evaluate_condition_tests ();
  exec_context_execute_assign_tests ();
  exec_context_execute_call_tests ();
  exec_context_allocate_tests ();
}

}

#endif
