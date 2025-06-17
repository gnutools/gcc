/* Copyright (C) 2025 Free Software Foundation, Inc.
   Contributed by Mikael Morin.

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


/* This implements a simulator of the gimple/SSA IR, that produces on the
   standard error a trace of the evolution of variables and memory allocated
   as statements are executed.
   This is targetted at debugging medium-sized testcases whose behaviour is
   not completely clear by simply starring at the IR.
   The simulator is called when the -fgimple-simulate argument is passed to
   the compiler.  It's supposed to be used with the IR produced by the gimple
   frontend.  */


#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "target.h"
#include "tree.h"
#include "tree-pretty-print.h"
#include "fold-const.h"
#include "stor-layout.h"
#include "tree-dfa.h"
#include "tree-cfg.h"
#include "gimple.h"
#include "gimple-pretty-print.h"
#include "gimple-iterator.h"
#include "gimple-ssa.h"
#include "cgraph.h"
#include "stringpool.h"
#include "value-range.h"
#include "tree-ssa-alias.h"
#include "tree-ssanames.h"


class simul_scope;
class data_storage;


static unsigned
get_constant_type_size (tree type)
{
  unsigned result;
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
  return result;
}


/* We store two kinds of values, known values and addresses.  Known values
   are sets of bits having a defined value.  Addresses reference some storage
   holding a value that can itself store known values or addresses.  Values
   that haven't been initialized to either a known value or an address have
   the undefined value.  Record types can have different fields that can store
   different kinds of values, they have a mixed value, unless all their fields
   have the same kind.  */

enum value_type
{
  /* An invalid value that doesn't represent any meaningful case.  */
  VAL_NONE,
  /* A value that hasn't been initialized.  */
  VAL_UNDEFINED,
  /* An address of a storage.  */
  VAL_ADDRESS,
  /* A known value.  */
  VAL_KNOWN,
  /* A non-uniform value.  There is some of it that is of a different kind
     from the rest of it.  */
  VAL_MIXED
};


/* There are two kinds of storage: variables and allocated.
   A variable storage represents a local or global variable.  It has a type
   from which the size is deduced.  An allocated storage represents a chunk
   of memory allocated on the heap.  It has an explicit size and no type.  */

enum storage_type
{
  STRG_VARIABLE,
  STRG_ALLOC
};

class heap_memory;

/* A wrapper class to avoid storing direct pointers to data_storage,
   to ease memory management.  */

struct storage_ref
{
  enum storage_type type;
  union u
    {
      u (const simul_scope & ctx) : var_context (&ctx) {}
      u (const heap_memory & m) : alloc_mem (&m) {}
      const simul_scope * var_context;
      const heap_memory * alloc_mem;
    }
  u;
  unsigned storage_index;

  storage_ref (const simul_scope & ctx, unsigned idx)
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
  /* Reference to the whole storage.  */
  storage_ref storage;
  /* Offset in bits into that storage.  */
  unsigned offset;

  storage_address (const storage_ref & ref, unsigned off)
    : storage (ref), offset (off)
  {}
};


/* An address value in the middle of a possibly bigger value.  */

struct stored_address
{
  /* Value of the address itself.  */
  storage_address address;
  /* Offset in bits from the begginning of the bigger value where the address
     is.  */
  HOST_WIDE_INT position;

  stored_address (storage_address addr, HOST_WIDE_INT pos)
    : address (addr), position (pos)
  {}
  void invalidate ();
};


/* As we overwrite values, we don't remove stored address structures to
   ease memory management.  Instead we just invalidate them by setting their
   position to -1.  */

void
stored_address::invalidate ()
{
  position = HOST_WIDE_INT_M1;
}


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
  void simul_scope_evaluate_tests ();
  void simul_scope_evaluate_literal_tests ();
  void simul_scope_evaluate_unary_tests ();
  void simul_scope_evaluate_binary_tests ();
  void simul_scope_simulate_assign_tests ();
  void simul_scope_print_call_tests ();
  void simul_scope_simulate_call_tests ();
  void simul_scope_allocate_tests ();
  void simul_scope_evaluate_condition_tests ();
}


/* Class representing a value.  A value is a set of bits, some of which may
   be used to store some known value, some of which may be used to store
   addresses.
   Bits storing a known value are represented with two wide_int fields.  One
   is a mask telling which bits store a known value, and the other tells that
   value for those bits.
   Bits storing an address are represented with one wide_int field and a vector
   of addresses.  The wide_int field is a mask like for the known value case.
   Individual bits can't store addresses, so the mask will be set by full
   chunks of HOST_BITS_PER_PTR bits.  The vector of addresses represents all the
   addresses that have ever been written at any position in the value.  One
   write to any position in the value will be represented by appending a new
   element to the address vector, and a second write at the same position will
   be represented by appending a different element and invalidating the first
   one without removing it from the vector.  */

class data_value
{
  unsigned bit_width;
  wide_int known_mask;
  wide_int address_mask;
  wide_int known_value;
  auto_vec<stored_address> addresses;
  void set_known_at (unsigned dest_offset, unsigned value_width,
		     const wide_int &val, unsigned src_offset);
  stored_address *find_address (HOST_WIDE_INT offset) const;

  friend void selftest::data_value_classify_tests ();
  friend void selftest::data_value_set_address_tests ();
  friend void selftest::data_value_set_tests ();
  friend void selftest::data_value_set_at_tests ();

public:
  data_value (unsigned width)
    : bit_width (width),
    known_mask (wi::shwi (HOST_WIDE_INT_0, width)),
    address_mask (wi::shwi (HOST_WIDE_INT_0, width)),
    known_value (wi::shwi (HOST_WIDE_INT_0, width)),
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
  void set_at (unsigned dest_offset, unsigned value_width,
	       const data_value & value_src, unsigned src_offset);
  void set_at (const data_value & value, unsigned offset);
  void set (const data_value & value);
  void set_known_at (const wide_int & val, unsigned offset);
  void set_known (const wide_int & val);
  wide_int get_known_at (unsigned offset, unsigned width) const;
  wide_int get_known () const;
  storage_address *get_address () const;
  storage_address *get_address_at (unsigned offset) const;
  data_value get_at (unsigned offset, unsigned width) const;
  bool is_fully_defined () const { return (~(known_mask | address_mask)) == 0; }
  tree to_tree (tree type) const;
};


class heap_memory;
class context_printer;


/* Class representing memory storage, either variable memory or heap-allocated
   memory.  */

class data_storage
{
  const storage_type type;
  data_value value;

  union u
    {
      u (const simul_scope & ctx, tree t) : variable (ctx, t) {}
      u (const heap_memory & mem, unsigned alloc_idx, unsigned alloc_amount)
	: allocated (mem, alloc_idx, alloc_amount)
      {}

      struct v
	{
	  v (const simul_scope & ctx, tree t) : context (ctx), decl (t) {}
	  const simul_scope & context;
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
  data_storage (const simul_scope & ctx, tree decl)
    : type (STRG_VARIABLE), value (TREE_TYPE (decl)),
    u (ctx, decl)
  {}
  data_storage (const heap_memory & mem, unsigned alloc_index,
		unsigned alloc_amount)
    : type (STRG_ALLOC), value (alloc_amount),
    u (mem, alloc_index, alloc_amount)
  {}
  storage_type get_type () const { return type; }
  tree get_variable () const;

  bool matches (tree var) const
  {
    return type == STRG_VARIABLE && u.variable.decl == var;
  }

  bool matches_alloc (unsigned index) const
  {
    return type == STRG_ALLOC && u.allocated.index == index;
  }

  const data_value & get_value () const { return value; }
  data_storage & operator= (const data_storage& other) = default;
  void set (const data_value & val) { return value.set (val); }

  void set_at (const data_value & val, unsigned offset)
  {
    return value.set_at (val, offset);
  }

  void print (context_printer & printer) const;
  storage_ref get_ref () const;
};


/* Higher lever wrapper around pretty_printer.  */

class context_printer
{
  pretty_printer pp;
  dump_flags_t flags;
  unsigned indent;

  friend void selftest::simul_scope_evaluate_tests ();
  friend void selftest::data_value_print_tests ();
  friend void selftest::context_printer_print_tests ();
  friend void selftest::context_printer_print_first_data_ref_part_tests ();
  friend void selftest::context_printer_print_value_update_tests ();
  friend void selftest::simul_scope_print_call_tests ();

public:
  context_printer ();
  context_printer (dump_flags_t f);

  pretty_printer & get_pretty_printer () const
  {
    return const_cast <pretty_printer &> (pp);
  }

  void begin_stmt (gimple *);
  void print (simul_scope *, tree);
  void print_newline ();
  void print_function_entry (struct function * func);
  void print_function_exit (struct function * func);
  void print_bb_jump (edge e);
  void print_bb_entry (basic_block bb);
  tree print_first_data_ref_part (simul_scope & context, tree data_ref,
				  unsigned offset, int * ignored_bits,
				  enum value_type);
  void print_ignored_stmt ();
  void print_value_update (simul_scope & context, tree, const data_value &);
  void end_stmt (gimple *);
  void print_at (const data_value & value, tree type, unsigned offset,
		 unsigned width);
  void print_at (const data_value & value, tree type, unsigned offset);
  void print (const data_value & value, tree type);
  void print_condition_value (bool value);
};


/* Hackish implementation of optional, missing in pre-c++17.  */

template <typename T>
class optional
{
  union u
    {
      u (T arg) : value (arg) {}
      u () : dummy (0) {}

      u (const u & other, bool present)
      {
	if (present)
	  new (&value) T (other.value);
	else
	  new (&dummy) char (0);
      }

      ~u () {} // TODO
      T value;
      char dummy;
    }
  u;
  bool present;

public:
  optional () : u (), present (false) {}
  optional (T arg) : u (arg), present (true) {}

  optional (const optional & other)
  : u (other.u, other.present), present (other.present)
  {}

  ~optional () {}  // TODO

  optional & operator= (const optional & other)
  {
    new (this) optional (other);
    return *this;
  }

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
simulate (struct function *func, simul_scope &caller,
	  context_printer & printer, vec<tree> * args);


/* Class representing the whole heap, in which dynamic memory is to be
   allocated.  */

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


/* Class representing a function scope, providing access to local variables.
   The parent context makes it possible to access to variables from parent
   caller functions, as well as global variables.  Every context has also
   a pointer to the global heap, providing access to heap-allocated memory.  */

class simul_scope
{
  simul_scope * const parent;
  context_printer & printer;
  heap_memory & alloc_mem;
  vec<data_storage> var_storages;
  data_value evaluate_constructor (tree cstr) const;
  data_value evaluate_unary (enum tree_code code, tree type, tree arg) const;
  data_value evaluate_binary (enum tree_code code, tree type, tree lhs,
			      tree rhs) const;
  bool evaluate_condition (gcond *cond) const;
  template <typename A, typename L>
  void add_variables (vec<tree, A, L> *variables, unsigned vars_count);
  template <typename A>
  void add_variables (vec<tree, A, vl_ptr> *variables);
  template <typename A>
  void add_variables (vec<tree, A, vl_embed> *variables);
  void simulate_assign (gassign *g);
  void simulate_call (gcall *g);
  data_storage *find_var (tree variable) const;
  data_storage *find_alloc (unsigned index) const;
  data_storage &allocate (unsigned amount);
  void decompose_ref (tree data_ref, data_storage * & storage,
		      int & offset) const;
  void simulate_phi (gphi *phi, edge e);
  simul_scope (simul_scope * caller, context_printer & printer,
		heap_memory & mem, vec<tree> & decls);

  friend void selftest::data_value_print_tests ();
  friend void selftest::data_value_set_address_tests ();
  friend void selftest::data_value_set_tests ();
  friend void selftest::context_printer_print_tests ();
  friend void selftest::simul_scope_evaluate_literal_tests ();
  friend void selftest::simul_scope_evaluate_unary_tests ();
  friend void selftest::simul_scope_evaluate_binary_tests ();
  friend void selftest::simul_scope_simulate_assign_tests ();
  friend void selftest::simul_scope_simulate_call_tests ();
  friend void selftest::simul_scope_allocate_tests ();
  friend void selftest::simul_scope_evaluate_condition_tests ();

public:
  simul_scope (simul_scope & caller, context_printer & printer,
		vec<tree> & decls);
  simul_scope (heap_memory & mem, context_printer & printer, vec<tree> & decls);
  const simul_scope & root () const;
  int find (const data_storage &storage) const;
  data_storage *find_reachable_var (tree variable) const;
  void simulate (gimple *g);
  gimple * simulate (basic_block bb);
  data_storage & get_storage (unsigned idx) const;
  context_printer & get_printer () const { return printer; }
  data_value evaluate (tree expr) const;
  optional <data_value> simulate_function (struct function *);
  edge select_leaving_edge (basic_block bb, gimple *last_stmt);
  void jump (edge e);
};


class context_builder
{
  vec<tree> decls;

public:
  context_builder ();
  simul_scope build (simul_scope & caller, context_printer & printer);
  simul_scope build (heap_memory & mem, context_printer & printer);
  template <typename A, typename L>
  void add_decls (vec<tree, A, L> *additional_decls);
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


simul_scope
context_builder::build (simul_scope & caller, context_printer & printer)
{
  return simul_scope (caller, printer, decls);
}


simul_scope
context_builder::build (heap_memory & mem, context_printer & printer)
{
  return simul_scope (mem, printer, decls);
}


simul_scope::simul_scope (simul_scope *caller, context_printer & printer,
			  heap_memory & mem, vec<tree> & decls)
  : parent (caller), printer (printer), alloc_mem (mem), var_storages ()
{
  add_variables (&decls);
}

simul_scope::simul_scope (simul_scope & caller, context_printer & printer,
			  vec<tree> & decls)
  : simul_scope::simul_scope (&caller, printer, caller.alloc_mem, decls)
{}

simul_scope::simul_scope (heap_memory & mem, context_printer & printer,
			  vec<tree> & decls)
  : simul_scope::simul_scope (nullptr, printer, mem, decls)
{}


template <typename A, typename L>
void
simul_scope::add_variables (vec<tree, A, L> *variables, unsigned vars_count)
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
simul_scope::add_variables (vec<tree, A, vl_ptr> *variables)
{
  add_variables (variables, variables->length ());
}

template <typename A>
void
simul_scope::add_variables (vec<tree, A, vl_embed> *variables)
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
  /* We want to flush output as soon as possible, to avoid the compiler ICEing
     before one has to chance to see what it is currently doing.  Thus, flush
     on every new line.  As the pretty_printer resets indentation on every
     flush, save it and restore it afterwards.  */
  int indent = pp_indentation (&pp);
  pp_newline_and_flush (&pp);
  pp_indentation (&pp) = indent;
}


/* Print the current statement being processed, before actually processing it.
 */
void
context_printer::begin_stmt (gimple *g)
{
  pp_indent (&pp);
  pp_gimple_stmt_1 (&pp, g, pp_indentation (&pp), TDF_NONE);
  print_newline ();
  pp_indentation (&pp) += 2;
}


/* Print the value of a condition.  */

void
context_printer::print_condition_value (bool value)
{
  pp_indent (&pp);
  pp_string (&pp, "# Condition evaluates to ");
  pp_string (&pp, value ? "true" : "false");
  print_newline ();
}

/* Print a tree.  For memory references, explicit the data storage they
   point to.  */

void
context_printer::print (simul_scope * ctx, tree expr)
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
	gcc_assert (val_off.classify () == VAL_KNOWN);
	wide_int wi_off = val_off.get_known ();
	wi_off = (wi_off * CHAR_BIT) + address->offset;

	if (!wi::fits_uhwi_p (wi_off))
	  gcc_unreachable ();
	unsigned offset_bits = wi_off.to_uhwi ();
	gcc_assert (offset_bits % CHAR_BIT == 0);
	unsigned offset_bytes = offset_bits / CHAR_BIT;

