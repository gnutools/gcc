/* Array descriptor translation routines
   Copyright (C) 2002-2025 Free Software Foundation, Inc.

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
#include "gfortran.h"
#include "trans.h"
#include "fold-const.h"
#include "trans-types.h"
#include "trans-const.h"
#include "gimplify.h"
#include "trans-descriptor.h"
#include "trans-array.h"
#include "stor-layout.h"


tree
gfc_array_dataptr_type (tree desc)
{
  return (GFC_TYPE_ARRAY_DATAPTR_TYPE (TREE_TYPE (desc)));
}

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

#define DATA_FIELD 0
#define OFFSET_FIELD 1
#define DTYPE_FIELD 2
#define SPAN_FIELD 3
#define DIMENSION_FIELD 4
#define CAF_TOKEN_FIELD 5

#define SPACING_SUBFIELD 0
#define LBOUND_SUBFIELD 1
#define UBOUND_SUBFIELD 2


static tree
substitute_placeholder_in_type (tree type, tree root_struct)
{
  tree type_size = TYPE_SIZE (type);
  tree modified_type_size = SUBSTITUTE_PLACEHOLDER_IN_EXPR (type_size,
							    root_struct);
  tree type_size_unit = TYPE_SIZE_UNIT (type);
  tree modified_type_size_unit = SUBSTITUTE_PLACEHOLDER_IN_EXPR (type_size_unit,
								 root_struct);

  switch (TREE_CODE (type))
    {
    case POINTER_TYPE:
      {
	tree subtype = TREE_TYPE (type);
	tree modified_subtype = substitute_placeholder_in_type (subtype,
								root_struct);
	if (modified_subtype == subtype
	    && modified_type_size == type_size
	    && modified_type_size_unit == type_size_unit)
	  return type;
	else
	  return build_pointer_type (modified_subtype);
      }
      break;

    case ARRAY_TYPE:
      {
	tree elt_type = TREE_TYPE (type);
	tree modified_elt_type = substitute_placeholder_in_type (elt_type,
								 root_struct);
	tree idx_type = TYPE_DOMAIN (type);
	tree modified_idx_type = substitute_placeholder_in_type (idx_type,
								 root_struct);
	if (modified_elt_type == elt_type
	    && modified_idx_type == idx_type
	    && modified_type_size == type_size
	    && modified_type_size_unit == type_size_unit)
	  return type;
	else
	  {
	    tree new_type = build_array_type (modified_elt_type,
					      modified_idx_type);
	    TYPE_STRING_FLAG (new_type) = TYPE_STRING_FLAG (type);
	    return new_type;
	  }
      }
      break;

    case INTEGER_TYPE:
      {
	tree min_val = TYPE_MIN_VALUE (type);
	tree modified_min_val = SUBSTITUTE_PLACEHOLDER_IN_EXPR (min_val,
								root_struct);
	tree max_val = TYPE_MAX_VALUE (type);
	tree modified_max_val = SUBSTITUTE_PLACEHOLDER_IN_EXPR (max_val,
								root_struct);
	if (modified_min_val == min_val
	    && modified_max_val == max_val
	    && modified_type_size == type_size
	    && modified_type_size_unit == type_size_unit)
	  return type;
	else
	  {
	    tree new_type = build_range_type (type, modified_min_val,
					      modified_max_val);
	    TYPE_SIZE (new_type) = modified_type_size;
	    TYPE_SIZE_UNIT (new_type) = modified_type_size_unit;
	    return new_type;
	  }
      }
      break;

    default:
      gcc_unreachable ();
    }
}


namespace gfc_descriptor
{

namespace
{

tree
get_field (tree desc, unsigned field_idx)
{
  tree type = TREE_TYPE (desc);
  gcc_assert (GFC_DESCRIPTOR_TYPE_P (type));

  tree field = gfc_advance_chain (TYPE_FIELDS (type), field_idx);
  gcc_assert (field != NULL_TREE);

  return field;
}

tree
get_dtype_subfield (tree desc, unsigned subfield)
{
  tree dtype = get_field (desc, DTYPE_FIELD);
  tree field = gfc_advance_chain (TYPE_FIELDS (TREE_TYPE (dtype)), subfield);
  gcc_assert (field != NULL_TREE);
  return field;
}

tree
get_component (tree desc, unsigned field_idx)
{
  tree field = get_field (desc, field_idx);

  return fold_build3_loc (input_location, COMPONENT_REF, TREE_TYPE (field),
			  desc, field, NULL_TREE);
}

tree
get_data (tree desc)
{
  return get_component (desc, DATA_FIELD);
}

tree
conv_data_get (tree desc)
{
  tree type = TREE_TYPE (desc);
  gcc_assert (TREE_CODE (type) != REFERENCE_TYPE);

  tree field = get_data (desc);
  tree target_type = GFC_TYPE_ARRAY_DATAPTR_TYPE (type);
  gcc_assert (TREE_CODE (target_type) == POINTER_TYPE);
  if (type_contains_placeholder_p (TREE_TYPE (target_type)))
    target_type = substitute_placeholder_in_type (target_type, desc);
  tree t = fold_convert (target_type, field);
  return non_lvalue_loc (input_location, t);
}

void
conv_data_set (stmtblock_t *block, tree desc, tree value)
{
  tree field = get_data (desc);
  gfc_add_modify (block, field, fold_convert (TREE_TYPE (field), value));
}

tree
get_offset (tree desc)
{
  tree field = get_component (desc, OFFSET_FIELD);
  gcc_assert (TREE_TYPE (field) == gfc_array_index_type);
  return field;
}

tree
conv_offset_get (tree desc)
{
  return non_lvalue_loc (input_location, get_offset (desc));
}

void
conv_offset_set (stmtblock_t *block, tree desc, tree value)
{
  tree t = get_offset (desc);
  gfc_add_modify (block, t, fold_convert (TREE_TYPE (t), value));
}

tree
get_dtype (tree desc)
{
  tree field = get_component (desc, DTYPE_FIELD);
  gcc_assert (TREE_TYPE (field) == get_dtype_type_node ());
  return field;
}

tree
conv_dtype_get (tree desc)
{
  return non_lvalue_loc (input_location, get_dtype (desc));
}

void
conv_dtype_set (stmtblock_t *block, tree desc, tree val)
{
  tree t = get_dtype (desc);
  gfc_add_modify (block, t, val);
}

tree
get_span (tree desc)
{
  tree field = get_component (desc, SPAN_FIELD);
  gcc_assert (TREE_TYPE (field) == gfc_array_index_type);
  return field;
}

tree
conv_span_get (tree desc)
{
  return non_lvalue_loc (input_location, get_span (desc));
}

void
conv_span_set (stmtblock_t *block, tree desc, tree value)
{
  tree t = get_span (desc);
  gfc_add_modify (block, t, fold_convert (TREE_TYPE (t), value));
}

tree
get_rank (tree desc)
{
  tree tmp;
  tree dtype;

  dtype = get_dtype (desc);
  tmp = gfc_advance_chain (TYPE_FIELDS (TREE_TYPE (dtype)), GFC_DTYPE_RANK);
  gcc_assert (tmp != NULL_TREE
	      && TREE_TYPE (tmp) == signed_char_type_node);
  return fold_build3_loc (input_location, COMPONENT_REF, TREE_TYPE (tmp),
			  dtype, tmp, NULL_TREE);
}

tree
conv_rank_get (tree desc)
{
  return non_lvalue_loc (input_location, get_rank (desc));
}

void
conv_rank_set (stmtblock_t *block, tree desc, tree val)
{
  location_t loc = input_location;
  tree t = get_rank (desc);
  gfc_add_modify_loc (loc, block, t,
		      fold_convert_loc (loc, TREE_TYPE (t), val));
}

void
conv_rank_set (stmtblock_t *block, tree desc, int val)
{
  tree t = get_rank (desc);
  conv_rank_set (block, desc, build_int_cst (TREE_TYPE (t), val));
}

tree
get_version (tree desc)
{
  tree tmp;
  tree dtype;

  dtype = get_dtype (desc);
  tmp = gfc_advance_chain (TYPE_FIELDS (TREE_TYPE (dtype)), GFC_DTYPE_VERSION);
  gcc_assert (tmp != NULL_TREE
	      && TREE_TYPE (tmp) == integer_type_node);
  return fold_build3_loc (input_location, COMPONENT_REF, TREE_TYPE (tmp),
			  dtype, tmp, NULL_TREE);
}

tree
conv_version_get (tree desc)
{
  return non_lvalue_loc (input_location, get_version (desc));
}

void
conv_version_set (stmtblock_t *block, tree desc, tree val)
{
  location_t loc = input_location;
  tree t = get_version (desc);
  gfc_add_modify_loc (loc, block, t,
		      fold_convert_loc (loc, TREE_TYPE (t), val));
}

tree
get_elem_len (tree desc)
{
  tree tmp;
  tree dtype;

  dtype = get_dtype (desc);
  tmp = gfc_advance_chain (TYPE_FIELDS (TREE_TYPE (dtype)),
			   GFC_DTYPE_ELEM_LEN);
  gcc_assert (tmp != NULL_TREE
	      && TREE_TYPE (tmp) == size_type_node);
  return fold_build3_loc (input_location, COMPONENT_REF, TREE_TYPE (tmp),
			  dtype, tmp, NULL_TREE);
}

tree
conv_elem_len_get (tree desc)
{
  return non_lvalue_loc (input_location, get_elem_len (desc));
}

void
conv_elem_len_set (stmtblock_t *block, tree desc, tree value)
{
  location_t loc = input_location;
  tree t = get_elem_len (desc);
  gfc_add_modify_loc (loc, block, t, 
		      fold_convert_loc (loc, TREE_TYPE (t), value));
}

tree
get_attribute (tree desc)
{
  tree tmp;
  tree dtype;

  dtype = get_dtype (desc);
  tmp = gfc_advance_chain (TYPE_FIELDS (TREE_TYPE (dtype)),
			   GFC_DTYPE_ATTRIBUTE);
  gcc_assert (tmp!= NULL_TREE
	      && TREE_TYPE (tmp) == short_integer_type_node);
  return fold_build3_loc (input_location, COMPONENT_REF, TREE_TYPE (tmp),
			  dtype, tmp, NULL_TREE);
}

tree
conv_attribute_get (tree desc)
{
  return non_lvalue_loc (input_location, get_attribute (desc));
}

void
conv_attribute_set (stmtblock_t *block, tree desc, tree value)
{
  location_t loc = input_location;
  tree t = get_attribute (desc);
  gfc_add_modify_loc (loc, block, t,
		      fold_convert_loc (loc, TREE_TYPE (t), value));
}

tree
get_type (tree desc)
{
  tree tmp;
  tree dtype;

  dtype = get_dtype (desc);
  tmp = gfc_advance_chain (TYPE_FIELDS (TREE_TYPE (dtype)), GFC_DTYPE_TYPE);
  gcc_assert (tmp!= NULL_TREE
	      && TREE_TYPE (tmp) == signed_char_type_node);
  return fold_build3_loc (input_location, COMPONENT_REF, TREE_TYPE (tmp),
			  dtype, tmp, NULL_TREE);
}

tree
conv_type_get (tree desc)
{
  return non_lvalue_loc (input_location, get_type (desc));
}

void
conv_type_set (stmtblock_t *block, tree desc, tree value)
{
  location_t loc = input_location;
  tree t = get_type (desc);
  gfc_add_modify_loc (loc, block, t,
		      fold_convert_loc (loc, TREE_TYPE (t), value));
}

void
conv_type_set (stmtblock_t *block, tree desc, int value)
{
  tree field = get_dtype_subfield (desc, GFC_DTYPE_TYPE);
  tree val = build_int_cst (TREE_TYPE (field), value);
  conv_type_set (block, desc, val);
}

tree
get_dimensions (tree desc)
{
  tree field = get_component (desc, DIMENSION_FIELD);
  gcc_assert (TREE_CODE (TREE_TYPE (field)) == ARRAY_TYPE
	      && TREE_CODE (TREE_TYPE (TREE_TYPE (field))) == RECORD_TYPE);
  return field;
}

tree
get_dimensions (tree desc, tree type)
{
  tree t = get_dimensions (desc);
  return build4_loc (input_location, ARRAY_RANGE_REF, type, t,
		     gfc_index_zero_node, NULL_TREE, NULL_TREE);
}

tree
conv_dimensions_get (tree desc)
{
  return non_lvalue_loc (input_location, get_dimensions (desc));
}

tree
conv_dimensions_get (tree desc, tree type)
{
  tree t = get_dimensions (desc, type);
  return non_lvalue_loc (input_location, t);
}

void
conv_dimensions_set (stmtblock_t *block, tree desc, tree value)
{
  location_t loc = input_location;
  tree t = get_dimensions (desc, TREE_TYPE (value));
  gfc_add_modify_loc (loc, block, t, value);
}

tree
get_dimension (tree desc, tree dim)
{
  tree tmp;

  tmp = get_dimensions (desc);

  return gfc_build_array_ref (tmp, dim, true);
}

tree
conv_dimension_get (tree desc, tree dim)
{
  return non_lvalue_loc (input_location, get_dimension (desc, dim));
}

void
conv_dimension_set (stmtblock_t *block, tree desc, tree dim, tree value)
{
  location_t loc = input_location;
  tree t = get_dimension (desc, dim);
  gfc_add_modify_loc (loc, block, t, value);
}


tree
get_token_field (tree desc)
{
  gcc_assert (flag_coarray == GFC_FCOARRAY_LIB);
  return get_field (desc, CAF_TOKEN_FIELD);
}

tree
get_token (tree desc)
{
  gcc_assert (flag_coarray == GFC_FCOARRAY_LIB);
  tree field = get_component (desc, CAF_TOKEN_FIELD);
  /* Should be a restricted pointer - except in the finalization wrapper.  */
  gcc_assert (TREE_TYPE (field) == prvoid_type_node
	      || TREE_TYPE (field) == pvoid_type_node);
  return field;
}

