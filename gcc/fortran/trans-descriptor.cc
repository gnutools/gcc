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
#include "trans-descriptor.h"


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
  tmp = gfc_build_array_ref (tmp, idx, true);
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
  GFC_DTYPE_ATTRIBUTE,
  GFC_DTYPE_BYTES_COUNTED_STRIDES
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

tree
gfc_conv_descriptor_offset_units_get (tree desc)
{
  gcc_assert (!GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (desc)));
  return gfc_conv_descriptor_offset_get (desc);
}

static tree get_descriptor_elem_len (tree desc);

tree
gfc_conv_descriptor_offset_bytes_get (tree desc)
{
  if (GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (desc)))
    return gfc_conv_descriptor_offset_get (desc);
  else
    {
      tree offset_units = gfc_conv_descriptor_offset_get (desc);
      tree elem_len = get_descriptor_elem_len (desc);
      return fold_build2_loc (input_location, MULT_EXPR, gfc_array_index_type,
			      offset_units, elem_len);
    }
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


static tree
get_descriptor_bytes_counted_strides (tree desc)
{
  return get_dtype_comp (desc, GFC_DTYPE_BYTES_COUNTED_STRIDES, short_unsigned_type_node);
}

static void
gfc_conv_descriptor_bytes_counted_strides_set (stmtblock_t *block, tree desc, tree value)
{
  set_value (block, get_descriptor_bytes_counted_strides (desc), value);
}

static void
gfc_conv_descriptor_bytes_counted_strides_set (stmtblock_t *block, tree desc, int value)
{
  tree type = TREE_TYPE (desc);
  gcc_assert (GFC_DESCRIPTOR_TYPE_P (type));
  gcc_assert (value == 0 || value == 1);

  tree dtype = get_type_field (type, DTYPE_FIELD);

  tree field = get_type_field (TREE_TYPE (dtype), GFC_DTYPE_BYTES_COUNTED_STRIDES);

  tree type_value = build_int_cst (TREE_TYPE (field), value);
  gfc_conv_descriptor_bytes_counted_strides_set (block, desc, type_value);
}

static void
gfc_conv_descriptor_bytes_counted_strides_set (stmtblock_t *block, tree desc)
{
  int value = GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (desc));
  gfc_conv_descriptor_bytes_counted_strides_set (block, desc, value);
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

  return gfc_build_array_ref (tmp, dim, true);
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

tree
gfc_conv_descriptor_stride_get (tree desc, int dim)
{
  return gfc_conv_descriptor_stride_get (desc, gfc_rank_cst[dim]);
}


tree
gfc_conv_descriptor_stride_units_get (tree desc, tree dim)
{
  gcc_assert (!GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (desc)));
  return gfc_conv_descriptor_stride_get (desc, dim);
}


tree
gfc_conv_descriptor_stride_bytes_get (tree desc, tree dim)
{
  if (GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (desc)))
    return gfc_conv_descriptor_stride_get (desc, dim);
  else
    {
      tree stride_units = gfc_conv_descriptor_stride_get (desc, dim);
      tree element_len = gfc_conv_descriptor_elem_len_get (desc);
      element_len = fold_convert_loc (input_location, gfc_array_index_type,
				      element_len);
      return fold_build2_loc (input_location, MULT_EXPR, gfc_array_index_type,
			      stride_units, element_len);
    }
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

tree
gfc_conv_descriptor_lbound_get (tree desc, int dim)
{
  return gfc_conv_descriptor_lbound_get (desc, gfc_rank_cst[dim]);
}

static void
gfc_conv_descriptor_lbound_set (stmtblock_t *block, tree desc,
				tree dim, tree value)
{
  set_value (block, get_descriptor_lbound (desc, dim), value);
}