	unsigned size_bits = get_constant_type_size (TREE_TYPE (expr));
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


/* Annonce the execution of a function.  */

void
context_printer::print_function_entry (struct function *func)
{
  pp_indent (&pp);
  pp_string (&pp, "# Entering function ");
  pp_string (&pp, get_func_name (func));
  print_newline ();
  pp_indentation (&pp) += 2;
}


/* Signal a function return.  */

void
context_printer::print_function_exit (struct function *func)
{
  pp_indentation (&pp) -= 2;
  pp_indent (&pp);
  pp_string (&pp, "# Leaving function ");
  pp_string (&pp, get_func_name (func));
  print_newline ();
}


/* Annonce a jump from a of basic block to another.  */

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


/* Annonce the execution of a basic block.  */

void
context_printer::print_bb_entry (basic_block bb)
{
  pp_indent (&pp);
  pp_string (&pp, "# Executing bb ");
  pp_decimal_int (&pp, bb->index);
  print_newline ();
}


/* Find the smallest subreference of VAR_REF starting at at least OFFSET
   bit and of minimal size min_size.  If no subreference was found at
   OFFSET exactly but a reference was found at a further offset, store the
   difference of offset in IGNORED_BITS.  Return NULL_TREE if the smallest
   reference is smaller than MIN_SIZE.  Otherwise return the reference found.
*/
static tree
pick_subref_at (tree var_ref, unsigned offset, int * ignored_bits,
		unsigned min_size)
{
  tree ref = var_ref;
  unsigned remaining_offset = offset;
  while (true)
    {
      tree var_type = TREE_TYPE (ref);
      if (TREE_CODE (var_type) == ARRAY_TYPE)
	{
	  tree elt_type = TREE_TYPE (var_type);
	  unsigned elt_width = get_constant_type_size (elt_type);
	  if (elt_width < min_size)
	    return NULL_TREE;
	  unsigned HOST_WIDE_INT hw_idx = remaining_offset / elt_width;
	  tree t_idx = build_int_cst (integer_type_node, hw_idx);
	  ref = build4 (ARRAY_REF, elt_type, ref,
			t_idx, NULL_TREE, NULL_TREE);
	  remaining_offset -= hw_idx * elt_width;
	  if (elt_width == min_size)
	    break;
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
		  field_size = get_constant_type_size (TREE_TYPE (field));

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

	  tree field_type = TREE_TYPE (field);

	  unsigned field_width = get_constant_type_size (field_type);
	  if (field_width < min_size)
	    return NULL_TREE;

	  ref = build3 (COMPONENT_REF, field_type,
			ref, field, NULL_TREE);
	  if (field_position > remaining_offset)
	    {
	      gcc_assert (ignored_bits != nullptr);
	      *ignored_bits = field_position - remaining_offset;
	      remaining_offset = 0;
	    }
	  else
	    remaining_offset -= field_position;

	  if (field_width == min_size)
	    break;
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


/* Try to find a replacement data reference for an aggregate type MEM_REF,
   picking only the smallest leading part after OFFSET bits and of minimal
   size MIN_SIZE bits.  Return NULL_TREE if such a data reference was not found,
   otherwise return the reference found.  */

static tree
find_mem_ref_replacement (simul_scope & context, tree data_ref,
			  unsigned offset, unsigned min_size)
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
      HOST_WIDE_INT remaining_offset = shwi_offset * CHAR_BIT
				       + offset + ptr_address->offset;

      return pick_subref_at (var_ref, remaining_offset, nullptr, min_size);
    }
}


/* Print the first part of data reference DATA_REF starting after OFFSET bits,
   picking at least a value of minimal size HOST_BITS_PER_PTR if VAL_TYPE has
   value VAL_ADDRESS, otherwise picking a value of minimal size CHAR_BIT.  If
   no partial data reference was found, print DATA_REF itself unmodified.  The
   minimal size is made necessary because heap-allocated memory doesn't have
   any type that can guide the decomposition into smaller subreferences.  */

tree
context_printer::print_first_data_ref_part (simul_scope & context,
					    tree data_ref, unsigned offset,
					    int * ignored_bits,
					    enum value_type val_type)
{
  unsigned min_size;
  if (val_type == VAL_ADDRESS)
    min_size = HOST_BITS_PER_PTR;
  else
    min_size = CHAR_BIT;

  tree default_ref = NULL_TREE;
  switch (TREE_CODE (data_ref))
    {
    case MEM_REF:
      {
	tree mem_replacement = find_mem_ref_replacement (context, data_ref,
							 offset, min_size);
	if (mem_replacement != NULL_TREE)
	  return print_first_data_ref_part (context, mem_replacement, 0,
					    ignored_bits, val_type);

	unsigned orig_type_size;
	orig_type_size = get_constant_type_size (TREE_TYPE (data_ref));
	if (min_size < orig_type_size)
	  {
	    tree elt_type;
	    if (val_type == VAL_ADDRESS)
	      elt_type = ptr_type_node;
	    else
	      elt_type = char_type_node;

	    gcc_assert (offset % CHAR_BIT == 0);
	    tree ptr_type = build_pointer_type (elt_type);
	    default_ref = build2 (MEM_REF, elt_type,
				  TREE_OPERAND (data_ref, 0),
				  build_int_cst (ptr_type, offset / CHAR_BIT));
	  }
	else
	  gcc_assert (min_size == orig_type_size);
      }

    /* Fall through.  */

    default:
      tree ref = pick_subref_at (data_ref, offset, ignored_bits, min_size);
      if (ref == NULL_TREE)
	{
	  if (ignored_bits != nullptr && *ignored_bits > 0)
	    return NULL_TREE;

	  ref = default_ref;
	  if (ref == NULL_TREE)
	    ref = data_ref;
	}

      pp_indent (&pp);
      pp_character (&pp, '#');
      pp_space (&pp);
      print (&context, ref);
      return TREE_TYPE (ref);
    }
}


/* Inform that a gimple statement was ignored.  */

void
context_printer::print_ignored_stmt ()
{
  pp_indent (&pp);
  pp_string (&pp, "# ignored");
  print_newline ();
}


/* Inform the write of value VALUE in the data storage referred to by LHS.
   If LHS has an aggregate type, try to explode LHS and VALUE into smaller
   bits and represent the global write as a set of individual writes, one for
   each leaf field.  */

void
context_printer::print_value_update (simul_scope & context, tree lhs,
				     const data_value & value)
{
  unsigned previously_done = 0;
  unsigned width = get_constant_type_size (TREE_TYPE (lhs));
  while (previously_done < width)
    {
      enum value_type val_type = value.classify (previously_done, 1);
      int ignored_bits = 0;
      tree type_done = print_first_data_ref_part (context, lhs, previously_done,
						  &ignored_bits, val_type);
      if (type_done == NULL_TREE)
	{
	  gcc_assert (ignored_bits > 0);
	  previously_done += ignored_bits;
	  continue;
	}

      unsigned just_done = get_constant_type_size (type_done);
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


/* Dereference a storage reference.  Note that pointers to data_storage
   should not be kept for a long time.  Only storage_ref classes are stable.  */

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


/* Create a reference from data_storage.  */

storage_ref
data_storage::get_ref () const
{
  switch (type)
    {
    case STRG_VARIABLE:
      {
	const simul_scope & ctx = u.variable.context;
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
  known_mask (other.known_mask),
  address_mask (other.address_mask),
  known_value (other.known_value),
  addresses (other.addresses.copy ())
{}


data_value & data_value::operator= (const data_value & other)
{
  new (this) data_value (other);
  return *this;
}


/* Return the kind of value stored by THIS.  */

enum value_type
data_value::classify () const
{
  return classify (0, bit_width);
}


/* Return the kind of value stored by bits [offset, offset+width) of THIS.
 */
value_type
data_value::classify (unsigned offset, unsigned width) const
{
  wide_int mask = wi::shifted_mask (offset, width, false, bit_width);
  bool has_address = (address_mask & mask) != 0;
  bool has_known = (known_mask & mask) != 0;

  int has_count = has_address + has_known;
  if (has_count > 1)
    return VAL_MIXED;
  else if (has_count == 0)
    return VAL_UNDEFINED;
  else if (has_known && ((~known_mask) & mask) == 0)
    return VAL_KNOWN;
  else if (has_address && ((~address_mask) & mask) == 0)
    return VAL_ADDRESS;
  else
    return VAL_MIXED;
}


/* Return the stored address stored in bits starting at OFFSET of the data
   represented by THIS.  */

stored_address *
data_value::find_address (HOST_WIDE_INT offset) const
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


/* Store address ADDRESS at offset OFFSET of the data represented by THIS.  */

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
      existing_address->invalidate ();
    }

  known_mask &= ~mask;
  address_mask |= mask;

  stored_address new_address (address, offset);
  addresses.safe_push (new_address);
}


/* Write address ADDRESS in the data represented by THIS, assuming it is
   pointer-sized.  */

void
data_value::set_address (storage_address & address)
{
  gcc_assert (bit_width == HOST_BITS_PER_PTR);
  set_address_at (address, 0);
}


/* Overwrite bits [OFFSET, OFFSET+WIDTH) of data represented by THIS by an
   undefined value.  */

void
data_value::set_undefined_at (unsigned offset, unsigned width)
{
  wide_int dest_mask = wi::shifted_mask (offset, width, false,
					 bit_width);

  // TODO: invalidate existing address if any
  known_mask &= ~dest_mask;
  address_mask &= ~dest_mask;
}


/* Overwrite bits [DEST_OFFSET, DEST_OFFSET+VALUE_WIDTH) of data represented
   by THIS by the known value held in VALUE_SRC at bits
   [SRC_OFFSET, SRC_OFFSET+VALUE_WIDTH).  */

void
data_value::set_known_at (unsigned dest_offset, unsigned value_width,
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
  if (orig_type == VAL_ADDRESS)
    {
      stored_address *existing_address = find_address (dest_offset);
      if (existing_address)
	existing_address->invalidate ();
    }

  if (orig_type != VAL_KNOWN)
    {
      known_mask |= dest_mask;
      address_mask &= ~dest_mask;
    }

  wide_int src_mask = wi::shifted_mask (src_offset, value_width, false,
					src_width);
  wide_int value = value_src & src_mask;
  if (src_offset > 0)
    value = wi::lrshift (value, src_offset);

  wide_int dest_value = wide_int_storage::from (wide_int_ref (value),
						bit_width, UNSIGNED);
  if (dest_offset > 0)
    dest_value <<= dest_offset;

  known_value &= ~dest_mask;
  known_value |= dest_value;
}


/* Overwrite bits [DEST_OFFSET, DEST_OFFSET+VALUE_WIDTH) of data represented
   by THIS by the value held in VALUE_SRC at bits
   [SRC_OFFSET, SRC_OFFSET+VALUE_WIDTH).  */

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

    case VAL_KNOWN:
      set_known_at (dest_offset, value_width, value_src.known_value,
		    src_offset);
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
	       by one.  */
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
	gcc_assert ((known_mask & address_mask) == 0);

	wide_int known_part = value_src.known_mask;
	wide_int address_part = value_src.address_mask;

	unsigned src_width = value_src.bit_width;

	wide_int mask = wi::shifted_mask (src_offset, value_width, false,
					  value_src.bit_width);

	known_part &= mask;
	address_part &= mask;

	while (known_part != 0 || address_part != 0)
	  {
	    int ctz_known = wi::ctz (known_part);
	    int ctz_addr = wi::ctz (address_part);

	    int next_offset;
	    wide_int selected_part;
	    if (ctz_known < ctz_addr)
	      {
		next_offset = ctz_known;
		selected_part = known_part;
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

	    wide_int mask = wi::shifted_mask (next_offset, width, false,
					      src_width);

	    known_part &= ~mask;
	    address_part &= ~mask;
	  }

      }
      break;

    default:
      gcc_unreachable ();
    }
}


/* Write the content of VALUE to the bits starting at OFFSET of the data
   represented by THIS.  */

void
data_value::set_at (const data_value & value, unsigned offset)
{
  set_at (offset, value.bit_width, value, 0);
}


/* Overwrite the content of THIS by that of VALUE.  */

void
data_value::set (const data_value & value)
{
  gcc_assert (value.get_bitwidth () == bit_width);
  set_at (value, 0);
}


/* Write the constant to THIS at bits starting at OFFSET as a known value.  */

void
data_value::set_known_at (const wide_int & val, unsigned offset)
{
  set_known_at (offset, val.get_precision (), val, 0);
}


/* Write the constant to THIS as a known value.  */

void
data_value::set_known (const wide_int & val)
{
  gcc_assert (val.get_precision () == bit_width);
  set_known_at (val, 0);
}


/* Return the known value at bits [OFFSET, OFFSET+WIDTH) of THIS.  */

wide_int
data_value::get_known_at (unsigned offset, unsigned width) const
{
  gcc_assert (offset < bit_width);
  gcc_assert (width <= bit_width - offset);

  enum value_type val_type = classify (offset, width);
  gcc_assert (val_type == VAL_KNOWN);
  wide_int tmp = wide_int::from (wide_int_ref (known_value), bit_width,
				 UNSIGNED);
  if (offset > 0)
    tmp = wi::lrshift (tmp, offset);
  return wide_int::from (tmp, width, UNSIGNED);
}


/* Return the known value of THIS.  */

wide_int
data_value::get_known () const
{
  return get_known_at (0, bit_width);
}


/* Return the address stored at bits starting at OFFSET of THIS.  */

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


/* Return the address stored in THIS.  */

storage_address *
data_value::get_address () const
{
  gcc_assert (bit_width == HOST_BITS_PER_PTR);
  return get_address_at (0);
}


/* Return the value(s) held in bits [OFFSET, OFFSET+WIDTH) of THIS.  */

data_value
data_value::get_at (unsigned offset, unsigned width) const
{
  data_value result (width);
  switch (classify (offset, width))
    {
    case VAL_KNOWN:
      result.set_known (get_known_at (offset, width));
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


/* Return the value(s) held in THIS as a TREE of type TYPE.  */

tree
data_value::to_tree (tree type) const
{
  gcc_assert (classify () == VAL_KNOWN);
  wide_int value = get_known ();
  if (TREE_CODE (type) == INTEGER_TYPE)
    return wide_int_to_tree (type, value);
  else
    {
      tree int_type = build_nonstandard_integer_type (bit_width, false);
      tree int_val = wide_int_to_tree (int_type, value);
      return fold_build1 (VIEW_CONVERT_EXPR, type, int_val);
    }
}


/* Print the value held in VALUE in bits [OFFSET, OFFSET+WIDTH), using TYPE
   to produce a best suited representation.  */

void
context_printer::print_at (const data_value & value, tree type, unsigned offset,
			   unsigned width)
{
  if (TREE_CODE (type) == VECTOR_TYPE)
    {
      gcc_assert (width == value.get_bitwidth ());
      gcc_assert (offset == 0);
      tree elt_type = TREE_TYPE (type);
      unsigned elt_width = get_constant_type_size (elt_type);
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

	case VAL_KNOWN:
	  {
	    const wide_int wi_val = value.get_known_at (offset, width);
	    if (TREE_CODE (type) == REAL_TYPE)
	      {
		tree int_type = make_signed_type (width);
		tree known = wide_int_to_tree (int_type, wi_val);
		tree real = fold_build1 (VIEW_CONVERT_EXPR, type, known);
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
}


/* Print the value held in VALUE in bits starting at OFFSET, with the knowledge
   that the value has type TYPE.  */

void
context_printer::print_at (const data_value & value, tree type, unsigned offset)
{
  unsigned width = get_constant_type_size (type);
  print_at (value, type, offset, width);
}


/* Print the value of type TYPE held in VALUE.  */

void
context_printer::print (const data_value & value, tree type)
{
  print_at (value, type, 0);
}


/* Return the variable decl associated with the variable storage that THIS
   represents.  */

tree
data_storage::get_variable () const
{
  gcc_assert (type == STRG_VARIABLE);
  return u.variable.decl;
}


/* Output a representation of the variable or memory allocation held in THIS
   using the context printer PRINTER.  */

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


/* Return the index of the data storage STORAGE if it's a heap storage present
   in THIS.  Return -1 if it's not in THIS.  */

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


/* Return the index of the data storage STORAGE if it's a variable storage
   present in THIS context.  Return -1 if it's not in THIS.  */

int
simul_scope::find (const data_storage & storage) const
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


/* Return the outermost context.  */

const simul_scope &
simul_scope::root () const
{
  const simul_scope * ctx = this;

  while (ctx->parent != nullptr)
    ctx = ctx->parent;

  return *ctx;
}


/* Return the data storage associated with variable decl VAR if it's present
   in THIS context.  Otherwise return NULL.  */

data_storage *
simul_scope::find_var (tree var) const
{
  data_storage *strgp;
  unsigned i;
  FOR_EACH_VEC_ELT (var_storages, i, strgp)
    if (strgp->matches (var))
      return strgp;

  return nullptr;
}


/* Return the data storage associated with variable if it's present in THIS
   context or one of its parents.  Otherwise return NULL.  */

data_storage *
simul_scope::find_reachable_var (tree variable) const
{
  data_storage * result = find_var (variable);
  if (result != nullptr)
    return result;

  if (parent != nullptr)
    return root ().find_var (variable);

  return nullptr;
}


/* Return the heap data storage at index INDEX if INDEX is below the number
   of allocations in THIS.  Otherwise return NULL.  */

data_storage *
heap_memory::find_alloc (unsigned index) const
{
  if (index >= storages.length ())
    return nullptr;

  return &const_cast <data_storage &> (storages[index]);
}


/* Return the data storage at index INDEX in HEAP associated with THIS.  */

data_storage *
simul_scope::find_alloc (unsigned index) const
{
  return alloc_mem.find_alloc (index);
}


/* Return the heap data storage at index INDEX.  */

data_storage &
heap_memory::get_storage (unsigned idx) const
{
  gcc_assert (idx < storages.length ());
  return const_cast <data_storage &> (storages[idx]);
}


/* Add a new allocated data storage of AMOUNT bytes to THIS and return it.
 */

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


/* Add a new allocated data storage of AMOUNT bytes to the heap associated
   with THIS and return it.  */

data_storage &
simul_scope::allocate (unsigned amount)
{
  return alloc_mem.allocate (amount);
}


/* Return the variable data storage at index INDEX.  */

data_storage &
simul_scope::get_storage (unsigned idx) const
{
  gcc_assert (idx < var_storages.length ());
  return const_cast <data_storage &> (var_storages[idx]);
}


/* Evaluate the expression EXPR using the values currently stored in
   accessible variables and allocated storages and return the resulting value.
   */
data_value
simul_scope::evaluate (tree expr) const
{
  enum tree_code code = TREE_CODE (expr);
  switch (code)
    {
    case ARRAY_REF:
    case COMPONENT_REF:
    case MEM_REF:
    case TARGET_MEM_REF:
      {
	data_storage *storage = nullptr;
	int offset = -1;
	decompose_ref (expr, storage, offset);
	gcc_assert (storage != nullptr && offset >= 0);
	data_value var_value = storage->get_value ();
	unsigned bitwidth = get_constant_type_size (TREE_TYPE (expr));
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

    case INTEGER_CST:
      {
	data_value result (TREE_TYPE (expr));
	wide_int wi_expr = wi::to_wide (expr);
	result.set_known (wi_expr);
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
	    result.set_known (zero);
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
	unsigned elt_size = get_constant_type_size (elt_type);

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

    case VIEW_CONVERT_EXPR:
      return evaluate (TREE_OPERAND (expr, 0));

    default:
      gcc_unreachable ();
    }
}


/* Evaluate the constructor expression CSTR using the values currently stored
   in variables and allocated storages and return the resulting value.  */

data_value
simul_scope::evaluate_constructor (tree cstr) const
{
  gcc_assert (TREE_CODE (TREE_TYPE (cstr)) == VECTOR_TYPE
	      || TREE_CODE (TREE_TYPE (cstr)) == ARRAY_TYPE
	      || TREE_CODE (TREE_TYPE (cstr)) == RECORD_TYPE);

  unsigned bit_width = get_constant_type_size (TREE_TYPE (cstr));

  data_value result (bit_width);

  if (CONSTRUCTOR_NELTS (cstr) == 0)
    {
      wide_int zero = wi::zero (bit_width);
      result.set_known (zero);
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

	  unsigned elt_size = get_constant_type_size (TREE_TYPE (elt));

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


/* Evaluate the unary expression with code CODE, type TYPE and argument ARG
   using the values currently stored in variables and allocated storages and
   return the resulting value.  */

data_value
simul_scope::evaluate_unary (enum tree_code code, tree type, tree arg) const
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

	gcc_assert (value.classify () == VAL_KNOWN);
	tree t = value.to_tree (TREE_TYPE (arg));
	tree r = fold_unary (code, type, t);
	gcc_assert (TREE_CODE (r) == INTEGER_CST);
	wide_int wi_r = wi::to_wide (r);

	data_value result (type);
	result.set_known (wi_r);
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


/* Evaluate the binary expression with code CODE, type TYPE and arguments LHS
   and RHS using the values stored in variables and allocated storages and
   return the resulting value.  */

data_value
simul_scope::evaluate_binary (enum tree_code code, tree type, tree lhs,
			      tree rhs) const
{
  gcc_assert (TREE_CODE (type) == INTEGER_TYPE
	      || TREE_CODE (type) == BOOLEAN_TYPE
	      || TREE_CODE (type) == POINTER_TYPE
	      || TREE_CODE (type) == REAL_TYPE);
  data_value val_lhs = evaluate (lhs);
  data_value val_rhs = evaluate (rhs);
  enum value_type lhs_type = val_lhs.classify ();
  enum value_type rhs_type = val_rhs.classify ();
  if (lhs_type == VAL_KNOWN && rhs_type == VAL_KNOWN)
    {
      gcc_assert (TREE_TYPE (lhs) == TREE_TYPE (rhs)
		  || (TREE_CODE (TREE_TYPE (lhs)) == INTEGER_TYPE
		      && TREE_CODE (TREE_TYPE (rhs)) == INTEGER_TYPE
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
  else if ((lhs_type == VAL_ADDRESS && rhs_type == VAL_KNOWN)
	   || (lhs_type == VAL_KNOWN && rhs_type == VAL_ADDRESS))
    {
      if (code == PLUS_EXPR || code == POINTER_PLUS_EXPR)
	{
	  data_value *val_address = nullptr, *val_offset = nullptr;
	  if (lhs_type == VAL_ADDRESS && rhs_type == VAL_KNOWN)
	    {
	      val_address = &val_lhs;
	      val_offset = &val_rhs;
	    }
	  else if (lhs_type == VAL_KNOWN && rhs_type == VAL_ADDRESS)
	    {
	      val_address = &val_rhs;
	      val_offset = &val_lhs;
	    }
	  else
	    gcc_unreachable ();

	  storage_address *address = val_address->get_address ();
	  gcc_assert (address != nullptr);
	  wide_int offset = val_offset->get_known ();
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
	  if (lhs_type == VAL_ADDRESS && rhs_type == VAL_KNOWN)
	    val_null = &val_rhs;
	  else if (lhs_type == VAL_KNOWN && rhs_type == VAL_ADDRESS)
	    val_null = &val_lhs;
	  else
	    gcc_unreachable ();

	  /* Check that val_null really is the null pointer.  */
	  wide_int wi_null_val = val_null->get_known ();
	  gcc_assert (wi_null_val == 0);

	  data_value result (type);
	  bool bool_result = code == NE_EXPR;
	  wide_int wi_result = wi::uhwi (bool_result, result.get_bitwidth ());
	  result.set_known (wi_result);
	  return result;
	}
      else
	gcc_unreachable ();
    }
  else
    gcc_unreachable ();
}


/* Indicate whether code CODE represents a data reference to a full variable
   storage.  */

static bool
is_zero_offset_data_ref (enum tree_code code)
{
  switch (code)
    {
    case VAR_DECL:
    case SSA_NAME:
      return true;

    default:
      return false;
    }
}


/* Decompose DATA_REF into a root data storage and a bit offset into that
   storage, and write them into STORAGE and OFFSET respectively.  */

void
simul_scope::decompose_ref (tree data_ref, data_storage * & storage,
			    int & offset) const
{
  offset = -1;
  enum tree_code code = TREE_CODE (data_ref);
  if (is_zero_offset_data_ref (code))
    {
      offset = 0;
      storage = find_reachable_var (data_ref);
    }
  else
    {
      tree parent_data_ref = nullptr;
      wide_int add_offset = wi::zero (HOST_BITS_PER_WIDE_INT);
      wide_int add_index = wi::zero (HOST_BITS_PER_WIDE_INT);
      wide_int add_multiplier = wi::zero (HOST_BITS_PER_WIDE_INT);
      switch (code)
	{
	case ARRAY_REF:
	  {
	    parent_data_ref = TREE_OPERAND (data_ref, 0);

	    tree idx = TREE_OPERAND (data_ref, 1);
	    data_value val = evaluate (idx);
	    gcc_assert (val.classify () == VAL_KNOWN);
	    add_index = val.get_known ();

	    gcc_assert (TREE_OPERAND (data_ref, 3) == NULL_TREE);
	    tree elt_type = TREE_TYPE (TREE_TYPE (parent_data_ref));
	    unsigned size_bits = get_constant_type_size (elt_type);
	    add_multiplier = wi::uhwi (size_bits, HOST_BITS_PER_WIDE_INT);
	  }
	  break;

	case COMPONENT_REF:
	  {
	    parent_data_ref = TREE_OPERAND (data_ref, 0);

	    int comp_offset = int_bit_position (TREE_OPERAND (data_ref, 1));
	    gcc_assert (comp_offset >= 0);

	    add_offset = wi::shwi (comp_offset, HOST_BITS_PER_WIDE_INT);
	  }
	  break;

	case MEM_REF:
	case TARGET_MEM_REF:
	  {
	    tree var = TREE_OPERAND (data_ref, 0);
	    data_value var_val = evaluate (var);
	    gcc_assert (var_val.classify () == VAL_ADDRESS);
	    storage_address *addr = var_val.get_address ();
	    gcc_assert (addr != nullptr);
	    storage = &(addr->storage.get ());

	    add_offset = wi::uhwi (addr->offset, HOST_BITS_PER_WIDE_INT);
	    tree off = TREE_OPERAND (data_ref, 1);
	    data_value off_val = evaluate (off);
	    gcc_assert (off_val.classify () == VAL_KNOWN);
	    add_offset += off_val.get_known () * CHAR_BIT;

	    if (code == TARGET_MEM_REF)
	      {
		tree index = TREE_OPERAND (data_ref, 2);
		tree step = TREE_OPERAND (data_ref, 3);
		if (index || step)
		  {
		    gcc_assert (index && step);

		    data_value idx_val = evaluate (index);
		    gcc_assert (idx_val.classify () == VAL_KNOWN);
		    add_index = idx_val.get_known ();

		    data_value step_val = evaluate (step);
		    gcc_assert (step_val.classify () == VAL_KNOWN);
		    add_multiplier = step_val.get_known ();
		    add_multiplier *= CHAR_BIT;
		  }
	      }
	  }
	  break;

	default:
	  gcc_unreachable ();
	}

      if (parent_data_ref != nullptr)
	{
	  int parent_offset;
	  decompose_ref (parent_data_ref, storage, parent_offset);
	  add_offset += parent_offset;
	}

      if (add_offset.get_precision () < HOST_BITS_PER_WIDE_INT)
	add_offset = wide_int_storage::from (add_offset,
					     HOST_BITS_PER_WIDE_INT,
					     UNSIGNED);

      if (add_index.get_precision () < HOST_BITS_PER_WIDE_INT)
	add_index = wide_int_storage::from (add_index,
					    HOST_BITS_PER_WIDE_INT,
					    UNSIGNED);

      if (add_multiplier.get_precision () < HOST_BITS_PER_WIDE_INT)
	add_multiplier = wide_int_storage::from (add_multiplier,
						 HOST_BITS_PER_WIDE_INT,
						 UNSIGNED);

      wide_int off = add_offset + add_index * add_multiplier;
      gcc_assert (wi::fits_shwi_p (off));
      offset = off.to_shwi ();
    }

  gcc_assert (storage != nullptr);
  gcc_assert (offset >= 0);
}


/* Do the modification of storage values simulating assignment G
   possibly using the values currently held in any of the data storages
   accessible from THIS.  */

void
simul_scope::simulate_assign (gassign *g)
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


/* Return TRUE if the function used in call G is not sumulated.  Otherwise
   return FALSE.  */

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


/* Do the modification of storage values simulating call C possibly using the
   values currently held in any of the data storages accessible from THIS.  */

void
simul_scope::simulate_call (gcall *g)
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
      gcc_assert (size.classify () == VAL_KNOWN);
      wide_int wi_size = size.get_known ();
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
      gcc_assert (size0.classify () == VAL_KNOWN);
      gcc_assert (size1.classify () == VAL_KNOWN);
      wide_int wi_size0 = size0.get_known ();
      wide_int wi_size1 = size1.get_known ();
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
      val.set_known (wi::zero (hwi_size));
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
      gcc_assert (len2.classify () == VAL_KNOWN);
      storage_address addr0 = *ptr0.get_address ();
      data_storage & storage0 = addr0.storage.get ();
      data_value dest_val = storage0.get_value ();
      storage_address addr1 = *ptr1.get_address ();
      data_storage & storage1 = addr1.storage.get ();
      data_value src = storage1.get_value ();
      wide_int wi_len2 = len2.get_known ();
      gcc_assert (wi::fits_shwi_p (wi_len2));
      HOST_WIDE_INT hwi_len2 = wi_len2.to_shwi ();
      tree array_len2 = build_array_type_nelts (char_type_node, hwi_len2);
      tree data_target = build2 (MEM_REF, array_len2, arg0,
				 build_zero_cst (ptr_type_node));
      wi_len2 *= CHAR_BIT;
      gcc_assert (wi::fits_uhwi_p (wi_len2));
      unsigned HOST_WIDE_INT uhwi_len2 = wi_len2.to_uhwi ();
      data_value data_src = src.get_at (addr1.offset, uhwi_len2);
      printer.print_value_update (*this, data_target, data_src);
      dest_val.set_at (0, uhwi_len2, src, addr1.offset);
      storage0.set (dest_val);
    }
  else if (gimple_call_builtin_p (g, BUILT_IN_MEMSET))
    {
      gcc_assert (gimple_call_num_args (g) == 3);
      tree arg0 = gimple_call_arg (g, 0);
      tree arg1 = gimple_call_arg (g, 1);
      tree arg2 = gimple_call_arg (g, 2);
      data_value ptr0 = evaluate (arg0);
      data_value val1 = evaluate (arg1);
      data_value len2 = evaluate (arg2);
      gcc_assert (ptr0.classify () == VAL_ADDRESS);
      gcc_assert (val1.classify () == VAL_KNOWN);
      gcc_assert (len2.classify () == VAL_KNOWN);
      storage_address addr0 = *ptr0.get_address ();
      data_storage & storage0 = addr0.storage.get ();
      data_value dest_val = storage0.get_value ();

      wide_int wi_val1 = val1.get_known ();
      gcc_assert (wi::fits_uhwi_p (wi_val1));
      data_value byte1 (CHAR_BIT);
      byte1.set_known (wi::uhwi (wi_val1.to_uhwi (), CHAR_BIT));

      wide_int wi_len2 = len2.get_known ();
      gcc_assert (wi::fits_uhwi_p (wi_len2));
      unsigned HOST_WIDE_INT uhwi_len2 = wi_len2.to_uhwi ();

      for (unsigned HOST_WIDE_INT i = 0; i < uhwi_len2; i++)
	dest_val.set_at (byte1, i * CHAR_BIT + addr0.offset);

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

      result = ::simulate (DECL_STRUCT_FUNCTION (fn), *this, printer,
			  &arguments);
    }

  if (lhs == NULL_TREE)
    return;

  printer.print_value_update (*this, lhs, *result);
  data_storage *lhs_strg = find_var (lhs);
  gcc_assert (lhs_strg != nullptr);
  lhs_strg->set (*result);
}


/* Do the modification of storage values simulating statement G possibly using
   the values currently held in any of the data storages accessible from THIS.
   */
void
simul_scope::simulate (gimple *g)
{
  printer.begin_stmt (g);
  switch (g->code)
    {
    case GIMPLE_ASSIGN:
      simulate_assign (as_a <gassign *> (g));
      break;

    case GIMPLE_CALL:
      simulate_call (as_a <gcall *> (g));
      break;

    case GIMPLE_LABEL:
      printer.print_ignored_stmt ();
      break;

    default:
      gcc_unreachable ();
    }
  printer.end_stmt (g);
}


/* Do the modification of storage values simulating basic block BB possibly
   using the values held in any of the data storages accessible from THIS.
   */
gimple *
simul_scope::simulate (basic_block bb)
{
  printer.print_bb_entry (bb);

  gimple *g = bb->il.gimple.seq;

  if (g == nullptr)
    return nullptr;

  while (g && !is_ctrl_stmt (g))
    {
      simulate (g);
      g = g->next;
    }

  gcc_assert (!g || !g->next);
  return g;
}


/* Evaluate the condition using the storage values currently held in any of the
   data storages accessible from THIS.  */

bool
simul_scope::evaluate_condition (gcond *cond) const
{
  enum tree_code code = gimple_cond_code (cond);
  tree lhs = gimple_cond_lhs (cond);
  tree rhs = gimple_cond_rhs (cond);

  data_value val_lhs = evaluate (lhs);
  data_value val_rhs = evaluate (rhs);

  enum value_type lhs_type, rhs_type;
  lhs_type = val_lhs.classify ();
  rhs_type = val_rhs.classify ();
  if (lhs_type == VAL_KNOWN && rhs_type == VAL_KNOWN)
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
  else if ((lhs_type == VAL_KNOWN && rhs_type == VAL_ADDRESS)
	   || (lhs_type == VAL_ADDRESS && rhs_type == VAL_KNOWN))
    {
      /* Comparison of an address against a null pointer.  */
      data_value * null = nullptr;
      if (lhs_type == VAL_KNOWN)
	null = &val_lhs;
      else if (rhs_type == VAL_KNOWN)
	null = &val_rhs;
      else
	gcc_unreachable ();

      gcc_assert (null->get_known () == 0);
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


/* Return the edge leaving basic block BB.  If the basic block falls back to
   the following block or ends with a goto, there is a single leaving edge
   to return.  If the basic block ends with a conditional goto, evaluate the
   conditional and chose the leaving edge using the values currently held in
   data storages accessible by THIS to decide which one to return.  */

edge
simul_scope::select_leaving_edge (basic_block bb, gimple *last_stmt)
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


/* Do the modification of storage values simulating phi PHI when coming from
   edge E, possibly using the values currently held in any of the data storages
   accessible from THIS.  */

void
simul_scope::simulate_phi (gphi *phi, edge e)
{
  printer.begin_stmt (phi);
  tree lhs = gimple_phi_result (phi);
  tree phi_val = gimple_phi_arg_def_from_edge (phi, e);
  gassign *assign = gimple_build_assign (lhs, phi_val);
  simulate_assign (assign);
  printer.end_stmt (phi);
}


/* Evaluate all of the phis at the entry the destination of E.  */

void
simul_scope::jump (edge e)
{
  printer.print_bb_jump (e);

  gphi_iterator gsi;
  for (gsi = gsi_start_nonvirtual_phis (e->dest); !gsi_end_p (gsi);
       gsi_next_nonvirtual_phi (&gsi))
    simulate_phi (*gsi, e);
}


/* Simulate execution of function FUNC using the values held in any of the data
   storages accessible from THIS.  Return a value if the function returns a
   value.  Otherwise return empty.  */

optional <data_value>
simul_scope::simulate_function (struct function *func)
{
  printer.print_function_entry (func);
  basic_block bb = ENTRY_BLOCK_PTR_FOR_FN (func);
  greturn *final_stmt = nullptr;

  while (true)
    {
      gimple *last_stmt = simulate (bb);

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

  printer.print_function_exit (func);
  if (final_stmt == nullptr)
    return {};
  tree retexpr = gimple_return_retval (final_stmt);
  if (retexpr == NULL_TREE)
    return {};
  data_value result = evaluate (retexpr);
  return result;
}


/* Simulate execution of function FUNC using the values held in any of the data
   storages accessible from CALLER.  Use ARG_VALUES to initialise the arguments
   and PRINTER to create the new context.  */

static optional <data_value>
simulate (struct function * func, simul_scope & caller,
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

  simul_scope ctx = builder.build (caller, printer);

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

  return ctx.simulate_function (func);
}


/* Find the main function among all the declared functions.  */

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


/* Simulate execution of the main program.  */

void
simulate_main_execution (void)
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
  simul_scope root_context = builder.build (alloc_mem, printer);

  tree *varp = nullptr;
  unsigned i = 0;
  FOR_EACH_VEC_ELT (static_vars, i, varp)
    {
      tree var = *varp;
      tree initial = DECL_INITIAL (var);
      if (initial != NULL_TREE)
	{
	  gassign *tmp_assign = gimple_build_assign (var, initial);
	  root_context.simulate (tmp_assign);
	}
    }

  struct function *main = find_main ();
  gcc_assert (main != nullptr);
  vec<tree> args{};
  args.safe_push (build_zero_cst (integer_type_node));
  args.safe_push (null_pointer_node);
  simulate (main, root_context, printer, &args);
}


#if CHECKING_P

#include "selftest.h"

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
  simul_scope ctx = builder.build (mem, printer);

  data_value val (integer_type_node);

  ASSERT_EQ (val.classify (), VAL_UNDEFINED);

  wide_int i = wi::shwi (17, get_constant_type_size (integer_type_node));

  val.set_known (i);

  ASSERT_EQ (val.classify (), VAL_KNOWN);


  data_storage *storage_a = ctx.find_reachable_var (a);
  gcc_assert (storage_a != nullptr);

  storage_address address_a (storage_a->get_ref (), 0);

  data_value ptr (ptr_type_node);

  ASSERT_EQ (ptr.classify (), VAL_UNDEFINED);

  ptr.set_address (address_a);

  ASSERT_EQ (ptr.classify (), VAL_ADDRESS);


  tree vec2int = build_vector_type (integer_type_node, 2);
  data_value val2 (vec2int);

  ASSERT_EQ (val2.classify (0, HOST_BITS_PER_INT), VAL_UNDEFINED);
  ASSERT_EQ (val2.classify (HOST_BITS_PER_INT, HOST_BITS_PER_INT),
	     VAL_UNDEFINED);
  ASSERT_EQ (val2.classify (), VAL_UNDEFINED);

  val2.set_known_at (i, HOST_BITS_PER_INT);

  ASSERT_EQ (val2.classify (0, HOST_BITS_PER_INT), VAL_UNDEFINED);
  ASSERT_EQ (val2.classify (HOST_BITS_PER_INT, HOST_BITS_PER_INT), VAL_KNOWN);
  ASSERT_EQ (val2.classify (), VAL_MIXED);

  val2.set_known_at (i, 0);

  ASSERT_EQ (val2.classify (0, HOST_BITS_PER_INT), VAL_KNOWN);
  ASSERT_EQ (val2.classify (HOST_BITS_PER_INT, HOST_BITS_PER_INT), VAL_KNOWN);
  ASSERT_EQ (val2.classify (), VAL_KNOWN);


  tree vec2ptr = build_vector_type (ptr_type_node, 2);
  data_value val3 (vec2ptr);

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
simul_scope_find_reachable_var_tests ()
{
  heap_memory mem;
  context_printer printer;

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
  simul_scope ctx = builder.build (mem, printer);

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
  simul_scope ctx2 = builder.build (ctx, printer);

  ASSERT_NE (ctx2.find_reachable_var (e), nullptr);
  ASSERT_NE (ctx2.find_reachable_var (d), nullptr);
  ASSERT_NE (ctx2.find_reachable_var (b), nullptr);
  ASSERT_NE (ctx2.find_reachable_var (a), nullptr);

  vec<tree> empty{};

  simul_scope ctx3 = context_builder ().build (ctx2, printer);

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
  simul_scope ctx = builder.build (mem, printer);

  data_value val1 (ptr_type_node);

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

  simul_scope ctx2 = context_builder ().build (ctx, printer);

  data_value val2 (ptr_type_node);

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
  simul_scope ctx = builder.build (mem, printer);

  data_storage *storage_a = ctx.find_var (a);
  storage_address address_a (storage_a->get_ref (), 0);

  data_value val1 (ptr_type_node);

  val1.set_address (address_a);

  data_value val2 (ptr_type_node);

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
  simul_scope ctx = builder.build (mem, printer);

  data_storage *storage_a = ctx.find_reachable_var (a);
  data_storage *storage_b = ctx.find_reachable_var (b);

  storage_address address_a (storage_a->get_ref (), 0);
  storage_address address_b (storage_b->get_ref (), 0);

  data_value val1 (ptr_type_node);

  val1.set_address (address_a);

  tree vec2ptr = build_vector_type (ptr_type_node, 2);
  data_value val2 (vec2ptr);

  val2.set_at (val1, HOST_BITS_PER_PTR);
  ASSERT_EQ (val2.classify (HOST_BITS_PER_PTR, HOST_BITS_PER_PTR), VAL_ADDRESS);
  ASSERT_EQ (&val2.get_address_at (HOST_BITS_PER_PTR)->storage.get (),
	     storage_a);

  val1.set_address (address_b);

  val2.set_at (val1, 0);
  ASSERT_EQ (val2.classify (0, HOST_BITS_PER_PTR), VAL_ADDRESS);
  ASSERT_EQ (&val2.get_address_at (0)->storage.get (), storage_b);

  data_value val3 (vec2ptr);
  val3.set_at (val2, 0);

  ASSERT_EQ (val3.classify (0, HOST_BITS_PER_PTR), VAL_ADDRESS);
  ASSERT_EQ (&val3.get_address_at (0)->storage.get (), storage_b);
  ASSERT_EQ (val3.classify (HOST_BITS_PER_PTR, HOST_BITS_PER_PTR), VAL_ADDRESS);
  ASSERT_EQ (&val3.get_address_at (HOST_BITS_PER_PTR)->storage.get (),
	     storage_a);

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

  data_value val_derived (derived);

  ASSERT_EQ (val_derived.classify (), VAL_UNDEFINED);

  wide_int cst = wi::shwi (13, HOST_BITS_PER_INT);
  val_derived.set_known_at (cst, 0);

  ASSERT_EQ (val_derived.classify (), VAL_MIXED);
  ASSERT_EQ (val_derived.classify (0, HOST_BITS_PER_INT), VAL_KNOWN);
  wide_int wi_val = val_derived.get_known_at (0, HOST_BITS_PER_INT);
  ASSERT_TRUE (wi::fits_shwi_p (wi_val));
  ASSERT_EQ (wi_val.to_shwi (), 13);


  data_value vv (integer_type_node);
  wide_int wi23 = wi::shwi (23, HOST_BITS_PER_INT);
  vv.set_known (wi23);

  data_value vv2 (derived);
  vv2.set_at (vv, HOST_BITS_PER_INT);

  ASSERT_EQ (vv2.classify (), VAL_MIXED);

  ASSERT_EQ (vv2.classify (0, HOST_BITS_PER_INT), VAL_UNDEFINED);
  ASSERT_EQ (vv2.classify (HOST_BITS_PER_INT, HOST_BITS_PER_INT), VAL_KNOWN);
  wide_int wi_field2 = vv2.get_known_at (HOST_BITS_PER_INT, HOST_BITS_PER_INT);
  ASSERT_PRED1 (wi::fits_shwi_p, wi_field2);
  ASSERT_EQ (wi_field2.to_shwi (), 23);


  tree c12 = build_array_type_nelts (char_type_node, 12);

  data_value v (c12);

  wide_int wi33 = wi::shwi (33, CHAR_BIT);
  v.set_known_at (wi33, 9 * CHAR_BIT);

  data_value v2 (c12);
  v2.set_at (9 * CHAR_BIT, CHAR_BIT, v, 9 * CHAR_BIT);

  ASSERT_EQ (v2.classify (), VAL_MIXED);

  ASSERT_EQ (v2.classify (8 * CHAR_BIT, CHAR_BIT), VAL_UNDEFINED);
  ASSERT_EQ (v2.classify (10 * CHAR_BIT, CHAR_BIT), VAL_UNDEFINED);
  ASSERT_EQ (v2.classify (9 * CHAR_BIT, CHAR_BIT), VAL_KNOWN);
  wide_int wi_c9 = v2.get_known_at (9 * CHAR_BIT, CHAR_BIT);
  ASSERT_PRED1 (wi::fits_shwi_p, wi_c9);
  ASSERT_EQ (wi_c9.to_shwi (), 33);

  data_value v3 (c12);
  v3.set (v);

  ASSERT_EQ (v3.classify (), VAL_MIXED);

  ASSERT_EQ (v3.classify (8 * CHAR_BIT, CHAR_BIT), VAL_UNDEFINED);
  ASSERT_EQ (v3.classify (10 * CHAR_BIT, CHAR_BIT), VAL_UNDEFINED);
  ASSERT_EQ (v3.classify (9 * CHAR_BIT, CHAR_BIT), VAL_KNOWN);
  wide_int wi_c9_bis = v3.get_known_at (9 * CHAR_BIT, CHAR_BIT);
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
  simul_scope ctx4 = builder4.build (mem, printer);

  data_value mv (mixed);

  wide_int wi4 = wi::shwi (4, HOST_BITS_PER_LONG);
  mv.set_known_at (wi4, 0);

  data_storage *storage = ctx4.find_reachable_var (t);
  gcc_assert (storage != nullptr);
  storage_address address (storage->get_ref (), 0);

  mv.set_address_at (address, HOST_BITS_PER_LONG);

  wide_int wi7 = wi::shwi (7, HOST_BITS_PER_INT);
  mv.set_known_at (wi7, HOST_BITS_PER_LONG + HOST_BITS_PER_PTR);

  data_value mv2 (mixed);
  mv2.set (mv);

  ASSERT_EQ (mv2.classify (), VAL_MIXED);

  ASSERT_EQ (mv2.classify (0, HOST_BITS_PER_LONG), VAL_KNOWN);
  wide_int wi_i1 = mv2.get_known_at (0, HOST_BITS_PER_LONG);
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
	     VAL_KNOWN);
  wide_int wi_i3 = mv2.get_known_at (HOST_BITS_PER_LONG + HOST_BITS_PER_PTR,
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
  simul_scope ctx5 = builder5.build (mem, printer);

  data_value z17 (array_char_17);

  wide_int wi0 = wi::shwi (0, 17 * CHAR_BIT);
  z17.set_known_at (wi0, 0);

  data_storage *strg_c17 = ctx5.find_reachable_var (c17);
  gcc_assert (strg_c17 != nullptr);
  strg_c17->set (z17);

  data_storage *strg_i5 = ctx5.find_reachable_var (i5);
  gcc_assert (strg_i5 != nullptr);

  storage_address addr_i5 (strg_i5->get_ref (), 0);
  data_value val_addr_i5 (ptr_type_node);
  val_addr_i5.set_address (addr_i5);
  strg_c17->set_at (val_addr_i5, HOST_BITS_PER_PTR);

  data_value val17_before = strg_c17->get_value ();

  ASSERT_EQ (val17_before.classify (), VAL_MIXED);
  ASSERT_EQ (val17_before.classify (0, HOST_BITS_PER_PTR), VAL_KNOWN);
  ASSERT_EQ (val17_before.classify (HOST_BITS_PER_PTR, HOST_BITS_PER_PTR),
	     VAL_ADDRESS);
  ASSERT_EQ (val17_before.classify (2 * HOST_BITS_PER_PTR, CHAR_BIT),
	     VAL_KNOWN);

  data_value undef (5 * CHAR_BIT + HOST_BITS_PER_PTR);
  strg_c17->set_at (undef, 3 * CHAR_BIT);

  data_value val17_after = strg_c17->get_value ();

  ASSERT_EQ (val17_after.classify (), VAL_MIXED);
  ASSERT_EQ (val17_after.classify (0, 3 * CHAR_BIT), VAL_KNOWN);
  ASSERT_EQ (val17_after.classify (3 * CHAR_BIT,
				   5 * CHAR_BIT + HOST_BITS_PER_PTR),
	     VAL_UNDEFINED);
  ASSERT_EQ (val17_after.classify (2 * HOST_BITS_PER_PTR, CHAR_BIT),
	     VAL_KNOWN);
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
  simul_scope ctx = builder.build (mem, printer);

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
  simul_scope ctx2 = builder2.build (mem, printer2);

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
  val_int.set_known (wi_val);

  printer3.print (val_int, integer_type_node);

  ASSERT_STREQ (pp_formatted_text (&pp3), "17");


  heap_memory mem4;
  context_printer printer4;
  pretty_printer & pp4 = printer4.pp;

  simul_scope ctx4 = context_builder ().build (mem4, printer4);

  data_storage & alloc1 = ctx4.allocate (12);
  storage_address address1 (alloc1.get_ref (), 0);

  data_value val_ptr (ptr_type_node);
  val_ptr.set_address (address1);

  printer4.print (val_ptr, ptr_type_node);

  ASSERT_STREQ (pp_formatted_text (&pp4), "&<alloc00(12)>");


  heap_memory mem5;
  context_printer printer5;
  pretty_printer & pp5 = printer5.pp;

  simul_scope ctx5 = context_builder ().build (mem5, printer5);

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
  val6_259.set_known (cst259);

  printer6.print (val6_259, char_type_node);

  ASSERT_STREQ (pp_formatted_text (&pp6), "3");


  context_printer printer7;
  pretty_printer & pp7 = printer7.pp;

  data_value val7_259 (short_integer_type_node);
  val7_259.set_known (cst259);

  printer7.print_at (val7_259, char_type_node, CHAR_BIT);

  ASSERT_STREQ (pp_formatted_text (&pp7), "1");


  heap_memory mem8;
  context_printer printer8;
  pretty_printer & pp8 = printer8.pp;

  simul_scope ctx8 = context_builder ().build (mem8, printer8);
  data_storage & strg = ctx8.allocate (10);

  data_value v = strg.get_value ();
  wide_int cst41 = wi::shwi (41, CHAR_BIT);
  v.set_known_at (cst41, HOST_BITS_PER_PTR);

  printer8.print_at (v, char_type_node, HOST_BITS_PER_PTR, CHAR_BIT);

  ASSERT_STREQ (pp_formatted_text (&pp8), "41");


  context_printer printer9;
  pretty_printer & pp9 = printer9.pp;

  tree real2 = build_real (float_type_node, dconst2);
  tree sint = make_signed_type (TYPE_PRECISION (float_type_node));
  tree int_r2 = fold_build1 (VIEW_CONVERT_EXPR, sint, real2);
  wide_int wi_r2 = wi::to_wide (int_r2);

  data_value v9 (float_type_node);
  v9.set_known (wi_r2);

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
  simul_scope ctx1 = builder1.build (mem1, printer);

  data_value val1 (mixed1);
  wide_int wi7 = wi::uhwi (7, HOST_BITS_PER_INT);
  val1.set_known_at (wi7, tree_to_shwi (bit_position (i1)));

  data_storage * storage1 = ctx1.find_reachable_var (m1);
  storage_address address (storage1->get_ref (), 0);
  val1.set_address_at (address, tree_to_shwi (bit_position (p2)));

  data_value sub_val = val1.get_at (0, tree_to_shwi (bit_position (i3)));

  ASSERT_EQ (sub_val.classify (), VAL_MIXED);
  ASSERT_EQ (sub_val.classify (0, HOST_BITS_PER_INT), VAL_KNOWN);
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
  simul_scope ctx = builder.build (mem, printer);

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
  simul_scope ctx1 = builder1.build (mem1, printer1);

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
  simul_scope ctx2 = builder2.build (mem2, printer2);

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
  simul_scope ctx3 = builder3.build (mem3, printer3);

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
  simul_scope ctx1 = context_builder ().build (mem1, printer1);

  tree res1 = printer1.print_first_data_ref_part (ctx1, var2i, 0, nullptr,
						  VAL_UNDEFINED);

  ASSERT_EQ (res1, integer_type_node);
  const char * str1 = pp_formatted_text (&pp1);
  ASSERT_STREQ (str1, "# var2i.der2i_i1");


  context_printer printer2;
  pretty_printer & pp2 = printer2.pp;

  vec<tree> decls2{};
  decls2.safe_push (var2i);

  context_builder builder2 {};
  builder2.add_decls (&decls2);
  simul_scope ctx2 = builder2.build (mem1, printer2);

  tree mem_var2i = build2 (MEM_REF, der2i,
			   build1 (ADDR_EXPR, ptr_type_node, var2i),
			   build_zero_cst (ptr_type_node));

  tree res2 = printer2.print_first_data_ref_part (ctx2, mem_var2i, 0, nullptr,
						  VAL_UNDEFINED);

  ASSERT_EQ (res2, integer_type_node);
  const char * str2 = pp_formatted_text (&pp2);
  ASSERT_STREQ (str2, "# var2i.der2i_i1");


  context_printer printer3;
  pretty_printer & pp3 = printer3.pp;

  context_builder builder3 {};
  builder3.add_decls (&decls2);
  simul_scope ctx3 = builder3.build (mem1, printer3);

  tree long_var2i = build2 (MEM_REF, long_integer_type_node,
			   build1 (ADDR_EXPR, ptr_type_node, var2i),
			   build_zero_cst (ptr_type_node));

  tree res3 = printer3.print_first_data_ref_part (ctx3, long_var2i, 0, nullptr,
						  VAL_UNDEFINED);

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
				get_identifier ("der1d1i_i2"),
				integer_type_node);
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
  simul_scope ctx4 = builder4.build (mem1, printer4);

  tree mem_var1d1i = build2 (MEM_REF, long_integer_type_node,
			     build1 (ADDR_EXPR, ptr_type_node, var1d1i),
			     build_zero_cst (ptr_type_node));

  tree res4 = printer4.print_first_data_ref_part (ctx4, mem_var1d1i, 0, nullptr,
						  VAL_UNDEFINED);

  ASSERT_EQ (res4, short_integer_type_node);
  const char * str4 = pp_formatted_text (&pp4);
  ASSERT_STREQ (str4, "# var1d1i.der1d1i_d1.der2s_s1");


  context_printer printer5;
  pretty_printer & pp5 = printer5.pp;

  context_builder builder5 {};
  builder5.add_decls (&decls4);
  simul_scope ctx5 = builder5.build (mem1, printer5);

  tree mem_var1d1i_s2 = build2 (MEM_REF, short_integer_type_node,
				build1 (ADDR_EXPR, ptr_type_node, var1d1i),
				build_int_cst (ptr_type_node,
					       sizeof (short)));

  tree res5 = printer5.print_first_data_ref_part (ctx5, mem_var1d1i_s2, 0,
						  nullptr, VAL_UNDEFINED);

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
  simul_scope ctx6 = builder6.build (mem1, printer6);

  tree mem_var4c = build2 (MEM_REF, long_integer_type_node,
			   build1 (ADDR_EXPR, ptr_type_node, var4c),
			   build_int_cst (ptr_type_node, 2));

  tree res6 = printer6.print_first_data_ref_part (ctx6, mem_var4c, 0, nullptr,
						  VAL_UNDEFINED);

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
  simul_scope ctx7 = builder7.build (mem1, printer7);

  tree mem_var1i1d = build2 (MEM_REF, char_type_node,
			     build1 (ADDR_EXPR, ptr_type_node, var1i1d),
			     build_int_cst (ptr_type_node,
					    sizeof (int) + 1));

  tree res7 = printer7.print_first_data_ref_part (ctx7, mem_var1i1d, 0, nullptr,
						  VAL_UNDEFINED);

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
  simul_scope ctx8 = builder8.build (mem1, printer8);

  tree mem_var_i5 = build2 (MEM_REF, char_type_node,
			    build1 (ADDR_EXPR, ptr_type_node, var_i5),
			    build_int_cst (ptr_type_node,
					   3 * sizeof (int)));

  tree res8 = printer8.print_first_data_ref_part (ctx8, mem_var_i5, 0, nullptr,
						  VAL_UNDEFINED);

  ASSERT_EQ (res8, integer_type_node);
  const char * str8 = pp_formatted_text (&pp8);
  ASSERT_STREQ (str8, "# var_i5[3]");


  context_printer printer9;
  pretty_printer & pp9 = printer9.pp;

  context_builder builder9 {};
  builder9.add_decls (&decls8);
  simul_scope ctx9 = builder9.build (mem1, printer9);

  tree mem2_var_i5 = build2 (MEM_REF, char_type_node,
			     build1 (ADDR_EXPR, ptr_type_node, var_i5),
			     build_int_cst (ptr_type_node,
					    sizeof (int)));

  tree res9 = printer9.print_first_data_ref_part (ctx9, mem2_var_i5,
						  HOST_BITS_PER_INT, nullptr,
						  VAL_UNDEFINED);

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
  simul_scope ctx10 = builder10.build (mem1, printer10);

  tree mem_var_d1i1a5d = build2 (MEM_REF, char_type_node,
				 build1 (ADDR_EXPR, ptr_type_node, var_d1i1a5d),
				 build_int_cst (ptr_type_node,
						sizeof (int) + 13));

  tree res10 = printer10.print_first_data_ref_part (ctx10, mem_var_d1i1a5d, 0,
						    nullptr, VAL_UNDEFINED);

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
  simul_scope ctx11 = builder11.build (mem1, printer11);

  data_storage *var_storage = ctx11.find_reachable_var (var_i);
  storage_address var_address (var_storage->get_ref (), 0);

  data_value ptr_val (ptr_type_node);
  ptr_val.set_address (var_address);

  data_storage *ptr_storage = ctx11.find_reachable_var (ptr);
  ptr_storage->set (ptr_val);

  tree ref_ptr = build2 (MEM_REF, integer_type_node, ptr,
			 build_zero_cst (ptr_type_node));

  tree res11 = printer11.print_first_data_ref_part (ctx11, ref_ptr, 0, nullptr,
						    VAL_UNDEFINED);

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
  simul_scope ctx = builder.build (mem, printer);

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

  simul_scope ctx2 = builder.build (mem, printer2);

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
  simul_scope ctx3 = builder3.build (mem, printer3);

  tree mem_var2c = build2 (MEM_REF, short_integer_type_node,
			   build1 (ADDR_EXPR, ptr_type_node, var2c),
			   build_int_cst (ptr_type_node, 0));

  data_value val259 (short_integer_type_node);
  wide_int wi259 = wi::shwi (259, HOST_BITS_PER_SHORT);
  val259.set_known (wi259);

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
  simul_scope ctx4 = builder4.build (mem, printer4);

  tree vec2i = build_vector_type (integer_type_node, 2);

  tree mem_v2i = build2 (MEM_REF, vec2i,
			 build1 (ADDR_EXPR, ptr_type_node, v2i),
			 build_int_cst (ptr_type_node, 0));

  data_value val2i = data_value (vec2i);
  wide_int cst2 = wi::shwi (2, HOST_BITS_PER_INT);
  wide_int cst11 = wi::shwi (11, HOST_BITS_PER_INT);
  val2i.set_known_at (cst2, 0);
  val2i.set_known_at (cst11, HOST_BITS_PER_INT);

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
  simul_scope ctx5 = builder5.build (mem5, printer5);

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
  simul_scope ctx6 = builder6.build (mem, printer6);

  wide_int wi12 = wi::shwi (12, HOST_BITS_PER_INT);
  wide_int wi7 = wi::shwi (7, HOST_BITS_PER_INT);
  data_value cstr_12_7 (der2i_bis);
  cstr_12_7.set_known_at (wi12, 0);
  cstr_12_7.set_known_at (wi7, HOST_BITS_PER_INT);

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
  simul_scope ctx7 = builder7.build (mem, printer7);

  wide_int wi3 = wi::shwi (3, CHAR_BIT);
  wide_int wi11 = wi::shwi (11, HOST_BITS_PER_INT);
  data_value cstr_3_11 (der1c1i);
  cstr_3_11.set_known_at (wi3, 0);
  cstr_3_11.set_known_at (wi11, int_bit_position (der1c1i_i2));

  printer7.print_value_update (ctx7, var1c1i, cstr_3_11);

  const char *str7 = pp_formatted_text (&pp7);
  ASSERT_STREQ (str7, "# var1c1i.der1c1i_c1 = 3\n# var1c1i.der1c1i_i2 = 11\n");


  heap_memory mem8;
  context_printer printer8;
  pretty_printer & pp8 = printer8.pp;
  pp_buffer (&pp8)->m_flush_p = false;

  tree c5i_8 = build_array_type_nelts (char_type_node, 5);
  tree v5c_8 = create_var (c5i_8, "v5c");
  tree p_8 = create_var (ptr_type_node, "p");

  vec<tree> decls8{};
  decls8.safe_push (v5c_8);
  decls8.safe_push (p_8);

  context_builder builder8;
  builder8.add_decls (&decls8);
  simul_scope ctx8 = builder8.build (mem8, printer8);

  data_storage *strg8_v5c = ctx8.find_reachable_var (v5c_8);
  gcc_assert (strg8_v5c != nullptr);
  storage_address addr8_v5c_p1 (strg8_v5c->get_ref (), CHAR_BIT);
  data_value val8_addr (ptr_type_node);
  val8_addr.set_address (addr8_v5c_p1);

  data_storage *strg8_p = ctx8.find_reachable_var (p_8);
  gcc_assert (strg8_p != nullptr);
  strg8_p->set (val8_addr);

  data_value val8_17 (char_type_node);
  wide_int wi8_17 = wi::shwi (17, CHAR_BIT);
  val8_17.set_known (wi8_17);

  tree ref8 = build2 (MEM_REF, char_type_node, p_8,
		      build_zero_cst (build_pointer_type (char_type_node)));

  printer8.print_value_update (ctx8, ref8, val8_17);
  const char *str8 = pp_formatted_text (&pp8);
  ASSERT_STREQ (str8, "# v5c[1] = 17\n");


  heap_memory mem9;
  context_printer printer9;
  pretty_printer & pp9 = printer9.pp;
  pp_buffer (&pp9)->m_flush_p = false;

  tree a17c_9 = build_array_type_nelts (char_type_node, 17);
  tree v17c_9 = create_var (a17c_9, "v17c");
  tree p_9 = create_var (ptr_type_node, "p");
  tree i_9 = create_var (integer_type_node, "i");

  vec<tree> decls9{};
  decls9.safe_push (v17c_9);
  decls9.safe_push (p_9);
  decls9.safe_push (i_9);

  context_builder builder9;
  builder9.add_decls (&decls9);
  simul_scope ctx9 = builder9.build (mem9, printer9);

  data_storage *strg9_i = ctx9.find_reachable_var (i_9);
  gcc_assert (strg9_i != nullptr);
  storage_address addr9_i (strg9_i->get_ref (), 0);

  data_value val9_addr_i (ptr_type_node);
  val9_addr_i.set_address (addr9_i);

  data_storage *strg9_v17c = ctx9.find_reachable_var (v17c_9);
  gcc_assert (strg9_v17c != nullptr);
  storage_address addr9_v17c (strg9_v17c->get_ref (), HOST_BITS_PER_PTR);

  data_value val9_addr_v17c (ptr_type_node);
  val9_addr_v17c.set_address (addr9_v17c);

  data_storage *strg9_p = ctx9.find_reachable_var (p_9);
  gcc_assert (strg9_p != nullptr);
  strg9_p->set (val9_addr_v17c);

  tree a8c = build_array_type_nelts (char_type_node,
				     HOST_BITS_PER_PTR / CHAR_BIT);
  tree ref9 = build2 (MEM_REF, a8c, p_9,
		      build_zero_cst (build_pointer_type (ptr_type_node)));

  printer9.print_value_update (ctx9, ref9, val9_addr_i);
  const char *str9 = pp_formatted_text (&pp9);
  ASSERT_STREQ (str9, "# v17c[8B:+8B] = &i\n");
}


void
simul_scope_evaluate_tests ()
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
  simul_scope ctx = builder.build (mem, printer);

  tree int_ptr = build_pointer_type (integer_type_node);
  tree var_addr = build1 (ADDR_EXPR,  int_ptr, a);

  data_value val = ctx.evaluate (var_addr);
  storage_address *ptr_addr = val.get_address ();
  ASSERT_NE (ptr_addr, nullptr);
  data_storage &strg_ptr = ptr_addr->storage.get ();
  ASSERT_PRED1 (strg_ptr.matches, a);

  simul_scope ctx2 = context_builder ().build (ctx, printer);

  data_value val2 = ctx2.evaluate (var_addr);
  storage_address *ptr2_addr = val2.get_address ();
  ASSERT_NE (ptr2_addr, nullptr);
  data_storage &strg_ptr2 = ptr2_addr->storage.get ();
  ASSERT_PRED1 (strg_ptr2.matches, a);


  data_storage *strg_a = ctx.find_reachable_var (a);
  gcc_assert (strg_a != nullptr);
  data_value tmp22 (integer_type_node);
  wide_int wi22 = wi::shwi (22, HOST_BITS_PER_INT);
  tmp22.set_known (wi22);
  strg_a->set (tmp22);

  data_value val_a = ctx.evaluate (a);

  ASSERT_EQ (val_a.classify (), VAL_KNOWN);
  wide_int wi_a = val_a.get_known ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi_a);
  ASSERT_EQ (wi_a.to_shwi (), 22);


  tree cst33 = build_int_cst (integer_type_node, 33);

  data_value val_33 = ctx.evaluate (cst33);

  ASSERT_EQ (val_33.get_bitwidth (), HOST_BITS_PER_INT);
  ASSERT_EQ (val_33.classify (), VAL_KNOWN);
  wide_int wi33 = val_33.get_known ();
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
  ASSERT_EQ (val_v.classify (), VAL_KNOWN);
  wide_int low_part = val_v.get_known_at (0, HOST_BITS_PER_INT);
  ASSERT_PRED1 (wi::fits_shwi_p, low_part);
  ASSERT_EQ (low_part.to_shwi (), 2);
  wide_int high_part = val_v.get_known_at (HOST_BITS_PER_INT,
					   HOST_BITS_PER_INT);
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
  ASSERT_EQ (val_v2.classify (), VAL_KNOWN);

  tree sint = make_signed_type (TYPE_PRECISION (float_type_node));

#define HOST_BITS_PER_FLOAT (sizeof (float) * CHAR_BIT)
  wide_int low_wi = val_v2.get_known_at (0, HOST_BITS_PER_FLOAT);
  tree low_int = wide_int_to_tree (sint, low_wi);
  tree low_float = fold_build1 (VIEW_CONVERT_EXPR, float_type_node, low_int);
  ASSERT_EQ (TREE_CODE (low_float), REAL_CST);
  ASSERT_TRUE (real_identical (TREE_REAL_CST_PTR (low_float), &dconst2));

  wide_int high_wi = val_v2.get_known_at (HOST_BITS_PER_FLOAT,
					  HOST_BITS_PER_FLOAT);
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
  simul_scope ctx3 = builder3.build (mem3, printer);

  wide_int cst18 = wi::shwi (18, HOST_BITS_PER_INT);

  data_value value (a5i);
  value.set_known_at (cst18, 3 * HOST_BITS_PER_INT);

  data_storage *storage = ctx3.find_reachable_var (v5i);
  gcc_assert (storage != nullptr);
  storage->set (value);

  tree v5ref = build4 (ARRAY_REF, integer_type_node, v5i,
		       build_int_cst (integer_type_node, 3),
		       NULL_TREE, NULL_TREE);

  data_value evaluation = ctx3.evaluate (v5ref);

  ASSERT_EQ (evaluation.get_bitwidth (), HOST_BITS_PER_INT);
  ASSERT_EQ (evaluation.classify (), VAL_KNOWN);
  wide_int wi_val = evaluation.get_known ();
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
  simul_scope ctx4 = builder4.build (mem3, printer);

  wide_int cst15 = wi::shwi (15, HOST_BITS_PER_INT);

  data_value tmp (a3d);
  tmp.set_known_at (cst15, 3 * HOST_BITS_PER_INT);

  data_storage *storage2 = ctx4.find_reachable_var (v3d);
  gcc_assert (storage2 != nullptr);
  storage2->set (tmp);

  tree v3aref = build4 (ARRAY_REF, derived, v3d,
		       build_int_cst (integer_type_node, 1),
		       NULL_TREE, NULL_TREE);
  tree v3cref = build3 (COMPONENT_REF, integer_type_node, v3aref,
			field2, NULL_TREE);

  data_value eval2 = ctx4.evaluate (v3cref);

  ASSERT_EQ (eval2.get_bitwidth (), HOST_BITS_PER_INT);
  ASSERT_EQ (eval2.classify (), VAL_KNOWN);
  wide_int wi_val2 = eval2.get_known ();
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
  tree ssa_var = make_ssa_name_fn (DECL_STRUCT_FUNCTION (func), def_var,
				   nullptr);
  SSA_NAME_IS_DEFAULT_DEF (ssa_var) = 1;

  vec<tree> decls5{};
  decls5.safe_push (def_var);
  decls5.safe_push (ssa_var);

  context_builder builder5 {};
  builder5.add_decls (&decls5);
  simul_scope ctx5 = builder5.build (mem3, printer);

  wide_int cst14 = wi::shwi (14, HOST_BITS_PER_INT);

  data_value tmp14 (integer_type_node);
  tmp14.set_known (cst14);

  data_storage *storage5 = ctx5.find_reachable_var (def_var);
  gcc_assert (storage5 != nullptr);
  storage5->set (tmp14);

  data_value eval5 = ctx5.evaluate (ssa_var);

  ASSERT_EQ (eval5.get_bitwidth (), HOST_BITS_PER_INT);
  ASSERT_EQ (eval5.classify (), VAL_KNOWN);
  wide_int wi_val5 = eval5.get_known ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi_val5);
  ASSERT_EQ (wi_val5.to_shwi (), 14);


  tree a5c = build_array_type_nelts (char_type_node, 5);
  tree v5c = create_var (a5c,"v5c");

  vec<tree> decls6{};
  decls6.safe_push (v5c);

  context_builder builder6 {};
  builder6.add_decls (&decls6);
  simul_scope ctx6 = builder6.build (mem3, printer);

  wide_int cst8 = wi::shwi (8, CHAR_BIT);

  data_value tmp8 (a5c);
  tmp8.set_known_at (cst8, 3 * CHAR_BIT);

  data_storage *storage6 = ctx6.find_reachable_var (v5c);
  gcc_assert (storage5 != nullptr);
  storage6->set (tmp8);

  tree ref = build2 (MEM_REF, char_type_node,
		     build1 (ADDR_EXPR, ptr_type_node, v5c),
		     build_int_cst (ptr_type_node, 3));

  data_value eval6 = ctx6.evaluate (ref);

  ASSERT_EQ (eval6.get_bitwidth (), CHAR_BIT);
  ASSERT_EQ (eval6.classify (), VAL_KNOWN);
  wide_int wi_val6 = eval6.get_known ();
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
  simul_scope ctx8 = builder8.build (mem, printer);

  data_storage * strg29 = ctx8.find_reachable_var (a29);
  gcc_assert (strg29 != nullptr);

  tree ref0 = build5 (TARGET_MEM_REF, char_type_node,
		     build1 (ADDR_EXPR, ptr_type_node, a29),
		     build_zero_cst (ptr_type_node),
		     size_zero_node, size_zero_node, NULL_TREE);

  wide_int cst21 = wi::shwi (21, CHAR_BIT);
  data_value tmp29_0 (CHAR_BIT);
  tmp29_0.set_known (cst21);
  strg29->set_at (tmp29_0, 0);

  data_value val29_0 = ctx8.evaluate (ref0);

  ASSERT_EQ (val29_0.get_bitwidth (), CHAR_BIT);
  ASSERT_EQ (val29_0.classify (), VAL_KNOWN);
  wide_int wi29_0 = val29_0.get_known ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi29_0);
  ASSERT_EQ (wi29_0.to_uhwi (), 21);

  tree ref3 = build5 (TARGET_MEM_REF, char_type_node,
		     build1 (ADDR_EXPR, ptr_type_node, a29),
		     build_int_cst (ptr_type_node, 3),
		     size_zero_node, size_zero_node, NULL_TREE);

  wide_int cst26 = wi::shwi (26, CHAR_BIT);
  data_value tmp29_3 (CHAR_BIT);
  tmp29_3.set_known (cst26);
  strg29->set_at (tmp29_3, 24);

  data_value val29_3 = ctx8.evaluate (ref3);

  ASSERT_EQ (val29_3.get_bitwidth (), CHAR_BIT);
  ASSERT_EQ (val29_3.classify (), VAL_KNOWN);
  wide_int wi29_3 = val29_3.get_known ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi29_3);
  ASSERT_EQ (wi29_3.to_uhwi (), 26);

  tree ref12 = build5 (TARGET_MEM_REF, char_type_node,
		       build1 (ADDR_EXPR, ptr_type_node, a29),
		       build_zero_cst (ptr_type_node),
		       ssa8, build_int_cst (size_type_node, 4), NULL_TREE);

  wide_int cst17 = wi::shwi (17, CHAR_BIT);
  data_value tmp29_12 (CHAR_BIT);
  tmp29_12.set_known (cst17);
  strg29->set_at (tmp29_12, 96);

  wide_int cst3_bis = wi::shwi (3, sizeof (size_t) * CHAR_BIT);
  data_value val3_bis (sizeof (size_t) * CHAR_BIT);
  val3_bis.set_known (cst3_bis);
  data_storage * ssa8_strg = ctx8.find_reachable_var (ssa8);
  gcc_assert (ssa8_strg != nullptr);
  ssa8_strg->set (val3_bis);

  data_value val29_12 = ctx8.evaluate (ref12);

  ASSERT_EQ (val29_12.get_bitwidth (), CHAR_BIT);
  ASSERT_EQ (val29_12.classify (), VAL_KNOWN);
  wide_int wi29_12 = val29_12.get_known ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi29_12);
  ASSERT_EQ (wi29_12.to_uhwi (), 17);

  tree ref27 = build5 (TARGET_MEM_REF, char_type_node,
		       build1 (ADDR_EXPR, ptr_type_node, a29),
		       build_int_cst (ptr_type_node, 7),
		       ssa9, build_int_cst (size_type_node, 4), NULL_TREE);

  wide_int cst14_bis = wi::shwi (14, CHAR_BIT);
  data_value tmp29_27 (CHAR_BIT);
  tmp29_27.set_known (cst14_bis);
  strg29->set_at (tmp29_27, 216);

  wide_int cst5 = wi::shwi (5, sizeof (size_t) * CHAR_BIT);
  data_value val5_bis (sizeof (size_t) * CHAR_BIT);
  val5_bis.set_known (cst5);
  data_storage * ssa9_strg = ctx8.find_reachable_var (ssa9);
  gcc_assert (ssa9_strg != nullptr);
  ssa9_strg->set (val5_bis);

  data_value val29_27 = ctx8.evaluate (ref27);

  ASSERT_EQ (val29_27.get_bitwidth (), CHAR_BIT);
  ASSERT_EQ (val29_27.classify (), VAL_KNOWN);
  wide_int wi29_27 = val29_27.get_known ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi29_27);
  ASSERT_EQ (wi29_27.to_uhwi (), 14);


  tree a9i = build_array_type_nelts (integer_type_node, 9);
  tree v9i = create_var (a9i, "v9i");
  tree p = create_var (ptr_type_node, "p");

  vec<tree> decls9 {};
  decls9.safe_push (v9i);
  decls9.safe_push (p);

  context_builder builder9;
  builder9.add_decls (&decls9);
  simul_scope ctx9 = builder9.build (mem, printer);

  data_storage * strg9 = ctx9.find_reachable_var (v9i);
  gcc_assert (strg9 != nullptr);
  wide_int cst10 = wi::shwi (10, HOST_BITS_PER_INT);
  data_value val10 (integer_type_node);
  val10.set_known (cst10);
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

  ASSERT_EQ (val1.classify (), VAL_KNOWN);
  wide_int wi1 = val1.get_known ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi1);
  ASSERT_EQ (wi1.to_uhwi (), 10);

  storage_address addr_a9p3 (strg9->get_ref (), 3 * HOST_BITS_PER_INT);
  data_value val_addr9p3 (ptr_type_node);
  val_addr9p3.set_address (addr_a9p3);
  strg_p->set (val_addr9p3);

  wide_int cst16 = wi::shwi (16, HOST_BITS_PER_INT);
  data_value val16 (integer_type_node);
  val16.set_known (cst16);
  strg9->set_at (val16, 3 * HOST_BITS_PER_INT);

  tree ref3_bis = build2 (MEM_REF, integer_type_node,
			  p, build_zero_cst (ptr_type_node));

  data_value val3 = ctx9.evaluate (ref3_bis);

  ASSERT_EQ (val3.classify (), VAL_KNOWN);
  wide_int wi3 = val3.get_known ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi3);
  ASSERT_EQ (wi3.to_uhwi (), 16);

  wide_int cst19 = wi::shwi (19, HOST_BITS_PER_INT);
  data_value val19 (integer_type_node);
  val19.set_known (cst19);
  strg9->set_at (val19, 5 * HOST_BITS_PER_INT);

  tree ref5 = build2 (MEM_REF, integer_type_node,
		      p, build_int_cst (ptr_type_node, 2 * sizeof (int)));

  data_value val5 = ctx9.evaluate (ref5);

  ASSERT_EQ (val5.classify (), VAL_KNOWN);
  wide_int wi5 = val5.get_known ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi5);
  ASSERT_EQ (wi5.to_uhwi (), 19);


  tree c19 = build_array_type_nelts (char_type_node, 19);
  tree a19 = create_var (c19, "a19");
  tree ssa101 = make_node (SSA_NAME);
  TREE_TYPE (ssa101) = size_type_node;
  tree ssa102 = make_node (SSA_NAME);
  TREE_TYPE (ssa102) = size_type_node;

  vec<tree> decls10{};
  decls10.safe_push (a19);
  decls10.safe_push (ssa101);
  decls10.safe_push (ssa102);

  context_builder builder10;
  builder10.add_decls (&decls10);
  simul_scope ctx10 = builder10.build (mem, printer);

  data_storage * strg19 = ctx10.find_reachable_var (a19);
  gcc_assert (strg19 != nullptr);

  tree ref19 = build5 (TARGET_MEM_REF, char_type_node,
		       build1 (ADDR_EXPR, ptr_type_node, a19),
		       build_zero_cst (ptr_type_node),
		       NULL_TREE, NULL_TREE, NULL_TREE);

  wide_int cst23 = wi::shwi (23, CHAR_BIT);
  data_value tmp19_0 (CHAR_BIT);
  tmp19_0.set_known (cst23);
  strg19->set_at (tmp19_0, 0);

  data_value val19_0 = ctx10.evaluate (ref19);

  ASSERT_EQ (val19_0.get_bitwidth (), CHAR_BIT);
  ASSERT_EQ (val19_0.classify (), VAL_KNOWN);
  wide_int wi19_0 = val19_0.get_known ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi19_0);
  ASSERT_EQ (wi19_0.to_uhwi (), 23);


  tree ta11 = build_array_type_nelts (integer_type_node, 2);
  tree taa11 = build_array_type_nelts (ta11, 2);
  tree a11 = create_var (taa11, "a11");
  tree p11 = create_var (ptr_type_node, "p11");

  vec<tree> decls11{};
  decls11.safe_push (a11);
  decls11.safe_push (p11);

  context_builder builder11;
  builder11.add_decls (&decls11);
  simul_scope ctx11 = builder11.build (mem, printer);

  data_value val_a11 (integer_type_node);
  wide_int wi_a11 = wi::shwi (7, TYPE_PRECISION (integer_type_node));
  val_a11.set_known (wi_a11);

  data_storage * strg_a11 = ctx11.find_reachable_var (a11);
  gcc_assert (strg_a11 != nullptr);
  strg_a11->set_at (val_a11, 2 * HOST_BITS_PER_INT);

  storage_address addr_a11 (strg_a11->get_ref (), 2 * HOST_BITS_PER_INT);

  data_value val_p11 (ptr_type_node);
  val_p11.set_address (addr_a11);

  data_storage * strg_p11 = ctx11.find_reachable_var (p11);
  gcc_assert (strg_p11 != nullptr);
  strg_p11->set (val_p11);

  tree pta11 = build_pointer_type (ta11);
  tree mem11 = build2 (MEM_REF, ta11,
		       p11, build_zero_cst (pta11));
  tree ar11 = build4 (ARRAY_REF, integer_type_node,
		      mem11, build_zero_cst (integer_type_node),
		      NULL_TREE, NULL_TREE);

  data_value val_ref11 = ctx11.evaluate (ar11);

  ASSERT_EQ (val_ref11.get_bitwidth (), HOST_BITS_PER_INT);
  ASSERT_EQ (val_ref11.classify (), VAL_KNOWN);
  wide_int wi_ref11 = val_ref11.get_known ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi_ref11);
  ASSERT_EQ (wi_ref11.to_uhwi (), 7);
}


void
simul_scope_evaluate_literal_tests ()
{
  heap_memory mem;
  context_printer printer;

  vec<tree> empty{};
  vec<tree> empty2{};

  simul_scope ctx = context_builder ().build (mem, printer);

  tree cst = build_int_cst (integer_type_node, 13);

  data_value val = ctx.evaluate (cst);
  ASSERT_EQ (val.classify (), VAL_KNOWN);
  wide_int wi_value = val.get_known ();
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
  simul_scope ctx2 = builder2.build (mem2, printer2);

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
  simul_scope ctx3 = builder3.build (mem3, printer3);

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
  simul_scope ctx4 = builder4.build (mem4, printer4);

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
simul_scope_evaluate_constructor_tests ()
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
  simul_scope ctx = builder.build (mem, printer);

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

  simul_scope ctx2 = context_builder ().build (mem, printer);

  tree cst3 = build_int_cst (integer_type_node, 3);
  tree cst7 = build_int_cst (integer_type_node, 7);
  vec<constructor_elt, va_gc> * vec_elts2 = nullptr;
  CONSTRUCTOR_APPEND_ELT (vec_elts2, NULL_TREE, cst3);
  CONSTRUCTOR_APPEND_ELT (vec_elts2, NULL_TREE, cst7);
  tree cstr2 = build_constructor (a2i, vec_elts2);

  data_value val_cstr2 = ctx2.evaluate (cstr2);

  ASSERT_EQ (val_cstr2.classify (), VAL_KNOWN);
  wide_int wi_3 = val_cstr2.get_known_at (0, HOST_BITS_PER_INT);
  ASSERT_PRED1 (wi::fits_uhwi_p, wi_3);
  ASSERT_EQ (wi_3.to_uhwi (), 3);
  wide_int wi_7 = val_cstr2.get_known_at (HOST_BITS_PER_INT, HOST_BITS_PER_INT);
  ASSERT_PRED1 (wi::fits_uhwi_p, wi_7);
  ASSERT_EQ (wi_7.to_uhwi (), 7);

  vec_elts2 = nullptr;
  CONSTRUCTOR_APPEND_ELT (vec_elts2, integer_zero_node, cst3);
  CONSTRUCTOR_APPEND_ELT (vec_elts2, integer_one_node, cst7);
  cstr2 = build_constructor (a2i, vec_elts2);

  val_cstr2 = ctx2.evaluate (cstr2);

  ASSERT_EQ (val_cstr2.classify (), VAL_KNOWN);
  wi_3 = val_cstr2.get_known_at (0, HOST_BITS_PER_INT);
  ASSERT_PRED1 (wi::fits_uhwi_p, wi_3);
  ASSERT_EQ (wi_3.to_uhwi (), 3);
  wi_7 = val_cstr2.get_known_at (HOST_BITS_PER_INT, HOST_BITS_PER_INT);
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
  simul_scope ctx3 = builder3.build (mem, printer);

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
	     VAL_KNOWN);
  wide_int wi_2 = val_cstr3.get_known_at (HOST_BITS_PER_PTR, HOST_BITS_PER_INT);
  ASSERT_PRED1 (wi::fits_uhwi_p, wi_2);
  ASSERT_EQ (wi_2.to_uhwi (), 2);


  simul_scope ctx4 = context_builder ().build (mem, printer);

  tree cstr4 = build_constructor (a2i, nullptr);

  data_value val_cstr4 = ctx4.evaluate (cstr4);

  ASSERT_EQ (val_cstr4.classify (), VAL_KNOWN);
  wide_int wi_0 = val_cstr4.get_known_at (0, HOST_BITS_PER_INT);
  ASSERT_PRED1 (wi::fits_uhwi_p, wi_0);
  ASSERT_EQ (wi_0.to_uhwi (), 0);
  wide_int wi_0_bis = val_cstr4.get_known_at (HOST_BITS_PER_INT,
					      HOST_BITS_PER_INT);
  ASSERT_PRED1 (wi::fits_uhwi_p, wi_0_bis);
  ASSERT_EQ (wi_0_bis.to_uhwi (), 0);
}


void
simul_scope_evaluate_unary_tests ()
{
  heap_memory mem;
  context_printer printer;

  tree c1 = create_var (char_type_node, "c1");

  vec<tree> decls1 {};
  decls1.safe_push (c1);

  context_builder builder1;
  builder1.add_decls (&decls1);
  simul_scope ctx1 = builder1.build (mem, printer);

  wide_int wi18 = wi::uhwi (18, CHAR_BIT);
  data_value val18 (char_type_node);
  val18.set_known (wi18);
  data_storage *strg_c1 = ctx1.find_reachable_var (c1);
  gcc_assert (strg_c1 != nullptr);
  strg_c1->set (val18);

  data_value val1 = ctx1.evaluate_unary (NOP_EXPR, integer_type_node, c1);

  ASSERT_EQ (val1.get_bitwidth (), HOST_BITS_PER_INT);
  ASSERT_EQ (val1.classify (), VAL_KNOWN);
  wide_int wi1 = val1.get_known ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi1);
  ASSERT_EQ (wi1.to_uhwi (), 18);