tree
conv_token_get (tree desc)
{
  return non_lvalue_loc (input_location, get_token (desc));
}

void
conv_token_set (stmtblock_t *block, tree desc, tree value)
{
  location_t loc = input_location;
  tree t = get_token (desc);
  gfc_add_modify_loc (loc, block, t,
		      fold_convert_loc (loc, TREE_TYPE (t), value));
}

tree
get_subfield (tree desc, tree dim, unsigned field_idx)
{
  tree tmp = get_dimension (desc, dim);
  tree field = gfc_advance_chain (TYPE_FIELDS (TREE_TYPE (tmp)), field_idx);
  gcc_assert (field != NULL_TREE);

  return fold_build3_loc (input_location, COMPONENT_REF, TREE_TYPE (field),
			  tmp, field, NULL_TREE);
}

tree
get_spacing (tree desc, tree dim)
{
  tree field = get_subfield (desc, dim, SPACING_SUBFIELD);
  gcc_assert (TREE_TYPE (field) == gfc_array_index_type);
  return field;
}

tree
conv_spacing_get (tree desc, tree dim)
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
    return conv_span_get (desc);

  return non_lvalue_loc (input_location, get_spacing (desc, dim));
}

void
conv_spacing_set (stmtblock_t *block, tree desc, tree dim, tree value)
{
  location_t loc = input_location;
  tree t = get_spacing (desc, dim);
  gfc_add_modify_loc (loc, block, t,
		      fold_convert_loc (loc, TREE_TYPE (t), value));
}

tree
conv_stride_get (tree desc, tree dim)
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

  tree spacing = conv_spacing_get (desc, dim);
  tree len = conv_elem_len_get (desc);
  return fold_build2_loc (input_location, EXACT_DIV_EXPR, gfc_array_index_type,
			  spacing, len);
}

tree
get_lbound (tree desc, tree dim)
{
  tree field = get_subfield (desc, dim, LBOUND_SUBFIELD);
  gcc_assert (TREE_TYPE (field) == gfc_array_index_type);
  return field;
}

tree
conv_lbound_get (tree desc, tree dim)
{
  return non_lvalue_loc (input_location, get_lbound (desc, dim));
}

void
conv_lbound_set (stmtblock_t *block, tree desc, tree dim, tree value)
{
  location_t loc = input_location;
  tree t = get_lbound (desc, dim);
  gfc_add_modify_loc (loc, block, t,
		      fold_convert_loc (loc, TREE_TYPE (t), value));
}

tree
get_ubound (tree desc, tree dim)
{
  tree field = get_subfield (desc, dim, UBOUND_SUBFIELD);
  gcc_assert (TREE_TYPE (field) == gfc_array_index_type);
  return field;
}

tree
conv_ubound_get (tree desc, tree dim)
{
  return non_lvalue_loc (input_location, get_ubound (desc, dim));
}

void
conv_ubound_set (stmtblock_t *block, tree desc, tree dim, tree value)
{
  location_t loc = input_location;
  tree t = get_ubound (desc, dim);
  gfc_add_modify_loc (loc, block, t,
		      fold_convert_loc (loc, TREE_TYPE (t), value));
}

}

}


/* This provides READ-ONLY access to the data field.  The field itself
   doesn't have the proper type.  */

tree
gfc_conv_descriptor_data_get (tree desc)
{
  return gfc_descriptor::conv_data_get (desc);
}

/* This provides WRITE access to the data field.

   TUPLES_P is true if we are generating tuples.

   This function gets called through the following macros:
     gfc_conv_descriptor_data_set
     gfc_conv_descriptor_data_set.  */

void
gfc_conv_descriptor_data_set (stmtblock_t *block, tree desc, tree value)
{
  return gfc_descriptor::conv_data_set (block, desc, value);
}


/* This provides address access to the data field.  This should only be
   used by array allocation, passing this on to the runtime.  */

tree
gfc_conv_descriptor_offset_get (tree desc)
{
  return gfc_descriptor::conv_offset_get (desc);
}

void
gfc_conv_descriptor_offset_set (stmtblock_t *block, tree desc, tree value)
{
  return gfc_descriptor::conv_offset_set (block, desc, value);
}


tree
gfc_conv_descriptor_dtype_get (tree desc)
{
  return gfc_descriptor::conv_dtype_get (desc);
}

void
gfc_conv_descriptor_dtype_set (stmtblock_t *block, tree desc, tree val)
{
  gfc_descriptor::conv_dtype_set (block, desc, val);
}

tree
gfc_conv_descriptor_span_get (tree desc)
{
  return gfc_descriptor::conv_span_get (desc);
}

static void
gfc_conv_descriptor_span_set (stmtblock_t *block, tree desc, tree value)
{
  return gfc_descriptor::conv_span_set (block, desc, value);
}

tree
gfc_conv_descriptor_dimension_get (tree desc, tree dim)
{
  return gfc_descriptor::conv_dimension_get (desc, dim);
}

tree
gfc_conv_descriptor_dimensions_get (tree desc)
{
  return gfc_descriptor::conv_dimensions_get (desc);
}

tree
gfc_conv_descriptor_dimensions_get (tree desc, tree type)
{
  return gfc_descriptor::conv_dimensions_get (desc, type);
}

void
gfc_conv_descriptor_dimensions_set (stmtblock_t *block, tree desc, tree value)
{
  return gfc_descriptor::conv_dimensions_set (block, desc, value);
}

tree
gfc_conv_descriptor_rank_get (tree desc)
{
  return gfc_descriptor::conv_rank_get (desc);
}

void
gfc_conv_descriptor_rank_set (stmtblock_t *block, tree desc, tree val)
{
  gfc_descriptor::conv_rank_set (block, desc, val);
}

void
gfc_conv_descriptor_rank_set (stmtblock_t *block, tree desc, int val)
{
  gfc_descriptor::conv_rank_set (block, desc, val);
}

tree
gfc_conv_descriptor_version_get (tree desc)
{
  return gfc_descriptor::conv_version_get (desc);
}

void
gfc_conv_descriptor_version_set (stmtblock_t *block, tree desc, tree val)
{
  gfc_descriptor::conv_version_set (block, desc, val);
}

/* Return the element length from the descriptor dtype field.  */

tree
gfc_conv_descriptor_elem_len_get (tree desc)
{
  return gfc_descriptor::conv_elem_len_get (desc);
}

void
gfc_conv_descriptor_elem_len_set (stmtblock_t *block, tree desc, tree value)
{
  gfc_descriptor::conv_elem_len_set (block, desc, value);
}

tree
gfc_conv_descriptor_attribute_get (tree desc)
{
  return gfc_descriptor::conv_attribute_get (desc);
}

void
gfc_conv_descriptor_attribute_set (stmtblock_t *block, tree desc, tree value)
{
  gfc_descriptor::conv_attribute_set (block, desc, value);
}

tree
gfc_conv_descriptor_type_get (tree desc)
{
  return gfc_descriptor::conv_type_get (desc);
}

void
gfc_conv_descriptor_type_set (stmtblock_t *block, tree desc, tree value)
{
  gfc_descriptor::conv_type_set (block, desc, value);
}

void
gfc_conv_descriptor_type_set (stmtblock_t *block, tree desc, int value)
{
  gfc_descriptor::conv_type_set (block, desc, value);
}

tree
gfc_conv_descriptor_token_get (tree desc)
{
  return gfc_descriptor::conv_token_get (desc);
}

tree
gfc_conv_descriptor_token_field (tree desc)
{
  return gfc_descriptor::get_token_field (desc);
}

void
gfc_conv_descriptor_token_set (stmtblock_t *block, tree desc, tree value)
{
  return gfc_descriptor::conv_token_set (block, desc, value);
}

tree
gfc_conv_descriptor_spacing_get (tree desc, tree dim)
{
  return gfc_descriptor::conv_spacing_get (desc, dim);
}

void
gfc_conv_descriptor_spacing_set (stmtblock_t *block, tree desc,
				tree dim, tree value)
{
  gfc_descriptor::conv_spacing_set (block, desc, dim, value);
}

tree
gfc_conv_descriptor_lbound_get (tree desc, tree dim)
{
  return gfc_descriptor::conv_lbound_get (desc, dim);
}

void
gfc_conv_descriptor_lbound_set (stmtblock_t *block, tree desc,
				tree dim, tree value)
{
  return gfc_descriptor::conv_lbound_set (block, desc, dim, value);
}

tree
gfc_conv_descriptor_ubound_get (tree desc, tree dim)
{
  return gfc_descriptor::conv_ubound_get (desc, dim);
}

void
gfc_conv_descriptor_ubound_set (stmtblock_t *block, tree desc,
				tree dim, tree value)
{
  return gfc_descriptor::conv_ubound_set (block, desc, dim, value);
}


/* Calculate the size of a given array dimension from the bounds.  This
   is simply (ubound - lbound + 1) if this expression is positive
   or 0 if it is negative (pick either one if it is zero).  Optionally
   (if or_expr is present) OR the (expression != 0) condition to it.  */

static tree
conv_array_extent_dim (tree lbound, tree ubound, bool maybe_negative, tree* or_expr)
{
  tree res;
  tree cond;

  /* Calculate (ubound - lbound + 1).  */
  res = fold_build2_loc (input_location, MINUS_EXPR, gfc_array_index_type,
			 ubound, lbound);
  res = fold_build2_loc (input_location, PLUS_EXPR, gfc_array_index_type, res,
			 gfc_index_one_node);

  /* Check whether the size for this dimension is negative.  */
  if (maybe_negative)
    {
      cond = fold_build2_loc (input_location, LE_EXPR, logical_type_node, res,
			      gfc_index_zero_node);
      res = fold_build3_loc (input_location, COND_EXPR, gfc_array_index_type, cond,
			     gfc_index_zero_node, res);
    }

  /* Build OR expression.  */
  if (maybe_negative && or_expr)
    *or_expr = fold_build2_loc (input_location, TRUTH_OR_EXPR,
				logical_type_node, *or_expr, cond);

  return res;
}


tree
gfc_conv_descriptor_extent_get (tree desc, tree dim)
{
  tree ubound = gfc_conv_descriptor_ubound_get (desc, dim);
  tree lbound = gfc_conv_descriptor_lbound_get (desc, dim);

  return conv_array_extent_dim (lbound, ubound, false, NULL);
}


tree
gfc_conv_descriptor_stride_get (tree desc, tree dim)
{
  return gfc_descriptor::conv_stride_get (desc, dim);
}


static bt
get_type_info (const bt &type)
{
  switch (type)
    {
    case BT_INTEGER:
    case BT_LOGICAL:
    case BT_REAL:
    case BT_COMPLEX:
    case BT_DERIVED:
    case BT_CHARACTER:
    case BT_VOID:
    case BT_UNSIGNED:
      return type;

    case BT_CLASS:
      return BT_DERIVED;

    case BT_PROCEDURE:
    case BT_ASSUMED:
      return BT_VOID;

    default:
      gcc_unreachable ();
      break;
    }

  return BT_UNKNOWN;
}


static tree
get_size_info (gfc_typespec &ts)
{
  switch (ts.type)
    {
    case BT_INTEGER:
    case BT_LOGICAL:
    case BT_REAL:
    case BT_COMPLEX:
    case BT_DERIVED:
    case BT_UNSIGNED:
      return size_in_bytes (gfc_typenode_for_spec (&ts));

    case BT_CHARACTER:
      {
	tree type = gfc_typenode_for_spec (&ts);
	if (POINTER_TYPE_P (type))
	  type = TREE_TYPE (type);
	gcc_assert (TREE_CODE (type) == ARRAY_TYPE);
	tree char_type = TREE_TYPE (type);
	tree len = ts.u.cl->backend_decl;
	return fold_build2_loc (input_location, MULT_EXPR, size_type_node,
				size_in_bytes (char_type),
				fold_convert (size_type_node, len));
      }

    case BT_CLASS:
      return get_size_info (ts.u.derived->components->ts);

    case BT_PROCEDURE:
    case BT_VOID:
    case BT_ASSUMED:
    default:
      gcc_unreachable ();
    }

  return NULL_TREE;
}


enum descr_change_type {
  UNKNOWN_CHANGE,
  EXPLICIT_NULLIFICATION,
  INITIALISATION,
  DEFAULT_INITIALISATION,
  NULL_INITIALISATION,
  SCALAR_VALUE
};


struct descr_change_info {
  enum descr_change_type type;
  tree descriptor_type;
  union
    {
      struct
	{
	  gfc_typespec *ts;
	}
      null_init;
      struct
	{
	  const symbol_attribute *attr; 
	}
      default_init;
      struct
	{
	  gfc_typespec *ts;
	  tree value;
	  tree caf_token;
	  bool clear_token;
	  bool use_declared_type;
	}
      scalar_value;
    }
  u;
};


#if 0
static modify_info *
get_internal_info (const descr_change_info &info)
{
  switch (info.type)
    {
    case UNKNOWN_CHANGE:
      return info.u.unknown_info;

    case EXPLICIT_NULLIFICATION:
      return info.u.nullification_info;

    case INITIALISATION:
      return info.u.initialization_info;

    case SCALAR_VALUE:
      return info.u.scalar_value.info;

    default:
      gcc_unreachable ();
    }
}
#endif


static tree
get_descr_data_value (const descr_change_info &info)
{
  switch (info.type)
    {
    case UNKNOWN_CHANGE:
      return NULL_TREE;

    case EXPLICIT_NULLIFICATION:
    case NULL_INITIALISATION:
      return null_pointer_node;

    case INITIALISATION:
      return NULL_TREE;

    case DEFAULT_INITIALISATION:
      if (!info.u.default_init.attr->pointer
	  || (gfc_option.rtcheck & GFC_RTCHECK_POINTER))
	return null_pointer_node;
      else
	return NULL_TREE;

    case SCALAR_VALUE:
      {
	tree value = info.u.scalar_value.value;
	if (POINTER_TYPE_P (TREE_TYPE (value)))
	  return value;
	else
	  return gfc_build_addr_expr (NULL_TREE, value);
      }

    default:
      gcc_unreachable ();
    }
}


