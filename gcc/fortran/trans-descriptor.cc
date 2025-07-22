/* Copyright (C) 2002-2025 Free Software Foundation, Inc.

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
      

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tree.h"
#include "fold-const.h"
#include "gfortran.h"
#include "trans.h"
#include "trans-const.h"
#include "trans-types.h"
#include "trans-array.h"


/******************************************************************************/
/* BIND(C) array descriptor (AKA CFI array descriptor) access routines        */
/******************************************************************************/

/* Build expressions to access members of the CFI descriptor.  */
#define CFI_FIELD_BASE_ADDR 0
#define CFI_FIELD_ELEM_LEN 1
#define CFI_FIELD_VERSION 2
#define CFI_FIELD_RANK 3
#define CFI_FIELD_ATTRIBUTE 4
#define CFI_FIELD_TYPE 5
#define CFI_FIELD_DIM 6

#define CFI_DIM_FIELD_LOWER_BOUND 0
#define CFI_DIM_FIELD_EXTENT 1
#define CFI_DIM_FIELD_SM 2

static tree
gfc_get_cfi_descriptor_field (tree desc, unsigned field_idx)
{
  tree type = TREE_TYPE (desc);
  gcc_assert (TREE_CODE (type) == RECORD_TYPE
	      && TYPE_FIELDS (type)
	      && (strcmp ("base_addr",
			 IDENTIFIER_POINTER (DECL_NAME (TYPE_FIELDS (type))))
		  == 0));
  tree field = gfc_advance_chain (TYPE_FIELDS (type), field_idx);
  gcc_assert (field != NULL_TREE);

  return fold_build3_loc (input_location, COMPONENT_REF, TREE_TYPE (field),
			  desc, field, NULL_TREE);
}

tree
gfc_get_cfi_desc_base_addr (tree desc)
{
  return gfc_get_cfi_descriptor_field (desc, CFI_FIELD_BASE_ADDR);
}

tree
gfc_get_cfi_desc_elem_len (tree desc)
{
  return gfc_get_cfi_descriptor_field (desc, CFI_FIELD_ELEM_LEN);
}

tree
gfc_get_cfi_desc_version (tree desc)
{
  return gfc_get_cfi_descriptor_field (desc, CFI_FIELD_VERSION);
}

tree
gfc_get_cfi_desc_rank (tree desc)
{
  return gfc_get_cfi_descriptor_field (desc, CFI_FIELD_RANK);
}

tree
gfc_get_cfi_desc_type (tree desc)
{
  return gfc_get_cfi_descriptor_field (desc, CFI_FIELD_TYPE);
}

tree
gfc_get_cfi_desc_attribute (tree desc)
{
  return gfc_get_cfi_descriptor_field (desc, CFI_FIELD_ATTRIBUTE);
}

static tree
gfc_get_cfi_dim_item (tree desc, tree idx, unsigned field_idx)
{
  tree tmp = gfc_get_cfi_descriptor_field (desc, CFI_FIELD_DIM);
  tmp = gfc_build_array_ref (tmp, idx, NULL_TREE, true);
  tree field = gfc_advance_chain (TYPE_FIELDS (TREE_TYPE (tmp)), field_idx);
  gcc_assert (field != NULL_TREE);
  return fold_build3_loc (input_location, COMPONENT_REF, TREE_TYPE (field),
			  tmp, field, NULL_TREE);
}

tree
gfc_get_cfi_dim_lbound (tree desc, tree idx)
{
  return gfc_get_cfi_dim_item (desc, idx, CFI_DIM_FIELD_LOWER_BOUND);
}

tree
gfc_get_cfi_dim_extent (tree desc, tree idx)
{
  return gfc_get_cfi_dim_item (desc, idx, CFI_DIM_FIELD_EXTENT);
}

tree
gfc_get_cfi_dim_sm (tree desc, tree idx)
{
  return gfc_get_cfi_dim_item (desc, idx, CFI_DIM_FIELD_SM);
}

#undef CFI_FIELD_BASE_ADDR
#undef CFI_FIELD_ELEM_LEN
#undef CFI_FIELD_VERSION
#undef CFI_FIELD_RANK
#undef CFI_FIELD_ATTRIBUTE
#undef CFI_FIELD_TYPE
#undef CFI_FIELD_DIM