  REAL_VALUE_TYPE f25 = REAL_VALUE_ATOF ("2.5", TYPE_MODE (float_type_node));
  tree t25 = build_real (float_type_node, f25);

  data_value valfm25 = ctx1.evaluate_unary (NEGATE_EXPR, float_type_node, t25);

  ASSERT_EQ (valfm25.get_bitwidth (), TYPE_PRECISION (float_type_node));
  ASSERT_EQ (valfm25.classify (), VAL_KNOWN);
  tree tfm25 = valfm25.to_tree (float_type_node);
  ASSERT_EQ (TREE_CODE (tfm25), REAL_CST);
  REAL_VALUE_TYPE fm25 = REAL_VALUE_ATOF ("-2.5", TYPE_MODE (float_type_node));
  ASSERT_TRUE (real_equal (TREE_REAL_CST_PTR (tfm25), &fm25));


  tree i2 = create_var (intSI_type_node, "i2");

  vec<tree> decls2 {};
  decls2.safe_push (i2);

  context_builder builder2;
  builder2.add_decls (&decls2);
  simul_scope ctx2 = builder2.build (mem, printer);

  wide_int wi23 = wi::uhwi (23, TYPE_PRECISION (intSI_type_node));
  data_value val23 (intSI_type_node);
  val23.set_known (wi23);
  data_storage *strg_i2 = ctx2.find_reachable_var (i2);
  gcc_assert (strg_i2 != nullptr);
  strg_i2->set (val23);