static void
gfc_conv_descriptor_lbound_set (stmtblock_t *block, tree desc,
				int dim, tree value)
{
  gfc_conv_descriptor_lbound_set (block, desc, gfc_rank_cst[dim], value);
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

tree
gfc_conv_descriptor_ubound_get (tree desc, int dim)
{
  return gfc_conv_descriptor_ubound_get (desc, gfc_rank_cst[dim]);
}

static void
gfc_conv_descriptor_ubound_set (stmtblock_t *block, tree desc,
				tree dim, tree value)
{
  set_value (block, get_descriptor_ubound (desc, dim), value);
}

static void
gfc_conv_descriptor_ubound_set (stmtblock_t *block, tree desc,
				int dim, tree value)
{
  gfc_conv_descriptor_ubound_set (block, desc, gfc_rank_cst[dim], value);
}

tree
gfc_conv_descriptor_sm_get (tree desc, tree dim)
{
  return gfc_conv_descriptor_stride_bytes_get (desc, dim);
}


tree
gfc_conv_descriptor_extent_get (tree desc, tree dim)
{
  tree lbound = gfc_conv_descriptor_lbound_get (desc, dim);
  tree ubound = gfc_conv_descriptor_ubound_get (desc, dim);

  tree tmp = fold_build2_loc (input_location, MINUS_EXPR, gfc_array_index_type,
			      lbound, gfc_index_one_node);
  return fold_build2_loc (input_location, MINUS_EXPR, gfc_array_index_type,
			  ubound, tmp);
}


/*******************************************************************************
 * Array descriptor higher level routines.                                     *
 ******************************************************************************/

/* Return the DTYPE for an array.  This describes the type and type parameters
   of the array.  */
/* TODO: Only call this when the value is actually used, and make all the
   unknown cases abort.  */

tree
gfc_get_dtype_rank_type_slen (int rank, tree etype, bool bytes_counted_strides,
			      tree length)
{
  tree ptype;
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

  tree size = NULL_TREE;
  switch (n)
    {
    case BT_CHARACTER:
      gcc_assert (TREE_CODE (ptype) == ARRAY_TYPE);
      size = gfc_get_character_len_in_bytes (ptype, length);
      break;
    case BT_VOID:
      if (TREE_CODE (ptype) == POINTER_TYPE)
	size = size_in_bytes (ptype);
      break;
    default:
      size = size_in_bytes (etype);
      break;
    }

  tree dtype_type_node = get_dtype_type_node ();
  if (size)
    {
      STRIP_NOPS (size);
      size = fold_convert (size_type_node, size);
      field = gfc_advance_chain (TYPE_FIELDS (dtype_type_node),
				 GFC_DTYPE_ELEM_LEN);
      CONSTRUCTOR_APPEND_ELT (v, field,
			      fold_convert (TREE_TYPE (field), size));
    }
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

  field = gfc_advance_chain (TYPE_FIELDS (dtype_type_node),
			     GFC_DTYPE_BYTES_COUNTED_STRIDES);
  CONSTRUCTOR_APPEND_ELT (v, field,
			  build_int_cst (TREE_TYPE (field),
					 bytes_counted_strides));

  dtype = build_constructor (dtype_type_node, v);

  return dtype;
}


tree
gfc_get_dtype_rank_type (int rank, tree etype, bool bytes_counted_strides)
{
  return gfc_get_dtype_rank_type_slen (rank, etype, bytes_counted_strides,
				       NULL_TREE);
}


enum descriptor_write_case
{
  POINTER_NULLIFY,
  RESULT_INIT,
  ABSENT_ARG_INIT,
  STATIC_INIT,
  NONSTATIC_INIT
};


class constructor_elements
{
  vec<constructor_elt, va_gc> *values;
  bool constant;

public:
  constructor_elements () : values (nullptr), constant (true) {}
  void add_value (tree elt, tree val);
  tree build (tree type);
};


void
constructor_elements::add_value (tree elt, tree val)
{
  CONSTRUCTOR_APPEND_ELT (values, elt, val);
  if (!TREE_CONSTANT (val))
    constant = false;
}


tree
constructor_elements::build (tree type)
{
  tree cstr = build_constructor (type, values);
  if (constant)
    TREE_CONSTANT (cstr) = 1;

  return cstr;
}


struct descriptor_write
{
  const enum write_type
  {
    STATIC_INIT,
    REGULAR_ASSIGN
  }
  type;

  const tree ref;

  union u
  {
    struct rw
    {
      stmtblock_t * const block;

      rw (stmtblock_t *b) : block(b) {}
    }
    regular_assign;

    constructor_elements static_init;

    u(stmtblock_t *block) : regular_assign (block) {}
    u() : static_init () {}
  }
  u;

  descriptor_write (tree r, stmtblock_t *b)
      : type (REGULAR_ASSIGN), ref (r), u (b) {}
  descriptor_write (tree d) : type (STATIC_INIT), ref (d), u () {}
};


struct value_source
{
  const descriptor_write_case type;

  union u
  {
    struct nsi
    {
      gfc_symbol * const sym;
      gfc_expr * const expr;
      tree string_length;

      nsi (gfc_symbol *s, gfc_expr *e, tree sl)
	  : sym (s), expr (e), string_length (sl) {}
    }
    nonstatic_init;

    struct si
    {
      gfc_symbol * const sym;

      si (gfc_symbol *s) : sym (s) {}
    }
    static_init;

    u () {}
    u (gfc_symbol *s) : static_init (s) {}
    u (gfc_symbol *s, gfc_expr *e, tree sl) : nonstatic_init (s, e, sl) {}
  }
  u;

  value_source (descriptor_write_case t) : type (t), u () {}
  value_source (gfc_symbol *s) : type (STATIC_INIT), u (s) {}
  value_source (gfc_symbol *s, gfc_expr *e, tree sl)
      : type (NONSTATIC_INIT), u (s, e, sl) {}
};


static void
set_descriptor_field (descriptor_write &dest, descriptor_field field, tree value)
{
  if (dest.type == descriptor_write::STATIC_INIT)
    {
      tree field_decl = gfc_advance_chain (TYPE_FIELDS (TREE_TYPE (dest.ref)),
					   field);
      dest.u.static_init.add_value (field_decl, value);
    }
  else
    {
      tree comp_ref = get_ref_comp (dest.ref, field);
      set_value (dest.u.regular_assign.block, comp_ref, value);
    }
}


static tree
get_descriptor_data_value (const value_source &src)
{
  if (src.type == NONSTATIC_INIT)
    {
      gfc_symbol *sym = src.u.nonstatic_init.sym;

      symbol_attribute attr = gfc_symbol_attr (sym);
      if (!attr.save
	  && (attr.allocatable
	      || (attr.pointer && (gfc_option.rtcheck & GFC_RTCHECK_POINTER))))
	return null_pointer_node;
      else
	return NULL_TREE;
    }
  else
    return null_pointer_node;
}


static tree
get_descriptor_dtype_value (tree descr, const value_source &src)
{
  if (src.type == NONSTATIC_INIT)
    {
      gfc_symbol *sym = src.u.nonstatic_init.sym;
      gfc_expr *expr = src.u.nonstatic_init.expr;
      tree string_length = src.u.nonstatic_init.string_length;

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

      tree type = TREE_TYPE (descr);
      tree etype = gfc_get_element_type (type);
      bool bytes_counted_strides = GFC_BYTES_STRIDES_ARRAY_TYPE_P (type);
      return gfc_get_dtype_rank_type_slen (rank, etype, bytes_counted_strides,
					   string_length);
    }
  else if (src.type == STATIC_INIT)
    {
      gfc_symbol *sym = src.u.nonstatic_init.sym;

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
      else
	rank = -1;

      return gfc_get_dtype (TREE_TYPE (descr), &rank);
    }

  return NULL_TREE;
}


static tree
get_descriptor_offset_value (const value_source &src)
{
  if (src.type == NONSTATIC_INIT)
    {
      gfc_symbol *sym = src.u.nonstatic_init.sym;

      symbol_attribute attr = gfc_symbol_attr (sym);
      if ((attr.allocatable
	   || attr.optional
	   || (attr.pointer && (gfc_option.rtcheck & GFC_RTCHECK_POINTER)))
	  && attr.codimension)
	return null_pointer_node;
    }

  return NULL_TREE;
}


static void
set_descriptor (descriptor_write &dest, const value_source &src)
{
  tree data_value = get_descriptor_data_value (src);
  if (data_value != NULL_TREE)
    set_descriptor_field (dest, DATA_FIELD, data_value);

  tree dtype_value = get_descriptor_dtype_value (dest.ref, src);
  if (dtype_value != NULL_TREE)
    set_descriptor_field (dest, DTYPE_FIELD, dtype_value);

  if (flag_coarray == GFC_FCOARRAY_LIB)
    {
      tree offset_value = get_descriptor_offset_value (src);
      if (offset_value != NULL_TREE)
	set_descriptor_field (dest, OFFSET_FIELD, offset_value);
    }

  if (dest.type == descriptor_write::STATIC_INIT)
    {
      tree decl = dest.ref;
      tree type = TREE_TYPE (decl);
      tree cstr = dest.u.static_init.build (type);
      DECL_INITIAL (decl) = cstr;
    }
  else if (dtype_value == NULL_TREE)
    gfc_conv_descriptor_bytes_counted_strides_set (dest.u.regular_assign.block,
						   dest.ref);
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
      tree extent = gfc_conv_descriptor_extent_get (desc, gfc_rank_cst[dim]);
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
  descriptor_write dest(descr, block);
  set_descriptor (dest, value_source (POINTER_NULLIFY));
}


void
gfc_init_descriptor_result (stmtblock_t *block, tree descr)
{
  descriptor_write dest(descr, block);
  set_descriptor (dest, value_source (RESULT_INIT));
}


void
gfc_init_absent_descriptor (stmtblock_t *block, tree descr)
{
  descriptor_write dest(descr, block);
  set_descriptor (dest, value_source (ABSENT_ARG_INIT));
}


void
gfc_init_static_descriptor (gfc_symbol *sym)
{
  descriptor_write dest (sym->backend_decl);
  set_descriptor (dest, value_source (sym));
}


static void
init_descriptor_variable (stmtblock_t *block, gfc_symbol *sym, gfc_expr *expr,
			  tree descr, tree string_length)
{
  descriptor_write dest (descr, block);
  set_descriptor (dest, value_source (sym, expr, string_length));
}

void
gfc_init_descriptor_variable (stmtblock_t *block, gfc_symbol *sym,
			      gfc_expr *expr, tree descr)
{
  return init_descriptor_variable (block, sym, expr, descr, NULL_TREE);
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

  bool bytes_strides = GFC_BYTES_STRIDES_ARRAY_TYPE_P (type);
  gfc_conv_descriptor_dtype_set (block, desc,
				 gfc_get_dtype_rank_type (rank, etype,
							  bytes_strides));
  gfc_conv_descriptor_data_set (block, desc, null_pointer_node);
  gfc_conv_descriptor_span_set (block, desc,
				gfc_conv_descriptor_elem_len_get (desc));

  return desc;
}


void
gfc_set_descriptor_from_scalar (stmtblock_t *block, tree descr, tree scalar,
				symbol_attribute attr,
				tree cond_presence, tree caf_token)
{
  if (flag_coarray == GFC_FCOARRAY_LIB && caf_token)
    gfc_conv_descriptor_token_set (block, descr, caf_token);

  tree type = gfc_get_scalar_to_descriptor_type (TREE_TYPE (scalar), attr);
  if (!POINTER_TYPE_P (TREE_TYPE (scalar)))
    scalar = gfc_build_addr_expr (NULL_TREE, scalar);
  if (cond_presence)
    scalar = build3_loc (input_location, COND_EXPR,
			 TREE_TYPE (scalar),
			 cond_presence, scalar,
			 fold_convert (TREE_TYPE (scalar),
				       null_pointer_node));

  gfc_conv_descriptor_dtype_set (block, descr, gfc_get_dtype (type));
  gfc_conv_descriptor_data_set (block, descr, scalar);
  gfc_conv_descriptor_span_set (block, descr,
				gfc_conv_descriptor_elem_len_get (descr));

  int corank = GFC_TYPE_ARRAY_CORANK (TREE_TYPE (descr));
  if (corank != 0)
    {
      tree type = TREE_TYPE (scalar);
      if (POINTER_TYPE_P (type))
	type = TREE_TYPE (type);
      gcc_assert (GFC_TYPE_ARRAY_CORANK (type) == corank);
      for (int i = 0; i < corank; i++)
	{
	  gfc_conv_descriptor_lbound_set (block, descr, i,
					  GFC_TYPE_ARRAY_LBOUND (type, i));
	  if (i < corank - 1)
	    gfc_conv_descriptor_ubound_set (block, descr, i,
					    GFC_TYPE_ARRAY_UBOUND (type, i));
	}
    }
}


void
gfc_set_descriptor_from_scalar_class (stmtblock_t *block, tree descr,
				      tree scalar, gfc_expr *scalar_expr)
{
  gfc_set_descriptor_from_scalar (block, descr, gfc_class_data_get (scalar),
				  gfc_expr_attr (scalar_expr));
}


void
gfc_set_descriptor_from_scalar (stmtblock_t *block, tree descr,
				tree scalar, gfc_expr *scalar_expr,
				tree cond_presence, tree caf_token)
{
  gfc_set_descriptor_from_scalar (block, descr, scalar,
				  gfc_expr_attr (scalar_expr), cond_presence,
				  caf_token);
}


static void
set_dimension_bounds (stmtblock_t * block, tree descr, tree dim,
		      tree lbound, tree ubound, tree stride, tree *offset)
{
  lbound = gfc_evaluate_now (lbound, block);

  gfc_conv_descriptor_ubound_set (block, descr, dim, ubound);

  tree tmp = fold_build2_loc (input_location, MULT_EXPR,
			      gfc_array_index_type, lbound, stride);
  *offset = fold_build2_loc (input_location, MINUS_EXPR,
			     gfc_array_index_type, *offset, tmp);

  /* Finally set lbound to value we want.  */
  gfc_conv_descriptor_lbound_set (block, descr, dim, lbound);
}


static void
set_dimension_bounds (stmtblock_t * block, tree descr, tree dim,
		      tree lbound, tree ubound, tree stride, tree offset_var)
{
  tree offset = offset_var;
  set_dimension_bounds (block, descr, dim, lbound, ubound, stride, &offset);
  gfc_add_modify (block, offset_var, offset);
}


static void
set_dimension_fields (stmtblock_t * block, tree descr, tree dim,
		      tree lbound, tree ubound, tree stride, tree *offset)
{
  stride = gfc_evaluate_now (stride, block);
  set_dimension_bounds (block, descr, dim, lbound, ubound, stride, offset);
  gfc_conv_descriptor_stride_set (block, descr, dim, stride);
}


static void
set_dimension_fields (stmtblock_t * block, tree descr, tree dim,
		      tree lbound, tree ubound, tree stride, tree offset_var)
{
  stride = gfc_evaluate_now (stride, block);
  set_dimension_bounds (block, descr, dim, lbound, ubound, stride, offset_var);
  gfc_conv_descriptor_stride_set (block, descr, dim, stride);
}

static void
shift_dimension_bounds (stmtblock_t * block, tree descr, tree dim,
			tree new_lbound, tree orig_lbound, tree orig_ubound,
			tree orig_stride, tree *offset_value)
{
  new_lbound = fold_convert (gfc_array_index_type, new_lbound);

  orig_stride = gfc_evaluate_now (orig_stride, block);

  /* Get difference (new - old) by which to shift stuff.  */
  tree diff = fold_build2_loc (input_location, MINUS_EXPR, gfc_array_index_type,
			       new_lbound, orig_lbound);

  /* Shift ubound and offset accordingly.  This has to be done before
     updating the lbound, as they depend on the lbound expression!  */
  tree ubound = fold_build2_loc (input_location, PLUS_EXPR,
				 gfc_array_index_type, orig_ubound, diff);

  set_dimension_bounds (block, descr, dim, new_lbound, ubound, orig_stride,
			offset_value);
}


static void
shift_dimension_fields (stmtblock_t * block, tree descr, tree dim,
			tree new_lbound, tree orig_lbound, tree orig_ubound,
			tree orig_stride, tree *offset_value)
{
  tree stride = gfc_evaluate_now (orig_stride, block);
  shift_dimension_bounds (block, descr, dim, new_lbound, orig_lbound, orig_ubound,
			  stride, offset_value);
  gfc_conv_descriptor_stride_set (block, descr, dim, stride);
}


/* Modify a descriptor such that the lbound of a given dimension is the value
   specified.  This also updates ubound and offset accordingly.  */

static void
conv_shift_descriptor_lbound (stmtblock_t* block, tree desc,
			      int dim, tree new_lbound, tree *offset)
{
  tree ubound, lbound, stride;

  new_lbound = fold_convert (gfc_array_index_type, new_lbound);

  lbound = gfc_conv_descriptor_lbound_get (desc, gfc_rank_cst[dim]);
  ubound = gfc_conv_descriptor_ubound_get (desc, gfc_rank_cst[dim]);
  stride = gfc_conv_descriptor_stride_get (desc, gfc_rank_cst[dim]);

  shift_dimension_bounds (block, desc, gfc_rank_cst[dim], new_lbound, lbound,
			  ubound, stride, offset);
}


void
gfc_conv_shift_descriptor (stmtblock_t* block, tree desc, int rank)
{
  /* Apply a shift of the lbound when supplied.  */
  tree offset = gfc_index_zero_node;
  for (int dim = 0; dim < rank; ++dim)
    conv_shift_descriptor_lbound (block, desc, dim, gfc_index_one_node,
				  &offset);
  gfc_conv_descriptor_offset_set (block, desc, offset);
}


static void
conv_shift_descriptor (stmtblock_t *block, tree desc, int rank,
		       gfc_expr * const (lbound[GFC_MAX_DIMENSIONS]))
{
  /* Apply a shift of the lbound when supplied.  */
  tree offset = gfc_index_zero_node;
  for (int dim = 0; dim < rank; ++dim)
    {
      gfc_expr *lb_expr = lbound[dim];

      tree lower_bound;
      if (lb_expr == nullptr)
	lower_bound = gfc_index_one_node;
      else
	{
	  gfc_se lb_se;

	  gfc_init_se (&lb_se, nullptr);
	  gfc_conv_expr (&lb_se, lb_expr);

	  gfc_add_block_to_block (block, &lb_se.pre);
	  tree lb_var = gfc_create_var (TREE_TYPE (lb_se.expr), "lower_bound");
	  gfc_add_modify (block, lb_var, lb_se.expr);
	  gfc_add_block_to_block (block, &lb_se.post);

	  lower_bound = lb_var;
	}

      conv_shift_descriptor_lbound (block, desc, dim, lower_bound, &offset);
    }
  gfc_conv_descriptor_offset_set (block, desc, offset);
}


static void
conv_shift_descriptor (stmtblock_t *block, tree desc,
		       const gfc_array_spec &as)
{
  conv_shift_descriptor (block, desc, as.rank, as.lower);
}


static void
set_type (array_type &type, array_type value)
{
  gcc_assert (type == AS_UNKNOWN || type == value);
  type = value;
}


static void
array_ref_to_array_spec (const gfc_array_ref &ref, gfc_array_spec &spec)
{
  spec.rank = ref.dimen;
  spec.corank = ref.codimen;

  spec.type = AS_UNKNOWN;
  spec.cotype = AS_ASSUMED_SIZE;

  for (int dim = 0; dim < spec.rank + spec.corank; dim++)
    switch (ref.dimen_type[dim])
      {
      case DIMEN_ELEMENT:
	spec.upper[dim] = ref.start[dim];
	set_type (spec.type, AS_EXPLICIT);
	break;

      case DIMEN_RANGE:
	spec.lower[dim] = ref.start[dim];
	spec.upper[dim] = ref.end[dim];
	if (spec.upper[dim] == nullptr)
	  set_type (spec.type, AS_DEFERRED);
	else
	  set_type (spec.type, AS_EXPLICIT);
	break;

      default:
	break;
      }
}


void
gfc_conv_shift_descriptor (stmtblock_t *block, tree desc,
			   const gfc_array_ref &ar)
{
  gfc_array_spec as;

  array_ref_to_array_spec (ar, as);

  conv_shift_descriptor (block, desc, as);
}


void
gfc_conv_shift_descriptor (stmtblock_t *block, tree dest, tree src,
			   int rank, tree zero_cond)
{
  gcc_assert (GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (dest))
	      == GFC_BYTES_STRIDES_ARRAY_TYPE_P  (TREE_TYPE (src)));

  tree tmp = gfc_conv_descriptor_data_get (src);
  gfc_conv_descriptor_data_set (block, dest, tmp);

  tree offset = gfc_index_zero_node;
  for (int n = 0 ; n < rank; n++)
    {
      tree lbound = gfc_conv_descriptor_lbound_get (dest, gfc_rank_cst[n]);
      lbound = fold_build3_loc (input_location, COND_EXPR,
				gfc_array_index_type, zero_cond,
				gfc_index_one_node, lbound);
      lbound = gfc_evaluate_now (lbound, block);

      tree dim = gfc_rank_cst[n];
      tree stride = gfc_conv_descriptor_stride_get (src, dim);
      shift_dimension_fields (block, dest, gfc_rank_cst[n],
			      lbound, gfc_index_zero_node,
			      gfc_conv_descriptor_ubound_get (src, dim),
			      stride, &offset);
    }

  gfc_conv_descriptor_offset_set (block, dest, offset);
}