#undef CFI_DIM_FIELD_LOWER_BOUND
#undef CFI_DIM_FIELD_EXTENT
#undef CFI_DIM_FIELD_SM


/******************************************************************************/
/* Array descriptor low level access routines.                                */
/******************************************************************************/

/* Build expressions to access the members of an array descriptor.
   It's surprisingly easy to mess up here, so never access
   an array descriptor by "brute force", always use these
   functions.  This also avoids problems if we change the format
   of an array descriptor.

   To understand these magic numbers, look at the comments
   before gfc_build_array_type() in trans-types.cc.

   The code within these defines should be the only code which knows the format
   of an array descriptor.

   Any code just needing to read obtain the bounds of an array should use
   gfc_conv_array_* rather than the following functions as these will return
   know constant values, and work with arrays which do not have descriptors.

   Don't forget to #undef these!  */

enum descriptor_field
{
  DATA_FIELD = 0,
  OFFSET_FIELD,
  DTYPE_FIELD,
  SPAN_FIELD,
  DIMENSION_FIELD,
  CAF_TOKEN_FIELD,
};

enum dim_subfield
{
  STRIDE_SUBFIELD = 0,
  LBOUND_SUBFIELD,
  UBOUND_SUBFIELD,
};

enum dtype_subfield
{
  GFC_DTYPE_ELEM_LEN = 0,
  GFC_DTYPE_VERSION,
  GFC_DTYPE_RANK,
  GFC_DTYPE_TYPE,
  GFC_DTYPE_ATTRIBUTE
};


static tree
get_type_field (tree type, unsigned field_idx, tree field_type = NULL_TREE)
{
  tree field = gfc_advance_chain (TYPE_FIELDS (type), field_idx);
  gcc_assert (field != NULL_TREE
	      && (field_type == NULL_TREE
		  || TREE_TYPE (field) == field_type));

  return field;
}

static tree
get_ref_comp (tree ref, unsigned field_idx, tree type = NULL_TREE)
{
  tree field = get_type_field (TREE_TYPE (ref), field_idx, type);
  return fold_build3_loc (input_location, COMPONENT_REF, TREE_TYPE (field),
			  ref, field, NULL_TREE);
}


static tree
get_descr_comp (tree desc, descriptor_field field, tree type = NULL_TREE)
{
  gcc_assert (GFC_DESCRIPTOR_TYPE_P (TREE_TYPE (desc)));

  return get_ref_comp (desc, field, type);
}


static void
set_value (stmtblock_t *block, tree ref, tree value)
{
  location_t loc = input_location;
  gfc_add_modify_loc (loc, block, ref,
		      fold_convert_loc (loc, TREE_TYPE (ref), value));
}


static tree
get_descriptor_data (tree desc)
{
  return get_descr_comp (desc, DATA_FIELD);
}

/* This provides READ-ONLY access to the data field.  The field itself
   doesn't have the proper type.  */

tree
gfc_conv_descriptor_data_get (tree desc)
{
  tree type = TREE_TYPE (desc);
  gcc_assert (TREE_CODE (type) != REFERENCE_TYPE);

  location_t loc = input_location;
  tree field = get_descriptor_data (desc);
  tree target_type = GFC_TYPE_ARRAY_DATAPTR_TYPE (TREE_TYPE (desc));
  return non_lvalue_loc (loc, fold_convert_loc (loc, target_type, field));
}

/* This provides WRITE access to the data field.

   TUPLES_P is true if we are generating tuples.

   This function gets called through the following macros:
     gfc_conv_descriptor_data_set
     gfc_conv_descriptor_data_set.  */

void
gfc_conv_descriptor_data_set (stmtblock_t *block, tree desc, tree value)
{
  set_value (block, get_descriptor_data (desc), value);
}


static tree
get_descriptor_offset (tree desc)
{
  return get_descr_comp (desc, OFFSET_FIELD, gfc_array_index_type);
}

tree
gfc_conv_descriptor_offset_get (tree desc)
{
  return non_lvalue_loc (input_location, get_descriptor_offset (desc));
}

void
gfc_conv_descriptor_offset_set (stmtblock_t *block, tree desc, tree value)
{
  set_value (block, get_descriptor_offset (desc), value);
}


static tree
get_descriptor_dtype (tree desc)
{
  return get_descr_comp (desc, DTYPE_FIELD, get_dtype_type_node ());
}

