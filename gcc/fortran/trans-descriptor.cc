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

#define DATA_FIELD 0
#define OFFSET_FIELD 1
#define DTYPE_FIELD 2
#define SPAN_FIELD 3
#define DIMENSION_FIELD 4
#define CAF_TOKEN_FIELD 5

#define STRIDE_SUBFIELD 0
#define LBOUND_SUBFIELD 1
#define UBOUND_SUBFIELD 2


tree
gfc_get_type_field (tree type, unsigned field_idx, tree field_type = NULL_TREE)
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
  tree field = gfc_get_type_field (TREE_TYPE (ref), field_idx, type);

  return fold_build3_loc (input_location, COMPONENT_REF, TREE_TYPE (field),
			  ref, field, NULL_TREE);
}


static tree
get_descr_comp (tree desc, unsigned field_idx, tree type = NULL_TREE)
{
  gcc_assert (GFC_DESCRIPTOR_TYPE_P (TREE_TYPE (desc)));

  return get_ref_comp (desc, field_idx, type);
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
  location_t loc = input_location;
  tree field = get_descriptor_data (desc);
  gfc_add_modify_loc (loc, block, field,
		      fold_convert_loc (loc, TREE_TYPE (field), value));
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
  location_t loc = input_location;
  tree t = get_descriptor_offset (desc);
  gfc_add_modify_loc (loc, block, t,
		      fold_convert_loc (loc, TREE_TYPE (t), value));
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
  location_t loc = input_location;
  tree t = get_descriptor_dtype (desc);
  gfc_add_modify_loc (loc, block, t,
		      fold_convert_loc (loc, TREE_TYPE (t), value));
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
  location_t loc = input_location;
  tree t = gfc_conv_descriptor_span (desc);
  gfc_add_modify_loc (loc, block, t,
		      fold_convert_loc (loc, TREE_TYPE (t), value));
}


static tree
get_dtype_comp (tree desc, unsigned field_idx, tree type = NULL_TREE)
{ 
  tree dtype_ref = get_descriptor_dtype (desc);
  return get_ref_comp (dtype_ref, field_idx, type);
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
  location_t loc = input_location;
  tree t = get_descriptor_rank (desc);
  gfc_add_modify_loc (loc, block, t,
		      fold_convert_loc (loc, TREE_TYPE (t), value));
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
  location_t loc = input_location;
  tree t = get_descriptor_version (desc);
  gfc_add_modify_loc (loc, block, t,
		      fold_convert_loc (loc, TREE_TYPE (t), value));
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
  location_t loc = input_location;
  tree t = get_descriptor_elem_len (desc);
  gfc_add_modify_loc (loc, block, t,
		      fold_convert_loc (loc, TREE_TYPE (t), value));
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
  location_t loc = input_location;
  tree t = get_descriptor_type (desc);
  gfc_add_modify_loc (loc, block, t,
		      fold_convert_loc (loc, TREE_TYPE (t), value));
}

void
gfc_conv_descriptor_type_set (stmtblock_t *block, tree desc, int value)
{
  tree type = TREE_TYPE (desc);
  gcc_assert (GFC_DESCRIPTOR_TYPE_P (type));

  tree dtype_field = gfc_get_type_field (type, DTYPE_FIELD,
					 get_dtype_type_node ());
  tree field = gfc_get_type_field (TREE_TYPE (dtype_field), GFC_DTYPE_TYPE);

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
  location_t loc = input_location;
  tree t = get_descriptor_dimension (desc, dim);
  gfc_add_modify_loc (loc, block, t,
		      fold_convert_loc (loc, TREE_TYPE (t), value));
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
  location_t loc = input_location;
  tree t = gfc_conv_descriptor_token (desc);
  gfc_add_modify_loc (loc, block, t,
		      fold_convert_loc (loc, TREE_TYPE (t), value));
}