  data_value val2 = ctx2.evaluate_unary (VIEW_CONVERT_EXPR,
					 unsigned_intSI_type_node, i2);

  ASSERT_EQ (val2.get_bitwidth (), HOST_BITS_PER_INT);
  ASSERT_EQ (val2.classify (), VAL_KNOWN);
  wide_int wi2 = val2.get_known ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi2);
  ASSERT_EQ (wi2.to_uhwi (), 23);
}


void
simul_scope_evaluate_binary_tests ()
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
  simul_scope ctx = builder.build (mem, printer);

  wide_int cst12 = wi::shwi (12, HOST_BITS_PER_INT);
  data_value val12 (HOST_BITS_PER_INT);
  val12.set_known (cst12);
  data_storage *strg_a = ctx.find_reachable_var (a);
  gcc_assert (strg_a != nullptr);
  strg_a->set (val12);

  wide_int cst7 = wi::shwi (7, HOST_BITS_PER_INT);
  data_value val7 (HOST_BITS_PER_INT);
  val7.set_known (cst7);
  data_storage *strg_b = ctx.find_reachable_var (b);
  gcc_assert (strg_b != nullptr);
  strg_b->set (val7);

  data_value val = ctx.evaluate_binary (MINUS_EXPR, integer_type_node, a, b);

