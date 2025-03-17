/* Header for array descriptor functions
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

/* Build a null array descriptor constructor.  */
tree gfc_array_dataptr_type (tree);
tree gfc_build_null_descriptor (tree);
tree gfc_build_default_class_descriptor (tree, gfc_typespec &);
void gfc_clear_descriptor (stmtblock_t *block, gfc_symbol *, tree);
void gfc_nullify_descriptor (stmtblock_t *block, gfc_expr *, tree);
void gfc_clear_descriptor (stmtblock_t *block, gfc_symbol *, gfc_expr *, tree);
void gfc_set_scalar_null_descriptor (stmtblock_t *block, tree, gfc_symbol *, gfc_expr *, tree);
void gfc_set_descriptor_with_shape (stmtblock_t *, tree, tree,
				    gfc_expr *, gfc_expr *, locus *);
tree gfc_get_scalar_to_descriptor_type (tree scalar, symbol_attribute attr);
void gfc_set_descriptor_from_scalar (stmtblock_t *, tree, tree,
				     symbol_attribute *, tree = NULL_TREE);
void gfc_copy_sequence_descriptor (stmtblock_t &, tree, tree, bool);
void gfc_set_gfc_from_cfi (stmtblock_t *, stmtblock_t *, tree, tree, tree,
			   gfc_symbol *, bool, bool, bool);
int gfc_descriptor_rank (tree);

void gfc_copy_descriptor (stmtblock_t *block, tree dest, tree src,
			  gfc_expr *src_expr, bool subref);

tree gfc_conv_descriptor_data_get (tree);
tree gfc_conv_descriptor_offset_get (tree);
tree gfc_conv_descriptor_span_get (tree);
tree gfc_conv_descriptor_dtype_get (tree);
tree gfc_conv_descriptor_rank_get (tree);
tree gfc_conv_descriptor_elem_len_get (tree);
tree gfc_conv_descriptor_version_get (tree);
tree gfc_conv_descriptor_attribute_get (tree);
tree gfc_conv_descriptor_type_get (tree);
tree gfc_conv_descriptor_dimension_get (tree);
tree gfc_conv_descriptor_dimensions_get (tree);
tree gfc_conv_descriptor_dimensions_get (tree, tree);
tree gfc_conv_descriptor_stride_get (tree, tree);
tree gfc_conv_descriptor_lbound_get (tree, tree);
tree gfc_conv_descriptor_ubound_get (tree, tree);
tree gfc_conv_descriptor_extent_get (tree, tree);
tree gfc_conv_descriptor_sm_get (tree, tree);
tree gfc_conv_descriptor_token_get (tree);
tree gfc_conv_descriptor_token_field (tree);

void gfc_conv_descriptor_data_set (stmtblock_t *, tree, tree);
void gfc_conv_descriptor_offset_set (stmtblock_t *, tree, tree);
void gfc_conv_descriptor_token_set (stmtblock_t *, tree, tree);
void gfc_conv_descriptor_dtype_set (stmtblock_t *, tree, tree);
void gfc_conv_descriptor_dimensions_set (stmtblock_t *, tree, tree);
void gfc_conv_descriptor_version_set (stmtblock_t *, tree, tree);
void gfc_conv_descriptor_rank_set (stmtblock_t *, tree, tree);
void gfc_conv_descriptor_rank_set (stmtblock_t *, tree, int);
void gfc_conv_descriptor_span_set (stmtblock_t *, tree, tree);
void gfc_conv_descriptor_stride_set (stmtblock_t *, tree, tree, tree);
void gfc_conv_descriptor_lbound_set (stmtblock_t *, tree, tree, tree);
void gfc_conv_descriptor_ubound_set (stmtblock_t *, tree, tree, tree);

/* CFI descriptor.  */
tree gfc_get_cfi_desc_base_addr (tree);
tree gfc_get_cfi_desc_elem_len (tree);
tree gfc_get_cfi_desc_version (tree);
tree gfc_get_cfi_desc_rank (tree);
tree gfc_get_cfi_desc_type (tree);
tree gfc_get_cfi_desc_attribute (tree);
tree gfc_get_cfi_dim_lbound (tree, tree);
tree gfc_get_cfi_dim_extent (tree, tree);
tree gfc_get_cfi_dim_sm (tree, tree);

/* Shift lower bound of descriptor, updating ubound and offset.  */
void gfc_conv_shift_descriptor (stmtblock_t*, tree, const gfc_array_ref &);
void gfc_conv_shift_descriptor (stmtblock_t*, tree, int);
void gfc_conv_shift_descriptor (stmtblock_t*, tree, tree, int, tree);
void gfc_conv_shift_descriptor_subarray (stmtblock_t*, tree, gfc_expr *, gfc_expr *);
void gfc_conv_shift_descriptor (stmtblock_t *, tree, int, tree *, tree *);

void gfc_set_temporary_descriptor (stmtblock_t *, tree, tree, tree, tree,
				   tree[GFC_MAX_DIMENSIONS], tree[GFC_MAX_DIMENSIONS],
				   tree[GFC_MAX_DIMENSIONS], int, bool, bool, bool);

void gfc_set_descriptor (stmtblock_t *, tree, tree, gfc_expr *, int, int,
			 gfc_ss *, gfc_array_info *, tree [GFC_MAX_DIMENSIONS],
			 tree [GFC_MAX_DIMENSIONS], bool, bool);

tree gfc_descr_init_count (tree, int, int, gfc_expr **, gfc_expr **,
			   stmtblock_t *, stmtblock_t *, tree *, tree,
			   gfc_expr *, tree, bool, gfc_expr *, tree, bool,
			   tree *);
void
gfc_copy_descriptor_info (stmtblock_t *, tree, tree, int, gfc_ss *);
void
gfc_set_contiguous_array (stmtblock_t *block, tree desc, tree size,
			  tree data_ptr);
void
gfc_class_array_data_assign (stmtblock_t *, tree, tree, bool);

void
gfc_copy_descriptor (stmtblock_t *, tree, tree, int);

void
gfc_set_descriptor_for_assign_realloc (stmtblock_t *, gfc_loopinfo *,
				       gfc_expr *, gfc_expr *, tree, tree,
				       tree, tree);
tree
gfc_set_pdt_array_descriptor (stmtblock_t *, tree, gfc_array_spec *,
			      gfc_actual_arglist *);