void
gfc_set_subarray_descriptor (stmtblock_t *block, tree descr, tree value,
			     gfc_expr *value_expr, gfc_expr *conv_arg)
{
  if (value_expr->expr_type != EXPR_VARIABLE)
    gfc_conv_descriptor_data_set (block, value,
				  null_pointer_node);

  /* Obtain the array spec of full array references.  */
  gfc_array_spec *as;
  if (conv_arg)
    as = gfc_get_full_arrayspec_from_expr (conv_arg);
  else
    as = gfc_get_full_arrayspec_from_expr (value_expr);

  /* Shift the lbound and ubound of temporaries to being unity,
     rather than zero, based. Always calculate the offset.  */
  tree offset = gfc_index_zero_node;

  for (int n = 0; n < value_expr->rank; n++)
    {
      tree lbound;

      /* Obtain the correct lbound - ISO/IEC TR 15581:2001 page 9.
	 TODO It looks as if gfc_conv_expr_descriptor should return
	 the correct bounds and that the following should not be
	 necessary.  This would simplify gfc_conv_intrinsic_bound
	 as well.  */
      if (as && as->lower[n])
	{
	  gfc_se lbse;
	  gfc_init_se (&lbse, NULL);
	  gfc_conv_expr (&lbse, as->lower[n]);
	  gfc_add_block_to_block (block, &lbse.pre);
	  lbound = gfc_evaluate_now (lbse.expr, block);
	}
      else if (as && conv_arg)
	{
	  tree tmp = gfc_get_symbol_decl (conv_arg->symtree->n.sym);
	  lbound = gfc_conv_descriptor_lbound_get (tmp, gfc_rank_cst[n]);
	}
      else if (as)
	lbound = gfc_conv_descriptor_lbound_get (descr, gfc_rank_cst[n]);
      else
	lbound = gfc_index_one_node;

      lbound = fold_convert (gfc_array_index_type, lbound);

      /* Shift the bounds and set the offset accordingly.  */
      tree dim = gfc_rank_cst[n];
      shift_dimension_bounds (block, descr, dim, lbound,
			      gfc_conv_descriptor_lbound_get (descr, dim),
			      gfc_conv_descriptor_ubound_get (descr, dim),
			      gfc_conv_descriptor_stride_get (descr, dim),
			      &offset);
    }
  gfc_conv_descriptor_offset_set (block, descr, offset);
}


void
gfc_shift_descriptor (stmtblock_t *block, tree descr, int rank,
		      tree lbound[GFC_MAX_DIMENSIONS],
		      tree ubound[GFC_MAX_DIMENSIONS])
{
  tree size = gfc_index_one_node;
  tree offset = gfc_index_zero_node;
  for (int n = 0; n < rank; n++)
    {
      tree dim = gfc_rank_cst[n];
      shift_dimension_bounds (block, descr, dim, gfc_index_one_node,
			      lbound[n], ubound[n], size, &offset);

      tree tmp = fold_build2_loc (input_location, MINUS_EXPR,
				  gfc_array_index_type, ubound[n], lbound[n]);
      tmp = fold_build2_loc (input_location, PLUS_EXPR,
			     gfc_array_index_type, tmp, gfc_index_one_node);
      size = fold_build2_loc (input_location, MULT_EXPR,
			      gfc_array_index_type, size, tmp);
    }

  gfc_conv_descriptor_offset_set (block, descr, offset);
}
 

void
gfc_copy_sequence_descriptor (stmtblock_t *block, tree dest, tree src, int rank)
{
  gcc_assert (GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (dest))
	      == GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (src)));

  gfc_conv_descriptor_data_set (block, dest,
				gfc_conv_descriptor_data_get (src));
  gfc_conv_descriptor_lbound_set (block, dest, gfc_index_zero_node,
				  gfc_index_zero_node);
  gfc_conv_descriptor_ubound_set (block, dest, gfc_index_zero_node,
				  gfc_conv_descriptor_size (src, rank));
  tree stride = gfc_conv_descriptor_stride_get (src, gfc_index_zero_node);
  gfc_conv_descriptor_stride_set (block, dest, gfc_index_zero_node, stride);
  gfc_conv_descriptor_dtype_set (block, dest,
				 gfc_conv_descriptor_dtype_get (src));
  gfc_conv_descriptor_rank_set (block, dest, 1);
  gfc_conv_descriptor_span_set (block, dest,
				gfc_conv_descriptor_span_get (src));
  gfc_conv_descriptor_offset_set (block, dest, gfc_index_zero_node);
}


void
gfc_copy_descriptor (stmtblock_t *block, tree dest, tree src,
		     gfc_expr *src_expr, bool subref)
{
  if (GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (dest))
      == GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (src)))
    {
      struct lang_type *dest_ls = TYPE_LANG_SPECIFIC (TREE_TYPE (dest));
      struct lang_type *src_ls = TYPE_LANG_SPECIFIC (TREE_TYPE (src));

      /* When only the array_kind differs, do a view_convert.  */
      tree tmp1;
      if (dest_ls
	  && src_ls
	  && dest_ls->rank == src_ls->rank
	  && dest_ls->akind != src_ls->akind)
	tmp1 = build1 (VIEW_CONVERT_EXPR, TREE_TYPE (dest), src);
      else
	tmp1 = src;

      /* Copy the descriptor for pointer assignments.  */
      gfc_add_modify (block, dest, tmp1);
    }
  else
    {
      gcc_assert (GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (dest))
		  && !GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (src)));
      gfc_copy_descriptor (block, dest, src, src_expr);
    }

  /* Add any offsets from subreferences.  */
  gfc_get_dataptr_offset (block, dest, src, NULL_TREE, subref, src_expr);

  /* ....and set the span field.  */
  tree tmp2;
  if (src_expr->ts.type == BT_CHARACTER)
    tmp2 = gfc_conv_descriptor_span_get (src);
  else
    tmp2 = gfc_get_array_span (src, src_expr);
  gfc_conv_descriptor_span_set (block, dest, tmp2);
}


void
gfc_copy_descriptor (stmtblock_t *block, tree dest, tree src, bool lhs_type)
{
  if (GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (dest))
      == GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (src)))
    {
      gfc_conv_descriptor_data_set (block, dest,
				    gfc_conv_descriptor_data_get (src));
      gfc_conv_descriptor_offset_set (block, dest,
				      gfc_conv_descriptor_offset_get (src));

      gfc_conv_descriptor_dtype_set (block, dest,
				     gfc_conv_descriptor_dtype_get (src));

      gfc_conv_descriptor_span_set (block, dest,
				    gfc_conv_descriptor_span_get (src));

      /* Assign the dimension as range-ref.  */
      tree tmp = gfc_get_descriptor_dimension (dest);
      tree tmp2 = gfc_get_descriptor_dimension (src);

      tree type = lhs_type ? TREE_TYPE (tmp) : TREE_TYPE (tmp2);
      tmp = build4_loc (input_location, ARRAY_RANGE_REF, type, tmp,
			gfc_index_zero_node, NULL_TREE, NULL_TREE);
      tmp2 = build4_loc (input_location, ARRAY_RANGE_REF, type, tmp2,
			 gfc_index_zero_node, NULL_TREE, NULL_TREE);
      gfc_add_modify (block, tmp, tmp2);
    }
  else
    {
      gcc_assert (lhs_type);
      gfc_copy_descriptor (block, dest, src);
    }
}


void
gfc_copy_descriptor (stmtblock_t *block, tree dst, tree src, int rank)
{
  int n;
  tree dim;
  tree tmp;
  tree tmp2;
  tree size;
  tree offset;

  gcc_assert (GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (dst))
	      == GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (src)));

  offset = gfc_index_zero_node;

  /* Use memcpy to copy the descriptor. The size is the minimum of
     the sizes of 'src' and 'dst'. This avoids a non-trivial conversion.  */
  tmp = TYPE_SIZE_UNIT (TREE_TYPE (src));
  tmp2 = TYPE_SIZE_UNIT (TREE_TYPE (dst));
  size = fold_build2_loc (input_location, MIN_EXPR,
			  TREE_TYPE (tmp), tmp, tmp2);
  tmp = builtin_decl_explicit (BUILT_IN_MEMCPY);
  tmp = build_call_expr_loc (input_location, tmp, 3,
			     gfc_build_addr_expr (NULL_TREE, dst),
			     gfc_build_addr_expr (NULL_TREE, src),
			     fold_convert (size_type_node, size));
  gfc_add_expr_to_block (block, tmp);

  /* Set the offset correctly.  */
  for (n = 0; n < rank; n++)
    {
      dim = gfc_rank_cst[n];
      tmp = gfc_conv_descriptor_lbound_get (src, dim);
      tmp2 = gfc_conv_descriptor_stride_get (src, dim);
      tmp = fold_build2_loc (input_location, MULT_EXPR, TREE_TYPE (tmp),
			     tmp, tmp2);
      offset = fold_build2_loc (input_location, MINUS_EXPR,
			TREE_TYPE (offset), offset, tmp);
      offset = gfc_evaluate_now (offset, block);
    }

  gfc_conv_descriptor_offset_set (block, dst, offset);
}


void
gfc_copy_descriptor (stmtblock_t *block, tree dest, tree src, tree ptr,
		     int rank, gfc_ss *ss)
{
  gcc_assert (GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (dest))
	      == GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (src)));

  gfc_conv_descriptor_dtype_set (block, dest,
				 gfc_conv_descriptor_dtype_get (src));

  gfc_conv_descriptor_offset_set (block, dest,
				  gfc_conv_descriptor_offset_get (src));

  for (int i = 0; i < rank; i++)
    {
      int idx = gfc_get_array_ref_dim_for_loop_dim (ss, i);
      tree old_field = gfc_conv_descriptor_dimension_get (src, idx);
      gfc_conv_descriptor_dimension_set (block, dest, i, old_field);
    }

  if (flag_coarray == GFC_FCOARRAY_LIB
      && GFC_DESCRIPTOR_TYPE_P (TREE_TYPE (src))
      && GFC_TYPE_ARRAY_AKIND (TREE_TYPE (src)) == GFC_ARRAY_ALLOCATABLE)
    gfc_conv_descriptor_token_set (block, dest,
				   gfc_conv_descriptor_token (src));

  gfc_conv_descriptor_data_set (block, dest, ptr);
}


static tree
find_parent_coarray_descriptor (tree t)
{
  do
    {
      switch (TREE_CODE (t))
	{
	case COMPONENT_REF:
	case INDIRECT_REF:
	case NOP_EXPR:
	  t = TREE_OPERAND (t, 0);
	  break;

	default:
	  gcc_unreachable ();
	}

      tree type = TREE_TYPE (t);
      if (GFC_DESCRIPTOR_TYPE_P (type)
	  && TYPE_LANG_SPECIFIC (type)
	  && GFC_TYPE_ARRAY_CORANK (type) > 0)
	return t;
    }
  while (!DECL_P (t));

  return NULL_TREE;
}