static tree
get_descr_span (const descr_change_info &info)
{
  switch (info.type)
    {
    case UNKNOWN_CHANGE:
    case EXPLICIT_NULLIFICATION:
    case INITIALISATION:
    case DEFAULT_INITIALISATION:
    case NULL_INITIALISATION:
      return NULL_TREE;

    case SCALAR_VALUE:
      {
	tree fields = TYPE_FIELDS (info.descriptor_type);
	tree span_field = gfc_advance_chain (fields, SPAN_FIELD);
	return build_zero_cst (TREE_TYPE (span_field));
      }

    default:
      gcc_unreachable ();
    }
}


static tree
get_descr_caf_token (const descr_change_info &info)
{
  switch (info.type)
    {
    case UNKNOWN_CHANGE:
    case EXPLICIT_NULLIFICATION:
    case INITIALISATION:
    case DEFAULT_INITIALISATION:
    case NULL_INITIALISATION:
      return null_pointer_node;

    case SCALAR_VALUE:
      {
	if (info.u.scalar_value.caf_token != NULL_TREE)
	  return info.u.scalar_value.caf_token;
	else if (info.u.scalar_value.clear_token)
	  return null_pointer_node;
	else
	  return NULL_TREE;
      }

    default:
      gcc_unreachable ();
    }
}


static tree
get_elt_type (tree value)
{
  tree tmp = value;

  if (POINTER_TYPE_P (TREE_TYPE (tmp)))
    tmp = TREE_TYPE (tmp);

  tree etype = TREE_TYPE (tmp);

  /* For arrays, which are not scalar coarrays.  */
  if (TREE_CODE (etype) == ARRAY_TYPE && !TYPE_STRING_FLAG (etype))
    etype = TREE_TYPE (etype);

  return etype;
}


static tree
get_descr_element_length (const descr_change_info &change_info,
			  gfc_typespec *ts)
{
  if (change_info.type == UNKNOWN_CHANGE
      || change_info.type == EXPLICIT_NULLIFICATION
      || (ts
	  && (ts->type == BT_CLASS
	      || (ts->type == BT_CHARACTER && ts->deferred))))
    return NULL_TREE;

  if (change_info.type == SCALAR_VALUE)
    {
      tree value = change_info.u.scalar_value.value;
      if (!change_info.u.scalar_value.use_declared_type)
	{
	  if (TREE_CODE (value) == COMPONENT_REF)
	    {
	      tree parent_obj = TREE_OPERAND (value, 0);
	      tree len;
	      if (GFC_CLASS_TYPE_P (TREE_TYPE (parent_obj))
		  && gfc_class_len_get (parent_obj, &len))
		return len;
	    }

	  tree size;
	  tree etype = get_elt_type (value);
	  gfc_get_type_info (etype, nullptr, &size);
	  return size;
	}
    }

  return get_size_info (*ts);
}


static tree
get_descr_type (const struct descr_change_info &change_info,
		gfc_typespec *type_info)
{
  bt n;
  switch (change_info.type)
    {
    case UNKNOWN_CHANGE:
    case EXPLICIT_NULLIFICATION:
      n = BT_UNKNOWN;
      break;

    case INITIALISATION:
    case DEFAULT_INITIALISATION:
    case NULL_INITIALISATION:
      n = get_type_info (type_info->type);
      break;

    case SCALAR_VALUE:
      {
	if (change_info.u.scalar_value.use_declared_type)
	  n = get_type_info (type_info->type);
	else
	  {
	    tree etype = get_elt_type (change_info.u.scalar_value.value);
	    gfc_get_type_info (etype, &n, nullptr);
	  }
      }
      break;

    default:
      gcc_unreachable ();
    }

  tree descriptor_type = change_info.descriptor_type;
  tree dtype_field = gfc_advance_chain (TYPE_FIELDS (descriptor_type),
					DTYPE_FIELD);
  tree dtype_type = TREE_TYPE (dtype_field);
  tree type_info_field = gfc_advance_chain (TYPE_FIELDS (dtype_type),
					    GFC_DTYPE_TYPE);
  return build_int_cst (TREE_TYPE (type_info_field), n);
}


static tree
get_descr_dtype (const descr_change_info &change_info, gfc_typespec *ts,
		 int rank, const symbol_attribute & ATTRIBUTE_UNUSED)
{
  if (change_info.type == UNKNOWN_CHANGE
      || change_info.type == EXPLICIT_NULLIFICATION)
    return NULL_TREE;

  vec<constructor_elt, va_gc> *v = nullptr;

  tree type = get_dtype_type_node ();

  tree fields = TYPE_FIELDS (type);

  gfc_typespec *type_info;
  switch (change_info.type)
    {
    case INITIALISATION:
    case DEFAULT_INITIALISATION:
      type_info = ts;
      break;

    case NULL_INITIALISATION:
      type_info = change_info.u.null_init.ts;
      break;

    case SCALAR_VALUE:
      type_info = change_info.u.scalar_value.ts;
      break;

    default:
      gcc_unreachable ();
    }
  if (type_info == nullptr)
    type_info = ts;

  tree elem_len_val = get_descr_element_length (change_info, type_info);
  if (elem_len_val != NULL_TREE)
    {
      tree elem_len_field = gfc_advance_chain (fields, GFC_DTYPE_ELEM_LEN);
      elem_len_val = fold_convert (TREE_TYPE (elem_len_field), elem_len_val);
      CONSTRUCTOR_APPEND_ELT (v, elem_len_field, elem_len_val);
    }

  tree version_field = gfc_advance_chain (fields, GFC_DTYPE_VERSION);
  tree version_val = build_int_cst (TREE_TYPE (version_field), 0);
  CONSTRUCTOR_APPEND_ELT (v, version_field, version_val);

  if (rank != -1)
    {
      tree rank_field = gfc_advance_chain (fields, GFC_DTYPE_RANK);
      tree rank_val = build_int_cst (TREE_TYPE (rank_field), rank);
      CONSTRUCTOR_APPEND_ELT (v, rank_field, rank_val);
    }

  tree type_val = get_descr_type (change_info, type_info);
  if (type_val != NULL_TREE)
    {
      tree type_info_field = gfc_advance_chain (fields, GFC_DTYPE_TYPE);
      CONSTRUCTOR_APPEND_ELT (v, type_info_field, type_val);
    }

  return build_constructor (type, v);
}


/* Build a null array descriptor constructor.  */

vec<constructor_elt, va_gc> *
get_descriptor_init (tree type, gfc_typespec *ts, int rank,
		     const symbol_attribute *attr,
		     const descr_change_info &change)
{
  vec<constructor_elt, va_gc> *v = nullptr;

  gcc_assert (GFC_DESCRIPTOR_TYPE_P (type));
  gcc_assert (DATA_FIELD == 0);
  tree fields = TYPE_FIELDS (type);

  /* Don't init pointers by default.  */
  tree data_value = get_descr_data_value (change);
  if (data_value != NULL_TREE)
    {
      tree data_field = gfc_advance_chain (fields, DATA_FIELD);
      data_value = fold_convert (TREE_TYPE (data_field), data_value);
      CONSTRUCTOR_APPEND_ELT (v, data_field, data_value);
    }

  tree dtype_value = get_descr_dtype (change, ts, rank, *attr);
  if (dtype_value != NULL_TREE)
    {
      tree dtype_field = gfc_advance_chain (fields, DTYPE_FIELD);
      CONSTRUCTOR_APPEND_ELT (v, dtype_field, dtype_value);
    }

  tree span_value = get_descr_span (change);
  if (span_value != NULL_TREE)
    {
      tree span_field = gfc_advance_chain (fields, SPAN_FIELD);
      tree span_value = build_zero_cst (TREE_TYPE (span_field));
      CONSTRUCTOR_APPEND_ELT (v, span_field, span_value);
    }

  if (flag_coarray == GFC_FCOARRAY_LIB && attr->codimension)
    {
      tree caf_token = get_descr_caf_token (change);
      if (caf_token != NULL_TREE)
	{
	  /* Declare the variable static so its array descriptor stays present
	     after leaving the scope.  It may still be accessed through another
	     image.  This may happen, for example, with the caf_mpi
	     implementation.  */
	  bool dim_present = GFC_TYPE_ARRAY_RANK (type) > 0
			     || GFC_TYPE_ARRAY_CORANK (type) > 0;
	  tree token_field = gfc_advance_chain (fields,
						CAF_TOKEN_FIELD
						- (!dim_present));
	  tree token_value = fold_convert (TREE_TYPE (token_field),
					   caf_token);
	  CONSTRUCTOR_APPEND_ELT (v, token_field, token_value);
	}
    }

  return v;
}


vec<constructor_elt, va_gc> *
get_default_array_descriptor_init (tree type, gfc_typespec &ts, int rank,
				   const symbol_attribute &attr)
{
  gcc_assert (GFC_DESCRIPTOR_TYPE_P (type));
  gcc_assert (DATA_FIELD == 0);

  struct descr_change_info info;
  info.type = DEFAULT_INITIALISATION;
  info.descriptor_type = type;

  return get_descriptor_init (type, &ts, rank, &attr, info);
}


vec<constructor_elt, va_gc> *
get_null_array_descriptor_init (tree type, gfc_typespec &ts, int rank,
				const symbol_attribute &attr)
{
  struct descr_change_info info;
  info.type = NULL_INITIALISATION;
  info.descriptor_type = type;
  info.u.null_init.ts = &ts;

  return get_descriptor_init (type, &ts, rank, &attr, info);
}


vec<constructor_elt, va_gc> *
get_null_array_descriptor (tree type, const symbol_attribute &attr)
{
  struct descr_change_info info;
  info.type = EXPLICIT_NULLIFICATION;
  info.descriptor_type = type;

  return get_descriptor_init (type, nullptr, 0, &attr, info);
}


tree
gfc_build_default_array_descriptor (tree type, gfc_typespec &ts, int rank,
				    const symbol_attribute &attr)
{
  gcc_assert (GFC_DESCRIPTOR_TYPE_P (type));

  struct descr_change_info info;
  info.type = DEFAULT_INITIALISATION;
  info.descriptor_type = type;

  return build_constructor (type,
			    get_descriptor_init (type, &ts, rank, &attr, info));
}


tree
gfc_build_null_array_descriptor (tree type, gfc_typespec &ts, int rank,
				 const symbol_attribute &attr)
{
  gcc_assert (GFC_DESCRIPTOR_TYPE_P (type));

  return build_constructor (type,
			    get_null_array_descriptor_init (type, ts, rank,
							    attr));
}


tree
gfc_build_null_array_descriptor (tree type, gfc_typespec &ts,
				 const symbol_attribute &attr)
{
  return gfc_build_null_array_descriptor (type, ts, -1, attr);
}


tree
gfc_build_default_class_descriptor (tree type, gfc_typespec &ts)
{
  vec<constructor_elt, va_gc> *v = nullptr;

  tree fields = TYPE_FIELDS (type);

#define CLASS_DATA_FIELD 0
#define CLASS_VPTR_FIELD 1

  tree data_field = gfc_advance_chain (fields, CLASS_DATA_FIELD);
  tree data_type = TREE_TYPE (data_field);

  gcc_assert (ts.type == BT_CLASS);
  tree data_value;
  if (ts.u.derived->components->attr.dimension
      || (ts.u.derived->components->attr.codimension
	  && flag_coarray != GFC_FCOARRAY_LIB))
    {
      gcc_assert (GFC_DESCRIPTOR_TYPE_P (data_type));
      gfc_component *data_comp = ts.u.derived->components;
      data_value = gfc_build_null_array_descriptor (data_type, ts,
						    data_comp->as->rank,
						    data_comp->attr);
    }
  else
    {
      gcc_assert (POINTER_TYPE_P (data_type));
      data_value = fold_convert (data_type, null_pointer_node);
    }
  CONSTRUCTOR_APPEND_ELT (v, data_field, data_value);

  tree vptr_field = gfc_advance_chain (fields, CLASS_VPTR_FIELD);

  tree vptr_value;
  if (ts.u.derived->attr.unlimited_polymorphic)
    vptr_value = fold_convert (TREE_TYPE (vptr_field), null_pointer_node);
  else
    {
      gfc_symbol *vsym = gfc_find_derived_vtab (ts.u.derived);
      tree vsym_decl = gfc_get_symbol_decl (vsym);
      vptr_value = gfc_build_addr_expr (nullptr, vsym_decl);
    }
  CONSTRUCTOR_APPEND_ELT (v, vptr_field, vptr_value);

#undef CLASS_DATA_FIELD
#undef CLASS_VPTR_FIELD
  
  return build_constructor (type, v);
}


void
gfc_clear_descriptor (gfc_expr *var_ref, gfc_se &var)
{
  symbol_attribute attr;

  gfc_array_spec *as = gfc_get_full_arrayspec_from_expr (var_ref);
  int rank = as != nullptr ? as->rank : 0;

  attr = gfc_expr_attr (var_ref);

  gfc_add_modify (&var.pre, var.expr,
		  gfc_build_null_array_descriptor (TREE_TYPE (var.expr),
						   var_ref->ts,
						   rank, attr));
}


static int
field_count (tree type)
{
  gcc_assert (TREE_CODE (type) == RECORD_TYPE);

  int count = 0;
  tree field = TYPE_FIELDS (type);
  while (field != NULL_TREE)
    {
      count++;
      field = DECL_CHAIN (field);
    }

  return count;
}


#if 0
static bool
complete_init_p (tree type, vec<constructor_elt, va_gc> *init_values)
{
  return (unsigned) field_count (type) == vec_safe_length (init_values);
}
#endif


static int
cmp_wi (const void *x, const void *y)
{
  const offset_int *wix = (const offset_int *) x;
  const offset_int *wiy = (const offset_int *) y;

  return wi::cmpu (*wix, *wiy);
}