tree
gfc_conv_descriptor_dtype_get (tree desc)
{
  return non_lvalue_loc (input_location, get_descriptor_dtype (desc));
}

void
gfc_conv_descriptor_dtype_set (stmtblock_t *block, tree desc, tree value)
{
  set_value (block, get_descriptor_dtype (desc), value);
}


static tree
gfc_conv_descriptor_span (tree desc)
{
  return get_descr_comp (desc, SPAN_FIELD, gfc_array_index_type);
}

tree
gfc_conv_descriptor_span_get (tree desc)
{
  return non_lvalue_loc (input_location, gfc_conv_descriptor_span (desc));
}

void
gfc_conv_descriptor_span_set (stmtblock_t *block, tree desc, tree value)
{
  set_value (block, gfc_conv_descriptor_span (desc), value);
}


static tree
get_dtype_comp (tree desc, dtype_subfield field, tree type = NULL_TREE)
{
  tree dtype_ref = get_descriptor_dtype (desc);
  return get_ref_comp (dtype_ref, field, type);
}


static tree
get_descriptor_rank (tree desc)
{
  return get_dtype_comp (desc, GFC_DTYPE_RANK, signed_char_type_node);
}

tree
gfc_conv_descriptor_rank_get (tree desc)
{
  return non_lvalue_loc (input_location, get_descriptor_rank (desc));
}

void
gfc_conv_descriptor_rank_set (stmtblock_t *block, tree desc, tree value)
{
  set_value (block, get_descriptor_rank (desc), value);
}

void
gfc_conv_descriptor_rank_set (stmtblock_t *block, tree desc, int value)
{
  gfc_conv_descriptor_rank_set (block, desc, gfc_rank_cst[value]);
}

static tree
get_descriptor_version (tree desc)
{
  return get_dtype_comp (desc, GFC_DTYPE_VERSION, integer_type_node);
}

tree
gfc_conv_descriptor_version_get (tree desc)
{
  return non_lvalue_loc (input_location, get_descriptor_version (desc));
}

void
gfc_conv_descriptor_version_set (stmtblock_t *block, tree desc, tree value)
{
  set_value (block, get_descriptor_version (desc), value);
}


/* Return the element length from the descriptor dtype field.  */

static tree
get_descriptor_elem_len (tree desc)
{
  return get_dtype_comp (desc, GFC_DTYPE_ELEM_LEN, size_type_node);
}

tree
gfc_conv_descriptor_elem_len_get (tree desc)
{
  return non_lvalue_loc (input_location, get_descriptor_elem_len (desc));
}

void
gfc_conv_descriptor_elem_len_set (stmtblock_t *block, tree desc, tree value)
{
  set_value (block, get_descriptor_elem_len (desc), value);
}


static tree
get_descriptor_type (tree desc)
{
  return get_dtype_comp (desc, GFC_DTYPE_TYPE, signed_char_type_node);
}

tree
gfc_conv_descriptor_type_get (tree desc)
{
  return non_lvalue_loc (input_location, get_descriptor_type (desc));
}

void
gfc_conv_descriptor_type_set (stmtblock_t *block, tree desc, tree value)
{
  set_value (block, get_descriptor_type (desc), value);
}

void
gfc_conv_descriptor_type_set (stmtblock_t *block, tree desc, int value)
{
  tree type = TREE_TYPE (desc);
  gcc_assert (GFC_DESCRIPTOR_TYPE_P (type));

  tree dtype = get_type_field (type, DTYPE_FIELD);

  tree field = get_type_field (TREE_TYPE (dtype), GFC_DTYPE_TYPE);

  tree type_value = build_int_cst (TREE_TYPE (field), value);
  gfc_conv_descriptor_type_set (block, desc, type_value);
}

tree
gfc_conv_descriptor_type_set (tree desc, tree value)
{
  stmtblock_t block;

  gfc_init_block (&block);
  gfc_conv_descriptor_type_set (&block, desc, value);
  return gfc_finish_block (&block);
}

tree
gfc_conv_descriptor_type_set (tree desc, int value)
{
  stmtblock_t block;

  gfc_init_block (&block);
  gfc_conv_descriptor_type_set (&block, desc, value);
  return gfc_finish_block (&block);
}