void
gfc_copy_descriptor (stmtblock_t *block, tree dest, tree src)
{
  if (GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (dest))
      == GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (src)))
    gfc_add_modify (block, dest, src);
  else
    {
      gfc_conv_descriptor_data_set (block, dest,
				    gfc_conv_descriptor_data_get (src));
      gfc_conv_descriptor_dtype_set (block, dest,
				     gfc_conv_descriptor_dtype_get (src));
      gfc_conv_descriptor_bytes_counted_strides_set (block, dest);
      gfc_conv_descriptor_span_set (block, dest,
				    gfc_conv_descriptor_span_get (src));

      tree element_len = gfc_conv_descriptor_elem_len_get (src);
      element_len = fold_convert_loc (input_location, gfc_array_index_type,
				      element_len);

      tree offset = gfc_index_zero_node;

      gcc_assert (GFC_TYPE_ARRAY_RANK (TREE_TYPE (dest))
		  == GFC_TYPE_ARRAY_RANK (TREE_TYPE (src)));
      int rank = GFC_TYPE_ARRAY_RANK (TREE_TYPE (dest));
      for (int i = 0; i < rank; i++) 
	{
	  tree lbound = gfc_conv_descriptor_lbound_get (src, i);
	  tree ubound = gfc_conv_descriptor_ubound_get (src, i);
	  tree stride_raw = gfc_conv_descriptor_stride_get (src, i);
	  gcc_assert (GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (dest)));
	  tree stride = fold_build2_loc (input_location, MULT_EXPR,
					 gfc_array_index_type, stride_raw,
					 element_len);
	  set_dimension_fields (block, dest, gfc_rank_cst[i],
				lbound, ubound, stride, &offset);
	}
      gfc_conv_descriptor_offset_set (block, dest, offset);

      int corank = GFC_TYPE_ARRAY_CORANK (TREE_TYPE (dest));
      if (corank > 0)
	{
	  tree codims_src_descr;
	  if (GFC_TYPE_ARRAY_CORANK (TREE_TYPE (src)) > 0)
	    {
	      gcc_assert (GFC_TYPE_ARRAY_CORANK (TREE_TYPE (src)) == corank);
	      codims_src_descr = src;
	    }
	  else
	    /* We may pointer assign a non-coarray target to a non-coarray
	       pointer subobject of a coarray.  Get the bounds from the parent
	       coarray in that case.  */
	    codims_src_descr = find_parent_coarray_descriptor (dest);

	  int codims_src_rank = GFC_TYPE_ARRAY_RANK (TREE_TYPE (codims_src_descr));
	  gcc_assert (GFC_TYPE_ARRAY_CORANK (TREE_TYPE (codims_src_descr))
		      == corank);
	  for (int i = 0; i < corank; i++)
	    {
	      int src_index = codims_src_rank + i;
	      tree lbound = gfc_conv_descriptor_lbound_get (codims_src_descr,
							    src_index);
	      gfc_conv_descriptor_lbound_set (block, dest, rank + i, lbound);
	      if (i < corank - 1)
		{
		  tree ubound = gfc_conv_descriptor_ubound_get (codims_src_descr,
								src_index);
		  gfc_conv_descriptor_ubound_set (block, dest, i, ubound);
		}
	    }
	}

      if (flag_coarray == GFC_FCOARRAY_LIB)
	gfc_conv_descriptor_token_set (block, dest,
				       gfc_conv_descriptor_token (src));
    }
}


void
gfc_conv_remap_descriptor (stmtblock_t *block, tree dest, int dest_rank,
			   tree src, bool contiguous_src, gfc_array_ref *ar)
{
  gcc_assert (GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (dest))
	      == GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (src)));

  /* Set dtype.  */
  gfc_conv_descriptor_dtype_set (block, dest,
				 gfc_get_dtype (TREE_TYPE (dest)));

  /* Copy data pointer.  */
  gfc_conv_descriptor_data_set (block, dest,
				gfc_conv_descriptor_data_get (src));

  /* Copy the span.  */
  tree span;
  if (VAR_P (src)
      && GFC_DECL_PTR_ARRAY_P (src))
    span = gfc_conv_descriptor_span_get (src);
  else
    {
      tree tmp = TREE_TYPE (src);
      tmp = TYPE_SIZE_UNIT (gfc_get_element_type (tmp));
      span = fold_convert (gfc_array_index_type, tmp);
    }
  gfc_conv_descriptor_span_set (block, dest, span);

  /* Copy offset but adjust it such that it would correspond
     to a lbound of zero.  */
  tree offset = gfc_index_zero_node;

  /* Set the bounds as declared for the LHS and calculate strides as
     well as another offset update accordingly.  */
  tree stride;
  if (contiguous_src)
    stride = gfc_index_one_node;
  else
    stride = gfc_conv_descriptor_stride_bytes_get (src, gfc_rank_cst[0]);

  for (int dim = 0; dim < dest_rank; ++dim)
    {
      gfc_se lower_se;
      gfc_se upper_se;

      gcc_assert (ar->start[dim] && ar->end[dim]);

      if (ar->start[dim]->expr_type != EXPR_CONSTANT
	  || ar->start[dim]->expr_type != EXPR_VARIABLE)
	gfc_resolve_expr (ar->start[dim]);
      if (ar->end[dim]->expr_type != EXPR_CONSTANT
	  || ar->end[dim]->expr_type != EXPR_VARIABLE)
	gfc_resolve_expr (ar->end[dim]);

      /* Convert declared bounds.  */
      gfc_init_se (&lower_se, NULL);
      gfc_init_se (&upper_se, NULL);
      gfc_conv_expr (&lower_se, ar->start[dim]);
      gfc_conv_expr (&upper_se, ar->end[dim]);

      gfc_add_block_to_block (block, &lower_se.pre);
      gfc_add_block_to_block (block, &upper_se.pre);

      tree lbound = fold_convert (gfc_array_index_type, lower_se.expr);
      tree ubound = fold_convert (gfc_array_index_type, upper_se.expr);

      lbound = gfc_evaluate_now (lbound, block);
      ubound = gfc_evaluate_now (ubound, block);

      gfc_add_block_to_block (block, &lower_se.post);
      gfc_add_block_to_block (block, &upper_se.post);

      stride = gfc_evaluate_now (stride, block);

      set_dimension_fields (block, dest, gfc_rank_cst[dim],
			    lbound, ubound, stride, &offset);

      /* Update stride.  */
      tree tmp = gfc_conv_array_extent_dim (lbound, ubound, NULL);
      stride = fold_build2_loc (input_location, MULT_EXPR,
				gfc_array_index_type, stride, tmp);
    }
  gfc_conv_descriptor_offset_set (block, dest, offset);
}


void
gfc_set_descriptor (stmtblock_t *block, tree dest, tree src, gfc_expr *src_expr,
		    int rank, int corank, gfc_ss *ss, gfc_array_info *info,
		    tree lowers[GFC_MAX_DIMENSIONS],
		    tree uppers[GFC_MAX_DIMENSIONS],
		    bool unlimited_polymorphic, bool data_needed,
		    bool subref)
{
  gcc_assert (GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (dest))
	      || !GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (src)));

  int ndim = info->ref ? info->ref->u.ar.dimen : rank;

  /* Set the span field.  */
  tree tmp = NULL_TREE;
  if (GFC_DESCRIPTOR_TYPE_P (TREE_TYPE (src)))
    tmp = gfc_conv_descriptor_span_get (src);
  else
    tmp = gfc_get_array_span (src, src_expr);
  if (tmp)
    gfc_conv_descriptor_span_set (block, dest, tmp);

  /* The following can be somewhat confusing.  We have two
     descriptors, a new one and the original array.
     {parm, parmtype, dim} refer to the new one.
     {desc, type, n, loop} refer to the original, which maybe
     a descriptorless array.
     The bounds of the scalarization are the bounds of the section.
     We don't have to worry about numeric overflows when calculating
     the offsets because all elements are within the array data.  */

  /* Set the dtype.  */
  tree dtype;
  if (unlimited_polymorphic)
    {
      if (UNLIMITED_POLY (src_expr))
	{
	  tree tmp2 = src;
	  if (TREE_CODE (tmp2) == INDIRECT_REF
	      && DECL_P (TREE_OPERAND (tmp2, 0)))
	    tmp2 = TREE_OPERAND (tmp2, 0);
	  if (DECL_P (tmp2)
	      && DECL_LANG_SPECIFIC (tmp2)
	      && GFC_DECL_SAVED_DESCRIPTOR (tmp2))
	    tmp2 = GFC_DECL_SAVED_DESCRIPTOR (tmp2);
	  tmp2 = gfc_class_data_get (tmp2);
	  if (POINTER_TYPE_P (TREE_TYPE (tmp2)))
	    tmp2 = build_fold_indirect_ref_loc (input_location, tmp2);
	  dtype = gfc_conv_descriptor_dtype_get (tmp2);
	}
      else
	dtype = gfc_get_dtype (TREE_TYPE (src), &rank);
    }
  else if (src_expr->ts.type == BT_ASSUMED)
    {
      tree tmp2 = src;
      if (DECL_LANG_SPECIFIC (tmp2) && GFC_DECL_SAVED_DESCRIPTOR (tmp2))
	tmp2 = GFC_DECL_SAVED_DESCRIPTOR (tmp2);
      if (POINTER_TYPE_P (TREE_TYPE (tmp2)))
	tmp2 = build_fold_indirect_ref_loc (input_location, tmp2);
      dtype = gfc_conv_descriptor_dtype_get (tmp2);
    }
  else
    dtype = gfc_get_dtype (TREE_TYPE (dest));
  gfc_conv_descriptor_dtype_set (block, dest, dtype);

  /* The 1st element in the section.  */
  tree base = gfc_index_zero_node;
  if (src_expr->ts.type == BT_CHARACTER && src_expr->rank == 0 && corank)
    base = gfc_index_one_node;

  /* The offset from the 1st element in the section.  */
  tree offset = gfc_index_zero_node;

  for (int n = 0; n < ndim; n++)
    {
      tree src_stride = gfc_conv_array_stride (src, n);
      src_stride = gfc_evaluate_now (src_stride, block);

      tree stride;
      if (GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (dest))
	  == GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (src)))
	stride = src_stride;
      else
	stride = gfc_conv_array_stride_bytes (src, n);

      /* Work out the 1st element in the section.  */
      tree start;
      if (info->ref
	  && info->ref->u.ar.dimen_type[n] == DIMEN_ELEMENT)
	{
	  gcc_assert (info->subscript[n]
		      && info->subscript[n]->info->type == GFC_SS_SCALAR);
	  start = info->subscript[n]->info->data.scalar.value;
	}
      else
	{
	  /* Evaluate and remember the start of the section.  */
	  start = info->start[n];
	  stride = gfc_evaluate_now (stride, block);
	}

      tmp = gfc_conv_array_lbound (src, n);
      tmp = fold_build2_loc (input_location, MINUS_EXPR, TREE_TYPE (tmp),
			     start, tmp);
      tmp = fold_build2_loc (input_location, MULT_EXPR, TREE_TYPE (tmp),
			     tmp, src_stride);
      base = fold_build2_loc (input_location, PLUS_EXPR, TREE_TYPE (tmp),
			      base, tmp);

      if (info->ref
	  && info->ref->u.ar.dimen_type[n] == DIMEN_ELEMENT)
	{
	  /* For elemental dimensions, we only need the 1st
	     element in the section.  */
	  continue;
	}

      /* Vector subscripts need copying and are handled elsewhere.  */
      if (info->ref)
	gcc_assert (info->ref->u.ar.dimen_type[n] == DIMEN_RANGE);

      /* look for the corresponding scalarizer dimension: dim.  */
      int dim;
      for (dim = 0; dim < ndim; dim++)
	if (ss->dim[dim] == n)
	  break;

      /* loop exited early: the DIM being looked for has been found.  */
      gcc_assert (dim < ndim);

      /* Set the new lower bound.  */
      tree from = lowers[dim];
      tree to = uppers[dim];

      /* Multiply the stride by the section stride to get the
	 total stride.  */
      stride = fold_build2_loc (input_location, MULT_EXPR,
				gfc_array_index_type,
				stride, info->stride[n]);

      set_dimension_fields (block, dest, gfc_rank_cst[dim], from, to, stride,
			    &offset);
    }

  /* For deferred-length character we need to take the dynamic length
     into account for the dataptr offset.  */
  if (src_expr->ts.type == BT_CHARACTER
      && src_expr->ts.deferred
      && src_expr->ts.u.cl->backend_decl
      && VAR_P (src_expr->ts.u.cl->backend_decl))
    {
      tree base_type = TREE_TYPE (base);
      base = fold_build2_loc (input_location, MULT_EXPR, base_type, base,
			      fold_convert (base_type,
					    src_expr->ts.u.cl->backend_decl));
    }

  for (int n = rank; n < rank + corank; n++)
    {
      tree from = lowers[n];
      tree to = uppers[n];
      gfc_conv_descriptor_lbound_set (block, dest,
				      gfc_rank_cst[n], from);
      if (n < rank + corank - 1)
	gfc_conv_descriptor_ubound_set (block, dest,
					gfc_rank_cst[n], to);
    }

  if (data_needed)
    /* Point the data pointer at the 1st element in the section.  */
    gfc_get_dataptr_offset (block, dest, src, base,
			    subref, src_expr);
  else
    gfc_conv_descriptor_data_set (block, dest,
				  gfc_index_zero_node);

  gfc_conv_descriptor_offset_set (block, dest, offset);

  if (flag_coarray == GFC_FCOARRAY_LIB && src_expr->corank)
    {
      tmp = INDIRECT_REF_P (src) ? TREE_OPERAND (src, 0) : src;
      if (GFC_DESCRIPTOR_TYPE_P (TREE_TYPE (tmp)))
	{
	  tmp = gfc_conv_descriptor_token (tmp);
	}
      else if (DECL_P (tmp) && DECL_LANG_SPECIFIC (tmp)
	       && GFC_DECL_TOKEN (tmp) != NULL_TREE)
	tmp = GFC_DECL_TOKEN (tmp);
      else
	{
	  tmp = GFC_TYPE_ARRAY_CAF_TOKEN (TREE_TYPE (tmp));
	}

      gfc_conv_descriptor_token_set (block, dest, tmp);
    }
}
 

