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

#ifndef GFC_TRANS_DESCRIPTOR_H
#define GFC_TRANS_DESCRIPTOR_H

/* Build a null array descriptor constructor.  */
void gfc_nullify_descriptor (stmtblock_t *block, gfc_expr *, tree);
void gfc_clear_descriptor (stmtblock_t *block, gfc_symbol *, gfc_expr *, tree);
void gfc_set_scalar_null_descriptor (stmtblock_t *block, tree, gfc_symbol *, gfc_expr *, tree);
void gfc_set_descriptor_with_shape (stmtblock_t *, tree, tree,
				    gfc_expr *, locus *);
tree gfc_get_scalar_to_descriptor_type (tree scalar, symbol_attribute attr);
void gfc_set_descriptor_from_scalar (stmtblock_t *, tree, tree,
				     symbol_attribute *, tree = NULL_TREE);
void gfc_copy_sequence_descriptor (stmtblock_t &, tree, tree, bool);
void gfc_set_gfc_from_cfi (stmtblock_t *, stmtblock_t *, tree, tree, tree,
			   gfc_symbol *, bool, bool, bool);
int gfc_descriptor_rank (tree);

tree gfc_get_cfi_desc_base_addr (tree desc);
tree gfc_get_cfi_desc_elem_len (tree desc);
tree gfc_get_cfi_desc_version (tree desc);
tree gfc_get_cfi_desc_rank (tree desc);
tree gfc_get_cfi_desc_type (tree desc);
tree gfc_get_cfi_desc_attribute (tree desc);

tree gfc_get_cfi_dim_lbound (tree desc, tree idx);
tree gfc_get_cfi_dim_extent (tree desc, tree idx);
tree gfc_get_cfi_dim_sm (tree desc, tree idx);


tree gfc_get_descriptor_dimension (tree desc);
tree gfc_conv_descriptor_token (tree desc);

tree gfc_conv_descriptor_data_get (tree desc);
tree gfc_conv_descriptor_offset_get (tree desc);
tree gfc_conv_descriptor_dtype_get (tree desc);
tree gfc_conv_descriptor_elem_len_get (tree desc);
tree gfc_conv_descriptor_version_get (tree desc);
tree gfc_conv_descriptor_rank_get (tree desc);
tree gfc_conv_descriptor_type_get (tree desc);
tree gfc_conv_descriptor_span_get (tree desc);

tree gfc_conv_descriptor_dimension_get (tree desc, tree dim);
tree gfc_conv_descriptor_dimension_get (tree desc, int dim);
tree gfc_conv_descriptor_stride_get (tree desc, tree dim);
tree gfc_conv_descriptor_lbound_get (tree desc, tree dim);
tree gfc_conv_descriptor_ubound_get (tree desc, tree dim);

void gfc_conv_descriptor_data_set (stmtblock_t *block, tree desc, tree value);
void gfc_conv_descriptor_data_set (stmtblock_t *block, tree desc, tree value);
void gfc_conv_descriptor_offset_set (stmtblock_t *block, tree desc, tree value);
void gfc_conv_descriptor_dtype_set (stmtblock_t *block, tree desc, tree value);
void gfc_conv_descriptor_elem_len_set (stmtblock_t *block, tree desc, tree value);
void gfc_conv_descriptor_version_set (stmtblock_t *block, tree desc, tree value);
void gfc_conv_descriptor_rank_set (stmtblock_t *block, tree desc, tree value);
void gfc_conv_descriptor_rank_set (stmtblock_t *block, tree desc, int value);
void gfc_conv_descriptor_type_set (stmtblock_t *block, tree desc, tree value);
tree gfc_conv_descriptor_type_set (tree desc, tree value);
tree gfc_conv_descriptor_type_set (tree desc, int value);
void gfc_conv_descriptor_span_set (stmtblock_t *block, tree desc, tree value);
void gfc_conv_descriptor_dimension_set (stmtblock_t *block, tree desc, tree dim, tree value);
void gfc_conv_descriptor_dimension_set (stmtblock_t *block, tree desc, int dim, tree value);
void gfc_conv_descriptor_stride_set (stmtblock_t *block, tree desc, tree dim, tree value);
void gfc_conv_descriptor_lbound_set (stmtblock_t *block, tree desc, tree dim, tree value);
void gfc_conv_descriptor_ubound_set (stmtblock_t *block, tree desc, tree dim, tree value);
void gfc_conv_descriptor_token_set (stmtblock_t *block, tree desc, tree value);

tree gfc_build_null_descriptor (tree type);
tree gfc_conv_descriptor_size (tree, int);
tree gfc_conv_descriptor_cosize (tree, int, int);

void
gfc_get_descriptor_offsets_for_info (const_tree desc_type, tree *data_off,
				     tree *dtype_off, tree *span_off,
				     tree *dim_off, tree *dim_size,
				     tree *stride_suboff, tree *lower_suboff,
				     tree *upper_suboff);

/* Build a null array descriptor constructor.  */
void gfc_nullify_descriptor (stmtblock_t *block, tree);
void gfc_init_descriptor_result (stmtblock_t *block, tree descr);
void gfc_init_absent_descriptor (stmtblock_t *block, tree descr);
void gfc_init_static_descriptor (tree descr);

#endif /* GFC_TRANS_DESCRIPTOR_H */