tree
gfc_get_descriptor_dimension (tree desc)
{
  tree field = get_descr_comp (desc, DIMENSION_FIELD);
  gcc_assert (TREE_CODE (TREE_TYPE (field)) == ARRAY_TYPE
	      && TREE_CODE (TREE_TYPE (TREE_TYPE (field))) == RECORD_TYPE);
  return field;
}


static tree
get_descriptor_dimension (tree desc, tree dim)
{
  tree tmp;

  tmp = gfc_get_descriptor_dimension (desc);

  return gfc_build_array_ref (tmp, dim, NULL_TREE, true);
}

tree
gfc_conv_descriptor_dimension_get (tree desc, tree dim)
{
  return non_lvalue_loc (input_location, get_descriptor_dimension (desc, dim));
}

tree
gfc_conv_descriptor_dimension_get (tree desc, int dim)
{
  return gfc_conv_descriptor_dimension_get (desc, gfc_rank_cst[dim]);
}

void
gfc_conv_descriptor_dimension_set (stmtblock_t *block, tree desc, tree dim,
				   tree value)
{
  set_value (block, get_descriptor_dimension (desc, dim), value);
}

void
gfc_conv_descriptor_dimension_set (stmtblock_t *block, tree desc, int dim,
				   tree value)
{
  gfc_conv_descriptor_dimension_set (block, desc, gfc_rank_cst[dim], value);
}


tree
gfc_conv_descriptor_token (tree desc)
{
  gcc_assert (flag_coarray == GFC_FCOARRAY_LIB);
  tree field = get_descr_comp (desc, CAF_TOKEN_FIELD);
  /* Should be a restricted pointer - except in the finalization wrapper.  */
  gcc_assert (TREE_TYPE (field) == prvoid_type_node
	      || TREE_TYPE (field) == pvoid_type_node);
  return field;
}

void
gfc_conv_descriptor_token_set (stmtblock_t *block, tree desc, tree value)
{
  set_value (block, gfc_conv_descriptor_token (desc), value);
}


static tree
get_descr_dim_comp (tree desc, tree dim, dim_subfield field,
		    tree type = NULL_TREE)
{
  tree tmp = get_descriptor_dimension (desc, dim);
  return get_ref_comp (tmp, field, type);
}

static tree
get_descriptor_stride (tree desc, tree dim)
{
  return get_descr_dim_comp (desc, dim, STRIDE_SUBFIELD, gfc_array_index_type);
}

tree
gfc_conv_descriptor_stride_get (tree desc, tree dim)
{
  tree type = TREE_TYPE (desc);
  gcc_assert (GFC_DESCRIPTOR_TYPE_P (type));
  if (integer_zerop (dim)
      && (GFC_TYPE_ARRAY_AKIND (type) == GFC_ARRAY_ALLOCATABLE
	  || GFC_TYPE_ARRAY_AKIND (type) == GFC_ARRAY_ASSUMED_SHAPE_CONT
	  || GFC_TYPE_ARRAY_AKIND (type) == GFC_ARRAY_ASSUMED_RANK_CONT
	  || GFC_TYPE_ARRAY_AKIND (type) == GFC_ARRAY_ASSUMED_RANK_ALLOCATABLE
	  || GFC_TYPE_ARRAY_AKIND (type) == GFC_ARRAY_ASSUMED_RANK_POINTER_CONT
	  || GFC_TYPE_ARRAY_AKIND (type) == GFC_ARRAY_POINTER_CONT))
    return gfc_index_one_node;

  return non_lvalue_loc (input_location, get_descriptor_stride (desc, dim));
}

void
gfc_conv_descriptor_stride_set (stmtblock_t *block, tree desc,
				tree dim, tree value)
{
  set_value (block, get_descriptor_stride (desc, dim), value);
}

static tree
get_descriptor_lbound (tree desc, tree dim)
{
  return get_descr_dim_comp (desc, dim, LBOUND_SUBFIELD, gfc_array_index_type);
}

tree
gfc_conv_descriptor_lbound_get (tree desc, tree dim)
{
  return non_lvalue_loc (input_location, get_descriptor_lbound (desc, dim));
}

void
gfc_conv_descriptor_lbound_set (stmtblock_t *block, tree desc,
				tree dim, tree value)
{
  set_value (block, get_descriptor_lbound (desc, dim), value);
}

static tree
get_descriptor_ubound (tree desc, tree dim)
{
  return get_descr_dim_comp (desc, dim, UBOUND_SUBFIELD, gfc_array_index_type);
}