static offset_int
get_offset_bits (tree field)
{
  offset_int field_offset = wi::to_offset (DECL_FIELD_OFFSET (field));
  offset_int field_bit_offset = wi::to_offset (DECL_FIELD_BIT_OFFSET (field));
  unsigned long offset_align = DECL_OFFSET_ALIGN (field);

  return field_offset * offset_align + field_bit_offset;
}


static bool
check_cleared_low_bits (const offset_int &val, int bitcount)
{
  if (bitcount == 0)
    return true;

  offset_int mask = wi::mask <offset_int> (bitcount, false);
  if ((val & mask) != 0)
    return false;

  return true;
}


static bool
right_shift_if_clear (const offset_int &val, int bitcount, offset_int *result)
{
  if (bitcount == 0)
    {
      *result = val;
      return true;
    }

  if (!check_cleared_low_bits (val, bitcount))
    return false;

  *result = val >> bitcount;
  return true;
}


static bool
contiguous_init_p (tree type, tree value)
{
  gcc_assert (TREE_CODE (value) == CONSTRUCTOR);
  auto_vec<offset_int> field_offsets;
  int count = field_count (type);
  field_offsets.reserve (count);

  tree field = TYPE_FIELDS (type);
  offset_int expected_offset = 0;
  while (field != NULL_TREE)
    {
      offset_int field_offset_bits = get_offset_bits (field);
      offset_int field_offset;
      if (!right_shift_if_clear (field_offset_bits, 3, &field_offset))
	return false;

      offset_int type_size = wi::to_offset (TYPE_SIZE_UNIT (TREE_TYPE (field)));
      int align = wi::ctz (type_size);
      if (!check_cleared_low_bits (field_offset, align))
	return false;

      if (field_offset != expected_offset)
	return false;

      expected_offset += type_size;
      field_offsets.quick_push (field_offset);

      field = DECL_CHAIN (field);
    }

  auto_vec<offset_int> value_offsets;
  value_offsets.reserve (count);

  unsigned i;
  tree field_init;
  FOR_EACH_CONSTRUCTOR_ELT (CONSTRUCTOR_ELTS (value), i, field, field_init)
    {
      if (TREE_TYPE (field) != TREE_TYPE (field_init))
	return false;

      offset_int field_offset_bits = get_offset_bits (field);
      offset_int field_offset;
      if (!right_shift_if_clear (field_offset_bits, 3, &field_offset))
	return false;

      value_offsets.quick_push (field_offset);
    }

  value_offsets.qsort (cmp_wi);

  unsigned idx = 0;
  offset_int field_off, val_off;
  while (field_offsets.iterate (idx, &field_off)
	 && value_offsets.iterate (idx, &val_off))
    {
      if (val_off != field_off)
	return false;

      idx++;
    }

  return true;
}


static bool
modifiable_p (tree data_ref)
{
  switch (TREE_CODE (data_ref))
    {
    case INDIRECT_REF:
      return true;

    case CONST_DECL:
      return false;

    case VAR_DECL:
    case PARM_DECL:
    case RESULT_DECL:
      return !TREE_CONSTANT (data_ref) && !TREE_READONLY (data_ref);

    case COMPONENT_REF:
      {
	tree field_decl = TREE_OPERAND (data_ref, 1);

	if (TREE_CONSTANT (field_decl) || TREE_READONLY (field_decl))
	  return false;
      }

    /* fallthrough  */
    case ARRAY_REF:
    case ARRAY_RANGE_REF:
    case REALPART_EXPR:
    case IMAGPART_EXPR:
    case VIEW_CONVERT_EXPR:
    case NOP_EXPR:
      {
	tree parent_ref = TREE_OPERAND (data_ref, 0);
	return modifiable_p (parent_ref);
      }

    default:
      gcc_unreachable ();
    }
}


typedef enum
{
  SINGLE,
  MULTIPLE
} init_kind;

typedef union
{
  tree single;
  vec<constructor_elt, va_gc> *multiple;
} init_values;

static void
init_struct (stmtblock_t *block, tree data_ref, tree value);

static void
init_struct (stmtblock_t *block, tree data_ref, init_kind kind,
	     init_values values)
{
  tree type = TREE_TYPE (data_ref);

  if (kind == SINGLE)
    {
      tree value = values.single;
      if (TREE_STATIC (data_ref)
	  || !modifiable_p (data_ref))
	DECL_INITIAL (data_ref) = value;
      else if (TREE_CODE (value) == CONSTRUCTOR
	       && !(TREE_CONSTANT (value)
		    && contiguous_init_p (type, value)))
	{
	  unsigned i;
	  tree field, field_init;
	  FOR_EACH_CONSTRUCTOR_ELT (CONSTRUCTOR_ELTS (value), i, field, field_init)
	    {
	      tree ref = fold_build3_loc (input_location, COMPONENT_REF,
					  TREE_TYPE (field),
					  unshare_expr (data_ref),
					  field, NULL_TREE);
	      init_struct (block, ref, field_init);
	    }
	}
      else
	gfc_add_modify (block, data_ref, value);
    }
  else if (TREE_STATIC (data_ref))
    return init_struct (block, data_ref,
			build_constructor (type, values.multiple));
  else
    {
      gcc_assert (TREE_CODE (type) == RECORD_TYPE);

      unsigned i;
      constructor_elt *ce;
      FOR_EACH_VEC_ELT (*values.multiple, i, ce)
	{
	  tree field_decl = ce->index;
	  tree ref = fold_build3_loc (input_location, COMPONENT_REF,
				      TREE_TYPE (field_decl),
				      unshare_expr (data_ref),
				      field_decl, NULL_TREE);
	  init_struct (block, ref, ce->value);
	}
    }
}


static void
init_struct (stmtblock_t *block, tree data_ref, tree value)
{
  init_values wrapped_values;
  wrapped_values.single = value;

  return init_struct (block, data_ref, SINGLE, wrapped_values);
}


static void
init_struct (stmtblock_t *block, tree data_ref,
	     vec<constructor_elt, va_gc> *values)
{
  init_values wrapped_values;
  wrapped_values.multiple = values;

  return init_struct (block, data_ref, MULTIPLE, wrapped_values);
}


#if 0
static void
set_from_constructor_elts (stmtblock_t *block, tree data_ref,
			   vec<constructor_elt, va_gc> *constructor_values)
{
  unsigned i;
  constructor_elt *ce;
  FOR_EACH_VEC_ELT (*constructor_values, i, ce)
    {
      tree field_decl = ce->index;
      tree ref = fold_build3_loc (input_location, COMPONENT_REF,
				  TREE_TYPE (field_decl), data_ref,
				  field_decl, NULL_TREE);
      gfc_add_modify (block, ref, ce->value);
    }
}
#endif


void
gfc_clear_descriptor (stmtblock_t *block, gfc_symbol *sym, tree descriptor)
{
  symbol_attribute attr;

  gfc_array_spec *as = sym->ts.type == BT_CLASS
		       ? CLASS_DATA (sym)->as
		       : sym->as;
  int rank = as != nullptr ? as->rank : 0;

  attr = gfc_symbol_attr (sym);

  init_struct (block, descriptor,
	       get_null_array_descriptor_init (TREE_TYPE (descriptor),
					       sym->ts, rank, attr));
}


void
gfc_nullify_descriptor (stmtblock_t *block, gfc_expr *expr, tree descriptor)
{
  symbol_attribute attr;

  attr = gfc_expr_attr (expr);

  init_struct (block, descriptor,
	       get_null_array_descriptor (TREE_TYPE (descriptor), attr));
}


void
gfc_clear_descriptor (stmtblock_t *block, gfc_symbol *sym, 
		      gfc_expr *expr, tree descriptor)
{
  symbol_attribute attr;

  gfc_array_spec *as = sym->ts.type == BT_CLASS
		       ? CLASS_DATA (sym)->as
		       : sym->as;
  int rank = as == nullptr
	     ? 0
	     : as->type == AS_ASSUMED_RANK
	       ? expr->rank
	       : as->rank;

  attr = gfc_symbol_attr (sym);

  init_struct (block, descriptor,
	       get_null_array_descriptor_init (TREE_TYPE (descriptor),
					       expr->ts, rank, attr));
}


void
gfc_set_scalar_null_descriptor (stmtblock_t *block, tree descriptor,
				gfc_symbol *sym, gfc_expr *expr, tree value)
{
  symbol_attribute attr;

  attr = gfc_symbol_attr (sym);

  struct descr_change_info info;
  info.type = SCALAR_VALUE;
  info.descriptor_type = TREE_TYPE (descriptor);
  info.u.scalar_value.ts = &expr->ts;
  info.u.scalar_value.value = value;
  info.u.scalar_value.caf_token = value;
  info.u.scalar_value.clear_token = true;
  info.u.scalar_value.use_declared_type = true;

  init_struct (block, descriptor,
	       get_descriptor_init (TREE_TYPE (descriptor), &sym->ts, 0,
				    &attr, info));
}