void
gfc_set_contiguous_descriptor (stmtblock_t *block, tree desc, tree size,
			       tree data_ptr)
{
  gcc_assert (!GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (desc)));
  gfc_conv_descriptor_dtype_set (block, desc,
				 gfc_get_dtype_rank_type (1, TREE_TYPE (desc),
							  false));
  gfc_conv_descriptor_lbound_set (block, desc,
				  gfc_index_zero_node,
				  gfc_index_one_node);
  gfc_conv_descriptor_stride_set (block, desc,
				  gfc_index_zero_node,
				  gfc_index_one_node);
  gfc_conv_descriptor_ubound_set (block, desc,
				  gfc_index_zero_node, size);
  gfc_conv_descriptor_data_set (block, desc, data_ptr);
}


void
gfc_set_descriptor_with_shape (stmtblock_t *block, tree desc, tree ptr,
			       gfc_expr *shape, gfc_expr *lower, locus *where)
{
  gcc_assert (GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (desc)));

  /* Set the span field.  */
  tree tmp = TYPE_SIZE_UNIT (gfc_get_element_type (TREE_TYPE (desc)));
  tree elem_len = fold_convert (gfc_array_index_type, tmp);
  gfc_conv_descriptor_span_set (block, desc, elem_len);

  /* Set data value, dtype, and offset.  */
  tmp = GFC_TYPE_ARRAY_DATAPTR_TYPE (TREE_TYPE (desc));
  gfc_conv_descriptor_data_set (block, desc, fold_convert (tmp, ptr));
  gfc_conv_descriptor_dtype_set (block, desc,
				 gfc_get_dtype (TREE_TYPE (desc)));

  /* Start scalarization of the bounds, using the shape argument.  */

  gfc_ss *shape_ss = gfc_walk_expr (shape);
  gcc_assert (shape_ss != gfc_ss_terminator);
  gfc_se shapese, lowerse;
  gfc_init_se (&shapese, nullptr);
  gfc_ss *lower_ss = nullptr;
  if (lower)
    {
      lower_ss = gfc_walk_expr (lower);
      gcc_assert (lower_ss != gfc_ss_terminator);
      gfc_init_se (&lowerse, nullptr);
    }

  gfc_loopinfo loop;
  gfc_init_loopinfo (&loop);
  gfc_add_ss_to_loop (&loop, shape_ss);
  if (lower)
    gfc_add_ss_to_loop (&loop, lower_ss);
  gfc_conv_ss_startstride (&loop);
  gfc_conv_loop_setup (&loop, where);
  gfc_mark_ss_chain_used (shape_ss, 1);
  if (lower)
    gfc_mark_ss_chain_used (lower_ss, 1);

  gfc_copy_loopinfo_to_se (&shapese, &loop);
  shapese.ss = shape_ss;
  if (lower)
    {
      gfc_copy_loopinfo_to_se (&lowerse, &loop);
      lowerse.ss = lower_ss;
    }

  tree stride = gfc_create_var (gfc_array_index_type, "stride");
  tree offset = gfc_create_var (gfc_array_index_type, "offset");
  gfc_add_modify (block, stride, elem_len);
  gfc_add_modify (block, offset, gfc_index_zero_node);

  /* Loop body.  */
  stmtblock_t body;
  gfc_start_scalarized_body (&loop, &body);

  tree dim = fold_build2_loc (input_location, MINUS_EXPR, gfc_array_index_type,
			      loop.loopvar[0], loop.from[0]);

  tree lbound;
  if (lower)
    {
      gfc_conv_expr (&lowerse, lower);
      gfc_add_block_to_block (&body, &lowerse.pre);
      lbound = fold_convert (gfc_array_index_type, lowerse.expr);
      lbound = gfc_evaluate_now (lbound, &body);
      gfc_add_block_to_block (&body, &lowerse.post);
    }
  else
    lbound = gfc_index_one_node;

  gfc_conv_expr (&shapese, shape);
  gfc_add_block_to_block (&body, &shapese.pre);
  tree shapeval = fold_convert (gfc_array_index_type, shapese.expr);
  shapeval = gfc_evaluate_now (shapeval, &body);
  tmp = fold_build2_loc (input_location, MINUS_EXPR, gfc_array_index_type,
			 lbound, gfc_index_one_node);
  tree ubound = fold_build2_loc (input_location, PLUS_EXPR,
				 gfc_array_index_type, tmp, shapeval);
  ubound = gfc_evaluate_now (ubound, &body);
  gfc_add_block_to_block (&body, &shapese.post);

  set_dimension_fields (&body, desc, dim, lbound, ubound, stride, offset);

  /* Update stride.  */
  gfc_add_modify (&body, stride,
		  fold_build2_loc (input_location, MULT_EXPR,
				   gfc_array_index_type, stride, shapeval));

  /* Finish scalarization loop.  */
  gfc_trans_scalarizing_loops (&loop, &body);
  gfc_add_block_to_block (block, &loop.pre);
  gfc_add_block_to_block (block, &loop.post);
  gfc_cleanup_loop (&loop);

  gfc_conv_descriptor_offset_set (block, desc, offset);
}


static void
set_gfc_dimension_from_cfi (stmtblock_t *block, tree gfc, tree cfi, tree idx,
			    tree lbound, tree offset_var, tree cont_stride_var,
			    bool contiguous)
{
  /* gfc->dim[i].lbound = ... */
  lbound = fold_convert (gfc_array_index_type, lbound);

  /* gfc->dim[i].ubound = gfc->dim[i].lbound + cfi->dim[i].extent - 1. */
  tree tmp = fold_build2_loc (input_location, MINUS_EXPR, gfc_array_index_type,
			      lbound, gfc_index_one_node);
  tree ubound = fold_build2_loc (input_location, PLUS_EXPR, gfc_array_index_type,
				 gfc_get_cfi_dim_extent (cfi, idx), tmp);

  tree stride;
  if (contiguous)
    {
      gcc_assert (!GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (gfc)));

      /* gfc->dim[i].stride
	   = idx == 0 ? 1 : gfc->dim[i-1].stride * cfi->dim[i-1].extent */
      tree cond = fold_build2_loc (input_location, EQ_EXPR, boolean_type_node,
				   idx, build_zero_cst (TREE_TYPE (idx)));
      tmp = fold_build2_loc (input_location, MINUS_EXPR, TREE_TYPE (idx),
			     idx, build_int_cst (TREE_TYPE (idx), 1));
      tmp = gfc_get_cfi_dim_extent (cfi, tmp);
      tmp = fold_build2_loc (input_location, MULT_EXPR, TREE_TYPE (tmp),
			     tmp, cont_stride_var);
      tmp = build3_loc (input_location, COND_EXPR, gfc_array_index_type, cond,
			gfc_index_one_node, tmp);
      stride = gfc_evaluate_now (tmp, block);
      gfc_add_modify (block, cont_stride_var, stride);
    }
  else
    {
      gcc_assert (GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (gfc)));

      /* gfc->dim[i].stride = cfi->dim[i].sm / cfi>elem_len */
      stride = gfc_get_cfi_dim_sm (cfi, idx);
    }

  set_dimension_fields (block, gfc, idx, lbound, ubound, stride, offset_var);
}


void
gfc_set_gfc_from_cfi (stmtblock_t *block, tree gfc, gfc_expr *e, tree rank,
		      tree gfc_strlen, tree cfi, gfc_symbol *fsym)
{
  stmtblock_t block2;
  gfc_init_block (&block2);
  if (e->rank == 0)
    {
      tree tmp = gfc_get_cfi_desc_base_addr (cfi);
      gfc_add_modify (block, gfc, fold_convert (TREE_TYPE (gfc), tmp));
    }
  else
    {
      tree tmp = gfc_get_cfi_desc_base_addr (cfi);
      gfc_conv_descriptor_data_set (block, gfc, tmp);

      if (fsym->attr.allocatable)
	{
	  /* gfc->span = cfi->elem_len.  */
	  tmp = fold_convert (gfc_array_index_type,
			      gfc_get_cfi_dim_sm (cfi, gfc_rank_cst[0]));
	}
      else
	{
	  /* gfc->span = ((cfi->dim[0].sm % cfi->elem_len)
			  ? cfi->dim[0].sm : cfi->elem_len).  */
	  tmp = gfc_get_cfi_dim_sm (cfi, gfc_rank_cst[0]);
	  tree tmp2 = fold_convert (gfc_array_index_type,
				    gfc_get_cfi_desc_elem_len (cfi));
	  tmp = fold_build2_loc (input_location, TRUNC_MOD_EXPR,
				 gfc_array_index_type, tmp, tmp2);
	  tmp = fold_build2_loc (input_location, NE_EXPR, boolean_type_node,
			     tmp, gfc_index_zero_node);
	  tmp = build3_loc (input_location, COND_EXPR, gfc_array_index_type, tmp,
			    gfc_get_cfi_dim_sm (cfi, gfc_rank_cst[0]), tmp2);
	}
      gfc_conv_descriptor_span_set (&block2, gfc, tmp);

      /* Calculate offset + set lbound, ubound and stride.  */
      tree offset = gfc_create_var (gfc_array_index_type, "offset");
      gfc_add_modify (&block2, offset, gfc_index_zero_node);
      bool contiguous = fsym->attr.allocatable;
      tree stride;
      if (contiguous)
	stride = gfc_create_var (gfc_array_index_type, "stride");
      else
	stride = NULL_TREE;
      /* Loop: for (i = 0; i < rank; ++i).  */
      tree idx = gfc_create_var (TREE_TYPE (rank), "idx");
      /* Loop body.  */
      stmtblock_t loop_body;
      gfc_init_block (&loop_body);
      set_gfc_dimension_from_cfi (&loop_body, gfc, cfi, idx,
				  gfc_get_cfi_dim_lbound (cfi, idx), offset,
				  stride, contiguous);
      /* Generate loop.  */
      gfc_simple_for_loop (&block2, idx, build_int_cst (TREE_TYPE (idx), 0),
			   rank, LT_EXPR, build_int_cst (TREE_TYPE (idx), 1),
			   gfc_finish_block (&loop_body));
      gfc_conv_descriptor_offset_set (&block2, gfc, offset);
    }

  if (e->ts.type == BT_CHARACTER && !e->ts.u.cl->length)
    {
      tree tmp = fold_convert (gfc_charlen_type_node,
			       gfc_get_cfi_desc_elem_len (cfi));
      if (e->ts.kind != 1)
	tmp = fold_build2_loc (input_location, TRUNC_DIV_EXPR,
			       gfc_charlen_type_node, tmp,
			       build_int_cst (gfc_charlen_type_node,
					      e->ts.kind));
      gfc_add_modify (&block2, gfc_strlen, tmp);
    }

  tree tmp = gfc_get_cfi_desc_base_addr (cfi);
  tmp = fold_build2_loc (input_location, NE_EXPR, boolean_type_node,
			 tmp, null_pointer_node);
  tmp = build3_v (COND_EXPR, tmp, gfc_finish_block (&block2),
		  build_empty_stmt (input_location));
  gfc_add_expr_to_block (block, tmp);
}