  ASSERT_EQ (val.classify (), VAL_KNOWN);
  wide_int wi_val = val.get_known ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi_val);
  ASSERT_EQ (wi_val.to_shwi (), 5);


  tree a_bis = create_var (integer_type_node, "a");
  tree b_bis = create_var (integer_type_node, "b");

  vec<tree> decls2{};
  decls2.safe_push (a_bis);
  decls2.safe_push (b_bis);

  context_builder builder2 {};
  builder2.add_decls (&decls2);
  simul_scope ctx2 = builder2.build (mem, printer);

  data_value val12_bis (HOST_BITS_PER_INT);
  val12_bis.set_known (wi::shwi (12, HOST_BITS_PER_INT));
  ctx2.find_reachable_var (a_bis)->set (val12_bis);

  data_value val7_bis (HOST_BITS_PER_INT);
  val7_bis.set_known (wi::shwi (7, HOST_BITS_PER_INT));
  ctx2.find_reachable_var (b_bis)->set (val7_bis);

  data_value val2 = ctx2.evaluate_binary (GT_EXPR, boolean_type_node, a_bis,
					  b_bis);

  ASSERT_EQ (val2.classify (), VAL_KNOWN);
  ASSERT_EQ (val2.get_bitwidth (), 1);
  wide_int wi_val2 = val2.get_known ();
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
  simul_scope ctx3 = builder3.build (mem, printer);

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
  cst1.set_known (wi1);
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
  simul_scope ctx4 = builder4.build (mem, printer);

  machine_mode float_mode = TYPE_MODE (float_type_node);
  REAL_VALUE_TYPE val25 = REAL_VALUE_ATOF ("2.5", float_mode);
  tree c25 = build_real (float_type_node, val25);
  tree float_size = TYPE_SIZE (float_type_node);
  ASSERT_PRED1 (tree_fits_uhwi_p, float_size);
  tree itype = make_signed_type (tree_to_uhwi (float_size));
  tree i25 = fold_build1 (VIEW_CONVERT_EXPR, itype, c25);

  wide_int wi25 = wi::to_wide (i25);
  data_value data25 (float_type_node);
  data25.set_known (wi25);

  data_storage *f25_storage = ctx4.find_reachable_var (f25);
  gcc_assert (f25_storage != nullptr);
  f25_storage->set (data25);

  REAL_VALUE_TYPE val325 = REAL_VALUE_ATOF ("3.25", float_mode);
  tree c325 = build_real (float_type_node, val325);

  data_value val_f = ctx4.evaluate_binary (PLUS_EXPR, float_type_node,
					   f25, c325);

  ASSERT_EQ (val_f.classify (), VAL_KNOWN);
  wide_int wi_f = val_f.get_known ();
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
  simul_scope ctx5 = builder5.build (mem, printer);

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

  ASSERT_EQ (val_ne5.classify (), VAL_KNOWN);
  wide_int wi_ne5 = val_ne5.get_known ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi_ne5);
  ASSERT_EQ (wi_ne5.to_uhwi (), 1);
}