tree
gfc_conv_descriptor_ubound_get (tree desc, tree dim)
{
  return non_lvalue_loc (input_location, get_descriptor_ubound (desc, dim));
}

void
gfc_conv_descriptor_ubound_set (stmtblock_t *block, tree desc,
				tree dim, tree value)
{
  set_value (block, get_descriptor_ubound (desc, dim), value);
}


/*******************************************************************************
 * Array descriptor higher level routines.                                     *
 ******************************************************************************/

/* Return the DTYPE for an array.  This describes the type and type parameters
   of the array.  */
/* TODO: Only call this when the value is actually used, and make all the
   unknown cases abort.  */

tree
gfc_get_dtype_rank_type_slen (int rank, tree etype, tree length)
{
  tree ptype;
  tree size;
  int n;
  tree dtype;
  tree field;
  vec<constructor_elt, va_gc> *v = NULL;

  ptype = etype;
  while (TREE_CODE (etype) == POINTER_TYPE
	 || TREE_CODE (etype) == ARRAY_TYPE)
    {
      ptype = etype;
      etype = TREE_TYPE (etype);
    }

  gcc_assert (etype);

  switch (TREE_CODE (etype))
    {
    case INTEGER_TYPE:
      if (TREE_CODE (ptype) == ARRAY_TYPE
	  && TYPE_STRING_FLAG (ptype))
	n = BT_CHARACTER;
      else
	{
	  if (TYPE_UNSIGNED (etype))
	    n = BT_UNSIGNED;
	  else
	    n = BT_INTEGER;
	}
      break;

    case BOOLEAN_TYPE:
      n = BT_LOGICAL;
      break;

    case REAL_TYPE:
      n = BT_REAL;
      break;

    case COMPLEX_TYPE:
      n = BT_COMPLEX;
      break;

    case RECORD_TYPE:
      if (GFC_CLASS_TYPE_P (etype))
	n = BT_CLASS;
      else
	n = BT_DERIVED;
      break;

    case FUNCTION_TYPE:
    case VOID_TYPE:
      n = BT_VOID;
      break;

    default:
      /* TODO: Don't do dtype for temporary descriptorless arrays.  */
      /* We can encounter strange array types for temporary arrays.  */
      gcc_unreachable ();
    }

  switch (n)
    {
    case BT_CHARACTER:
      gcc_assert (TREE_CODE (ptype) == ARRAY_TYPE);
      size = gfc_get_character_len_in_bytes (ptype, length);
      break;
    case BT_VOID:
      gcc_assert (TREE_CODE (ptype) == POINTER_TYPE);
      size = size_in_bytes (ptype);
      break;
    default:
      size = size_in_bytes (etype);
      break;
    }

  gcc_assert (size);

  STRIP_NOPS (size);
  size = fold_convert (size_type_node, size);
  tree dtype_type_node = get_dtype_type_node ();
  field = gfc_advance_chain (TYPE_FIELDS (dtype_type_node),
			     GFC_DTYPE_ELEM_LEN);
  CONSTRUCTOR_APPEND_ELT (v, field,
			  fold_convert (TREE_TYPE (field), size));
  field = gfc_advance_chain (TYPE_FIELDS (dtype_type_node),
			     GFC_DTYPE_VERSION);
  CONSTRUCTOR_APPEND_ELT (v, field,
			  build_zero_cst (TREE_TYPE (field)));

  field = gfc_advance_chain (TYPE_FIELDS (dtype_type_node),
			     GFC_DTYPE_RANK);
  if (rank >= 0)
    CONSTRUCTOR_APPEND_ELT (v, field,
			    build_int_cst (TREE_TYPE (field), rank));

  field = gfc_advance_chain (TYPE_FIELDS (dtype_type_node),
			     GFC_DTYPE_TYPE);
  CONSTRUCTOR_APPEND_ELT (v, field,
			  build_int_cst (TREE_TYPE (field), n));

  dtype = build_constructor (dtype_type_node, v);

  return dtype;
}


tree
gfc_get_dtype_rank_type (int rank, tree etype)
{
  return gfc_get_dtype_rank_type_slen (rank, etype, NULL_TREE);
}


/* Build a null array descriptor constructor.  */