void
gfc_set_gfc_from_cfi (stmtblock_t *block, stmtblock_t *block2, tree gfc_desc,
		      tree rank, tree cfi, gfc_symbol *sym, bool do_copy_inout)
{
  /* gfc->dtype = ... (from declaration, not from cfi).  */
  gfc_conv_descriptor_dtype_set (block, gfc_desc,
				 gfc_get_dtype (TREE_TYPE (gfc_desc),
						&sym->as->rank));
  /* gfc->data = cfi->base_addr. */
  gfc_conv_descriptor_data_set (block, gfc_desc,
				gfc_get_cfi_desc_base_addr (cfi));

  if (sym->ts.type == BT_ASSUMED)
    {
      /* For type(*), take elem_len + dtype.type from the actual argument.  */
      gfc_conv_descriptor_elem_len_set (block, gfc_desc,
					gfc_get_cfi_desc_elem_len (cfi));
      tree ctype = gfc_get_cfi_desc_type (cfi);
      ctype = fold_build2_loc (input_location, BIT_AND_EXPR, TREE_TYPE (ctype),
			       ctype, build_int_cst (TREE_TYPE (ctype),
						     CFI_type_mask));

      /* if (CFI_type_cptr) BT_VOID else BT_UNKNOWN  */
      /* Note: BT_VOID is could also be CFI_type_funcptr, but assume c_ptr. */
      tree cond = fold_build2_loc (input_location, EQ_EXPR, boolean_type_node,
				   ctype, build_int_cst (TREE_TYPE (ctype),
							 CFI_type_cptr));
      tree tmp = gfc_conv_descriptor_type_set (gfc_desc, BT_VOID);
      tree tmp2 = gfc_conv_descriptor_type_set (gfc_desc, BT_UNKNOWN);
      tmp2 = fold_build3_loc (input_location, COND_EXPR, void_type_node, cond,
			      tmp, tmp2);
      /* if (CFI_type_struct) BT_DERIVED else  < tmp2 >  */
      cond = fold_build2_loc (input_location, EQ_EXPR, boolean_type_node, ctype,
			      build_int_cst (TREE_TYPE (ctype),
					     CFI_type_struct));
      tmp = gfc_conv_descriptor_type_set (gfc_desc, BT_DERIVED);
      tmp2 = fold_build3_loc (input_location, COND_EXPR, void_type_node, cond,
			      tmp, tmp2);
      /* if (CFI_type_Character) BT_CHARACTER else  < tmp2 >  */
      /* Note: this is kind=1, CFI_type_ucs4_char is handled in the 'else if'
	 before (see below, as generated bottom up).  */
      cond = fold_build2_loc (input_location, EQ_EXPR, boolean_type_node, ctype,
			      build_int_cst (TREE_TYPE (ctype),
			      CFI_type_Character));
      tmp = gfc_conv_descriptor_type_set (gfc_desc, BT_CHARACTER);
      tmp2 = fold_build3_loc (input_location, COND_EXPR, void_type_node, cond,
			      tmp, tmp2);
      /* if (CFI_type_ucs4_char) BT_CHARACTER else  < tmp2 >  */
      /* Note: gfc->elem_len = cfi->elem_len/4.  */
      /* However, assuming that CFI_type_ucs4_char cannot be recovered, leave
	 gfc->elem_len == cfi->elem_len, which helps with operations which use
	 sizeof() in Fortran and cfi->elem_len in C.  */
      tmp = gfc_get_cfi_desc_type (cfi);
      cond = fold_build2_loc (input_location, EQ_EXPR, boolean_type_node, tmp,
			      build_int_cst (TREE_TYPE (tmp),
					     CFI_type_ucs4_char));
      tmp = gfc_conv_descriptor_type_set (gfc_desc, BT_CHARACTER);
      tmp2 = fold_build3_loc (input_location, COND_EXPR, void_type_node, cond,
			      tmp, tmp2);
      /* if (CFI_type_Complex) BT_COMPLEX + cfi->elem_len/2 else  < tmp2 >  */
      cond = fold_build2_loc (input_location, EQ_EXPR, boolean_type_node, ctype,
			      build_int_cst (TREE_TYPE (ctype),
			      CFI_type_Complex));
      tmp = gfc_conv_descriptor_type_set (gfc_desc, BT_COMPLEX);
      tmp2 = fold_build3_loc (input_location, COND_EXPR, void_type_node, cond,
			      tmp, tmp2);
      /* if (CFI_type_Integer || CFI_type_Logical || CFI_type_Real)
	   ctype else  <tmp2>  */
      cond = fold_build2_loc (input_location, EQ_EXPR, boolean_type_node, ctype,
			      build_int_cst (TREE_TYPE (ctype),
					     CFI_type_Integer));
      tmp = fold_build2_loc (input_location, EQ_EXPR, boolean_type_node, ctype,
			      build_int_cst (TREE_TYPE (ctype),
					     CFI_type_Logical));
      cond = fold_build2_loc (input_location, TRUTH_OR_EXPR, boolean_type_node,
			      cond, tmp);
      tmp = fold_build2_loc (input_location, EQ_EXPR, boolean_type_node, ctype,
			      build_int_cst (TREE_TYPE (ctype),
					     CFI_type_Real));
      cond = fold_build2_loc (input_location, TRUTH_OR_EXPR, boolean_type_node,
			      cond, tmp);
      tmp = gfc_conv_descriptor_type_set (gfc_desc, ctype);
      tmp2 = fold_build3_loc (input_location, COND_EXPR, void_type_node, cond,
			      tmp, tmp2);
      gfc_add_expr_to_block (block, tmp2);
    }

  if (sym->as->rank < 0)
    /* Set gfc->dtype.rank, if assumed-rank.  */
    gfc_conv_descriptor_rank_set (block, gfc_desc, rank);

  /* if do_copy_inout:  gfc->dspan = gfc->dtype.elem_len
     We use gfc instead of cfi on the RHS as this might be a constant.  */
  tree tmp = fold_convert (gfc_array_index_type,
			   gfc_conv_descriptor_elem_len_get (gfc_desc));
  if (!do_copy_inout)
    {
      /* gfc->dspan = ((cfi->dim[0].sm % gfc->elem_len)
		       ? cfi->dim[0].sm : gfc->elem_len).  */
      tree cond;
      tree tmp2 = gfc_get_cfi_dim_sm (cfi, gfc_rank_cst[0]);
      cond = fold_build2_loc (input_location, TRUNC_MOD_EXPR,
			      gfc_array_index_type, tmp2, tmp);
      cond = fold_build2_loc (input_location, NE_EXPR, boolean_type_node,
			      cond, gfc_index_zero_node);
      tmp = build3_loc (input_location, COND_EXPR, gfc_array_index_type, cond,
			tmp2, tmp);
    }
  gfc_conv_descriptor_span_set (block2, gfc_desc, tmp);

  /* Calculate offset + set lbound, ubound and stride.  */
  tree offset = gfc_create_var (gfc_array_index_type, "offset");
  gfc_add_modify (block2, offset, gfc_index_zero_node);

  /* Stride  */
  bool contiguous = do_copy_inout || sym->attr.allocatable;
  tree stride;
  if (contiguous)
    stride = gfc_create_var (gfc_array_index_type, "stride");
  else
    stride = NULL_TREE;

  if (sym->as->rank > 0 && !sym->attr.pointer && !sym->attr.allocatable)
    {
      for (int i = 0; i < sym->as->rank; ++i)
	{
	  gfc_se se;
	  gfc_init_se (&se, NULL );
	  if (sym->as->lower[i])
	    {
	      gfc_conv_expr (&se, sym->as->lower[i]);
	      tmp = se.expr;
	    }
	  else
	    tmp = gfc_index_one_node;
	  gfc_add_block_to_block (block2, &se.pre);
	  tree lbound = gfc_evaluate_now (tmp, block2);
	  gfc_add_block_to_block (block2, &se.post);
	  set_gfc_dimension_from_cfi (block2, gfc_desc, cfi, gfc_rank_cst[i],
				      lbound, offset, stride, contiguous);
	}
    }
  else
    {
      /* Loop: for (i = 0; i < rank; ++i).  */
      tree idx = gfc_create_var (TREE_TYPE (rank), "idx");

      /* Loop body.  */
      stmtblock_t loop_body;
      gfc_init_block (&loop_body);
      /* gfc->dim[i].lbound = ... */
      tree lbound;
      if (sym->attr.pointer || sym->attr.allocatable)
	lbound = gfc_get_cfi_dim_lbound (cfi, idx);
      else if (sym->as->rank < 0)
	lbound = gfc_index_one_node;
      else
	gcc_unreachable ();

      set_gfc_dimension_from_cfi (&loop_body, gfc_desc, cfi, idx, lbound,
				  offset, stride, contiguous);

      /* Generate loop.  */
      gfc_simple_for_loop (block2, idx, build_zero_cst (TREE_TYPE (idx)),
			   rank, LT_EXPR, build_int_cst (TREE_TYPE (idx), 1),
			   gfc_finish_block (&loop_body));
    }

  gfc_conv_descriptor_offset_set (block2, gfc_desc, offset);
}


void
gfc_set_temporary_descriptor (stmtblock_t *block, tree descr, tree class_src,
			      tree elemsize, tree data_ptr,
			      tree lbound[GFC_MAX_DIMENSIONS],
			      tree ubound[GFC_MAX_DIMENSIONS],
			      tree stride[GFC_MAX_DIMENSIONS], int rank,
			      bool callee_allocated, bool rank_changer,
			      bool shift_bounds)
{
  if (!class_src)
    {
      /* Fill in the array dtype.  */
      gfc_conv_descriptor_dtype_set (block, descr,
				     gfc_get_dtype (TREE_TYPE (descr)));
    }
  else if (rank_changer)
    {
      /* For classes, we copy the whole original class descriptor to the
	 temporary one, so we don't need to set the individual dtype fields.
	 Except for the case of rank altering intrinsics for which we
	 generate descriptors of different rank.  */

      /* Take the dtype from the class expression.  */
      tree class_descr = gfc_class_data_get (class_src);
      tree dtype = gfc_conv_descriptor_dtype_get (class_descr);
      gfc_conv_descriptor_dtype_set (block, descr, dtype);

      /* These transformational functions change the rank.  */
      gfc_conv_descriptor_rank_set (block, descr, rank);
    }

  tree offset = gfc_index_zero_node;
  if (!callee_allocated)
    for (int n = 0; n < rank; n++)
      {
	if (shift_bounds)
	  shift_dimension_fields (block, descr, gfc_rank_cst[n],
				  gfc_index_zero_node, lbound[n], ubound[n],
				  stride[n], &offset);
	else
	  set_dimension_fields (block, descr, gfc_rank_cst[n], lbound[n],
				ubound[n], stride[n], &offset);
      }

  gfc_conv_descriptor_span_set (block, descr, elemsize);

  gfc_conv_descriptor_offset_set (block, descr, offset);

  gfc_conv_descriptor_data_set (block, descr, data_ptr);
}