void
gfc_set_descriptor_from_scalar (stmtblock_t *block, tree desc, tree scalar,
				symbol_attribute *attr, tree caf_token)
{
  struct descr_change_info info;
  info.type = SCALAR_VALUE;
  info.descriptor_type = TREE_TYPE (desc);
  info.u.scalar_value.ts = nullptr;
  info.u.scalar_value.value = scalar;
  info.u.scalar_value.caf_token = caf_token;
  info.u.scalar_value.clear_token = false;
  info.u.scalar_value.use_declared_type = false;

  init_struct (block, desc,
	       get_descriptor_init (TREE_TYPE (desc), nullptr, 0, attr, info));
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


static void
set_bounds_update_offset (stmtblock_t *block, tree desc, int dim,
			  tree lbound, tree ubound, tree spacing, tree lbound_diff,
			  tree *offset, tree *next_spacing, bool spacing_unchanged)
{
  /* Stabilize values in case the expressions depend on the existing bounds.  */
  lbound = fold_convert (gfc_array_index_type, lbound);
  lbound = gfc_evaluate_now (lbound, block);

  ubound = fold_convert (gfc_array_index_type, ubound);
  ubound = gfc_evaluate_now (ubound, block);

  spacing = fold_convert (gfc_array_index_type, spacing);
  spacing = gfc_evaluate_now (spacing, block);

  lbound_diff = fold_convert (gfc_array_index_type, lbound_diff);
  lbound_diff = gfc_evaluate_now (lbound_diff, block);

  gfc_conv_descriptor_lbound_set (block, desc,
				  gfc_rank_cst[dim], lbound);
  gfc_conv_descriptor_ubound_set (block, desc,
				  gfc_rank_cst[dim], ubound);
  if (!spacing_unchanged)
    gfc_conv_descriptor_spacing_set (block, desc, gfc_rank_cst[dim], spacing);

  if (!offset && !next_spacing)
    return;

  /* Update offset.  */
  if (!integer_zerop (lbound_diff))
    {
      tree tmp = fold_build2_loc (input_location, MULT_EXPR,
				  gfc_array_index_type, lbound_diff, spacing);
      tmp = fold_build2_loc (input_location, MINUS_EXPR,
			     gfc_array_index_type, *offset, tmp);
      *offset = gfc_evaluate_now (tmp, block);
    }

  if (!next_spacing)
    return;

  /* Set sm for next dimension.  */
  tree tmp = gfc_conv_array_extent_dim (lbound, ubound, NULL);
  *next_spacing = fold_build2_loc (input_location, MULT_EXPR,
			      gfc_array_index_type, spacing, tmp);
}


static void
set_descriptor_dimension (stmtblock_t *block, tree desc, int dim,
			  tree lbound, tree ubound, tree spacing, tree *offset,
			  tree *next_sm)
{
  set_bounds_update_offset (block, desc, dim, lbound, ubound, spacing, lbound,
			    offset, next_sm, false);
}


/* Modify a descriptor such that the lbound of a given dimension is the value
   specified.  This also updates ubound and offset accordingly.  */

static void
conv_shift_descriptor_lbound (stmtblock_t* block, tree from_desc, tree to_desc, int dim,
			      tree new_lbound, tree *offset, bool zero_based)
{
  new_lbound = fold_convert (gfc_array_index_type, new_lbound);
  new_lbound = gfc_evaluate_now (new_lbound, block);

  tree lbound = gfc_conv_descriptor_lbound_get (from_desc, gfc_rank_cst[dim]);
  tree ubound = gfc_conv_descriptor_ubound_get (from_desc, gfc_rank_cst[dim]);
  tree spacing = gfc_conv_descriptor_spacing_get (from_desc, gfc_rank_cst[dim]);

  tree diff;
  if (zero_based)
    diff = new_lbound;
  else
    {
      /* Get difference (new - old) by which to shift stuff.  */
      diff = fold_build2_loc (input_location, MINUS_EXPR, gfc_array_index_type,
			      new_lbound, lbound);
      diff = gfc_evaluate_now (diff, block);
    }

  /* Shift ubound and offset accordingly.  This has to be done before
     updating the lbound, as they depend on the lbound expression!  */
  tree tmp1 = fold_build2_loc (input_location, PLUS_EXPR, gfc_array_index_type,
			       ubound, diff);

  set_bounds_update_offset (block, to_desc, dim, new_lbound, tmp1, spacing, diff,
			    offset, nullptr, from_desc == to_desc);
}


class lb_info_base
{
public:
  virtual tree lower_bound (stmtblock_t *block, int dim) const = 0;
  virtual bool zero_based_src () const { return false; }
};


class lb_info : public lb_info_base
{
public:
  using lb_info_base::lower_bound;
  virtual gfc_expr *lower_bound (int dim) const = 0;
  virtual tree lower_bound (stmtblock_t *block, int dim) const;
};


tree
lb_info::lower_bound (stmtblock_t *block, int dim) const
{
  gfc_expr *lb_expr = lower_bound(dim);

  if (lb_expr == nullptr)
    return gfc_index_one_node;
  else
    {
      gfc_se lb_se;

      gfc_init_se (&lb_se, nullptr);
      gfc_conv_expr (&lb_se, lb_expr);

      gfc_add_block_to_block (block, &lb_se.pre);
      tree lb_var = gfc_create_var (gfc_array_index_type, "lower_bound");
      gfc_add_modify (block, lb_var,
		      fold_convert (gfc_array_index_type, lb_se.expr));
      gfc_add_block_to_block (block, &lb_se.post);

      return lb_var;
    }
}



class unset_lb : public lb_info
{
public:
  using lb_info::lower_bound;
  virtual gfc_expr *lower_bound (int) const { return nullptr; }
};


class defined_lb : public lb_info
{
  int rank;
  gfc_expr * const * lower_bounds;

public:
  using lb_info::lower_bound;
  defined_lb (int arg_rank, gfc_expr * const arg_lower_bounds[GFC_MAX_DIMENSIONS])
    : rank(arg_rank), lower_bounds(arg_lower_bounds) { }
  virtual gfc_expr *lower_bound (int dim) const { return lower_bounds[dim]; }
};


static void
conv_shift_descriptor (stmtblock_t *block, tree src, tree dest, int rank,
		       const lb_info_base &info)
{
  if (src != dest)
    {
      tree tmp = gfc_conv_descriptor_data_get (src);
      gfc_conv_descriptor_data_set (block, dest, tmp);

      tmp = gfc_conv_descriptor_span_get (src);
      gfc_conv_descriptor_span_set (block, dest, tmp);
    }

  tree offset = gfc_create_var (gfc_array_index_type, "offset");
  tree init_offset;
  if (info.zero_based_src ())
    init_offset = gfc_index_zero_node;
  else
    init_offset = gfc_conv_descriptor_offset_get (src);
  gfc_add_modify (block, offset, init_offset);

  /* Apply a shift of the lbound when supplied.  */
  for (int dim = 0; dim < rank; ++dim)
    {
      tree lower_bound = info.lower_bound (block, dim);
      conv_shift_descriptor_lbound (block, src, dest, dim, lower_bound, &offset,
				    info.zero_based_src ());
    }

  gfc_conv_descriptor_offset_set (block, dest, offset);
}


static void
conv_shift_descriptor (stmtblock_t *block, tree desc, int rank,
		       const lb_info_base &info)
{
  conv_shift_descriptor (block, desc, desc, rank, info);
}


class cond_descr_lb : public lb_info_base
{
  tree desc;
  tree cond;
public:
  cond_descr_lb (tree arg_desc, tree arg_cond)
    : desc (arg_desc), cond (arg_cond) { }

  virtual tree lower_bound (stmtblock_t *block, int dim) const;
  virtual bool zero_based_src () const { return true; }
};


tree
cond_descr_lb::lower_bound (stmtblock_t *block ATTRIBUTE_UNUSED, int dim) const
{
  tree lbound = gfc_conv_descriptor_lbound_get (desc, gfc_rank_cst[dim]);
  lbound = fold_build3_loc (input_location, COND_EXPR,
			    gfc_array_index_type, cond,
			    gfc_index_one_node, lbound);
  return lbound;
}


void
gfc_conv_shift_descriptor (stmtblock_t* block, tree desc, int rank)
{
  conv_shift_descriptor (block, desc, rank, unset_lb ());
}


static void
conv_shift_descriptor (stmtblock_t *block, tree desc, int rank,
		       gfc_expr * const lower_bounds[GFC_MAX_DIMENSIONS])
{
  conv_shift_descriptor (block, desc, rank, defined_lb (rank, lower_bounds));
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


class dataref_lb : public lb_info_base
{
  gfc_array_spec *as;
  gfc_expr *conv_arg;
  tree desc;

public:
  dataref_lb (gfc_array_spec *arg_as, gfc_expr *arg_conv_arg, tree arg_desc)
    : as(arg_as), conv_arg (arg_conv_arg), desc (arg_desc)
  {}
  virtual tree lower_bound (stmtblock_t *block, int dim) const;
};


tree
dataref_lb::lower_bound (stmtblock_t *block, int dim) const
{
  tree lbound;
  if (as && as->lower[dim])
    {
      gfc_se lbse;
      gfc_init_se (&lbse, NULL);
      gfc_conv_expr (&lbse, as->lower[dim]);
      gfc_add_block_to_block (block, &lbse.pre);
      lbound = gfc_evaluate_now (lbse.expr, block);
    }
  else if (as && conv_arg)
    {
      tree tmp = gfc_get_symbol_decl (conv_arg->symtree->n.sym);
      lbound = gfc_conv_descriptor_lbound_get (tmp, gfc_rank_cst[dim]);
    }
  else if (as)
    lbound = gfc_conv_descriptor_lbound_get (desc, gfc_rank_cst[dim]);
  else
    lbound = gfc_index_one_node;

  return fold_convert (gfc_array_index_type, lbound);
}


void
gfc_conv_shift_descriptor_subarray (stmtblock_t *block, tree desc,
				    gfc_expr *value_expr, gfc_expr *conv_arg)
{
  /* Obtain the array spec of full array references.  */
  gfc_array_spec *as;
  if (conv_arg)
    as = gfc_get_full_arrayspec_from_expr (conv_arg);
  else
    as = gfc_get_full_arrayspec_from_expr (value_expr);

  conv_shift_descriptor (block, desc, value_expr->rank, dataref_lb (as, conv_arg, desc));
}


void
gfc_conv_shift_descriptor (stmtblock_t *block, tree desc, int rank,
			   tree lbound[GFC_MAX_DIMENSIONS],
			   tree ubound[GFC_MAX_DIMENSIONS])
{
  tree size = gfc_index_one_node;
  tree offset = gfc_index_zero_node;
  for (int n = 0; n < rank; n++)
    {
      tree tmp = gfc_conv_descriptor_ubound_get (desc, gfc_rank_cst[n]);
      tmp = fold_build2_loc (input_location, PLUS_EXPR,
			     gfc_array_index_type, tmp,
			     gfc_index_one_node);
      gfc_conv_descriptor_ubound_set (block,
				      desc,
				      gfc_rank_cst[n],
				      tmp);
      gfc_conv_descriptor_lbound_set (block,
				      desc,
				      gfc_rank_cst[n],
				      gfc_index_one_node);
      size = gfc_evaluate_now (size, block);
      offset = fold_build2_loc (input_location, MINUS_EXPR,
				gfc_array_index_type,
				offset, size);
      offset = gfc_evaluate_now (offset, block);
      tmp = gfc_conv_array_extent_dim (lbound[n], ubound[n], nullptr);
      size = fold_build2_loc (input_location, MULT_EXPR,
			      gfc_array_index_type, size, tmp);
    }

  gfc_conv_descriptor_offset_set (block, desc,
				  offset);
}




int
gfc_descriptor_rank (tree descriptor)
{
  if (TREE_TYPE (descriptor) != NULL_TREE)
    return GFC_TYPE_ARRAY_RANK (TREE_TYPE (descriptor));

  tree dim = gfc_conv_descriptor_dimensions_get (descriptor);
  tree dim_type = TREE_TYPE (dim);
  gcc_assert (TREE_CODE (dim_type) == ARRAY_TYPE);
  tree idx_type = TYPE_DOMAIN (dim_type);
  gcc_assert (TREE_CODE (idx_type) == INTEGER_TYPE);
  gcc_assert (integer_zerop (TYPE_MIN_VALUE (idx_type)));
  tree idx_max = TYPE_MAX_VALUE (idx_type);
  if (idx_max == NULL_TREE)
    return GFC_MAX_DIMENSIONS;
  wide_int max = wi::to_wide (idx_max);
  return max.to_shwi () + 1;
}


void
gfc_conv_remap_descriptor (stmtblock_t *block, tree dest, tree src,
			   int src_rank, const gfc_array_spec &as)
{
  int dest_rank = gfc_descriptor_rank (dest);

  /* Set dtype.  */
  tree tmp = gfc_get_dtype (TREE_TYPE (src));
  gfc_conv_descriptor_dtype_set (block, dest, tmp);

  /* Copy data pointer.  */
  tree data = gfc_conv_descriptor_data_get (src);
  gfc_conv_descriptor_data_set (block, dest, data);

  /* Copy the span.  */
  tree span;
  if (VAR_P (src)
      && GFC_DECL_PTR_ARRAY_P (src))
    span = gfc_conv_descriptor_span_get (src);
  else
    {
      tmp = TREE_TYPE (src);
      tmp = TYPE_SIZE_UNIT (gfc_get_element_type (tmp));
      span = fold_convert (gfc_array_index_type, tmp);
    }
  gfc_conv_descriptor_span_set (block, dest, span);

  /* Copy offset but adjust it such that it would correspond
     to a lbound of zero.  */
  tree offset;
  if (src_rank == -1)
    offset = gfc_index_zero_node;
  else
    {
      tree offs = gfc_conv_descriptor_offset_get (src);
      for (int dim = 0; dim < src_rank; ++dim)
	{
	  tree spacing = gfc_conv_descriptor_spacing_get (src, gfc_rank_cst[dim]);
	  tree lbound = gfc_conv_descriptor_lbound_get (src, gfc_rank_cst[dim]);
	  tmp = fold_build2_loc (input_location, MULT_EXPR,
				 gfc_array_index_type, spacing, lbound);
	  offs = fold_build2_loc (input_location, PLUS_EXPR,
				  gfc_array_index_type, offs, tmp);
	}
      offset = offs;
    }
  /* Set the bounds as declared for the LHS and calculate strides as
     well as another offset update accordingly.  */
  tree spacing = gfc_conv_descriptor_spacing_get (src, gfc_rank_cst[0]);
  int last_dim = dest_rank - 1;
  for (int dim = 0; dim < dest_rank; ++dim)
    {
      gfc_se lower_se;
      gfc_se upper_se;

      gcc_assert (as.lower[dim] && as.upper[dim]);

      /* Convert declared bounds.  */
      gfc_init_se (&lower_se, NULL);
      gfc_init_se (&upper_se, NULL);
      gfc_conv_expr_val (&lower_se, as.lower[dim]);
      gfc_conv_expr_val (&upper_se, as.upper[dim]);

      gfc_add_block_to_block (block, &lower_se.pre);
      gfc_add_block_to_block (block, &upper_se.pre);

      set_descriptor_dimension (block, dest, dim, lower_se.expr, upper_se.expr,
				spacing, &offset,
				dim < last_dim ? &spacing : nullptr);
    }
  gfc_conv_descriptor_offset_set (block, dest, offset);
}


void
gfc_conv_remap_descriptor (stmtblock_t *block, tree dest, tree src,
			   int src_rank, const gfc_array_ref &ar)
{
  gfc_array_spec as;

  array_ref_to_array_spec (ar, as);

  gfc_conv_remap_descriptor (block, dest, src, src_rank, as);
}


void
gfc_conv_shift_descriptor (stmtblock_t *block, tree dest, tree src,
			   int rank, tree zero_cond)
{
  conv_shift_descriptor (block, src, dest, rank,
			 cond_descr_lb (dest, zero_cond));
}


void
gfc_copy_descriptor (stmtblock_t *block, tree dest, tree src,
		     gfc_expr *src_expr, bool subref)
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

  /* Add any offsets from subreferences.  */
  if (subref)
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
gfc_set_descriptor_with_shape (stmtblock_t *block, tree desc, tree ptr,
			       gfc_expr *shape, gfc_expr *lower, locus *where)
{
  /* Set the span field.  */
  tree elem_type = gfc_get_element_type (TREE_TYPE (desc));
  tree elem_len = TYPE_SIZE_UNIT (elem_type);
  elem_len = fold_convert (gfc_array_index_type, elem_len);
  gfc_conv_descriptor_span_set (block, desc, elem_len);

  /* Set data value, dtype, and offset.  */
  tree tmp = GFC_TYPE_ARRAY_DATAPTR_TYPE (TREE_TYPE (desc));
  gfc_conv_descriptor_data_set (block, desc, fold_convert (tmp, ptr));
  gfc_conv_descriptor_dtype_set (block, desc, gfc_get_dtype (TREE_TYPE (desc)));

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

  tree spacing = gfc_create_var (gfc_array_index_type, "spacing");
  tree offset = gfc_create_var (gfc_array_index_type, "offset");
  gfc_add_modify (block, spacing, elem_len);
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
      gfc_add_block_to_block (&body, &lowerse.post);
    }
  else
    lbound = gfc_index_one_node;

  /* Set bounds and spacing.  */
  gfc_conv_descriptor_lbound_set (&body, desc, dim, lbound);
  gfc_conv_descriptor_spacing_set (&body, desc, dim, spacing);

  gfc_conv_expr (&shapese, shape);
  gfc_add_block_to_block (&body, &shapese.pre);
  tree ubound = fold_build2_loc (
    input_location, MINUS_EXPR, gfc_array_index_type,
    fold_build2_loc (input_location, PLUS_EXPR, gfc_array_index_type, lbound,
		     fold_convert (gfc_array_index_type, shapese.expr)),
    gfc_index_one_node);
  gfc_conv_descriptor_ubound_set (&body, desc, dim, ubound);
  gfc_add_block_to_block (&body, &shapese.post);

  /* Calculate offset.  */
  tmp = fold_build2_loc (input_location, MULT_EXPR, gfc_array_index_type,
			 spacing, lbound);
  gfc_add_modify (&body, offset,
		  fold_build2_loc (input_location, PLUS_EXPR,
				   gfc_array_index_type, offset, tmp));

  /* Update spacing.  */
  gfc_add_modify (&body, spacing,
		  fold_build2_loc (input_location, MULT_EXPR,
				   gfc_array_index_type, spacing,
				   fold_convert (gfc_array_index_type,
						 shapese.expr)));
  /* Finish scalarization loop.  */
  gfc_trans_scalarizing_loops (&loop, &body);
  gfc_add_block_to_block (block, &loop.pre);
  gfc_add_block_to_block (block, &loop.post);
  gfc_cleanup_loop (&loop);

  gfc_add_modify (block, offset,
		  fold_build1_loc (input_location, NEGATE_EXPR,
				   gfc_array_index_type, offset));
  gfc_conv_descriptor_offset_set (block, desc, offset);
}

/* Convert a scalar to an array descriptor. To be used for assumed-rank
   arrays.  */

tree
gfc_get_scalar_to_descriptor_type (tree scalar, symbol_attribute attr)
{
  enum gfc_array_kind akind;

  if (attr.pointer)
    akind = GFC_ARRAY_POINTER_CONT;
  else if (attr.allocatable)
    akind = GFC_ARRAY_ALLOCATABLE;
  else
    akind = GFC_ARRAY_ASSUMED_SHAPE_CONT;

  if (POINTER_TYPE_P (TREE_TYPE (scalar)))
    scalar = TREE_TYPE (scalar);
  return gfc_get_array_type_bounds (TREE_TYPE (scalar), 0, 0, NULL, NULL, 1,
				    akind, !(attr.pointer || attr.target),
				    BT_UNKNOWN);
}


void
gfc_copy_sequence_descriptor (stmtblock_t &block, tree lhs_desc, tree rhs_desc,
			      bool assumed_rank_lhs)
{
  int lhs_rank = gfc_descriptor_rank (lhs_desc);
  int rhs_rank = gfc_descriptor_rank (rhs_desc);
  tree desc;

  if (assumed_rank_lhs || lhs_rank == rhs_rank)
    desc = rhs_desc;
  else
    {
      tree arr = gfc_create_var (TREE_TYPE (lhs_desc), "parm");
      gfc_conv_descriptor_data_set (&block, arr,
				    gfc_conv_descriptor_data_get (rhs_desc));
      gfc_conv_descriptor_lbound_set (&block, arr, gfc_index_zero_node,
				      gfc_index_zero_node);
      tree size = gfc_conv_descriptor_size (rhs_desc, rhs_rank);
      tree size_m1 = fold_build2_loc (input_location, MINUS_EXPR,
				      gfc_array_index_type, size,
				      gfc_index_one_node);
      gfc_conv_descriptor_ubound_set (&block, arr, gfc_index_zero_node, size_m1);
      tree spacing0 = 
	  gfc_conv_descriptor_spacing_get (rhs_desc, gfc_index_zero_node);
      size = fold_build2_loc (input_location, MULT_EXPR, gfc_array_index_type,
			      size, spacing0);
      gfc_conv_descriptor_spacing_set ( &block, arr, gfc_index_zero_node, spacing0);
      for (int i = 1; i < lhs_rank; i++)
	{
	  gfc_conv_descriptor_lbound_set (&block, arr, gfc_rank_cst[i],
					  gfc_index_zero_node);
	  gfc_conv_descriptor_ubound_set (&block, arr, gfc_rank_cst[i],
					  gfc_index_zero_node);
	  gfc_conv_descriptor_spacing_set (&block, arr, gfc_rank_cst[i], size);
	}
      gfc_conv_descriptor_dtype_set (&block, arr, 
				     gfc_conv_descriptor_dtype_get (rhs_desc));
      tree rank_value = build_int_cst (signed_char_type_node, lhs_rank);
      gfc_conv_descriptor_rank_set (&block, arr, rank_value);
      gfc_conv_descriptor_offset_set (&block, arr, gfc_index_zero_node);
      desc = arr;
    }

  gfc_class_array_data_assign (&block, lhs_desc, desc, true);
}


void
gfc_set_gfc_from_cfi (stmtblock_t *unconditional_block,
		      stmtblock_t *conditional_block, tree gfc, tree cfi,
		      tree rank, gfc_symbol *gfc_sym,
		      bool init_static, bool contiguous_gfc, bool contiguous_cfi)
{
  tree tmp = gfc_get_cfi_desc_base_addr (cfi);
  gfc_conv_descriptor_data_set (unconditional_block, gfc, tmp);

  if (init_static)
    {
      /* gfc->dtype = ... (from declaration, not from cfi).  */
      tree etype = gfc_get_element_type (TREE_TYPE (gfc));
      tree dtype = gfc_get_dtype_rank_type (gfc_sym->as->rank, etype);
      gfc_conv_descriptor_dtype_set (unconditional_block, gfc, dtype);

      if (gfc_sym->as->type == AS_ASSUMED_RANK)
	gfc_conv_descriptor_rank_set (unconditional_block, gfc, rank);
    }

  if (gfc_sym && gfc_sym->ts.type == BT_ASSUMED)
    {
      /* For type(*), take elem_len + dtype.type from the actual argument.  */
      tree elem_len_val = gfc_get_cfi_desc_elem_len (cfi);
      gfc_conv_descriptor_elem_len_set (unconditional_block, gfc, elem_len_val);

      tree cond;
      tree ctype = gfc_get_cfi_desc_type (cfi);
      ctype = fold_build2_loc (input_location, BIT_AND_EXPR, TREE_TYPE (ctype),
			       ctype, build_int_cst (TREE_TYPE (ctype),
						     CFI_type_mask));

      /* if (CFI_type_cptr) BT_VOID else BT_UNKNOWN  */
      /* Note: BT_VOID is could also be CFI_type_funcptr, but assume c_ptr. */
      cond = fold_build2_loc (input_location, EQ_EXPR, boolean_type_node, ctype,
			      build_int_cst (TREE_TYPE (ctype), CFI_type_cptr));

      stmtblock_t set_void;
      gfc_init_block (&set_void);
      gfc_conv_descriptor_type_set (&set_void, gfc, BT_VOID);

      stmtblock_t set_unknown;
      gfc_init_block (&set_unknown);
      gfc_conv_descriptor_type_set (&set_unknown, gfc, BT_UNKNOWN);

      tree tmp2 = fold_build3_loc (input_location, COND_EXPR, void_type_node, cond,
				   gfc_finish_block (&set_void),
				   gfc_finish_block (&set_unknown));

      /* if (CFI_type_struct) BT_DERIVED else  < tmp2 >  */
      cond = fold_build2_loc (input_location, EQ_EXPR, boolean_type_node, ctype,
			      build_int_cst (TREE_TYPE (ctype),
					     CFI_type_struct));
      stmtblock_t set_derived;
      gfc_init_block (&set_derived);
      gfc_conv_descriptor_type_set (&set_derived, gfc, BT_DERIVED);
      tmp2 = fold_build3_loc (input_location, COND_EXPR, void_type_node, cond,
			      gfc_finish_block (&set_derived), tmp2);

      /* if (CFI_type_Character) BT_CHARACTER else  < tmp2 >  */
      /* Note: this is kind=1, CFI_type_ucs4_char is handled in the 'else if'
	 before (see below, as generated bottom up).  */
      cond = fold_build2_loc (input_location, EQ_EXPR, boolean_type_node, ctype,
			      build_int_cst (TREE_TYPE (ctype),
			      CFI_type_Character));
      stmtblock_t set_character;
      gfc_init_block (&set_character);
      gfc_conv_descriptor_type_set (&set_character, gfc, BT_CHARACTER);
      tmp2 = fold_build3_loc (input_location, COND_EXPR, void_type_node, cond,
			      gfc_finish_block (&set_character), tmp2);

      /* if (CFI_type_ucs4_char) BT_CHARACTER else  < tmp2 >  */
      /* Note: gfc->elem_len = cfi->elem_len/4.  */
      /* However, assuming that CFI_type_ucs4_char cannot be recovered, leave
	 gfc->elem_len == cfi->elem_len, which helps with operations which use
	 sizeof() in Fortran and cfi->elem_len in C.  */
      tmp = gfc_get_cfi_desc_type (cfi);
      cond = fold_build2_loc (input_location, EQ_EXPR, boolean_type_node, tmp,
			      build_int_cst (TREE_TYPE (tmp),
					     CFI_type_ucs4_char));
      gfc_init_block (&set_character);
      gfc_conv_descriptor_type_set (&set_character, gfc, BT_CHARACTER);
      tmp2 = fold_build3_loc (input_location, COND_EXPR, void_type_node, cond,
			      gfc_finish_block (&set_character), tmp2);

      /* if (CFI_type_Complex) BT_COMPLEX + cfi->elem_len/2 else  < tmp2 >  */
      cond = fold_build2_loc (input_location, EQ_EXPR, boolean_type_node, ctype,
			      build_int_cst (TREE_TYPE (ctype),
			      CFI_type_Complex));
      stmtblock_t set_complex;
      gfc_init_block (&set_complex);
      gfc_conv_descriptor_type_set (&set_complex, gfc, BT_COMPLEX);
      tmp2 = fold_build3_loc (input_location, COND_EXPR, void_type_node, cond,
			      gfc_finish_block (&set_complex), tmp2);

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
      stmtblock_t set_ctype;
      gfc_init_block (&set_ctype);
      gfc_conv_descriptor_type_set (&set_ctype, gfc, ctype);
      tmp2 = fold_build3_loc (input_location, COND_EXPR, void_type_node, cond,
			      gfc_finish_block (&set_ctype), tmp2);
      gfc_add_expr_to_block (unconditional_block, tmp2);
    }

  tree elem_len;
  if (gfc_sym)
    /* We use gfc instead of cfi as this might be a constant.  */
    elem_len = fold_convert (gfc_array_index_type,
			     gfc_conv_descriptor_elem_len_get (gfc));
  else
    elem_len = fold_convert (gfc_array_index_type,
			     gfc_get_cfi_desc_elem_len (cfi));

  if (contiguous_cfi || contiguous_gfc)
    {
      /* gfc->span = elem_len (either cfi->elem_len or gfc.dtype.elem_len).  */
      tmp = elem_len;
    }
  else
    {
      /* gfc->span = ((cfi->dim[0].sm % cfi->elem_len)
		      ? cfi->dim[0].sm : cfi->elem_len).  */
      tree sm0 = gfc_get_cfi_dim_sm (cfi, gfc_rank_cst[0]);
      tmp = fold_build2_loc (input_location, TRUNC_MOD_EXPR,
			     gfc_array_index_type, sm0, elem_len);
      tmp = fold_build2_loc (input_location, NE_EXPR, boolean_type_node,
			     tmp, gfc_index_zero_node);
      tmp = build3_loc (input_location, COND_EXPR, gfc_array_index_type, tmp,
			sm0, elem_len);
    }
  gfc_conv_descriptor_span_set (conditional_block, gfc, tmp);

  /* Calculate offset + set lbound, ubound and stride.  */
  gfc_conv_descriptor_offset_set (conditional_block, gfc, gfc_index_zero_node);
  if (gfc_sym
      && gfc_sym->as->rank > 0
      && !gfc_sym->attr.pointer
      && !gfc_sym->attr.allocatable)
    for (int i = 0; i < gfc_sym->as->rank; ++i)
      {
	gfc_se se;
	gfc_init_se (&se, NULL );
	if (gfc_sym->as->lower[i])
	  {
	    gfc_conv_expr (&se, gfc_sym->as->lower[i]);
	    tmp = se.expr;
	  }
	else
	  tmp = gfc_index_one_node;
	gfc_add_block_to_block (conditional_block, &se.pre);
	gfc_conv_descriptor_lbound_set (conditional_block, gfc, gfc_rank_cst[i],
					tmp);
	gfc_add_block_to_block (conditional_block, &se.post);
      }

  /* Loop: for (i = 0; i < rank; ++i).  */
  tree idx = gfc_create_var (TREE_TYPE (rank), "idx");
  /* Loop body.  */
  stmtblock_t loop_body;
  gfc_init_block (&loop_body);
  /* gfc->dim[i].lbound = ... */
  if (!gfc_sym || (gfc_sym->attr.pointer || gfc_sym->attr.allocatable))
    {
      tmp = gfc_get_cfi_dim_lbound (cfi, idx);
      gfc_conv_descriptor_lbound_set (&loop_body, gfc, idx, tmp);
    }
  else if (gfc_sym && gfc_sym->as->type == AS_ASSUMED_RANK)
    gfc_conv_descriptor_lbound_set (&loop_body, gfc, idx,
				    gfc_index_one_node);

  /* gfc->dim[i].ubound = gfc->dim[i].lbound + cfi->dim[i].extent - 1. */
  tmp = fold_build2_loc (input_location, MINUS_EXPR, gfc_array_index_type,
			 gfc_conv_descriptor_lbound_get (gfc, idx),
			 gfc_index_one_node);
  tmp = fold_build2_loc (input_location, PLUS_EXPR, gfc_array_index_type,
			 gfc_get_cfi_dim_extent (cfi, idx), tmp);
  gfc_conv_descriptor_ubound_set (&loop_body, gfc, idx, tmp);

  if (contiguous_gfc)
    {
      /* gfc->dim[i].spacing
	   = idx == 0 ? cfi->elem_len / gfc->align : gfc->dim[i-1].spacing * cfi->dim[i-1].extent */
      tree cond = fold_build2_loc (input_location, EQ_EXPR, boolean_type_node,
				   idx, build_zero_cst (TREE_TYPE (idx)));
      tmp = fold_build2_loc (input_location, MINUS_EXPR, TREE_TYPE (idx),
			     idx, build_int_cst (TREE_TYPE (idx), 1));
      tree tmp2 = gfc_get_cfi_dim_extent (cfi, tmp);
      tmp = gfc_conv_descriptor_spacing_get (gfc, tmp);
      tmp = fold_build2_loc (input_location, MULT_EXPR, TREE_TYPE (tmp2),
			     tmp2, tmp);
      tmp = build3_loc (input_location, COND_EXPR, gfc_array_index_type, cond,
			fold_convert (gfc_array_index_type,
				      gfc_get_cfi_desc_elem_len (cfi)),
			tmp);
    }
  else
    {
      /* gfc->dim[i].spacing = cfi->dim[i].sm */
      tmp = gfc_get_cfi_dim_sm (cfi, idx);
    }
  gfc_conv_descriptor_spacing_set (&loop_body, gfc, idx, tmp);

  /* gfc->offset -= gfc->dim[i].spacing * gfc->dim[i].lbound. */
  tmp = fold_build2_loc (input_location, MULT_EXPR, gfc_array_index_type,
			 gfc_conv_descriptor_spacing_get (gfc, idx),
			 gfc_conv_descriptor_lbound_get (gfc, idx));
  tmp = fold_build2_loc (input_location, MINUS_EXPR, gfc_array_index_type,
			 gfc_conv_descriptor_offset_get (gfc), tmp);
  gfc_conv_descriptor_offset_set (&loop_body, gfc, tmp);
  /* Generate loop.  */
  gfc_simple_for_loop (conditional_block, idx, build_zero_cst (TREE_TYPE (idx)),
		       rank, LT_EXPR, build_one_cst (TREE_TYPE (idx)),
		       gfc_finish_block (&loop_body));
}


/* Obtain offsets for trans-types.cc(gfc_get_array_descr_info).  */

void
gfc_get_descriptor_offsets_for_info (const_tree desc_type, tree *data_off,
				     tree *dtype_off, tree *span_off,
				     tree *dim_off, tree *dim_size,
				     tree *spacing_suboff, tree *lower_suboff,
				     tree *upper_suboff)
{
  tree field;
  tree type;

  type = TYPE_MAIN_VARIANT (desc_type);
  field = gfc_advance_chain (TYPE_FIELDS (type), DATA_FIELD);
  *data_off = byte_position (field);
  field = gfc_advance_chain (TYPE_FIELDS (type), DTYPE_FIELD);
  *dtype_off = byte_position (field);
  field = gfc_advance_chain (TYPE_FIELDS (type), SPAN_FIELD);
  *span_off = byte_position (field);
  field = gfc_advance_chain (TYPE_FIELDS (type), DIMENSION_FIELD);
  *dim_off = byte_position (field);
  type = TREE_TYPE (TREE_TYPE (field));
  *dim_size = TYPE_SIZE_UNIT (type);
  field = gfc_advance_chain (TYPE_FIELDS (type), SPACING_SUBFIELD);
  *spacing_suboff = byte_position (field);
  field = gfc_advance_chain (TYPE_FIELDS (type), LBOUND_SUBFIELD);
  *lower_suboff = byte_position (field);
  field = gfc_advance_chain (TYPE_FIELDS (type), UBOUND_SUBFIELD);
  *upper_suboff = byte_position (field);
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


void
gfc_set_temporary_descriptor (stmtblock_t *block, tree desc, tree class_src,
			      tree elemsize, tree data_ptr,
			      tree lbound[GFC_MAX_DIMENSIONS],
			      tree ubound[GFC_MAX_DIMENSIONS],
			      tree spacing[GFC_MAX_DIMENSIONS], int rank,
			      bool omit_bounds, bool rank_changer,
			      bool shift_bounds)
{
  int n;

  if (!class_src)
    {
      /* Fill in the array dtype.  */
      gfc_conv_descriptor_dtype_set (block, desc,
				     gfc_get_dtype (TREE_TYPE (desc)));
    }
  else if (rank_changer)
    {
      /* For classes, we copy the whole original class descriptor to the
         temporary one, so we don't need to set the individual dtype fields.
	 Except for the case of rank altering intrinsics for which we
	 generate descriptors of different rank.  */

      /* Take the dtype from the class expression.  */
      tree src_data = gfc_class_data_get (class_src);
      tree dtype = gfc_conv_descriptor_dtype_get (src_data);
      gfc_conv_descriptor_dtype_set (block, desc, dtype);

      /* These transformational functions change the rank.  */
      gfc_conv_descriptor_rank_set (block, desc, rank);
    }

  tree offset = gfc_index_zero_node;
  if (!omit_bounds)
    {
      for (n = 0; n < rank; n++)
	{
	  /* Store the stride and bound components in the descriptor.  */
	  tree this_lbound = shift_bounds ? gfc_index_zero_node : lbound[n];
	  set_descriptor_dimension (block, desc, n, this_lbound, ubound[n],
				    spacing[n], &offset, nullptr);
	  if (TREE_CODE (spacing[n]) == INTEGER_CST
	      && GFC_TYPE_ARRAY_SPACING (TREE_TYPE (desc), n) == NULL_TREE)
	    GFC_TYPE_ARRAY_SPACING (TREE_TYPE (desc), n) = spacing[n];
	}
    }

  gfc_conv_descriptor_span_set (block, desc, elemsize);

  gfc_conv_descriptor_data_set (block, desc, data_ptr);

  /* The offset is zero because we create temporaries with a zero
     lower bound.  */
  gfc_conv_descriptor_offset_set (block, desc, offset);
}


/* Calculate the size of a given array dimension from the bounds.  This
   is simply (ubound - lbound + 1) if this expression is positive
   or 0 if it is negative (pick either one if it is zero).  Optionally
   (if or_expr is present) OR the (expression != 0) condition to it.  */

tree
gfc_conv_array_extent_dim (tree lbound, tree ubound, tree* or_expr)
{
  return conv_array_extent_dim (lbound, ubound, true, or_expr);
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


static bool
placeholder_free_element_type (tree type)
{
  if (!GFC_DESCRIPTOR_TYPE_P (type))
    return true;

  tree data_ptr_type = GFC_TYPE_ARRAY_DATAPTR_TYPE (type);
  gcc_assert (TREE_CODE (data_ptr_type) == POINTER_TYPE);

  return !type_contains_placeholder_p (TREE_TYPE (data_ptr_type));
}


static tree
get_descriptor_dtype (tree desc, int * prank)
{
  tree type = TREE_TYPE (desc);

  if (placeholder_free_element_type (type))
    return gfc_get_dtype (type, prank);
  else
    {
      tree data_ptr_type = GFC_TYPE_ARRAY_DATAPTR_TYPE (type);
      data_ptr_type = substitute_placeholder_in_type (data_ptr_type, desc);

      gcc_assert (TREE_CODE (data_ptr_type) == POINTER_TYPE);
      tree etype = TREE_TYPE (data_ptr_type);
      if (TREE_CODE (etype) == ARRAY_TYPE && ! TYPE_STRING_FLAG (etype))
	etype = TREE_TYPE (etype);

      int rank = prank ? *prank : GFC_TYPE_ARRAY_RANK (type);
      return gfc_get_dtype_rank_type (rank, etype);
    }
}


void
gfc_set_descriptor (stmtblock_t *block, tree dest, tree src, gfc_expr *src_expr,
		    int rank, int corank, gfc_ss *ss, gfc_array_info *info,
		    tree lowers[GFC_MAX_DIMENSIONS],
		    tree uppers[GFC_MAX_DIMENSIONS], bool data_needed,
		    bool subref, bool update_spacing_in_type)
{
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
     {dest, parmtype, dim} refer to the new one.
     {src, type, n, loop} refer to the original, which maybe
     a descriptorless array.
     The bounds of the scalarization are the bounds of the section.
     We don't have to worry about numeric overflows when calculating
     the offsets because all elements are within the array data.  */

  /* Set the dtype.  */
  tree dtype;
  if (src_expr->ts.type == BT_ASSUMED)
    {
      tree tmp2 = src;
      if (DECL_LANG_SPECIFIC (tmp2) && GFC_DECL_SAVED_DESCRIPTOR (tmp2))
	tmp2 = GFC_DECL_SAVED_DESCRIPTOR (tmp2);
      if (POINTER_TYPE_P (TREE_TYPE (tmp2)))
	tmp2 = build_fold_indirect_ref_loc (input_location, tmp2);
      dtype = gfc_conv_descriptor_dtype_get (tmp2);
    }
  else
    dtype = get_descriptor_dtype (src, &rank);
  gfc_conv_descriptor_dtype_set (block, dest, dtype);

  /* The 1st element in the section.  */
  tree base = gfc_index_zero_node;

  /* The offset from the 1st element in the section.  */
  tree offset = gfc_index_zero_node;

  for (int n = 0; n < ndim; n++)
    {
      tree spacing = gfc_conv_array_spacing (src, n);

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
	  spacing = gfc_evaluate_now (spacing, block);
	}

      tmp = gfc_conv_array_lbound (src, n);
      tmp = fold_build2_loc (input_location, MINUS_EXPR, TREE_TYPE (tmp),
			     start, tmp);
      tmp = fold_build2_loc (input_location, MULT_EXPR, TREE_TYPE (tmp),
			     tmp, spacing);
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

      gfc_conv_descriptor_lbound_set (block, dest,
				      gfc_rank_cst[dim], from);

      /* Set the new upper bound.  */
      gfc_conv_descriptor_ubound_set (block, dest,
				      gfc_rank_cst[dim], to);

      /* Multiply the spacing by the section stride to get the
	 total stride.  */
      spacing = fold_build2_loc (input_location, MULT_EXPR,
				 gfc_array_index_type,
				 spacing, info->stride[n]);

      tmp = fold_build2_loc (input_location, MULT_EXPR,
			     TREE_TYPE (offset), spacing, from);
      offset = fold_build2_loc (input_location, MINUS_EXPR,
			       TREE_TYPE (offset), offset, tmp);

      if (update_spacing_in_type
	  && TREE_CODE (spacing) == INTEGER_CST
	  && GFC_TYPE_ARRAY_SPACING (TREE_TYPE (dest), dim) == NULL_TREE)
	GFC_TYPE_ARRAY_SPACING (TREE_TYPE (dest), dim) = spacing;

      /* Store the new spacing.  */
      gfc_conv_descriptor_spacing_set (block, dest, gfc_rank_cst[dim], spacing);
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
    gfc_get_dataptr_offset (block, dest, src, base, subref, src_expr);
  else
    gfc_conv_descriptor_data_set (block, dest,
				  gfc_index_zero_node);

  gfc_conv_descriptor_offset_set (block, dest, offset);

  if (flag_coarray == GFC_FCOARRAY_LIB && src_expr->corank)
    {
      tmp = INDIRECT_REF_P (src) ? TREE_OPERAND (src, 0) : src;
      if (GFC_DESCRIPTOR_TYPE_P (TREE_TYPE (tmp)))
	{
	  tmp = gfc_conv_descriptor_token_get (tmp);
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



/* Fills in an array descriptor, and returns the number of elements in the
   array.  The pointer argument overflow, which should be of integer type,
   will increase in value if overflow occurs during the size calculation.
   Also sets the condition whether the array is empty through empty_array_cond.
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
    if (rank == 0)
      return 1;
    return stride;
   }  */
/*GCC ARRAYS*/

tree
gfc_descr_init_count (tree descriptor, int rank, int corank, gfc_expr ** lower,
		      gfc_expr ** upper, stmtblock_t * pblock,
		      stmtblock_t * descriptor_block, tree * overflow,
		      tree expr3_elem_size, gfc_expr *expr3, tree expr3_desc,
		      bool e3_has_nodescriptor, gfc_expr *expr,
		      tree element_size, gfc_typespec * explicit_ts,
		      tree *empty_array_cond)
{
  tree type;
  tree tmp;
  tree size;
  tree offset;
  tree stride;
  tree spacing;
  tree cond;
  gfc_expr *ubound;
  gfc_se se;
  int n;

  type = TREE_TYPE (descriptor);

  stride = gfc_index_one_node;
  offset = gfc_index_zero_node;

  /* Set the dtype before the alloc, because registration of coarrays needs
     it initialized.  */
  if (expr->ts.type == BT_CHARACTER
      && expr->ts.deferred
      && expr->ts.u.cl->backend_decl
      && VAR_P (expr->ts.u.cl->backend_decl))
    {
      type = gfc_typenode_for_spec (&expr->ts);
      tree dtype_value = gfc_get_dtype_rank_type (rank, type);
      gfc_conv_descriptor_dtype_set (pblock, descriptor, dtype_value);
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
      tree dtype_value = gfc_get_dtype_rank_type (rank, type);
      gfc_conv_descriptor_dtype_set (pblock, descriptor, dtype_value);
    }
  else if (explicit_ts)
    {
      type = gfc_typenode_for_spec (explicit_ts);
      tree dtype_value = gfc_get_dtype_rank_type (rank, type);
      gfc_conv_descriptor_dtype_set (pblock, descriptor, dtype_value);
    }
  else if (expr3_desc && GFC_DESCRIPTOR_TYPE_P (TREE_TYPE (expr3_desc)))
    {
      tree dtype_value = gfc_conv_descriptor_dtype_get (expr3_desc);
      gfc_conv_descriptor_dtype_set (pblock, descriptor, dtype_value);
    }
  else if (expr->ts.type == BT_CLASS && !explicit_ts
	   && expr3 && expr3->ts.type != BT_CLASS
	   && expr3_elem_size != NULL_TREE && expr3_desc == NULL_TREE)
    gfc_conv_descriptor_elem_len_set (pblock, descriptor, expr3_elem_size);
  else
    gfc_conv_descriptor_dtype_set (pblock, descriptor, gfc_get_dtype (type));

  gfc_conv_descriptor_elem_len_set (pblock, descriptor, element_size);

  tree empty_cond = logical_false_node;
  spacing = fold_convert_loc (input_location, gfc_array_index_type, element_size);

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
      gfc_conv_descriptor_lbound_set (descriptor_block, descriptor,
				      gfc_rank_cst[n], se.expr);
      conv_lbound = se.expr;

      /* Work out the offset for this component.  */
      tmp = fold_build2_loc (input_location, MULT_EXPR, gfc_array_index_type,
			     se.expr, spacing);
      offset = fold_build2_loc (input_location, MINUS_EXPR,
				gfc_array_index_type, offset, tmp);

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
	      tmp = gfc_conv_array_extent_dim (
		      gfc_conv_descriptor_lbound_get (expr3_desc,
						      gfc_rank_cst[n]),
		      gfc_conv_descriptor_ubound_get (expr3_desc,
						      gfc_rank_cst[n]),
		      nullptr);
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
      gfc_conv_descriptor_ubound_set (descriptor_block, descriptor,
				      gfc_rank_cst[n], se.expr);
      conv_ubound = se.expr;

      /* Store the spacing.  */
      gfc_conv_descriptor_spacing_set (descriptor_block, descriptor,
				       gfc_rank_cst[n], spacing);

      /* Calculate size and check whether extent is negative.  */
      size = gfc_conv_array_extent_dim (conv_lbound, conv_ubound,
					&empty_cond);
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
					    logical_type_node, tmp, spacing),
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

      spacing = fold_build2_loc (input_location, MULT_EXPR,
				 gfc_array_index_type, spacing, size);
      spacing = gfc_evaluate_now (spacing, pblock);
    }

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

  *empty_array_cond = empty_cond;

  if (rank == 0)
    return gfc_index_one_node;

  /* Update the array descriptor with the offset and the span.  */
  offset = gfc_evaluate_now (offset, pblock);
  gfc_conv_descriptor_offset_set (descriptor_block, descriptor, offset);
  tmp = fold_convert (gfc_array_index_type, element_size);
  gfc_conv_descriptor_span_set (descriptor_block, descriptor, tmp);

  return gfc_evaluate_now (stride, pblock);
}


void
gfc_copy_descriptor_info (stmtblock_t *block, tree src, tree dest, int rank,
			  gfc_ss *ss)
{
  tree old_field = gfc_conv_descriptor_dtype_get (src);
  gfc_conv_descriptor_dtype_set (block, dest, old_field);

  old_field = gfc_conv_descriptor_offset_get (src);
  gfc_conv_descriptor_offset_set (block, dest, old_field);

  for (int i = 0; i < rank; i++)
    {
      tree src_dim = gfc_rank_cst[gfc_get_array_ref_dim_for_loop_dim (ss, i)];
      old_field = gfc_conv_descriptor_dimension_get (src, src_dim);
      gfc_descriptor::conv_dimension_set (block, dest, gfc_rank_cst[i],
					  old_field);
    }

  if (flag_coarray == GFC_FCOARRAY_LIB
      && GFC_DESCRIPTOR_TYPE_P (TREE_TYPE (src))
      && GFC_TYPE_ARRAY_AKIND (TREE_TYPE (src))
	 == GFC_ARRAY_ALLOCATABLE)
    {
      old_field = gfc_conv_descriptor_token_get (src);
      gfc_conv_descriptor_token_set (block, dest, old_field);
    }
}


void
gfc_set_contiguous_array (stmtblock_t *block, tree desc, tree size,
			  tree data_ptr)
{
  tree dtype_value = gfc_get_dtype_rank_type (1, TREE_TYPE (desc));
  gfc_conv_descriptor_dtype_set (block, desc, dtype_value);
  gfc_conv_descriptor_lbound_set (block, desc,
				  gfc_index_zero_node,
				  gfc_index_one_node);
  tree span = gfc_conv_descriptor_span_get (desc);
  gfc_conv_descriptor_spacing_set (block, desc, gfc_index_zero_node, span);
  gfc_conv_descriptor_ubound_set (block, desc, gfc_index_zero_node, size);
  gfc_conv_descriptor_data_set (block, desc, data_ptr);
}


void
gfc_class_array_data_assign (stmtblock_t *block, tree lhs_desc, tree rhs_desc,
			     bool)
{
  tree tmp, tmp2, type;

  gfc_conv_descriptor_data_set (block, lhs_desc,
				gfc_conv_descriptor_data_get (rhs_desc));
  gfc_conv_descriptor_offset_set (block, lhs_desc,
				  gfc_conv_descriptor_offset_get (rhs_desc));

  gfc_conv_descriptor_dtype_set (block, lhs_desc,
				 gfc_conv_descriptor_dtype_get (rhs_desc));

  gfc_conv_descriptor_span_set (block, lhs_desc,
				gfc_conv_descriptor_span_get (rhs_desc));

  /* Assign the dimension as range-ref.  */
  tmp = gfc_conv_descriptor_dimensions_get (lhs_desc);
  tmp2 = gfc_conv_descriptor_dimensions_get (rhs_desc);

  int rank = gfc_descriptor_rank (lhs_desc);
  int rank2 = gfc_descriptor_rank (rhs_desc);
  if (rank == GFC_MAX_DIMENSIONS && rank2 != GFC_MAX_DIMENSIONS)
    type = TREE_TYPE (tmp2);
  else if (rank2 == GFC_MAX_DIMENSIONS && rank != GFC_MAX_DIMENSIONS)
    type = TREE_TYPE (tmp);
  else
    {
      int corank = GFC_TYPE_ARRAY_CORANK (TREE_TYPE (lhs_desc));
      int corank2 = GFC_TYPE_ARRAY_CORANK (TREE_TYPE (rhs_desc));
      if (corank > 0 && corank2 == 0)
	type = TREE_TYPE (tmp2);
      else if (corank2 > 0 && corank == 0)
	type = TREE_TYPE (tmp);
      else
	{
	  gcc_assert (TREE_TYPE (tmp) == TREE_TYPE (tmp2));
	  type = TREE_TYPE (tmp);
	}
    }

  tmp = gfc_conv_descriptor_dimensions_get (rhs_desc, type);
  gfc_conv_descriptor_dimensions_set (block, lhs_desc, tmp);
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
      tmp2 = gfc_conv_descriptor_spacing_get (src, dim);
      tmp = fold_build2_loc (input_location, MULT_EXPR, TREE_TYPE (tmp),
			     tmp, tmp2);
      offset = fold_build2_loc (input_location, MINUS_EXPR,
			TREE_TYPE (offset), offset, tmp);
      offset = gfc_evaluate_now (offset, block);
    }

  gfc_conv_descriptor_offset_set (block, dst, offset);
}


/* Returns the value of LBOUND for an expression.  This could be broken out
   from gfc_conv_intrinsic_bound but this seemed to be simpler.  This is
   called by gfc_alloc_allocatable_for_assignment.  */
static tree
get_std_lbound (gfc_expr *expr, tree desc, int dim, bool assumed_size)
{
  tree lbound;
  tree ubound;
  tree spacing;
  tree cond, cond1, cond3, cond4;
  tree tmp;
  gfc_ref *ref;

  if (GFC_DESCRIPTOR_TYPE_P (TREE_TYPE (desc)))
    {
      tmp = gfc_rank_cst[dim];
      lbound = gfc_conv_descriptor_lbound_get (desc, tmp);
      ubound = gfc_conv_descriptor_ubound_get (desc, tmp);
      spacing = gfc_conv_descriptor_spacing_get (desc, tmp);
      cond1 = fold_build2_loc (input_location, GE_EXPR, logical_type_node,
			       ubound, lbound);
      cond3 = fold_build2_loc (input_location, GE_EXPR, logical_type_node,
			       spacing, gfc_index_zero_node);
      cond3 = fold_build2_loc (input_location, TRUTH_AND_EXPR,
			       logical_type_node, cond3, cond1);
      cond4 = fold_build2_loc (input_location, LT_EXPR, logical_type_node,
			       spacing, gfc_index_zero_node);
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
				       tree class_expr2)
{
  bool coarray = (flag_coarray == GFC_FCOARRAY_LIB
		  && gfc_caf_attr (expr1, true).codimension);

  gfc_array_spec * as;
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

  /* Modify the lhs descriptor and the associated scalarizer
     variables. F2003 7.4.1.3: "If variable is or becomes an
     unallocated allocatable variable, then it is allocated with each
     deferred type parameter equal to the corresponding type parameters
     of expr , with the shape of expr , and with each lower bound equal
     to the corresponding element of LBOUND(expr)."
     Reuse size1 to keep a dimension-by-dimension track of the
     stride of the new array.  */
  tree size1 = elemsize2;
  tree offset = gfc_index_zero_node;

  for (int n = 0; n < expr2->rank; n++)
    {
      tree tmp = gfc_conv_array_extent_dim (loop->from[n], loop->to[n], NULL);

      tree lbound = gfc_index_one_node;
      tree ubound = tmp;

      if (as)
	{
	  tree lbd = get_std_lbound (expr2, desc2, n,
				     as->type == AS_ASSUMED_SIZE);
	  ubound = fold_build2_loc (input_location,
				    MINUS_EXPR,
				    gfc_array_index_type,
				    ubound, lbound);
	  ubound = fold_build2_loc (input_location,
				    PLUS_EXPR,
				    gfc_array_index_type,
				    ubound, lbd);
	  lbound = lbd;
	}

      gfc_conv_descriptor_lbound_set (block, desc, gfc_rank_cst[n], lbound);
      gfc_conv_descriptor_ubound_set (block, desc, gfc_rank_cst[n], ubound);
      gfc_conv_descriptor_spacing_set (block, desc, gfc_rank_cst[n], size1);
      lbound = gfc_conv_descriptor_lbound_get (desc,
					       gfc_rank_cst[n]);
      tree tmp2 = fold_build2_loc (input_location, MULT_EXPR,
				   gfc_array_index_type, lbound, size1);
      offset = fold_build2_loc (input_location, MINUS_EXPR,
				gfc_array_index_type,
				offset, tmp2);
      size1 = fold_build2_loc (input_location, MULT_EXPR,
			       gfc_array_index_type, tmp, size1);
    }

  /* Set the lhs descriptor and scalarizer offsets.  For rank > 1,
     the array offset is saved and the info.offset is used for a
     running offset.  Use the saved_offset instead.  */
  gfc_conv_descriptor_offset_set (block, desc, offset);

  if (GFC_DESCRIPTOR_TYPE_P (TREE_TYPE (desc)))
    gfc_conv_descriptor_span_set (block, desc, elemsize2);

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

      tree dtype_value = gfc_get_dtype_rank_type (expr1->rank, type);
      gfc_conv_descriptor_dtype_set (block, desc, dtype_value);
    }
  else if (expr1->ts.type == BT_CLASS)
    {
      tree type;
      if (expr2->ts.type != BT_CLASS)
	type = gfc_typenode_for_spec (&expr2->ts);
      else
	type = gfc_get_character_type_len (1, elemsize2);

      tree dtype_value = gfc_get_dtype_rank_type (expr2->rank, type);
      gfc_conv_descriptor_dtype_set (block, desc, dtype_value);

      /* Set the _len field as well...  */
      if (UNLIMITED_POLY (expr1))
	{
	  tree tmp = gfc_class_len_get (TREE_OPERAND (desc, 0));
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
      tree tmp = gfc_class_vptr_get (TREE_OPERAND (desc, 0));
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
    {
      gfc_conv_descriptor_dtype_set (block, desc,
				     gfc_get_dtype (TREE_TYPE (desc)));
    }
}


void
gfc_set_empty_descriptor (stmtblock_t *block, tree descr, int rank)
{
  for (int n = 0; n < rank; n++)
    {
      gfc_conv_descriptor_lbound_set (block, descr, gfc_rank_cst[n],
				      gfc_index_one_node);
      gfc_conv_descriptor_ubound_set (block, descr, gfc_rank_cst[n],
				      gfc_index_zero_node);
      gfc_conv_descriptor_spacing_set (block, descr, gfc_rank_cst[n],
				       gfc_index_zero_node);
    }

  gfc_conv_descriptor_offset_set (block, descr, gfc_index_zero_node);
}


tree
gfc_set_pdt_array_descriptor (stmtblock_t *block, tree desc,
			      gfc_array_spec *as,
			      gfc_actual_arglist *pdt_param_list)
{
  /* This chunk takes the expressions for 'lower' and 'upper'
     in the arrayspec and substitutes in the expressions for
     the parameters from 'pdt_param_list'. The descriptor
     fields can then be filled from the values so obtained.  */
  gcc_assert (GFC_DESCRIPTOR_TYPE_P (TREE_TYPE (desc)));

  gfc_conv_descriptor_dtype_set (block, desc,
				 gfc_get_dtype (TREE_TYPE (desc)));
  tree size = gfc_conv_descriptor_elem_len_get (desc);
  tree offset = gfc_index_zero_node;
  for (int i = 0; i < as->rank; i++)
    {
      gfc_se tse;
      gfc_init_se (&tse, NULL);
      gfc_expr *e = gfc_copy_expr (as->lower[i]);
      gfc_insert_parameter_exprs (e, pdt_param_list);
      gfc_conv_expr_type (&tse, e, gfc_array_index_type);
      gfc_free_expr (e);
      tree lower = tse.expr;
      gfc_conv_descriptor_lbound_set (block, desc,
				      gfc_rank_cst[i],
				      lower);
      e = gfc_copy_expr (as->upper[i]);
      gfc_insert_parameter_exprs (e, pdt_param_list);
      gfc_conv_expr_type (&tse, e, gfc_array_index_type);
      gfc_free_expr (e);
      tree upper = tse.expr;
      gfc_conv_descriptor_ubound_set (block, desc, gfc_rank_cst[i], upper);
      size = gfc_evaluate_now (size, block);
      size = fold_convert_loc (input_location, gfc_array_index_type, size);
      gfc_conv_descriptor_spacing_set (block, desc, gfc_rank_cst[i], size);
      offset = fold_build2_loc (input_location, MINUS_EXPR,
				gfc_array_index_type, offset, size);
      offset = gfc_evaluate_now (offset, block);
      tree tmp = gfc_conv_array_extent_dim (lower, upper, nullptr);
      size = fold_build2_loc (input_location, MULT_EXPR,
			      gfc_array_index_type, size, tmp);
    }
  gfc_conv_descriptor_offset_set (block, desc, offset);
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
  gcc_assert (integer_zerop (GFC_TYPE_ARRAY_LBOUND (TREE_TYPE (desc), 0)));
  tmp = fold_build2_loc (input_location, PLUS_EXPR, gfc_array_index_type,
			 ubound, gfc_index_one_node);
  arg1 = fold_build2_loc (input_location, MULT_EXPR, size_type_node,
			  fold_convert (size_type_node, tmp),
			  fold_convert (size_type_node, size));

  /* Reset array type upper bound if known.  */
  tree dataptr_type = GFC_TYPE_ARRAY_DATAPTR_TYPE (TREE_TYPE (desc));
  tree fixed_ptr_type = gfc_get_unbounded_array_type (dataptr_type);
  if (fixed_ptr_type != dataptr_type)
    GFC_TYPE_ARRAY_DATAPTR_TYPE (TREE_TYPE (desc)) = fixed_ptr_type;

  /* Call the realloc() function.  */
  tmp = gfc_call_realloc (pblock, arg0, arg1);
  gfc_conv_descriptor_data_set (pblock, desc, tmp);
}