void
simul_scope_evaluate_condition_tests ()
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
  simul_scope ctx1 = builder1.build (mem1, printer1);

  wide_int cst12 = wi::shwi (12, HOST_BITS_PER_INT);
  data_value val12 (HOST_BITS_PER_INT);
  val12.set_known (cst12);
  data_storage *strg_a = ctx1.find_reachable_var (a);
  gcc_assert (strg_a != nullptr);
  strg_a->set (val12);

  wide_int cst7 = wi::shwi (7, HOST_BITS_PER_INT);
  data_value val7 (HOST_BITS_PER_INT);
  val7.set_known (cst7);
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
  simul_scope ctx2 = builder2.build (mem2, printer2);

  wide_int wi_null = wi::shwi (0, HOST_BITS_PER_PTR);
  data_value val_null1 (ptr_type_node);
  val_null1.set_known (wi_null);

  data_storage *strg_p1 = ctx2.find_reachable_var (p1);
  gcc_assert (strg_p1 != nullptr);
  strg_p1->set (val_null1);

  tree tree_null = build_int_cst (ptr_type_node, 0);

  gcond *cond2 = gimple_build_cond (EQ_EXPR, p1, tree_null, NULL_TREE,
				    NULL_TREE);

  ASSERT_TRUE (ctx2.evaluate_condition (cond2));

  data_storage & alloc3 = ctx2.allocate (3);
  storage_address addr3 (alloc3.get_ref (), 0);
  data_value val_addr3 (ptr_type_node);
  val_addr3.set_address (addr3);
  data_storage *strg_p2 = ctx2.find_reachable_var (p2);
  gcc_assert (strg_p2 != nullptr);
  strg_p2->set (val_addr3);

  gcond *cond3 = gimple_build_cond (EQ_EXPR, p2, tree_null, NULL_TREE,
				    NULL_TREE);

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
  simul_scope ctx3 = builder3.build (mem3, printer3);

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
  simul_scope ctx4 = builder4.build (mem4, printer4);

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
simul_scope_simulate_assign_tests ()
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
  simul_scope ctx = builder.build (mem, printer);

  data_storage *storage_a = ctx.find_reachable_var (a);
  data_storage *storage_b = ctx.find_reachable_var (b);

  tree cst = build_int_cst (integer_type_node, 13);

  gimple *gassign1 = gimple_build_assign (a, cst);

  data_value val = storage_a->get_value ();
  ASSERT_EQ (val.classify (), VAL_UNDEFINED);

  ctx.simulate (gassign1);

  data_value val2 = storage_a->get_value ();
  ASSERT_EQ (val2.classify (), VAL_KNOWN);
  wide_int wi_val2 = val2.get_known ();
  ASSERT_TRUE (wi::fits_shwi_p (wi_val2));
  ASSERT_EQ (wi_val2.to_shwi (), 13);


  tree lhs = build3 (COMPONENT_REF, integer_type_node, b, field1, NULL_TREE);

  gimple *gassign2 = gimple_build_assign (lhs, cst);

  data_value val3 = storage_b->get_value ();
  ASSERT_EQ (val3.classify (), VAL_UNDEFINED);

  ctx.simulate (gassign2);

  data_value val4 = storage_b->get_value ();
  ASSERT_EQ (val4.classify (), VAL_MIXED);
  ASSERT_EQ (val4.classify (0, HOST_BITS_PER_INT), VAL_KNOWN);
  wide_int wi_val4 = val4.get_known_at (0, HOST_BITS_PER_INT);
  ASSERT_TRUE (wi::fits_shwi_p (wi_val4));
  ASSERT_EQ (wi_val4.to_shwi (), 13);

  tree lhs2 = build3 (COMPONENT_REF, integer_type_node, b, field2, NULL_TREE);
  tree cst2 = build_int_cst (integer_type_node, 17);
  gimple *gassign3 = gimple_build_assign (lhs2, cst2);

  data_value val5 = storage_b->get_value ();
  ASSERT_EQ (val5.classify (), VAL_MIXED);
  ASSERT_EQ (val5.classify (HOST_BITS_PER_INT, HOST_BITS_PER_INT),
	     VAL_UNDEFINED);

  ctx.simulate (gassign3);

  data_value val6 = storage_b->get_value ();
  ASSERT_EQ (val6.classify (), VAL_KNOWN);
  wide_int wi_val6 = val6.get_known_at (HOST_BITS_PER_INT, HOST_BITS_PER_INT);
  ASSERT_TRUE (wi::fits_shwi_p (wi_val4));
  ASSERT_EQ (wi_val6.to_shwi (), 17);


  tree ssa1 = make_node (SSA_NAME);
  TREE_TYPE (ssa1) = integer_type_node;

  vec<tree> ssanames{};
  ssanames.safe_push (ssa1);

  context_builder builder2 {};
  builder2.add_decls (&decls);
  builder2.add_decls (&ssanames);
  simul_scope ctx2 = builder2.build (mem, printer);

  tree i66 = build_int_cst (integer_type_node, 66);
  gimple *gassign5 = gimple_build_assign (ssa1, i66);

  data_storage *ssa1_strg = ctx2.find_reachable_var (ssa1);
  gcc_assert (ssa1_strg != nullptr);
  data_value ssa1_val = ssa1_strg->get_value ();

  ASSERT_EQ (ssa1_val.classify (), VAL_UNDEFINED);

  ctx2.simulate (gassign5);

  data_value ssa1_val2 = ssa1_strg->get_value ();
  ASSERT_EQ (ssa1_val2.classify (), VAL_KNOWN);
  wide_int wi_ssa1_2 = ssa1_val2.get_known ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi_ssa1_2);
  ASSERT_EQ (wi_ssa1_2.to_shwi (), 66);


  data_value cst66 (integer_type_node);
  wide_int wi66 = wi::shwi (66, HOST_BITS_PER_INT);
  cst66.set_known (wi66);

  ssa1_strg->set (cst66);

  gimple *gassign4 = gimple_build_assign (a, ssa1);

  data_storage *strg_a_ctx2 = ctx2.find_reachable_var (a);
  gcc_assert (strg_a_ctx2 != nullptr);
  data_value val_a = strg_a_ctx2->get_value ();

  ASSERT_EQ (val_a.classify (), VAL_UNDEFINED);

  ctx2.simulate (gassign4);

  data_value val_a2 = strg_a_ctx2->get_value ();
  ASSERT_EQ (val_a2.classify (), VAL_KNOWN);
  wide_int wi_a2 = val_a2.get_known ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi_a2);
  HOST_WIDE_INT hwi_a2 = wi_a2.to_shwi ();
  ASSERT_EQ (hwi_a2, 66);


  tree ptr = create_var (ptr_type_node, "ptr");

  vec<tree> decls2{};
  decls2.safe_push (ptr);

  context_builder builder3 {};
  builder3.add_decls (&decls2);
  simul_scope ctx3 = builder3.build (mem, printer);

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

  ctx3.simulate (assign);

  data_value val_alloc2 = alloc1.get_value ();
  ASSERT_EQ (val_alloc2.classify (), VAL_MIXED);
  ASSERT_EQ (val_alloc2.classify (0, HOST_BITS_PER_INT), VAL_KNOWN);
  wide_int wi_val_alloc2 = val_alloc2.get_known_at (0, HOST_BITS_PER_INT);
  ASSERT_PRED1 (wi::fits_shwi_p, wi_val_alloc2);
  ASSERT_EQ (wi_val_alloc2.to_shwi (), 123);


  tree u = create_var (integer_type_node, "u");

  vec<tree> decls3{};
  decls3.safe_push (u);

  context_builder builder4 {};
  builder4.add_decls (&decls3);
  simul_scope ctx4 = builder4.build (mem, printer);

  data_storage *strg_u = ctx4.find_reachable_var (u);
  gcc_assert (strg_u != nullptr);

  tree addr_u = build1 (ADDR_EXPR, ptr_type_node, u);
  tree ref_u = build2 (MEM_REF, integer_type_node, addr_u,
		     build_int_cst (ptr_type_node, 0));
  tree cst44 = build_int_cst (integer_type_node, 44);

  gimple *assign44 = gimple_build_assign (ref_u, cst44);

  data_value val_u = strg_u->get_value ();
  ASSERT_EQ (val_u.classify (), VAL_UNDEFINED);

  ctx4.simulate (assign44);

  data_value val_u2 = strg_u->get_value ();
  ASSERT_EQ (val_u2.classify (), VAL_KNOWN);
  wide_int wi_u2 = val_u2.get_known ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi_u2);
  ASSERT_EQ (wi_u2.to_shwi (), 44);


  tree var6 = create_var (integer_type_node, "var6");
  tree var2i = create_var (derived, "var2i");

  vec<tree> decls5{};
  decls5.safe_push (var2i);
  decls5.safe_push (var6);

  context_builder builder5 {};
  builder5.add_decls (&decls5);
  simul_scope ctx5 = builder5.build (mem, printer);

  wide_int wi8 = wi::shwi (8, HOST_BITS_PER_INT);
  wide_int wi13 = wi::shwi (13, HOST_BITS_PER_INT);

  data_value val7 (derived);
  val7.set_known_at (wi8, 0);
  val7.set_known_at (wi13, HOST_BITS_PER_INT);

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

  ctx5.simulate (assign5);

  gcc_assert (strg5 != nullptr);
  data_value after = strg5->get_value ();
  ASSERT_EQ (after.classify (), VAL_KNOWN);
  wide_int wi7 = after.get_known ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi7);
  ASSERT_EQ (wi7.to_shwi (), 13);


  tree a5c = build_array_type_nelts (char_type_node, 5);
  tree v5c = create_var (a5c,"v5c");

  vec<tree> decls6{};
  decls6.safe_push (v5c);

  context_builder builder6 {};
  builder6.add_decls (&decls6);
  simul_scope ctx6 = builder6.build (mem, printer);

  tree c8 = build_int_cst (char_type_node, 8);

  tree ref2 = build2 (MEM_REF, char_type_node,
		     build1 (ADDR_EXPR, ptr_type_node, v5c),
		     build_int_cst (ptr_type_node, 3));

  gassign *assign2 = gimple_build_assign (ref2, c8);

  ctx6.simulate (assign2);

  data_storage *storage = ctx6.find_reachable_var (v5c);
  gcc_assert (storage != nullptr);
  data_value c8val = storage->get_value ();

  ASSERT_EQ (c8val.classify (), VAL_MIXED);
  ASSERT_EQ (c8val.classify (3 * CHAR_BIT, CHAR_BIT), VAL_KNOWN);
  wide_int wi_val = c8val.get_known_at (3 * CHAR_BIT, CHAR_BIT);
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
  simul_scope ctx7 = builder7.build (mem, printer);

  wide_int cst12 = wi::shwi (12, HOST_BITS_PER_INT);
  data_value val12 (HOST_BITS_PER_INT);
  val12.set_known (cst12);
  data_storage *strg_a = ctx7.find_reachable_var (var_a);
  gcc_assert (strg_a != nullptr);
  strg_a->set (val12);

  wide_int cst7 = wi::shwi (7, HOST_BITS_PER_INT);
  data_value val7_bis (HOST_BITS_PER_INT);
  val7_bis.set_known (cst7);
  data_storage *strg_b = ctx7.find_reachable_var (var_b);
  gcc_assert (strg_b != nullptr);
  strg_b->set (val7_bis);

  data_storage *strg_x = ctx7.find_reachable_var (var_x);
  gcc_assert (strg_x != nullptr);

  gassign *assign7 = gimple_build_assign (var_x, MINUS_EXPR, var_a, var_b);

  data_value x_before = strg_x->get_value ();
  ASSERT_EQ (x_before.classify (), VAL_UNDEFINED);

  ctx7.simulate (assign7);

  data_value x_after = strg_x->get_value ();
  ASSERT_EQ (x_after.classify (), VAL_KNOWN);
  wide_int x_val = x_after.get_known ();
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
  simul_scope ctx8 = builder8.build (mem, printer);

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

  ctx8.simulate (assign29_0);

  data_value val29_after0 = strg29->get_value ();
  data_value val0_after = val29_after0.get_at (0, CHAR_BIT);
  ASSERT_EQ (val0_after.classify (), VAL_KNOWN);
  wide_int wi29_0 = val0_after.get_known_at (0, CHAR_BIT);
  ASSERT_PRED1 (wi::fits_uhwi_p, wi29_0);
  ASSERT_EQ (wi29_0.to_uhwi (), 21);

  tree ref3 = build5 (TARGET_MEM_REF, char_type_node,
		     build1 (ADDR_EXPR, ptr_type_node, a29),
		     build_int_cst (ptr_type_node, 3),
		     size_zero_node, size_zero_node, NULL_TREE);
  tree cst26 = build_int_cst (char_type_node, 26);
  gassign * assign29_3 = gimple_build_assign (ref3, cst26);

  data_value val29_before3 = strg29->get_value ();
  data_value val3_before = val29_before3.get_at (24, CHAR_BIT);
  ASSERT_EQ (val3_before.classify (), VAL_UNDEFINED);

  ctx8.simulate (assign29_3);

  data_value val29_after3 = strg29->get_value ();
  data_value val3_after = val29_after3.get_at (24, CHAR_BIT);
  ASSERT_EQ (val3_after.classify (), VAL_KNOWN);
  wide_int wi29_3 = val3_after.get_known ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi29_3);
  ASSERT_EQ (wi29_3.to_uhwi (), 26);

  tree ref12 = build5 (TARGET_MEM_REF, char_type_node,
		       build1 (ADDR_EXPR, ptr_type_node, a29),
		       build_zero_cst (ptr_type_node),
		       ssa8, build_int_cst (size_type_node, 4), NULL_TREE);
  tree cst17 = build_int_cst (char_type_node, 17);
  gassign * assign29_12 = gimple_build_assign (ref12, cst17);

  wide_int cst3_bis = wi::shwi (3, sizeof (size_t) * CHAR_BIT);
  data_value val3_bis (sizeof (size_t) * CHAR_BIT);
  val3_bis.set_known (cst3_bis);
  data_storage * ssa8_strg = ctx8.find_reachable_var (ssa8);
  gcc_assert (ssa8_strg != nullptr);
  ssa8_strg->set (val3_bis);

  data_value val29_before12 = strg29->get_value ();
  data_value val12_before = val29_before12.get_at (96, CHAR_BIT);
  ASSERT_EQ (val12_before.classify (), VAL_UNDEFINED);

  ctx8.simulate (assign29_12);

  data_value val29_after12 = strg29->get_value ();
  data_value val12_after = val29_after12.get_at (96, CHAR_BIT);
  ASSERT_EQ (val12_after.classify (), VAL_KNOWN);
  wide_int wi29_12 = val12_after.get_known ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi29_12);
  ASSERT_EQ (wi29_12.to_uhwi (), 17);

  tree ref27 = build5 (TARGET_MEM_REF, char_type_node,
		       build1 (ADDR_EXPR, ptr_type_node, a29),
		       build_int_cst (ptr_type_node, 7),
		       ssa9, build_int_cst (size_type_node, 4), NULL_TREE);
  tree cst14 = build_int_cst (char_type_node, 14);
  gassign * assign29_27 = gimple_build_assign (ref27, cst14);

  wide_int cst5 = wi::shwi (5, sizeof (size_t) * CHAR_BIT);
  data_value val5_bis (sizeof (size_t) * CHAR_BIT);
  val5_bis.set_known (cst5);
  data_storage * ssa9_strg = ctx8.find_reachable_var (ssa9);
  gcc_assert (ssa9_strg != nullptr);
  ssa9_strg->set (val5_bis);

  data_value val29_before27 = strg29->get_value ();
  data_value val27_before = val29_before27.get_at (216, CHAR_BIT);
  ASSERT_EQ (val27_before.classify (), VAL_UNDEFINED);

  ctx8.simulate (assign29_27);

  data_value val29_after27 = strg29->get_value ();
  data_value val27_after = val29_after27.get_at (216, CHAR_BIT);
  ASSERT_EQ (val27_after.classify (), VAL_KNOWN);
  wide_int wi29_27 = val27_after.get_known ();
  ASSERT_PRED1 (wi::fits_uhwi_p, wi29_27);
  ASSERT_EQ (wi29_27.to_uhwi (), 14);


  tree i10 = create_var (intSI_type_node, "i10");
  tree v10 = create_var (unsigned_intSI_type_node, "v10");

  vec<tree> decls10 {};
  decls10.safe_push (i10);
  decls10.safe_push (v10);

  context_builder builder10;
  builder10.add_decls (&decls10);
  simul_scope ctx10 = builder10.build (mem, printer);

  wide_int wi23 = wi::uhwi (23, TYPE_PRECISION (intSI_type_node));
  data_value val23 (intSI_type_node);
  val23.set_known (wi23);
  data_storage *strg_i10 = ctx10.find_reachable_var (i10);
  gcc_assert (strg_i10 != nullptr);
  strg_i10->set (val23);

  data_storage *strg_v10 = ctx10.find_reachable_var (v10);
  gcc_assert (strg_v10 != nullptr);

  data_value val10_before = strg_v10->get_value ();
  ASSERT_EQ (val10_before.classify (), VAL_UNDEFINED);

  tree conv10 = build1 (VIEW_CONVERT_EXPR, unsigned_intSI_type_node, i10);
  gimple *g10 = gimple_build_assign (v10, conv10);
  ctx10.simulate (g10);

  data_value val10_after = strg_v10->get_value ();
  ASSERT_EQ (val10_after.classify (), VAL_KNOWN);
  ASSERT_EQ (val10_after.get_bitwidth (),
	     TYPE_PRECISION (unsigned_intSI_type_node));
  wide_int wi10 = val10_after.get_known ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi10);
  ASSERT_EQ (wi10.to_shwi (), 23);
}