/* Returns the value of LBOUND for an expression.  This could be broken out
   from gfc_conv_intrinsic_bound but this seemed to be simpler.  This is
   called by gfc_alloc_allocatable_for_assignment.  */
static tree
get_std_lbound (gfc_expr *expr, tree desc, int dim, bool assumed_size)
{
  tree lbound;
  tree ubound;
  tree stride;
  tree cond, cond1, cond3, cond4;
  tree tmp;
  gfc_ref *ref;

  if (GFC_DESCRIPTOR_TYPE_P (TREE_TYPE (desc)))
    {
      tmp = gfc_rank_cst[dim];
      lbound = gfc_conv_descriptor_lbound_get (desc, tmp);
      ubound = gfc_conv_descriptor_ubound_get (desc, tmp);
      stride = gfc_conv_descriptor_stride_get (desc, tmp);
      cond1 = fold_build2_loc (input_location, GE_EXPR, logical_type_node,
			       ubound, lbound);
      cond3 = fold_build2_loc (input_location, GE_EXPR, logical_type_node,
			       stride, gfc_index_zero_node);
      cond3 = fold_build2_loc (input_location, TRUTH_AND_EXPR,
			       logical_type_node, cond3, cond1);
      cond4 = fold_build2_loc (input_location, LT_EXPR, logical_type_node,
			       stride, gfc_index_zero_node);
      if (assumed_size)
	cond = fold_build2_loc (input_location, EQ_EXPR, logical_type_node,
				tmp, build_int_cst (gfc_array_index_type,
						    expr->rank - 1));
      else
	cond = logical_false_node;

      cond1 = fold_build2_loc (input_location, TRUTH_OR_EXPR,
			       logical_type_node, cond3, cond4);
      cond = fold_build2_loc (input_location, TRUTH_OR_EXPR,
			      logical_type_node, cond, cond1);

      return fold_build3_loc (input_location, COND_EXPR,
			      gfc_array_index_type, cond,
			      lbound, gfc_index_one_node);
    }

  if (expr->expr_type == EXPR_FUNCTION)
    {
      /* A conversion function, so use the argument.  */
      gcc_assert (expr->value.function.isym
		  && expr->value.function.isym->conversion);
      expr = expr->value.function.actual->expr;
    }

  if (expr->expr_type == EXPR_VARIABLE)
    {
      tmp = TREE_TYPE (expr->symtree->n.sym->backend_decl);
      for (ref = expr->ref; ref; ref = ref->next)
	{
	  if (ref->type == REF_COMPONENT
		&& ref->u.c.component->as
		&& ref->next
		&& ref->next->u.ar.type == AR_FULL)
	    tmp = TREE_TYPE (ref->u.c.component->backend_decl);
	}
      return GFC_TYPE_ARRAY_LBOUND(tmp, dim);
    }

  return gfc_index_one_node;
}


void
gfc_set_descriptor_for_assign_realloc (stmtblock_t *block, gfc_loopinfo *loop,
				       gfc_expr *expr1, gfc_expr *expr2,
				       tree desc, tree desc2, tree elemsize2,
				       tree class_expr2, bool coarray)
{
  gcc_assert (!GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (desc)));

  gfc_array_spec *as;
  /* Get arrayspec if expr is a full array.  */
  if (expr2 && expr2->expr_type == EXPR_FUNCTION
	&& expr2->value.function.isym
	&& expr2->value.function.isym->conversion)
    {
      /* For conversion functions, take the arg.  */
      gfc_expr *arg = expr2->value.function.actual->expr;
      as = gfc_get_full_arrayspec_from_expr (arg);
    }
  else if (expr2)
    as = gfc_get_full_arrayspec_from_expr (expr2);
  else
    as = NULL;

  /* Now modify the lhs descriptor and the associated scalarizer
     variables. F2003 7.4.1.3: "If variable is or becomes an
     unallocated allocatable variable, then it is allocated with each
     deferred type parameter equal to the corresponding type parameters
     of expr , with the shape of expr , and with each lower bound equal
     to the corresponding element of LBOUND(expr)."
     Reuse size1 to keep a dimension-by-dimension track of the
     stride of the new array.  */
  tree size1 = gfc_index_one_node;
  tree offset = gfc_index_zero_node;

  for (int n = 0; n < expr2->rank; n++)
    {
      tree tmp = fold_build2_loc (input_location, MINUS_EXPR,
				  gfc_array_index_type,
				  loop->to[n], loop->from[n]);
      tree ubound = fold_build2_loc (input_location, PLUS_EXPR,
				     gfc_array_index_type,
				     tmp, gfc_index_one_node);

      if (as)
	shift_dimension_fields (block, desc, gfc_rank_cst[n],
				get_std_lbound (expr2, desc2, n,
						as->type == AS_ASSUMED_SIZE),
				gfc_index_one_node, ubound, size1, &offset);
      else
	set_dimension_fields (block, desc, gfc_rank_cst[n], gfc_index_one_node,
			      ubound, size1, &offset);

      size1 = fold_build2_loc (input_location, MULT_EXPR, gfc_array_index_type,
			       ubound, size1);
    }

  /* Set the lhs descriptor and scalarizer offsets.  For rank > 1,
     the array offset is saved and the info.offset is used for a
     running offset.  Use the saved_offset instead.  */
  gfc_conv_descriptor_offset_set (block, desc, offset);

  if (GFC_DESCRIPTOR_TYPE_P (TREE_TYPE (desc)))
    gfc_conv_descriptor_span_set (block, desc, elemsize2);

  bool bytes_counted_strides;
  bytes_counted_strides = GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (desc));

  /* For deferred character length, the 'size' field of the dtype might
     have changed so set the dtype.  */
  if (GFC_DESCRIPTOR_TYPE_P (TREE_TYPE (desc))
      && expr1->ts.type == BT_CHARACTER && expr1->ts.deferred)
    {
      tree type;
      if (expr2->ts.u.cl->backend_decl)
	type = gfc_typenode_for_spec (&expr2->ts);
      else
	type = gfc_typenode_for_spec (&expr1->ts);

      tree tmp = gfc_get_dtype_rank_type (expr1->rank, type,
					  bytes_counted_strides);
      gfc_conv_descriptor_dtype_set (block, desc, tmp);
    }
  else if (expr1->ts.type == BT_CLASS)
    {
      tree type;

      if (expr2->ts.type != BT_CLASS)
	type = gfc_typenode_for_spec (&expr2->ts);
      else
	type = gfc_get_character_type_len (1, elemsize2);

      tree tmp = gfc_get_dtype_rank_type (expr2->rank, type,
					  bytes_counted_strides);
      gfc_conv_descriptor_dtype_set (block, desc, tmp);

      /* Set the _len field as well...  */
      if (UNLIMITED_POLY (expr1))
	{
	  tmp = gfc_class_len_get (TREE_OPERAND (desc, 0));
	  if (expr2->ts.type == BT_CHARACTER)
	    gfc_add_modify (block, tmp,
			    fold_convert (TREE_TYPE (tmp),
					  TYPE_SIZE_UNIT (type)));
	  else if (UNLIMITED_POLY (expr2))
	    gfc_add_modify (block, tmp,
			    gfc_class_len_get (TREE_OPERAND (desc2, 0)));
	  else
	    gfc_add_modify (block, tmp,
			    build_int_cst (TREE_TYPE (tmp), 0));
	}
      /* ...and the vptr.  */
      tmp = gfc_class_vptr_get (TREE_OPERAND (desc, 0));
      tree tmp2;
      if (expr2->ts.type == BT_CLASS && !VAR_P (desc2)
	  && TREE_CODE (desc2) == COMPONENT_REF)
	{
	  tmp2 = gfc_get_class_from_expr (desc2);
	  tmp2 = gfc_class_vptr_get (tmp2);
	}
      else if (expr2->ts.type == BT_CLASS && class_expr2 != NULL_TREE)
	tmp2 = gfc_class_vptr_get (class_expr2);
      else
	{
	  tmp2 = gfc_get_symbol_decl (gfc_find_vtab (&expr2->ts));
	  tmp2 = gfc_build_addr_expr (TREE_TYPE (tmp), tmp2);
	}

      gfc_add_modify (block, tmp, fold_convert (TREE_TYPE (tmp), tmp2));
    }
  else if (coarray && GFC_DESCRIPTOR_TYPE_P (TREE_TYPE (desc)))
    gfc_conv_descriptor_dtype_set (block, desc,
				   gfc_get_dtype (TREE_TYPE (desc)));
}


tree
gfc_set_pdt_array_descriptor (stmtblock_t *block, tree descr,
			      gfc_array_spec *as,
			      gfc_actual_arglist *pdt_param_list, tree elt_size)
{
  gfc_se tse;
  tree size = gfc_index_one_node;
  tree offset = gfc_index_zero_node;
  gfc_expr *e;

  gcc_assert (!GFC_BYTES_STRIDES_ARRAY_TYPE_P (TREE_TYPE (descr)));

  /* This chunk takes the expressions for 'lower' and 'upper'
     in the arrayspec and substitutes in the expressions for
     the parameters from 'pdt_param_list'. The descriptor
     fields can then be filled from the values so obtained.  */
  gcc_assert (GFC_DESCRIPTOR_TYPE_P (TREE_TYPE (descr)));
  for (int i = 0; i < as->rank; i++)
    {
      gfc_init_se (&tse, NULL);
      e = gfc_copy_expr (as->lower[i]);
      gfc_insert_parameter_exprs (e, pdt_param_list);
      gfc_conv_expr_type (&tse, e, gfc_array_index_type);
      gfc_free_expr (e);
      tree lower = tse.expr;
      gfc_add_block_to_block (block, &tse.pre);

      e = gfc_copy_expr (as->upper[i]);
      gfc_insert_parameter_exprs (e, pdt_param_list);
      gfc_conv_expr_type (&tse, e, gfc_array_index_type);
      gfc_free_expr (e);
      tree upper = tse.expr;
      gfc_add_block_to_block (block, &tse.pre);

      set_dimension_fields (block, descr, gfc_rank_cst[i], lower, upper, size,
			    &offset);

      tree tmp = fold_build2_loc (input_location, MINUS_EXPR,
				  gfc_array_index_type, upper, lower);
      tmp = fold_build2_loc (input_location, PLUS_EXPR, gfc_array_index_type,
			     tmp, gfc_index_one_node);
      size = fold_build2_loc (input_location, MULT_EXPR,
			      gfc_array_index_type, size, tmp);
    }
  gfc_conv_descriptor_offset_set (block, descr, offset);
  gfc_conv_descriptor_dtype_set (block, descr,
				 gfc_get_dtype (TREE_TYPE (descr)));

  gfc_conv_descriptor_span_set (block, descr, elt_size);

  size = fold_build2_loc (input_location, MULT_EXPR,
			  gfc_array_index_type, size, elt_size);
  size = gfc_evaluate_now (size, block);
  gfc_conv_descriptor_data_set (block, descr,
				gfc_call_malloc (block, NULL, size));

  return size;
}


/* Extend the data in array DESC by EXTRA elements.  */

void
gfc_grow_array (stmtblock_t * pblock, tree desc, tree extra)
{
  tree arg0, arg1;
  tree tmp;
  tree size;
  tree ubound;

  if (integer_zerop (extra))
    return;

  ubound = gfc_conv_descriptor_ubound_get (desc, gfc_rank_cst[0]);

  /* Add EXTRA to the upper bound.  */
  tmp = fold_build2_loc (input_location, PLUS_EXPR, gfc_array_index_type,
			 ubound, extra);
  gfc_conv_descriptor_ubound_set (pblock, desc, gfc_rank_cst[0], tmp);

  /* Get the value of the current data pointer.  */
  arg0 = gfc_conv_descriptor_data_get (desc);

  /* Calculate the new array size.  */
  size = TYPE_SIZE_UNIT (gfc_get_element_type (TREE_TYPE (desc)));
  tmp = fold_build2_loc (input_location, PLUS_EXPR, gfc_array_index_type,
			 ubound, gfc_index_one_node);
  arg1 = fold_build2_loc (input_location, MULT_EXPR, size_type_node,
			  fold_convert (size_type_node, tmp),
			  fold_convert (size_type_node, size));

  /* Call the realloc() function.  */
  tmp = gfc_call_realloc (pblock, arg0, arg1);
  gfc_conv_descriptor_data_set (pblock, desc, tmp);
}