tree
gfc_build_null_descriptor (tree type)
{
  tree field;
  tree tmp;

  gcc_assert (GFC_DESCRIPTOR_TYPE_P (type));
  gcc_assert (DATA_FIELD == 0);
  field = TYPE_FIELDS (type);

  /* Set a NULL data pointer.  */
  tmp = build_constructor_single (type, field, null_pointer_node);
  TREE_CONSTANT (tmp) = 1;
  /* All other fields are ignored.  */

  return tmp;
}


/* Obtain offsets for trans-types.cc(gfc_get_array_descr_info).  */

void
gfc_get_descriptor_offsets_for_info (const_tree desc_type, tree *data_off,
				     tree *dtype_off, tree *rank_suboff,
				     tree *span_off, tree *dim_off,
				     tree *dim_size, tree *stride_suboff,
				     tree *lower_suboff, tree *upper_suboff)
{
  tree field;
  tree type;

  type = TYPE_MAIN_VARIANT (desc_type);
  tree fields = TYPE_FIELDS (type);
  field = gfc_advance_chain (fields, DATA_FIELD);
  *data_off = byte_position (field);
  field = gfc_advance_chain (fields, DTYPE_FIELD);
  *dtype_off = byte_position (field);
  type = TREE_TYPE (field);
  field = gfc_advance_chain (TYPE_FIELDS (type), GFC_DTYPE_RANK);
  *rank_suboff = byte_position (field);
  field = gfc_advance_chain (fields, SPAN_FIELD);
  *span_off = byte_position (field);
  field = gfc_advance_chain (fields, DIMENSION_FIELD);
  *dim_off = byte_position (field);
  type = TREE_TYPE (TREE_TYPE (field));
  *dim_size = TYPE_SIZE_UNIT (type);
  field = gfc_advance_chain (TYPE_FIELDS (type), STRIDE_SUBFIELD);
  *stride_suboff = byte_position (field);
  field = gfc_advance_chain (TYPE_FIELDS (type), LBOUND_SUBFIELD);
  *lower_suboff = byte_position (field);
  field = gfc_advance_chain (TYPE_FIELDS (type), UBOUND_SUBFIELD);
  *upper_suboff = byte_position (field);
}


/* For an array descriptor, get the total number of elements.  This is just
   the product of the extents along from_dim to to_dim.  */

static tree
gfc_conv_descriptor_size_1 (tree desc, int from_dim, int to_dim)
{
  tree res;
  int dim;

  res = gfc_index_one_node;

  for (dim = from_dim; dim < to_dim; ++dim)
    {
      tree lbound;
      tree ubound;
      tree extent;

      lbound = gfc_conv_descriptor_lbound_get (desc, gfc_rank_cst[dim]);
      ubound = gfc_conv_descriptor_ubound_get (desc, gfc_rank_cst[dim]);

      extent = gfc_conv_array_extent_dim (lbound, ubound, NULL);
      res = fold_build2_loc (input_location, MULT_EXPR, gfc_array_index_type,
			     res, extent);
    }

  return res;
}


/* Full size of an array.  */

tree
gfc_conv_descriptor_size (tree desc, int rank)
{
  return gfc_conv_descriptor_size_1 (desc, 0, rank);
}


/* Size of a coarray for all dimensions but the last.  */

tree
gfc_conv_descriptor_cosize (tree desc, int rank, int corank)
{
  return gfc_conv_descriptor_size_1 (desc, rank, rank + corank - 1);
}


void
gfc_nullify_descriptor (stmtblock_t *block, tree descr)
{
  gfc_conv_descriptor_data_set (block, descr, null_pointer_node); 
}


void
gfc_init_descriptor_result (stmtblock_t *block, tree descr)
{
  gfc_conv_descriptor_data_set (block, descr, null_pointer_node);
}


void
gfc_init_absent_descriptor (stmtblock_t *block, tree descr)
{
  gfc_conv_descriptor_data_set (block, descr, null_pointer_node);
}


void
gfc_init_static_descriptor (tree descr)
{
  tree type = TREE_TYPE (descr);
  DECL_INITIAL (descr) = gfc_build_null_descriptor (type);
}