void
simul_scope_print_call_tests ()
{
  heap_memory mem1;
  context_printer printer1;
  pretty_printer & pp1 = printer1.pp;
  pp_buffer (&pp1)->m_flush_p = false;

  tree ac5_1 = build_array_type_nelts (char_type_node, 5);
  tree v5c_1 = create_var (ac5_1, "v5c");
  tree si_1 = create_var (short_integer_type_node, "s1");
  tree p_1 = create_var (ptr_type_node, "p");

  vec<tree> decls1{};
  decls1.safe_push (v5c_1);
  decls1.safe_push (si_1);
  decls1.safe_push (p_1);

  context_builder builder1;
  builder1.add_decls (&decls1);
  simul_scope ctx1 = builder1.build (mem1, printer1);

  wide_int wi3113_1 = wi::shwi (13 * 256 + 31,
				TYPE_PRECISION (short_integer_type_node));

  data_value val3113_1 (short_integer_type_node);
  val3113_1.set_known (wi3113_1);

  data_storage *strg1_si = ctx1.find_reachable_var (si_1);
  gcc_assert (strg1_si != nullptr);
  strg1_si->set (val3113_1);

  data_storage *strg1_v5c = ctx1.find_reachable_var (v5c_1);
  gcc_assert (strg1_v5c != nullptr);
  storage_address addr1_v5cp1 (strg1_v5c->get_ref (), CHAR_BIT);

  data_value val_v5cp1_1 (ptr_type_node);
  val_v5cp1_1.set_address (addr1_v5cp1);

  data_storage *strg1_p = ctx1.find_reachable_var (p_1);
  gcc_assert (strg1_p != nullptr);
  strg1_p->set (val_v5cp1_1);

  tree addr_si_1 = build1 (ADDR_EXPR, ptr_type_node, si_1);

  tree memcpy_fn = builtin_decl_explicit (BUILT_IN_MEMCPY);
  set_decl_built_in_function (memcpy_fn, BUILT_IN_NORMAL, BUILT_IN_MEMCPY);
  gcall * memcpy_call1 = gimple_build_call (memcpy_fn, 3, p_1, addr_si_1,
					    build_int_cst (size_type_node, 2));

  ctx1.simulate (memcpy_call1);

  const char *str1 = pp_formatted_text (&pp1);
  ASSERT_STR_STARTSWITH (str1, "__builtin_memcpy (");
  const char *line_end = ");\n";
  ASSERT_STR_CONTAINS (str1, line_end);
  const char *sub1 = strstr (str1, line_end);
  ASSERT_NE (sub1, nullptr);
  ASSERT_STREQ (sub1 + strlen (line_end), "  # v5c[1] = 31\n  # v5c[2] = 13\n");
}