/* Fills in an array descriptor, and returns the size of the array.
   The size will be a simple_val, ie a variable or a constant.  Also
   calculates the offset of the base.  The pointer argument overflow,
   which should be of integer type, will increase in value if overflow
   occurs during the size calculation.  Returns the size of the array.
   {
    stride = 1;
    offset = 0;
    for (n = 0; n < rank; n++)
      {
	a.lbound[n] = specified_lower_bound;
	offset = offset + a.lbond[n] * stride;
	size = 1 - lbound;
	a.ubound[n] = specified_upper_bound;
	a.stride[n] = stride;
	size = size >= 0 ? ubound + size : 0; //size = ubound + 1 - lbound
	overflow += size == 0 ? 0: (MAX/size < stride ? 1: 0);
	stride = stride * size;
      }
    for (n = rank; n < rank+corank; n++)
      (Set lcobound/ucobound as above.)
    element_size = sizeof (array element);
    if (!rank)
      return element_size
    stride = (size_t) stride;
    overflow += element_size == 0 ? 0: (MAX/element_size < stride ? 1: 0);
    stride = stride * element_size;
    return (stride);
   }  */
/*GCC ARRAYS*/

tree
gfc_descriptor_init_count (tree descriptor, int rank, int corank,
			   gfc_expr ** lower, gfc_expr ** upper,
			   stmtblock_t * pblock, stmtblock_t * descriptor_block,
			   tree * overflow, tree expr3_elem_size,
			   gfc_expr *expr3, tree expr3_desc,
			   bool e3_has_nodescriptor, gfc_expr *expr,
			   tree element_size, bool explicit_ts,
			   tree *empty_array_cond)
{
  tree type;
  tree tmp;
  tree size;
  tree offset;
  tree stride;
  tree cond;
  gfc_expr *ubound;
  gfc_se se;
  int n;

  type = TREE_TYPE (descriptor);

  bool bytes_counted_strides = GFC_BYTES_STRIDES_ARRAY_TYPE_P (type);

  if (bytes_counted_strides)
    stride = fold_convert_loc (input_location, gfc_array_index_type,
			       element_size);
  else
    stride = gfc_index_one_node;

  offset = gfc_index_zero_node;

  /* Set the dtype before the alloc, because registration of coarrays needs
     it initialized.  */
  if (expr->ts.type == BT_CHARACTER
      && expr->ts.deferred
      && VAR_P (expr->ts.u.cl->backend_decl))
    {
      type = gfc_typenode_for_spec (&expr->ts);
      tree dtype = gfc_get_dtype_rank_type (rank, type, bytes_counted_strides);
      gfc_conv_descriptor_dtype_set (pblock, descriptor, dtype);
    }
  else if (expr->ts.type == BT_CHARACTER
	   && expr->ts.deferred
	   && TREE_CODE (descriptor) == COMPONENT_REF)
    {
      /* Deferred character components have their string length tucked away
	 in a hidden field of the derived type. Obtain that and use it to
	 set the dtype. The charlen backend decl is zero because the field
	 type is zero length.  */
      gfc_ref *ref;
      tmp = NULL_TREE;
      for (ref = expr->ref; ref; ref = ref->next)
	if (ref->type == REF_COMPONENT
	    && gfc_deferred_strlen (ref->u.c.component, &tmp))
	  break;
      gcc_assert (tmp != NULL_TREE);
      tmp = fold_build3_loc (input_location, COMPONENT_REF, TREE_TYPE (tmp),
			     TREE_OPERAND (descriptor, 0), tmp, NULL_TREE);
      tmp = fold_convert (gfc_charlen_type_node, tmp);
      type = gfc_get_character_type_len (expr->ts.kind, tmp);
      tree dtype = gfc_get_dtype_rank_type (rank, type, bytes_counted_strides);
      gfc_conv_descriptor_dtype_set (pblock, descriptor, dtype);
    }
  else if (expr3_desc && GFC_DESCRIPTOR_TYPE_P (TREE_TYPE (expr3_desc)))
    gfc_conv_descriptor_dtype_set (pblock, descriptor,
				   gfc_conv_descriptor_dtype_get (expr3_desc));
  else if (expr->ts.type == BT_CLASS && !explicit_ts
	   && expr3 && expr3->ts.type != BT_CLASS
	   && expr3_elem_size != NULL_TREE && expr3_desc == NULL_TREE)
    gfc_conv_descriptor_elem_len_set (pblock, descriptor, expr3_elem_size);
  else
    gfc_conv_descriptor_dtype_set (pblock, descriptor, gfc_get_dtype (type));

  tree empty_cond = logical_false_node;

  for (n = 0; n < rank; n++)
    {
      tree conv_lbound;
      tree conv_ubound;

      /* We have 3 possibilities for determining the size of the array:
	 lower == NULL    => lbound = 1, ubound = upper[n]
	 upper[n] = NULL  => lbound = 1, ubound = lower[n]
	 upper[n] != NULL => lbound = lower[n], ubound = upper[n]  */
      ubound = upper[n];

      /* Set lower bound.  */
      gfc_init_se (&se, NULL);
      if (expr3_desc != NULL_TREE)
	{
	  if (e3_has_nodescriptor)
	    /* The lbound of nondescriptor arrays like array constructors,
	       nonallocatable/nonpointer function results/variables,
	       start at zero, but when allocating it, the standard expects
	       the array to start at one.  */
	    se.expr = gfc_index_one_node;
	  else
	    se.expr = gfc_conv_descriptor_lbound_get (expr3_desc,
						      gfc_rank_cst[n]);
	}
      else if (lower == NULL)
	se.expr = gfc_index_one_node;
      else
	{
	  gcc_assert (lower[n]);
	  if (ubound)
	    {
	      gfc_conv_expr_type (&se, lower[n], gfc_array_index_type);
	      gfc_add_block_to_block (pblock, &se.pre);
	    }
	  else
	    {
	      se.expr = gfc_index_one_node;
	      ubound = lower[n];
	    }
	}
      conv_lbound = se.expr;
      conv_lbound = gfc_evaluate_now (conv_lbound, pblock);

      /* Set upper bound.  */
      gfc_init_se (&se, NULL);
      if (expr3_desc != NULL_TREE)
	{
	  if (e3_has_nodescriptor)
	    {
	      /* The lbound of nondescriptor arrays like array constructors,
		 nonallocatable/nonpointer function results/variables,
		 start at zero, but when allocating it, the standard expects
		 the array to start at one.  Therefore fix the upper bound to be
		 (desc.ubound - desc.lbound) + 1.  */
	      tmp = gfc_conv_descriptor_extent_get (expr3_desc,
						    gfc_rank_cst[n]);
	      se.expr = gfc_evaluate_now (tmp, pblock);
	    }
	  else
	    se.expr = gfc_conv_descriptor_ubound_get (expr3_desc,
						      gfc_rank_cst[n]);
	}
      else
	{
	  gcc_assert (ubound);
	  gfc_conv_expr_type (&se, ubound, gfc_array_index_type);
	  gfc_add_block_to_block (pblock, &se.pre);
	  if (ubound->expr_type == EXPR_FUNCTION)
	    se.expr = gfc_evaluate_now (se.expr, pblock);
	}
      conv_ubound = se.expr;
      conv_ubound = gfc_evaluate_now (conv_ubound, pblock);

      set_dimension_fields (descriptor_block, descriptor, gfc_rank_cst[n],
			    conv_lbound, conv_ubound, stride, &offset);

      /* Calculate size and check whether extent is negative.  */
      size = gfc_conv_array_extent_dim (conv_lbound, conv_ubound, &empty_cond);
      size = gfc_evaluate_now (size, pblock);

      /* Check whether multiplying the stride by the number of
	 elements in this dimension would overflow. We must also check
	 whether the current dimension has zero size in order to avoid
	 division by zero.
      */
      tmp = fold_build2_loc (input_location, TRUNC_DIV_EXPR,
			     gfc_array_index_type,
			     fold_convert (gfc_array_index_type,
					   TYPE_MAX_VALUE (gfc_array_index_type)),
					   size);
      cond = gfc_unlikely (fold_build2_loc (input_location, LT_EXPR,
					    logical_type_node, tmp, stride),
			   PRED_FORTRAN_OVERFLOW);
      tmp = fold_build3_loc (input_location, COND_EXPR, integer_type_node, cond,
			     integer_one_node, integer_zero_node);
      cond = gfc_unlikely (fold_build2_loc (input_location, EQ_EXPR,
					    logical_type_node, size,
					    gfc_index_zero_node),
			   PRED_FORTRAN_SIZE_ZERO);
      tmp = fold_build3_loc (input_location, COND_EXPR, integer_type_node, cond,
			     integer_zero_node, tmp);
      tmp = fold_build2_loc (input_location, PLUS_EXPR, integer_type_node,
			     *overflow, tmp);
      *overflow = gfc_evaluate_now (tmp, pblock);

      /* Multiply the stride by the number of elements in this dimension.  */
      stride = fold_build2_loc (input_location, MULT_EXPR,
				gfc_array_index_type, stride, size);
      stride = gfc_evaluate_now (stride, pblock);
    }

  *empty_array_cond = empty_cond;

  for (n = rank; n < rank + corank; n++)
    {
      ubound = upper[n];

      /* Set lower bound.  */
      gfc_init_se (&se, NULL);
      if (lower == NULL || lower[n] == NULL)
	{
	  gcc_assert (n == rank + corank - 1);
	  se.expr = gfc_index_one_node;
	}
      else
	{
	  if (ubound || n == rank + corank - 1)
	    {
	      gfc_conv_expr_type (&se, lower[n], gfc_array_index_type);
	      gfc_add_block_to_block (pblock, &se.pre);
	    }
	  else
	    {
	      se.expr = gfc_index_one_node;
	      ubound = lower[n];
	    }
	}
      gfc_conv_descriptor_lbound_set (descriptor_block, descriptor,
				      gfc_rank_cst[n], se.expr);

      if (n < rank + corank - 1)
	{
	  gfc_init_se (&se, NULL);
	  gcc_assert (ubound);
	  gfc_conv_expr_type (&se, ubound, gfc_array_index_type);
	  gfc_add_block_to_block (pblock, &se.pre);
	  gfc_conv_descriptor_ubound_set (descriptor_block, descriptor,
					  gfc_rank_cst[n], se.expr);
	}
    }

  if (rank == 0)
    return gfc_index_one_node;

  /* Update the array descriptor with the offset and the span.  */
  gfc_conv_descriptor_offset_set (descriptor_block, descriptor, offset);
  tmp = fold_convert (gfc_array_index_type, element_size);
  gfc_conv_descriptor_span_set (descriptor_block, descriptor, tmp);

  return stride;
}


void
gfc_set_empty_descriptor_bounds (stmtblock_t *block, tree descr, int rank)
{
  tree offset = gfc_index_zero_node;
  for (int n = 0; n < rank; n++)
    set_dimension_fields (block, descr, gfc_rank_cst[n], gfc_index_one_node,
			  gfc_index_zero_node, gfc_index_zero_node, &offset);

  gfc_conv_descriptor_offset_set (block, descr, gfc_index_zero_node);
}


tree
gfc_create_unallocated_library_result_descriptor (stmtblock_t *block, tree source_descr, tree dtype)
{
  if (dtype == NULL_TREE)
    dtype = gfc_get_dtype (TREE_TYPE (source_descr));

  gfc_conv_descriptor_dtype_set (block, source_descr, dtype);
  gfc_conv_descriptor_span_set (block, source_descr,
				gfc_conv_descriptor_elem_len_get (source_descr));

  tree res_desc = gfc_evaluate_now (source_descr, block);
  gfc_conv_descriptor_data_set (block, res_desc, null_pointer_node);

  return res_desc;
}
