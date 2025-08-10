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


/* Array descriptor low level access routines.
 ******************************************************************************/

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


/* Get FIELD_IDX'th field in struct TYPE.  */

static tree
get_type_field (tree type, unsigned field_idx, tree field_type = NULL_TREE)
{
  tree field = gfc_advance_chain (TYPE_FIELDS (type), field_idx);
  gcc_assert (field != NULL_TREE
	      && (field_type == NULL_TREE
		  || TREE_TYPE (field) == field_type));

  return field;
}


/* Get FIELD_IDX'th field in array descriptor DESC.  */

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


/* Return a reference to the data field of the array descriptor DESC.  */

static tree
conv_descriptor_data (tree desc)
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
  tree field = conv_descriptor_data (desc);
  tree target_type = GFC_TYPE_ARRAY_DATAPTR_TYPE (type);
  return non_lvalue_loc (loc, fold_convert_loc (loc, target_type, field));
}

/* This provides WRITE access to the data field.  */

void
gfc_conv_descriptor_data_set (stmtblock_t *block, tree desc, tree value)
{
  set_value (block, conv_descriptor_data (desc), value);
}


static tree
conv_descriptor_offset (tree desc)
{
  return get_descr_comp (desc, OFFSET_FIELD, gfc_array_index_type);
}

tree
gfc_conv_descriptor_offset_get (tree desc)
{
  return non_lvalue_loc (input_location, conv_descriptor_offset (desc));
}

void
gfc_conv_descriptor_offset_set (stmtblock_t *block, tree desc, tree value)
{
  set_value (block, conv_descriptor_offset (desc), value);
}


/* Return a reference to the dtype field of array descriptor DESC.  */

static tree
conv_descriptor_dtype (tree desc)
{
  return get_descr_comp (desc, DTYPE_FIELD, get_dtype_type_node ());
}

/* Return the value of the dtype field of the array descriptor DESC.  */

tree
gfc_conv_descriptor_dtype_get (tree desc)
{
  return non_lvalue_loc (input_location, conv_descriptor_dtype (desc));
}

/* Add code to BLOCK setting to VALUE the dtype field of the array descriptor
   DESC.  */