static tree
get_descr_dim_comp (tree desc, tree dim, unsigned field_idx,
			      tree type = NULL_TREE)
{
  tree tmp = get_descriptor_dimension (desc, dim);
  return get_ref_comp (tmp, field_idx, type);
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
  location_t loc = input_location;
  tree t = get_descriptor_stride (desc, dim);
  gfc_add_modify_loc (loc, block, t,
		      fold_convert_loc (loc, TREE_TYPE (t), value));
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
  location_t loc = input_location;
  tree t = get_descriptor_lbound (desc, dim);
  gfc_add_modify_loc (loc, block, t,
		      fold_convert_loc (loc, TREE_TYPE (t), value));
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
  location_t loc = input_location;
  tree t = get_descriptor_ubound (desc, dim);
  gfc_add_modify_loc (loc, block, t,
		      fold_convert_loc (loc, TREE_TYPE (t), value));
}


/*******************************************************************************
 * Array descriptor higher level routines.                                     *
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


/* Obtain offsets for trans-types.cc(gfc_get_array_descr_info).  */

void
gfc_get_descriptor_offsets_for_info (const_tree desc_type, tree *data_off,
				     tree *dtype_off, tree *span_off,
				     tree *dim_off, tree *dim_size,
				     tree *stride_suboff, tree *lower_suboff,
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
  field = gfc_advance_chain (TYPE_FIELDS (type), STRIDE_SUBFIELD);
  *stride_suboff = byte_position (field);
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
gfc_clear_descriptor (stmtblock_t *block, gfc_symbol *sym, tree descr)
{
  symbol_attribute attr = gfc_symbol_attr (sym);

  /* NULLIFY the data pointer for non-saved allocatables, or for non-saved
     pointers when -fcheck=pointer is specified.  */
  if (attr.allocatable
      || (attr.pointer && (gfc_option.rtcheck & GFC_RTCHECK_POINTER)))
    {
      gfc_conv_descriptor_data_set (block, descr, null_pointer_node);
      if (flag_coarray == GFC_FCOARRAY_LIB && attr.codimension)
	gfc_conv_descriptor_token_set (block, descr, null_pointer_node);
    }

  tree etype;

  gfc_array_spec *as;
  if (sym->ts.type == BT_CLASS)
    as = CLASS_DATA (sym)->as;
  else
    as = sym->as;

  gcc_assert (as && as->rank >= 0);
  etype = gfc_get_element_type (TREE_TYPE (descr));
  gfc_conv_descriptor_dtype_set (block, descr,
				 gfc_get_dtype_rank_type (as->rank, etype));
}


void
gfc_clear_descriptor (tree descr)
{
  tree type = TREE_TYPE (descr);
  DECL_INITIAL (descr) = gfc_build_null_descriptor (type);
}


tree
gfc_build_default_class_descriptor (const gfc_typespec &ts, tree class_type)
{
  gcc_assert (ts.type == BT_CLASS);

  gfc_symbol *derived = ts.u.derived;

  tree vptr;
  if (derived->attr.unlimited_polymorphic)
    vptr = null_pointer_node;
  else
    {
      gfc_symbol *vsym;
      vsym = gfc_find_derived_vtab (derived);
      vptr = gfc_get_symbol_decl (vsym);
      vptr = gfc_build_addr_expr (NULL, vptr);
    }

  tree tmp;
  if (derived->components->attr.dimension
      || (derived->components->attr.codimension
	  && flag_coarray != GFC_FCOARRAY_LIB))
    {
      tmp = gfc_class_type_data_field_get (class_type);
      tmp = gfc_build_null_descriptor (TREE_TYPE (tmp));
    }
  else
    tmp = null_pointer_node;

  return gfc_class_set_static_fields (class_type, vptr, tmp);
}


void
gfc_set_scalar_descriptor (stmtblock_t *block, tree descr, tree value)
{
  tree etype = TREE_TYPE (value);

  if (POINTER_TYPE_P (etype)
      && TREE_TYPE (etype)
      && TREE_CODE (TREE_TYPE (etype)) == ARRAY_TYPE)
    etype = TREE_TYPE (etype);
  gfc_conv_descriptor_dtype_set (block, descr,
				 gfc_get_dtype_rank_type (0, etype));
  gfc_conv_descriptor_data_set (block, descr, value);
  gfc_conv_descriptor_span_set (block, descr,
				gfc_conv_descriptor_elem_len_get (descr));
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
gfc_conv_shift_descriptor (stmtblock_t* block, tree desc, int rank)
{
  /* Apply a shift of the lbound when supplied.  */
  for (int dim = 0; dim < rank; ++dim)
    gfc_conv_shift_descriptor_lbound (block, desc, dim,
				      gfc_index_one_node);
}


static void
conv_shift_descriptor (stmtblock_t *block, tree desc, int rank,
		       gfc_expr * const (lbound[GFC_MAX_DIMENSIONS]))
{
  /* Apply a shift of the lbound when supplied.  */
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

      gfc_conv_shift_descriptor_lbound (block, desc, dim, lower_bound);
    }
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
gfc_nullify_descriptor (stmtblock_t *block, tree descr)
{
  gfc_conv_descriptor_data_set (block, descr, null_pointer_node); 
}


int
gfc_descriptor_rank (tree descriptor)
{
  if (TREE_TYPE (descriptor) != NULL_TREE)
    return GFC_TYPE_ARRAY_RANK (TREE_TYPE (descriptor));

  tree dim = gfc_get_descriptor_dimension (descriptor);
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
      gfc_conv_descriptor_ubound_set (&block, arr, gfc_index_zero_node, size);
      gfc_conv_descriptor_stride_set (
	&block, arr, gfc_index_zero_node,
	gfc_conv_descriptor_stride_get (rhs_desc, gfc_index_zero_node));
      for (int i = 1; i < lhs_rank; i++)
	{
	  gfc_conv_descriptor_lbound_set (&block, arr, gfc_rank_cst[i],
					  gfc_index_zero_node);
	  gfc_conv_descriptor_ubound_set (&block, arr, gfc_rank_cst[i],
					  gfc_index_zero_node);
	  gfc_conv_descriptor_stride_set (&block, arr, gfc_rank_cst[i], size);
	}
      gfc_conv_descriptor_dtype_set (&block, arr,
				     gfc_conv_descriptor_dtype_get (rhs_desc));
      gfc_conv_descriptor_rank_set (&block, arr, lhs_rank);
      gfc_conv_descriptor_span_set (&block, arr,
				    gfc_conv_descriptor_span_get (arr));
      gfc_conv_descriptor_offset_set (&block, arr, gfc_index_zero_node);
      desc = arr;
    }

  gfc_class_array_data_assign (&block, lhs_desc, desc, true);
}


void
gfc_conv_remap_descriptor (stmtblock_t *block, tree dest, tree src,
			   int src_rank, const gfc_array_spec &as)
{
  int dest_rank = gfc_descriptor_rank (dest);

  /* Set dtype.  */
  gfc_conv_descriptor_dtype_set (block, dest, gfc_get_dtype (TREE_TYPE (src)));

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
  if (src_rank == -1)
    gfc_conv_descriptor_offset_set (block, dest, gfc_index_zero_node);
  else
    {
      tree offs = gfc_conv_descriptor_offset_get (src);
      for (int dim = 0; dim < src_rank; ++dim)
	{
	  tree stride = gfc_conv_descriptor_stride_get (src,
					    gfc_rank_cst[dim]);
	  tree lbound = gfc_conv_descriptor_lbound_get (src,
					    gfc_rank_cst[dim]);
	  tree tmp = fold_build2_loc (input_location, MULT_EXPR,
				      gfc_array_index_type, stride, lbound);
	  offs = fold_build2_loc (input_location, PLUS_EXPR,
				  gfc_array_index_type, offs, tmp);
	}
      gfc_conv_descriptor_offset_set (block, dest, offs);
    }
  /* Set the bounds as declared for the LHS and calculate strides as
     well as another offset update accordingly.  */
  tree stride = gfc_conv_descriptor_stride_get (src, gfc_rank_cst[0]);
  for (int dim = 0; dim < dest_rank; ++dim)
    {
      gfc_se lower_se;
      gfc_se upper_se;

      gcc_assert (as.lower[dim] && as.upper[dim]);

      /* Convert declared bounds.  */
      gfc_init_se (&lower_se, NULL);
      gfc_init_se (&upper_se, NULL);
      gfc_conv_expr (&lower_se, as.lower[dim]);
      gfc_conv_expr (&upper_se, as.upper[dim]);

      gfc_add_block_to_block (block, &lower_se.pre);
      gfc_add_block_to_block (block, &upper_se.pre);

      tree lbound = fold_convert (gfc_array_index_type, lower_se.expr);
      tree ubound = fold_convert (gfc_array_index_type, upper_se.expr);

      lbound = gfc_evaluate_now (lbound, block);
      ubound = gfc_evaluate_now (ubound, block);

      gfc_add_block_to_block (block, &lower_se.post);
      gfc_add_block_to_block (block, &upper_se.post);

      /* Set bounds in descriptor.  */
      gfc_conv_descriptor_lbound_set (block, dest, gfc_rank_cst[dim], lbound);
      gfc_conv_descriptor_ubound_set (block, dest, gfc_rank_cst[dim], ubound);

      /* Set stride.  */
      stride = gfc_evaluate_now (stride, block);
      gfc_conv_descriptor_stride_set (block, dest, gfc_rank_cst[dim], stride);

      /* Update offset.  */
      tree offs = gfc_conv_descriptor_offset_get (dest);
      tree tmp = fold_build2_loc (input_location, MULT_EXPR,
				  gfc_array_index_type, lbound, stride);
      offs = fold_build2_loc (input_location, MINUS_EXPR,
			      gfc_array_index_type, offs, tmp);
      offs = gfc_evaluate_now (offs, block);
      gfc_conv_descriptor_offset_set (block, dest, offs);

      /* Update stride.  */
      tmp = gfc_conv_array_extent_dim (lbound, ubound, NULL);
      stride = fold_build2_loc (input_location, MULT_EXPR,
				gfc_array_index_type, stride, tmp);
    }
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
  tree tmp = gfc_conv_descriptor_data_get (src);
  gfc_conv_descriptor_data_set (block, dest, tmp);

  tree offset = gfc_index_zero_node;
  for (int n = 0 ; n < rank; n++)
    {
      tree lbound;

      lbound = gfc_conv_descriptor_lbound_get (dest, gfc_rank_cst[n]);
      lbound = fold_build3_loc (input_location, COND_EXPR,
				gfc_array_index_type, zero_cond,
				gfc_index_one_node, lbound);
      lbound = gfc_evaluate_now (lbound, block);

      tmp = gfc_conv_descriptor_ubound_get (src, gfc_rank_cst[n]);
      tmp = fold_build2_loc (input_location, PLUS_EXPR,
			     gfc_array_index_type, tmp, lbound);
      gfc_conv_descriptor_lbound_set (block, dest,
				      gfc_rank_cst[n], lbound);
      gfc_conv_descriptor_ubound_set (block, dest,
				      gfc_rank_cst[n], tmp);

      /* Set stride and accumulate the offset.  */
      tmp = gfc_conv_descriptor_stride_get (src, gfc_rank_cst[n]);
      gfc_conv_descriptor_stride_set (block, dest,
				      gfc_rank_cst[n], tmp);
      tmp = fold_build2_loc (input_location, MULT_EXPR,
			     gfc_array_index_type, lbound, tmp);
      offset = fold_build2_loc (input_location, MINUS_EXPR,
				gfc_array_index_type, offset, tmp);
      offset = gfc_evaluate_now (offset, block);
    }

  gfc_conv_descriptor_offset_set (block, dest, offset);
}