void
gfc_nullify_descriptor (stmtblock_t *block, gfc_symbol *sym, gfc_expr *expr,
			tree descr, tree string_length)
{
  symbol_attribute attr = gfc_symbol_attr (sym);

  /* NULLIFY the data pointer for non-saved allocatables, or for non-saved
     pointers when -fcheck=pointer is specified.  */
  if (attr.allocatable
      || attr.optional
      || (attr.pointer && (gfc_option.rtcheck & GFC_RTCHECK_POINTER)))
    {
      gfc_conv_descriptor_data_set (block, descr, null_pointer_node);
      if (flag_coarray == GFC_FCOARRAY_LIB && attr.codimension)
	gfc_conv_descriptor_token_set (block, descr, null_pointer_node);
    }

  gfc_array_spec *as;
  if (sym->ts.type == BT_CLASS)
    as = CLASS_DATA (sym)->as;
  else
    as = sym->as;

  int rank;
  if (as == nullptr)
    rank = 0;
  else if (as->type != AS_ASSUMED_RANK)
    rank = as->rank;
  else if (expr)
    rank = expr->rank;
  else
    rank = -1;

  tree etype = gfc_get_element_type (TREE_TYPE (descr));
  tree dtype = gfc_get_dtype_rank_type_slen (rank, etype, string_length);
  gfc_conv_descriptor_dtype_set (block, descr, dtype);
}

void
gfc_init_descriptor_variable (stmtblock_t *block, gfc_symbol *sym,
			      gfc_expr *expr, tree descr)
{
  return gfc_nullify_descriptor (block, sym, expr, descr, NULL_TREE);
}


void
gfc_init_descriptor_variable (stmtblock_t *block, gfc_symbol *sym, tree descr)
{
  return gfc_init_descriptor_variable (block, sym, nullptr, descr);
}


tree
gfc_create_null_actual_descriptor (stmtblock_t *block, gfc_typespec *ts,
				   symbol_attribute attr, int rank)
{
  tree etype = gfc_typenode_for_spec (ts);

  enum gfc_array_kind akind;

  if (attr.pointer)
    akind = GFC_ARRAY_POINTER_CONT;
  else if (attr.allocatable)
    akind = GFC_ARRAY_ALLOCATABLE;
  else
    akind = GFC_ARRAY_ASSUMED_SHAPE_CONT;

  tree lower[GFC_MAX_DIMENSIONS];
  tree upper[GFC_MAX_DIMENSIONS];
  memset (&lower, 0, rank * sizeof (lower[0]));
  memset (&upper, 0, rank * sizeof (upper[0]));

  tree type = gfc_get_array_type_bounds (etype, rank, 0, lower, upper, 1,
					 akind, !(attr.pointer || attr.target));
  tree desc = gfc_create_var (type, "desc");
  DECL_ARTIFICIAL (desc) = 1;

  gfc_conv_descriptor_dtype_set (block, desc,
				 gfc_get_dtype_rank_type (rank, etype));
  gfc_conv_descriptor_data_set (block, desc, null_pointer_node);
  gfc_conv_descriptor_span_set (block, desc,
				gfc_conv_descriptor_elem_len_get (desc));

  return desc;
}


void
gfc_set_descriptor_from_scalar_class (stmtblock_t *block, tree descr,
				      tree scalar, gfc_expr *scalar_expr)
{
  tree type = gfc_get_scalar_to_descriptor_type (TREE_TYPE (scalar),
						 gfc_expr_attr (scalar_expr));
  gfc_conv_descriptor_dtype_set (block, descr,
				 gfc_get_dtype (type));

  tree tmp = gfc_class_data_get (scalar);
  if (!POINTER_TYPE_P (TREE_TYPE (tmp)))
    tmp = gfc_build_addr_expr (NULL_TREE, tmp);

  gfc_conv_descriptor_data_set (block, descr, tmp);
}


void
gfc_set_descriptor_from_scalar (stmtblock_t *block, tree descr,
				tree scalar, gfc_expr *scalar_expr,
				tree cond_presence)
{
  tree type;
  type = gfc_get_scalar_to_descriptor_type (TREE_TYPE (scalar),
					    gfc_expr_attr (scalar_expr));
  gfc_conv_descriptor_dtype_set (block, descr,
				 gfc_get_dtype (type));
  if (cond_presence)
    scalar = build3_loc (input_location, COND_EXPR,
			 TREE_TYPE (scalar),
			 cond_presence, scalar,
			 fold_convert (TREE_TYPE (scalar),
				       null_pointer_node));
  gfc_conv_descriptor_data_set (block, descr, scalar);
}
