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


tree gfc_get_cfi_desc_base_addr (tree desc);
tree gfc_get_cfi_desc_elem_len (tree desc);
tree gfc_get_cfi_desc_version (tree desc);
tree gfc_get_cfi_desc_rank (tree desc);
tree gfc_get_cfi_desc_type (tree desc);
tree gfc_get_cfi_desc_attribute (tree desc);

tree gfc_get_cfi_dim_lbound (tree desc, tree idx);
tree gfc_get_cfi_dim_extent (tree desc, tree idx);
tree gfc_get_cfi_dim_sm (tree desc, tree idx);


tree gfc_conv_descriptor_data_addr (tree desc);
tree gfc_conv_descriptor_dtype (tree desc);
tree gfc_conv_descriptor_rank (tree desc);
tree gfc_conv_descriptor_version (tree desc);
tree gfc_conv_descriptor_elem_len (tree desc);
tree gfc_conv_descriptor_attribute (tree desc);
tree gfc_conv_descriptor_type (tree desc);
tree gfc_get_descriptor_dimension (tree desc);
tree gfc_conv_descriptor_dimension (tree desc, tree dim);
tree gfc_conv_descriptor_token (tree desc);
tree gfc_conv_descriptor_offset (tree desc);

tree gfc_conv_descriptor_data_get (tree desc);
tree gfc_conv_descriptor_offset_get (tree desc);
tree gfc_conv_descriptor_span_get (tree desc);

tree gfc_conv_descriptor_stride_get (tree desc, tree dim);
tree gfc_conv_descriptor_lbound_get (tree desc, tree dim);
tree gfc_conv_descriptor_ubound_get (tree desc, tree dim);

void gfc_conv_descriptor_data_set (stmtblock_t *block, tree desc, tree value);
void gfc_conv_descriptor_offset_set (stmtblock_t *block, tree desc, tree value);
void gfc_conv_descriptor_span_set (stmtblock_t *block, tree desc, tree value);
void gfc_conv_descriptor_stride_set (stmtblock_t *block, tree desc, tree dim, tree value);
void gfc_conv_descriptor_lbound_set (stmtblock_t *block, tree desc, tree dim, tree value);
void gfc_conv_descriptor_ubound_set (stmtblock_t *block, tree desc, tree dim, tree value);

tree gfc_build_null_descriptor (tree type);

void
gfc_get_descriptor_offsets_for_info (const_tree desc_type, tree *data_off,
				     tree *dtype_off, tree *span_off,
				     tree *dim_off, tree *dim_size,
				     tree *stride_suboff, tree *lower_suboff,
				     tree *upper_suboff);