void
gfc_conv_descriptor_dtype_set (stmtblock_t *block, tree desc, tree value)
{
  set_value (block, conv_descriptor_dtype (desc), value);
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


/* Return a reference to the rank of array descriptor DESC.  */

static tree
get_dtype_comp (tree desc, dtype_subfield field, tree type = NULL_TREE)
{
  tree dtype_ref = conv_descriptor_dtype (desc);
  return get_ref_comp (dtype_ref, field, type);
}


static tree
conv_descriptor_rank (tree desc)
{
  return get_dtype_comp (desc, GFC_DTYPE_RANK, signed_char_type_node);
}

/* Return the rank of the array descriptor DESC.  */

tree
gfc_conv_descriptor_rank_get (tree desc)
{
  return non_lvalue_loc (input_location, conv_descriptor_rank (desc));
}

/* Add code to BLOCK setting to VALUE the rank of the array descriptor DESC.  */

void
gfc_conv_descriptor_rank_set (stmtblock_t *block, tree desc, tree value)
{
  set_value (block, conv_descriptor_rank (desc), value);
}

/* Add code to BLOCK setting to VALUE the rank of the array descriptor DESC.  */

void
gfc_conv_descriptor_rank_set (stmtblock_t *block, tree desc, int value)
{
  gfc_conv_descriptor_rank_set (block, desc, gfc_rank_cst[value]);
}


/* Return a reference to descriptor DESC's format version.  */

static tree
conv_descriptor_version (tree desc)
{
  return get_dtype_comp (desc, GFC_DTYPE_VERSION, integer_type_node);
}

/* Return the value of descriptor DESC's format version.  */

tree
gfc_conv_descriptor_version_get (tree desc)
{
  return non_lvalue_loc (input_location, conv_descriptor_version (desc));
}

/* Add code to BLOCK setting to VALUE the descriptor DESC's format version.  */

void
gfc_conv_descriptor_version_set (stmtblock_t *block, tree desc, tree value)
{
  set_value (block, conv_descriptor_version (desc), value);
}


/* Return the element length from the descriptor dtype field.  */

static tree
conv_descriptor_elem_len (tree desc)
{
  return get_dtype_comp (desc, GFC_DTYPE_ELEM_LEN, size_type_node);
}

/* Return the descriptor DESC's array element size in bytes.  */

tree
gfc_conv_descriptor_elem_len_get (tree desc)
{
  return non_lvalue_loc (input_location, conv_descriptor_elem_len (desc));
}

/* Add code to BLOCK setting to VALUE the descriptor DESC's size (in bytes) of
   array elements.  */

void
gfc_conv_descriptor_elem_len_set (stmtblock_t *block, tree desc, tree value)
{
  set_value (block, conv_descriptor_elem_len (desc), value);
}


/* Return a reference to the type discriminator of array descriptor DESC.  */

static tree
conv_descriptor_type (tree desc)
{
  return get_dtype_comp (desc, GFC_DTYPE_TYPE, signed_char_type_node);
}

/* Return the value of the type discriminator of the array descriptor DESC.  */

tree
gfc_conv_descriptor_type_get (tree desc)
{
  return non_lvalue_loc (input_location, conv_descriptor_type (desc));
}

/* Add code to BLOCK setting to VALUE the type discriminator of the array
   descriptor DESC.  */

void
gfc_conv_descriptor_type_set (stmtblock_t *block, tree desc, tree value)
{
  set_value (block, conv_descriptor_type (desc), value);
}

/* Add code to BLOCK setting to VALUE the type discriminator of the array
   descriptor DESC.  */

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

/* Return some code setting to VALUE the type discriminator of the array
   descriptor DESC.  */

tree
gfc_conv_descriptor_type_set (tree desc, tree value)
{
  stmtblock_t block;

  gfc_init_block (&block);
  gfc_conv_descriptor_type_set (&block, desc, value);
  return gfc_finish_block (&block);
}

/* Return some code setting to VALUE the type discriminator of the array
   descriptor DESC.  */

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


/* Return a reference to the array access information of the (zero-based)
   dimension DIM of the array descriptor DESC.  */

static tree
conv_descriptor_dimension (tree desc, tree dim)
{
  tree tmp;

  tmp = gfc_get_descriptor_dimension (desc);

  return gfc_build_array_ref (tmp, dim, NULL_TREE, true);
}

/* Return the value of the array access information of the (zero-based)
   dimension DIM of the array represented by descriptor DESC.  */

tree
gfc_conv_descriptor_dimension_get (tree desc, tree dim)
{
  return non_lvalue_loc (input_location, conv_descriptor_dimension (desc, dim));
}

/* Return the value of the array access information of the (zero-based)
   dimension DIM of the array represented by descriptor DESC.  */

tree
gfc_conv_descriptor_dimension_get (tree desc, int dim)
{
  return gfc_conv_descriptor_dimension_get (desc, gfc_rank_cst[dim]);
}

/* Add code to BLOCK setting to VALUE the array access information of the
   (zero-based) dimension DIM of the array descriptor DESC.  */

void
gfc_conv_descriptor_dimension_set (stmtblock_t *block, tree desc, tree dim,
				   tree value)
{
  set_value (block, conv_descriptor_dimension (desc, dim), value);
}

/* Add code to BLOCK setting to VALUE the array access information of the
   (zero-based) dimension DIM of the array descriptor DESC.  */

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

/* Add code to BLOCK setting to VALUE the coarray token for the array
   represented by descriptor DESC.  */

void
gfc_conv_descriptor_token_set (stmtblock_t *block, tree desc, tree value)
{
  set_value (block, gfc_conv_descriptor_token (desc), value);
}


static tree
get_descr_dim_comp (tree desc, tree dim, dim_subfield field,
		    tree type = NULL_TREE)
{
  tree tmp = conv_descriptor_dimension (desc, dim);
  return get_ref_comp (tmp, field, type);
}


/* Return a reference to the stride for the (zero-based) dimension DIM of the
   array descriptor DESC.  */

static tree
get_descriptor_stride (tree desc, tree dim)
{
  return get_descr_dim_comp (desc, dim, STRIDE_SUBFIELD, gfc_array_index_type);
}

/* Return the value of the stride for the (zero-based) dimension DIM of the
   array represented by descriptor DESC.  */

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

/* Add code to BLOCK setting to VALUE the stride for the (zero-based) dimension
   DIM of the array descriptor DESC.  */

void
gfc_conv_descriptor_stride_set (stmtblock_t *block, tree desc,
				tree dim, tree value)
{
  set_value (block, get_descriptor_stride (desc, dim), value);
}


/* Return a reference to the lower bound for the (zero-based) dimension DIM of
   the array descriptor DESC.  */

static tree
get_descriptor_lbound (tree desc, tree dim)
{
  return get_descr_dim_comp (desc, dim, LBOUND_SUBFIELD, gfc_array_index_type);
}

/* Return the value of the lower bound for the (zero-based) dimension DIM of the
   array represented by descriptor DESC.  */

tree
gfc_conv_descriptor_lbound_get (tree desc, tree dim)
{
  return non_lvalue_loc (input_location, get_descriptor_lbound (desc, dim));
}

/* Add code to BLOCK setting to VALUE the lower bound for the (zero-based)
   dimension DIM of the array descriptor DESC.  */

void
gfc_conv_descriptor_lbound_set (stmtblock_t *block, tree desc,
				tree dim, tree value)
{
  set_value (block, get_descriptor_lbound (desc, dim), value);
}


/* Return a reference to the upper bound for the (zero-based) dimension DIM of
   the array descriptor DESC.  */

static tree
get_descriptor_ubound (tree desc, tree dim)
{
  return get_descr_dim_comp (desc, dim, UBOUND_SUBFIELD, gfc_array_index_type);
}

/* Return the value of the upper bound for the (zero-based) dimension DIM of the
   array represented by descriptor DESC.  */

tree
gfc_conv_descriptor_ubound_get (tree desc, tree dim)
{
  return non_lvalue_loc (input_location, get_descriptor_ubound (desc, dim));
}

/* Add code to BLOCK setting to VALUE the upper bound for the (zero-based)
   dimension DIM of the array descriptor DESC.  */

void
gfc_conv_descriptor_ubound_set (stmtblock_t *block, tree desc,
				tree dim, tree value)
{
  set_value (block, get_descriptor_ubound (desc, dim), value);
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


/* Array descriptor higher level routines.
 ******************************************************************************/

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


/* Cleanup those #defines.  */

#undef DATA_FIELD
#undef OFFSET_FIELD
#undef DTYPE_FIELD
#undef SPAN_FIELD
#undef DIMENSION_FIELD
#undef CAF_TOKEN_FIELD
#undef STRIDE_SUBFIELD
#undef LBOUND_SUBFIELD
#undef UBOUND_SUBFIELD



/* Return the DTYPE for an array.  This describes the type and type parameters
   of the array.  */
/* TODO: Only call this when the value is actually used, and make all the
   unknown cases abort.  */

tree
gfc_get_dtype_rank_type (int rank, tree etype)
{
  tree ptype;
  tree size;
  int n;
  tree tmp;
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
      size = gfc_get_character_len_in_bytes (ptype);
      break;
    case BT_VOID:
      gcc_assert (TREE_CODE (ptype) == POINTER_TYPE);
      size = size_in_bytes (ptype);
      break;
    default:
      size = size_in_bytes (etype);
      break;
    }

  tree dtype_type_node = get_dtype_type_node ();

  gcc_assert (size);

  STRIP_NOPS (size);
  size = fold_convert (size_type_node, size);
  tmp = get_dtype_type_node ();
  field = gfc_advance_chain (TYPE_FIELDS (tmp),
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

  dtype = build_constructor (tmp, v);

  return dtype;
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


/* Modify a descriptor such that the lbound of a given dimension is the value
   specified.  This also updates ubound and offset accordingly.  */

void
gfc_conv_shift_descriptor_lbound (stmtblock_t* block, tree desc,
				  int dim, tree new_lbound)
{
  tree offs, ubound, lbound, stride;
  tree diff, offs_diff;

  new_lbound = fold_convert (gfc_array_index_type, new_lbound);

  offs = gfc_conv_descriptor_offset_get (desc);
  lbound = gfc_conv_descriptor_lbound_get (desc, gfc_rank_cst[dim]);
  ubound = gfc_conv_descriptor_ubound_get (desc, gfc_rank_cst[dim]);
  stride = gfc_conv_descriptor_stride_get (desc, gfc_rank_cst[dim]);

  /* Get difference (new - old) by which to shift stuff.  */
  diff = fold_build2_loc (input_location, MINUS_EXPR, gfc_array_index_type,
			  new_lbound, lbound);

  /* Shift ubound and offset accordingly.  This has to be done before
     updating the lbound, as they depend on the lbound expression!  */
  ubound = fold_build2_loc (input_location, PLUS_EXPR, gfc_array_index_type,
			    ubound, diff);
  gfc_conv_descriptor_ubound_set (block, desc, gfc_rank_cst[dim], ubound);
  offs_diff = fold_build2_loc (input_location, MULT_EXPR, gfc_array_index_type,
			       diff, stride);
  offs = fold_build2_loc (input_location, MINUS_EXPR, gfc_array_index_type,
			  offs, offs_diff);
  gfc_conv_descriptor_offset_set (block, desc, offs);

  /* Finally set lbound to value we want.  */
  gfc_conv_descriptor_lbound_set (block, desc, gfc_rank_cst[dim], new_lbound);
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