void
simul_scope_simulate_call_tests ()
{
  heap_memory mem;
  context_printer printer;

  vec<tree> empty{};

  tree func_type = build_function_type (void_type_node, NULL_TREE);
  layout_type (func_type);

  simul_scope ctx = context_builder ().build (mem, printer);

  tree set_args_fn = build_decl (input_location, FUNCTION_DECL,
				 get_identifier ("_gfortran_set_args"),
				 func_type);

  gcall * set_args_call = gimple_build_call (set_args_fn, 0);

  ctx.simulate (set_args_call);

  tree set_options_fn = build_decl (input_location, FUNCTION_DECL,
				    get_identifier ("_gfortran_set_options"),
				    func_type);

  gcall * set_options_call = gimple_build_call (set_options_fn, 0);

  ctx.simulate (set_options_call);

  tree p = create_var (ptr_type_node, "p");

  vec<tree> decls{};
  decls.safe_push (p);

  heap_memory mem2;
  context_builder builder2 {};
  builder2.add_decls (&decls);
  simul_scope ctx2 = builder2.build (mem2, printer);

  tree malloc_fn = builtin_decl_explicit (BUILT_IN_MALLOC);
  tree cst = build_int_cst (size_type_node, 12);

  gcall * malloc_call = gimple_build_call (malloc_fn, 1, cst);
  gimple_set_lhs (malloc_call, p);

  ASSERT_EQ (ctx2.find_alloc (0), nullptr);

  ctx2.simulate (malloc_call);

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

  ctx2.simulate (malloc_call2);

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

  basic_block bb = init_lowered_empty_function (my_int_func, true,
						profile_count::one ());
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
  simul_scope ctx3 = builder3.build (mem3, printer);

  data_value ival = ctx3.evaluate (ivar);
  ASSERT_EQ (ival.classify (), VAL_UNDEFINED);

  ctx3.simulate (my_call);

  data_value ival2 = ctx3.evaluate (ivar);
  ASSERT_EQ (ival2.classify (), VAL_KNOWN);
  wide_int func_result = ival2.get_known ();
  ASSERT_PRED1 (wi::fits_shwi_p, func_result);
  ASSERT_EQ (func_result.to_shwi (), 6);


  tree int_func_arg_type = build_function_type_list (integer_type_node,
						     integer_type_node,
						     NULL_TREE);
  layout_type (int_func_arg_type);

  tree int_func_with_arg = build_decl (input_location, FUNCTION_DECL,
				       get_identifier ("int_func_with_arg"),
				       int_func_type);

  tree fn_result = build_decl (input_location, RESULT_DECL,
			       get_identifier ("fn_result"), integer_type_node);
  DECL_CONTEXT (fn_result) = int_func_with_arg;
  DECL_RESULT (int_func_with_arg) = fn_result;

  tree arg = build_decl (input_location, PARM_DECL,
			 get_identifier ("arg"), integer_type_node);
  DECL_ARG_TYPE (arg) = TREE_VALUE (TYPE_ARG_TYPES (int_func_arg_type));
  DECL_CONTEXT (arg) = int_func_with_arg;
  DECL_ARGUMENTS (int_func_with_arg) = arg;
  layout_decl (arg, 0);

  basic_block bb2 = init_lowered_empty_function (int_func_with_arg, true,
						 profile_count::one ());
  gimple_stmt_iterator gsi2 = gsi_last_bb (bb2);
  greturn *ret_stmt2 = gimple_build_return (arg);
  gsi_insert_after (&gsi2, ret_stmt2, GSI_CONTINUE_LINKING);

  tree i19 = build_int_cst (integer_type_node, 19);
  gcall *my_call2 = gimple_build_call (int_func_with_arg, 1, i19);

  tree ivar2 = create_var (integer_type_node, "ivar2");
  gimple_set_lhs (my_call2, ivar2);

  vec<tree> decls3{};
  decls3.safe_push (ivar2);

  heap_memory mem4;
  context_builder builder4 {};
  builder4.add_decls (&decls3);
  simul_scope ctx4 = builder4.build (mem4, printer);

  data_value ival3 = ctx4.evaluate (ivar2);
  ASSERT_EQ (ival3.classify (), VAL_UNDEFINED);

  ctx4.simulate (my_call2);

  data_value ival4 = ctx4.evaluate (ivar2);
  ASSERT_EQ (ival4.classify (), VAL_KNOWN);
  wide_int func_result2 = ival4.get_known ();
  ASSERT_PRED1 (wi::fits_shwi_p, func_result2);
  ASSERT_EQ (func_result2.to_shwi (), 19);


  tree i18 = create_var (size_type_node, "i18");

  vec<tree> decls5{};
  decls5.safe_push (p);
  decls5.safe_push (i18);

  heap_memory mem5;
  context_builder builder5 {};
  builder5.add_decls (&decls5);
  simul_scope ctx5 = builder5.build (mem5, printer);

  wide_int cst18 = wi::shwi (18, HOST_BITS_PER_LONG);
  data_value val18 (size_type_node);
  val18.set_known (cst18);
  data_storage *i18_strg = ctx5.find_reachable_var (i18);
  gcc_assert (i18_strg != nullptr);
  i18_strg->set (val18);

  gcall * malloc_call5 = gimple_build_call (malloc_fn, 1, i18);
  gimple_set_lhs (malloc_call5, p);

  ASSERT_EQ (ctx5.find_alloc (0), nullptr);

  ctx5.simulate (malloc_call5);

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

  simul_scope ctx6 = context_builder ().build (mem, printer);

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

  ctx6.simulate (simple_call);


  vec<tree> decls7{};
  decls7.safe_push (p);

  heap_memory mem7;
  context_builder builder7 {};
  builder7.add_decls (&decls7);
  simul_scope ctx7 = builder7.build (mem7, printer);

  tree s6 = build_int_cst (size_type_node, 6);
  tree s4 = build_int_cst (size_type_node, 4);

  tree calloc_fn = builtin_decl_explicit (BUILT_IN_CALLOC);
  gcall * calloc_call7 = gimple_build_call (calloc_fn, 2, s6, s4);
  gimple_set_lhs (calloc_call7, p);

  ASSERT_EQ (ctx7.find_alloc (0), nullptr);

  ctx7.simulate (calloc_call7);

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
  ASSERT_EQ (alloc_val7.classify (), VAL_KNOWN);
  wide_int wi7 = alloc_val7.get_known ();
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
  simul_scope ctx8 = builder8.build (mem8, printer);

  tree a81 = build1_loc (UNKNOWN_LOCATION, ADDR_EXPR,
			 ptr_type_node, v81);
  tree a82 = build1_loc (UNKNOWN_LOCATION, ADDR_EXPR,
			 ptr_type_node, v82);
  tree s84 = build_int_cst (size_type_node, HOST_BITS_PER_INT / CHAR_BIT);

  tree memcpy_fn = builtin_decl_explicit (BUILT_IN_MEMCPY);
  gcall * memcpy_call8 = gimple_build_call (memcpy_fn, 3, a82, a81, s84);

  data_value val17 (integer_type_node);
  wide_int wi17 = wi::uhwi (17, HOST_BITS_PER_INT);
  val17.set_known (wi17);

  data_storage * storage_v81 = ctx8.find_reachable_var (v81);
  gcc_assert (storage_v81 != nullptr);
  storage_v81->set (val17);

  data_storage *storage_v82 = ctx8.find_reachable_var (v82);
  gcc_assert (storage_v82 != nullptr);

  data_value v82_before = storage_v82->get_value ();

  ASSERT_EQ (v82_before.classify (), VAL_UNDEFINED);

  ctx8.simulate (memcpy_call8);

  data_value v82_after = storage_v82->get_value ();

  ASSERT_EQ (v82_after.classify (), VAL_KNOWN);
  wide_int wi_v82 = v82_after.get_known ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi_v82);
  ASSERT_EQ (wi_v82.to_shwi (), 17);


  tree i91 = create_var (integer_type_node, "i91");
  tree c92 = create_var (char_type_node, "c92");
  tree p93 = create_var (ptr_type_node, "p93");

  vec<tree> decls9{};
  decls9.safe_push (i91);
  decls9.safe_push (c92);
  decls9.safe_push (p93);

  heap_memory mem9;
  context_builder builder9 {};
  builder9.add_decls (&decls9);
  simul_scope ctx9 = builder9.build (mem9, printer);

  data_storage * storage_i91 = ctx9.find_reachable_var (i91);
  gcc_assert (storage_i91 != nullptr);
  storage_address addr91 (storage_i91->get_ref (), CHAR_BIT);

  data_value val_addr91 (ptr_type_node);
  val_addr91.set_address (addr91);

  data_storage * storage_p93 = ctx9.find_reachable_var (p93);
  gcc_assert (storage_p93 != nullptr);
  storage_p93->set (val_addr91);

  tree a92 = build1_loc (UNKNOWN_LOCATION, ADDR_EXPR,
			 ptr_type_node, c92);

  gcall * memcpy_call9 = gimple_build_call (memcpy_fn, 3, a92, p93,
					    size_one_node);

  data_value val91 (integer_type_node);
  wide_int wi91 = wi::uhwi (5 + 3 * 256 + 1 * 65536, HOST_BITS_PER_INT);
  val91.set_known (wi91);

  storage_i91->set (val91);

  data_storage *storage_c92 = ctx9.find_reachable_var (c92);
  gcc_assert (storage_c92 != nullptr);

  data_value c92_before = storage_c92->get_value ();

  ASSERT_EQ (c92_before.classify (), VAL_UNDEFINED);

  ctx9.simulate (memcpy_call9);

  data_value c92_after = storage_c92->get_value ();

  ASSERT_EQ (c92_after.classify (), VAL_KNOWN);
  wide_int wi_c92 = c92_after.get_known ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi_c92);
  ASSERT_EQ (wi_c92.to_shwi (), 3);


  tree i101 = create_var (integer_type_node, "i101");
  tree p102 = create_var (ptr_type_node, "p102");
  tree c5 = build_array_type_nelts (char_type_node, 5);
  tree ac105 = create_var (c5, "ac105");

  vec<tree> decls10{};
  decls10.safe_push (i101);
  decls10.safe_push (p102);
  decls10.safe_push (ac105);

  heap_memory mem10;
  context_builder builder10 {};
  builder10.add_decls (&decls10);
  simul_scope ctx10 = builder10.build (mem10, printer);

  wide_int hwi10_17 = wi::shwi (17, HOST_BITS_PER_INT);

  data_value val10_17 (integer_type_node);
  val10_17.set_known (hwi10_17);

  data_storage * storage_i101 = ctx10.find_reachable_var (i101);
  gcc_assert (storage_i101 != nullptr);
  storage_i101->set (val10_17);

  data_storage * storage_ac105 = ctx10.find_reachable_var (ac105);
  gcc_assert (storage_ac105 != nullptr);

  storage_address addr105_1 (storage_ac105->get_ref (), CHAR_BIT);
  data_value val_addr105 (ptr_type_node);
  val_addr105.set_address (addr105_1);

  data_storage * storage_p102 = ctx10.find_reachable_var (p102);
  gcc_assert (storage_p102 != nullptr);
  storage_p102->set (val_addr105);

  tree memset_fn = builtin_decl_explicit (BUILT_IN_MEMSET);
  gcall * memset_call10 = gimple_build_call (memset_fn, 3, p102, i101,
					     build_int_cst (size_type_node, 2));

  data_value val105_before = storage_ac105->get_value ();
  ASSERT_EQ (val105_before.classify (), VAL_UNDEFINED);

  ctx10.simulate (memset_call10);

  data_value val105_after = storage_ac105->get_value ();
  ASSERT_EQ (val105_after.classify (), VAL_MIXED);
  ASSERT_EQ (val105_after.classify (0, CHAR_BIT), VAL_UNDEFINED);
  ASSERT_EQ (val105_after.classify (3 * CHAR_BIT, 2 * CHAR_BIT), VAL_UNDEFINED);
  ASSERT_EQ (val105_after.classify (CHAR_BIT, 2 * CHAR_BIT), VAL_KNOWN);

  data_value val105_after1 = val105_after.get_at (CHAR_BIT, CHAR_BIT);
  wide_int wi105_after1 = val105_after1.get_known ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi105_after1);
  ASSERT_EQ (wi105_after1.to_shwi (), 17);

  data_value val105_after2 = val105_after.get_at (2 * CHAR_BIT, CHAR_BIT);
  wide_int wi105_after2 = val105_after2.get_known ();
  ASSERT_PRED1 (wi::fits_shwi_p, wi105_after2);
  ASSERT_EQ (wi105_after2.to_shwi (), 17);
}

void
simul_scope_allocate_tests ()
{
  heap_memory mem;
  context_printer printer;
  simul_scope ctx1 = context_builder ().build (mem, printer);

  data_storage & storage1 = ctx1.allocate (12);

  ASSERT_EQ (storage1.get_type (), STRG_ALLOC);


  data_storage & storage2 = ctx1.allocate (7);
  data_value val2 = storage2.get_value ();

  ASSERT_EQ (val2.get_bitwidth (), 56);
  ASSERT_EQ (val2.classify (), VAL_UNDEFINED);


  heap_memory mem3;
  simul_scope ctx3 = context_builder ().build (mem3, printer);

  ASSERT_EQ (ctx3.find_alloc (0), nullptr);

  data_storage & storage3 = ctx3.allocate (9);

  ASSERT_NE (ctx3.find_alloc (0), nullptr);
  ASSERT_EQ (ctx3.find_alloc (0), &storage3);
  ASSERT_EQ (ctx3.find_alloc (1), nullptr);


  simul_scope ctx4 = context_builder ().build (ctx3, printer);

  ASSERT_NE (ctx3.find_alloc (0), nullptr);
  ASSERT_EQ (ctx4.find_alloc (0), &storage3);


  heap_memory mem5;
  simul_scope ctx5 = context_builder ().build (mem5, printer);
  simul_scope ctx6 = context_builder ().build (ctx5, printer);

  data_storage & storage5 = ctx5.allocate (16);

  ASSERT_NE (ctx5.find_alloc (0), nullptr);
  ASSERT_EQ (ctx5.find_alloc (0), &storage5);
  ASSERT_NE (ctx6.find_alloc (0), nullptr);
  ASSERT_EQ (ctx6.find_alloc (0), &storage5);


  heap_memory mem7;
  simul_scope ctx7 = context_builder ().build (mem7, printer);
  simul_scope ctx8 = context_builder ().build (ctx7, printer);

  data_storage & storage8 = ctx8.allocate (11);

  ASSERT_NE (ctx8.find_alloc (0), nullptr);
  ASSERT_EQ (ctx8.find_alloc (0), &storage8);
  ASSERT_NE (ctx7.find_alloc (0), nullptr);
  ASSERT_EQ (ctx7.find_alloc (0), &storage8);
}


void
gimple_simulate_cc_tests ()
{
  get_constant_type_size_tests ();
  data_value_classify_tests ();
  simul_scope_find_reachable_var_tests ();
  data_value_set_address_tests ();
  data_value_set_tests ();
  data_value_set_at_tests ();
  data_value_print_tests ();
  data_value_get_at_tests ();
  data_storage_set_at_tests ();
  context_printer_print_tests ();
  context_printer_print_first_data_ref_part_tests ();
  context_printer_print_value_update_tests ();
  simul_scope_evaluate_tests ();
  simul_scope_evaluate_literal_tests ();
  simul_scope_evaluate_constructor_tests ();
  simul_scope_evaluate_unary_tests ();
  simul_scope_evaluate_binary_tests ();
  simul_scope_evaluate_condition_tests ();
  simul_scope_simulate_assign_tests ();
  simul_scope_print_call_tests ();
  simul_scope_simulate_call_tests ();
  simul_scope_allocate_tests ();
}

}

#endif
