#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "rtl.h"
#include "tree.h"
#include "gimple.h"
#include "tree-pass.h"
#include "ssa.h"
#include "gimple-pretty-print.h"
#include "fold-const.h"
#include "gimple-iterator.h"
#include "tree-cfg.h"
#include "tree-dfa.h"
#include "tree-cfgcleanup.h"
#include "alias.h"
#include "tree-ssa-loop.h"
#include "diagnostic.h"
#include "cfghooks.h"
#include "tree-into-ssa.h"
#include "cfganal.h"
#include "dbgcnt.h"
#include "target.h"
#include "alloc-pool.h"
#include "tree-switch-conversion.h"
#include "tree-ssa-reassoc.h"
#include "tree-ssa.h"
#include <vector>
#include <algorithm>

/* The struct holding the info about outer switch and all inner switches it
   points to. */
struct switch_info
{
  switch_info(gswitch *outer_switch, gswitch *inner_switch) :
  m_outer_switch(outer_switch), m_inner_switches()
  {
    m_inner_switches.create (1);
    m_inner_switches.safe_push (inner_switch);
  }

  void add_inner_switch (gswitch *inner_switch);
  gswitch *m_outer_switch;
  auto_vec<gswitch*> m_inner_switches;
};

void
switch_info::add_inner_switch (gswitch *inner_switch)
{
  m_inner_switches.safe_push(inner_switch);
}

void
print_nested_switches (basic_block bb, gswitch *sw1, gswitch *sw2)
{
  if (!dump_file)
    return;
  fprintf(dump_file, "Found nested switch in bb number %d:\n", bb->index);
  print_gimple_stmt(dump_file, sw1, 0, TDF_DETAILS);
  fprintf(dump_file, "- Index var is: ");
  print_generic_expr(dump_file, gimple_switch_index(sw1), TDF_DETAILS);
  for (size_t i = 0; i < gimple_switch_num_labels(sw1); ++i)
  {
    fprintf(dump_file, "\n * Label number %ld -> label is: ", i);
    print_generic_expr(dump_file, gimple_switch_label(sw1, i), TDF_DETAILS);
  }
  fprintf(dump_file, "\n !!! NESTED SWITCH !!!\n");
  fprintf(dump_file, "The nested switch is in the bb number %d\n", sw2->bb->index);
  print_gimple_stmt(dump_file, sw2, 0, TDF_DETAILS);
  fprintf(dump_file, "- Index var is: ");
  print_generic_expr(dump_file, gimple_switch_index(sw2), TDF_DETAILS);
  for (size_t i = 0; i < gimple_switch_num_labels(sw2); ++i)
  {
    fprintf(dump_file, "\n * Label number %ld -> label is: ", i);
    print_generic_expr(dump_file, gimple_switch_label(sw2, i), TDF_DETAILS);
  }
  fprintf(dump_file, "\n---------------\n\n");
}

/* This struct keeps the information about case label of some switch */
struct case_info
{
  using mapping_vec = auto_vec<std::pair<gphi *, tree>>;
  using case_info_ptr = std::shared_ptr<case_info>;

  case_info (tree low, tree high, tree label, basic_block this_bb,
             basic_block dest_bb, bool is_inner, bool points_to_nested) :
    m_low (low), m_high (high), m_label (label), m_this_bb (this_bb),
    m_dest_bb (dest_bb), m_is_inner (is_inner),
    m_points_to_nested (points_to_nested), m_phi_mapping (),
    m_forwarder_bb (NULL)
  {
    m_phi_mapping.create (0);
  }

  case_info(const case_info& other) :
    m_low (other.m_low), m_high (other.m_high), m_label (other.m_label),
    m_this_bb (other.m_this_bb), m_dest_bb (other.m_dest_bb),
    m_is_inner (other.m_is_inner),
    m_points_to_nested (other.m_points_to_nested), m_phi_mapping (),
    m_forwarder_bb (other.m_forwarder_bb)
  {
    m_phi_mapping = other.m_phi_mapping.copy();
  }

  case_info(case_info_ptr& other) :
  m_low (other->m_low), m_high (other->m_high), m_label (other->m_label),
  m_this_bb (other->m_this_bb), m_dest_bb (other->m_dest_bb),
  m_is_inner (other->m_is_inner),
  m_points_to_nested (other->m_points_to_nested), m_phi_mapping (),
  m_forwarder_bb (other->m_forwarder_bb)
  {
    m_phi_mapping = other->m_phi_mapping.copy();
  }

  /* Creates case label to be added to gimple switch statement from
    this struct */
  tree build_case ();
  /* Checks if the case label is default */
  bool is_default ();
  /* Checks if 2 case ranges have empty intersection */
  bool is_intersection_empty (case_info_ptr other);
  /* Checks if case range of this struct is a subset of other case range */
  bool is_subset_of (case_info_ptr other);
  /* Checks if 2 ranges are equal*/
  bool is_range_equal (case_info_ptr other);
  /* Checks if 2 ranges have the same lower bound */
  bool share_lower_bound (case_info_ptr other);
  /* Checks if 2 ranges have the same upper bound */
  bool share_upper_bound (case_info_ptr other);
  /* Recond PHI mapping for an original edge E from switch statement to case
    label dest and save these into vector m_phi_mapping.  */
  void record_phi_mapping ();
  /* Makes this case point to the same label as the other case */
  void point_to_as (case_info_ptr other);

  tree m_low; /* lower bound of case range */
  tree m_high; /* upper bound of case range */
  tree m_label; /* destination label of case */
  basic_block m_this_bb; /* case location bb */
  basic_block m_dest_bb; /* destination label location bb */
  bool m_is_inner; /* case belonging to inner switch */
  bool m_points_to_nested; /* case pointing to inner switch */
  mapping_vec m_phi_mapping;
  basic_block m_forwarder_bb;
};

using case_info_ptr = std::shared_ptr<case_info>;

/* Creates case label */
tree
case_info::build_case ()
{
  tree label = (m_forwarder_bb == NULL)
               ? gimple_block_label (m_dest_bb)
               : gimple_block_label (m_forwarder_bb);
  return build_case_label (m_low,
                           m_low == m_high ? NULL_TREE : m_high,
                           label);
}

/* Checks if the case label is default */
bool
case_info::is_default ()
{
  return m_low == NULL_TREE && m_high == NULL_TREE;
}


/* Checks if 2 case ranges have empty intersection */
bool
case_info::is_intersection_empty (case_info_ptr other)
{
  return (tree_int_cst_lt (m_high, other->m_low)
          || tree_int_cst_lt (other->m_high, m_low));
}

/* Checks if case range of this struct is a subset of other case range */
bool
case_info::is_subset_of (case_info_ptr other)
{
  return (tree_int_cst_le (other->m_low, m_low)
          && tree_int_cst_le (m_high, other->m_high));
}

/* Checks if 2 ranges are equal*/
bool
case_info::is_range_equal (case_info_ptr other)
{
  return (tree_int_cst_equal (m_low, other->m_low)
          && tree_int_cst_equal (m_high, other->m_high));
}

/* Checks if 2 ranges have the same lower bound */
bool
case_info::share_lower_bound (case_info_ptr other)
{
  return tree_int_cst_equal (m_low, other->m_low);
}

/* Checks if 2 ranges have the same upper bound */
bool
case_info::share_upper_bound (case_info_ptr other)
{
  return tree_int_cst_equal (m_high, other->m_high);
}

/* Makes this case point to the same label as the other case */
void
case_info::point_to_as (case_info_ptr other)
{
  m_label = other->m_label;
  m_dest_bb = other->m_dest_bb;
  m_is_inner = other->m_is_inner;
  m_points_to_nested = other->m_points_to_nested;
}

/* Recond PHI mapping for an original edge E from switch statement to case label dest
   and save these into vector m_phi_mapping.  */
void
case_info::record_phi_mapping ()
{
  edge e = find_edge (m_this_bb, m_dest_bb);
  if (e == NULL)
    return;

  for (gphi_iterator gsi = gsi_start_phis (e->dest); !gsi_end_p (gsi);
       gsi_next (&gsi))
    {
      gphi *phi = gsi.phi ();
      tree arg = PHI_ARG_DEF_FROM_EDGE (phi, e);
      m_phi_mapping.safe_push (std::make_pair (phi, arg));
    }
}


/* Compare 2 case label ranges by lower bound, default label always compares
  as lower */

auto case_info_cmp = [](const std::shared_ptr<case_info>& a,
                        const std::shared_ptr<case_info>& b)
  {
    if (a->m_low == NULL_TREE && b->m_low == NULL_TREE) return false;
    if (a->m_low == NULL_TREE) return true;
    if (b->m_low == NULL_TREE) return false;
    return tree_int_cst_compare(a->m_low, b->m_low) < 0;
  };

using case_informations = std::vector<case_info_ptr>;

void print_merged_switch (case_informations &merged_infos)
{
  if (!dump_file)
    return;
  fprintf(dump_file, "\n\nMERGED SWITCH WOULD LOOK LIKE THIS:");
  int n = 0;
  for (auto info : merged_infos)
  {
    tree case_label = info->build_case();
    fprintf(dump_file, "\n * Label number %d -> label is: ", n);
    print_generic_expr(dump_file, case_label, TDF_DETAILS);
    ++n;
  }
}

void print_inner_outer (case_info_ptr outer, case_info_ptr inner)
{
  if (!dump_file)
    return;
  if (outer == NULL)
    fprintf(dump_file, "\n Outer label is: NULL");
  else
    {
      fprintf(dump_file, "\n Outer label is: ");
      print_generic_expr(dump_file, outer->build_case(), TDF_DETAILS);
    }
  fprintf(dump_file, "\n Inner label is: ");
  print_generic_expr(dump_file, inner->build_case(), TDF_DETAILS);
  fprintf(dump_file, "\n ---------- ");
}



/* Create struct case_info */
static case_info_ptr
get_case_info (function* fun, basic_block this_bb, tree case_label,
               bool is_inner, bool points_to_nested)
{
  tree low  = CASE_LOW (case_label);
  tree high = CASE_HIGH (case_label);
  tree dest = CASE_LABEL (case_label);
  basic_block dest_bb = label_to_block (fun, dest);

  return std::make_shared<case_info> (low, (high == NULL_TREE) ? low : high,
                        dest, this_bb, dest_bb, is_inner, points_to_nested);
}

/* Fills case_infos vector with structs created from inner switch labels */
static void
inner_switch_case_informations (function* fun, const gswitch* inner_switch,
                                case_informations &case_infos)
{
  unsigned n = gimple_switch_num_labels (inner_switch);
  basic_block bb = gimple_bb (inner_switch);
  for (unsigned i = 0; i < n; ++i)
    {
      tree label = gimple_switch_label (inner_switch, i);
      case_info_ptr info = get_case_info (fun, bb, label, true, false);
      case_infos.push_back (info);
    }
}

/* Fills case_infos vector with structs created from outer switch labels */
static void
outer_switch_case_informations (function *fun, const gswitch *outer_switch,
                                const gswitch *inner_switch,
                                case_informations &case_infos)
{
  basic_block inner_switch_bb = gimple_bb (inner_switch);
  basic_block outer_switch_bb = gimple_bb (outer_switch);
  unsigned n = gimple_switch_num_labels (outer_switch);
  for (unsigned i = 0; i < n; ++i)
    {
      tree label = gimple_switch_label (outer_switch, i);
      tree dest  = CASE_LABEL (label);
      bool points_to_inner = (label_to_block (fun, dest) == inner_switch_bb);
      case_info_ptr info = get_case_info (fun, outer_switch_bb, label,
                                       false, points_to_inner);
      case_infos.push_back (info);
    }
}


/*
Checks if the nested inner switch can be merged with the outer switch:
- If the default label points to inner nested switch then in order to merge
  succesfully the intersection of ranges not pointing to the inner nested switch
  with inner switch nondefault label ranges must be empty.
- If the default label doesn't point to inner nested switch then in order
  to merge succesfully each inner nondefault range must be a subset of some
  outer switch label range pointing to the inner switch.
*/
static bool
check_if_switches_mergeable (case_informations &outer, case_informations &inner)
{
  bool outer_default_points_to_inner = outer[0]->m_points_to_nested;
  for (auto inner_info : inner)
    {

      if (inner_info->is_default ())
        continue;

      if (outer_default_points_to_inner)
        {
          for (auto outer_info : outer)
            {
              if (outer_info->m_points_to_nested)
                continue;

              bool empty = inner_info->is_intersection_empty (outer_info);
              if (!empty)
                return false;
            }
        }
      else
        {
          for (auto inner_info : inner)
            {
              if (inner_info->is_default ())
                continue;

              bool is_subset = false;
              for (auto outer_info : outer)
                {
                  if (!outer_info->m_points_to_nested)
                    continue;

                  is_subset |= inner_info->is_subset_of (outer_info);
                }

              if (!is_subset)
                return false;
            }
        }

    }

  return true;
}



/* Adds 1 to int constant */
tree
int_cst_plus_one (tree cst)
{
  gcc_assert (TREE_CODE (cst) == INTEGER_CST);
  return int_const_binop (PLUS_EXPR, cst, build_int_cst (TREE_TYPE (cst), 1));
}

/* Subtracts 1 from int constant */
tree
int_cst_minus_one (tree cst)
{
  gcc_assert (TREE_CODE (cst) == INTEGER_CST);
  return int_const_binop (PLUS_EXPR, cst, build_int_cst (TREE_TYPE (cst), -1));
}


/* This cuts out inner range from outer, adds left result to merged case infos
   and returns the rest to be processed later. */
case_info_ptr
subtract_ranges_and_merge (case_info_ptr outer,
                           case_info_ptr inner,
                           case_info_ptr inner_default,
                           case_informations &merged_case_infos)
{
  print_inner_outer(outer, inner);

  if (outer == NULL)
    return NULL;

  /* If either of cases is default we don't cut anything */
  if (outer->is_default () || inner->is_default ())
    return outer;

  /* Same if the ranges don't intersect */
  if (outer->is_intersection_empty (inner))
    return outer;


  /* If ranges are equal we add inner case to merged cases and nothing is
    left to process in next iterations, we return NULL */
  if (outer->is_range_equal (inner))
    {
      merged_case_infos.push_back (inner);
      return NULL;
    }

  /* If ranges share lower bound we cut out inner from outer, add it to
    merged cases and return right side to process later:
    - outer=[0,4], inner=[0,1] -> we have to process later [2,4] */
  if (outer->share_lower_bound (inner))
    {
      merged_case_infos.push_back (inner);
      outer->m_low = int_cst_plus_one (inner->m_high);
      return outer;
    }

  /* If ranges share upper bound we cut out inner from outer and nothing is
    left to process:
    - outer=[0,4], inner=[3,4] -> new cases are [0,2] and [3,4] */
  if (outer->share_upper_bound (inner))
    {
      outer->m_high = int_cst_minus_one (inner->m_low);
      outer->point_to_as (inner_default);
      merged_case_infos.push_back (outer);
      merged_case_infos.push_back (inner);
      return NULL;
    }

  /* If inner is subset of outer and share no bounds we cut out inner,
    add left side to merged cases and return right side:
    - outer=[0,5], inner=[2,3] -> new cases are [0,1] and [2,3], we process
      later [4,5] */
  case_info_ptr copy_outer = std::make_shared<case_info> (*outer);
  copy_outer->m_high = int_cst_minus_one (inner->m_low);
  copy_outer->point_to_as (inner_default);
  outer->m_low = int_cst_plus_one (inner->m_high);
  merged_case_infos.push_back (copy_outer);
  merged_case_infos.push_back (inner);
  return outer;
}

/* Merges case infos from inner and outer switch when outer default doesn't
  point to inner switch, i.e.:
  - every outer case label not pointing to inner switch should be in
    merged switch
  - for outer cases pointing to inner switch we iterate over sorted inner
    cases and cut the inner ranges from outer, always keeping rightmost side
    of remaining outer range to be compared with next larger inner ranges.
    */
static void
get_merged_case_infos_def_not_to_inner (const case_informations &outer_infos,
                                        const case_informations &inner_infos,
                                        case_informations &merged_case_infos)
{
  case_info_ptr inner_default = inner_infos[0];
  for (auto outer_info : outer_infos)
    {
      if (!outer_info->m_points_to_nested)
        {
          merged_case_infos.push_back (outer_info);
          continue;
        }

      case_info_ptr remaining_outer_info = outer_info;
      for (auto inner_info : inner_infos)
        {
          remaining_outer_info = subtract_ranges_and_merge (
                remaining_outer_info, inner_info, inner_default,
                merged_case_infos);
        }
      if (remaining_outer_info != NULL)
        {
          remaining_outer_info->point_to_as (inner_default);
          merged_case_infos.push_back (remaining_outer_info);
        }

    }
}

/* Merges case infos from inner and outer switch when outer default points
   to inner switch, i.e.:
   - every inner case label should be in merged switch
   - every outer case label not pointing to inner switch should be in
     merged switch */
static void
get_merged_case_infos_def_to_inner (const case_informations &outer_infos,
                                    const case_informations &inner_infos,
                                    case_informations &merged_case_infos)
{
  for (auto outer_info : outer_infos)
    {
      if (outer_info->m_points_to_nested)
        continue;
      merged_case_infos.push_back (outer_info);
    }

  for (auto inner_info : inner_infos)
    {
      merged_case_infos.push_back (inner_info);
    }
}


/* Merges the case informations for inner and outer switch */
static void
merge_case_infos (case_informations &outer_infos,
                  case_informations &inner_infos,
                  case_informations &merged_case_infos)
{
  bool outer_default_points_to_inner = outer_infos[0]->m_points_to_nested;
  std::sort(outer_infos.begin(), outer_infos.end(), case_info_cmp);
  std::sort(inner_infos.begin(), inner_infos.end(), case_info_cmp);

  if (outer_default_points_to_inner)
    get_merged_case_infos_def_to_inner (outer_infos, inner_infos,
                                       merged_case_infos);
  else
    get_merged_case_infos_def_not_to_inner (outer_infos, inner_infos,
                                       merged_case_infos);

  std::sort(merged_case_infos.begin(), merged_case_infos.end(), case_info_cmp);
  if (dump_file)
    print_merged_switch(merged_case_infos);
}

/* Repairs the cfg, makes new edges from merged switch, possibly adding
  forwarder blocks and adds the new PHI args where needed */
static void
repair_cfg (case_informations &merged_case_infos, basic_block merged_switch_bb)
{
  /* Find the bbs poited to by original outer switch cases and record any phi
     statements where the case labels of both switches point to */
  hash_set<basic_block> outer_switch_points_to;
  for (auto info : merged_case_infos)
    {
      if (!info->m_is_inner)
        outer_switch_points_to.add (info->m_dest_bb);
      info->record_phi_mapping ();
    }

  /* Make the new edges for merged cases and possibly add forwarder block */
  for (auto info : merged_case_infos)
    {
      info->m_this_bb = merged_switch_bb;
      edge e = make_edge (info->m_this_bb, info->m_dest_bb, 0);

      if (e == NULL)
        e = find_edge(info->m_this_bb, info->m_dest_bb);

      if (!info->m_is_inner)
        continue;
      if (!outer_switch_points_to.contains (info->m_dest_bb))
        continue;
      if (info->m_phi_mapping.is_empty ())
        continue;

      info->m_forwarder_bb = split_edge (e);
      make_edge (info->m_this_bb, info->m_dest_bb, 0);
    }

    /* Add the missing phi arguments */
    for (auto info : merged_case_infos)
      {
        edge e = (info->m_forwarder_bb == NULL)
                 ? find_edge(info->m_this_bb, info->m_dest_bb)
                 : find_edge(info->m_forwarder_bb, info->m_dest_bb);

        for (auto item : info->m_phi_mapping)
          {
            add_phi_arg (item.first, item.second, e, UNKNOWN_LOCATION);
          }
      }
}


/* Adjusts the outer switch to be merged switch */
static void
change_outer_switch_to_merged_switch (gswitch *outer,
                                      case_informations &merged_case_infos)
{
  gcc_assert (merged_case_infos.size () > 0);
  gcc_assert (merged_case_infos[0]->is_default ());
  gimple_switch_set_num_labels (outer, merged_case_infos.size ());
  for (unsigned i = 0; i < merged_case_infos.size (); ++i)
    {
      case_info_ptr info = merged_case_infos[i];
      tree label = info->build_case ();
      gimple_switch_set_label (outer, i, label);
    }
}

/* Merges nested switches */
static void
merge_nested_switches (function *fun, switch_info *info)
{
  gswitch* outer = info->m_outer_switch;
  for (gswitch* inner : info->m_inner_switches)
    {
      case_informations outer_case_info{}, inner_case_info{};
      outer_switch_case_informations (fun, outer, inner, outer_case_info);
      inner_switch_case_informations (fun, inner, inner_case_info);

      if (!check_if_switches_mergeable (outer_case_info, inner_case_info))
        continue;

      case_informations merged_case_info{};
      dump_function_to_file (fun->decl, stderr, TDF_DETAILS);
      merge_case_infos (outer_case_info, inner_case_info, merged_case_info);
      repair_cfg (merged_case_info, outer->bb);
      change_outer_switch_to_merged_switch (outer, merged_case_info);
      delete_basic_block (inner->bb);
    }
}


/* Checks if the bb contains only debug stmts, labels and switches */
static bool
bb_only_labels_and_switch (basic_block bb)
{
  for (gimple_stmt_iterator gsi = gsi_start_bb (bb);
       !gsi_end_p (gsi);
       gsi_next (&gsi))
    {
      gimple *stmt = gsi_stmt (gsi);

      if (is_gimple_debug (stmt))
        continue;

      if (gimple_code (stmt) != GIMPLE_SWITCH
          && gimple_code (stmt) != GIMPLE_LABEL)
        return false;
    }
  return true;
}

/* Finds the pairs of outer and inner switches */
static void
find_nested_switches (hash_map<basic_block, switch_info *> *switch_in_bb,
                      basic_block bb)
{
  gimple_stmt_iterator gsi = gsi_last_nondebug_bb(bb);
  if (gsi_end_p(gsi))
    return;

  gswitch *outer_switch = dyn_cast<gswitch *>(gsi_stmt(gsi));
  if (outer_switch == NULL)
    return;

  edge e;
  edge_iterator ei;
  FOR_EACH_EDGE (e, ei, bb->succs)
    {
      basic_block dest = e->dest;
      if (!bb_only_labels_and_switch (dest) )
        continue;

      if (dest->preds->length() != 1)
        continue;

      gimple_stmt_iterator gsi_dest = gsi_last_nondebug_bb(dest);
      if (gsi_end_p(gsi_dest))
        continue;


      gswitch *inner_switch = dyn_cast<gswitch *>(gsi_stmt(gsi_dest));
      if (inner_switch == NULL)
        continue;

      tree idx_outer = gimple_switch_index(outer_switch);
      tree idx_inner = gimple_switch_index(inner_switch);
      if (idx_inner != idx_outer)
        continue;

      print_nested_switches(bb, outer_switch, inner_switch);

      switch_info** info = switch_in_bb->get(bb);
      if (info == NULL)
        {
          switch_info *new_info = new switch_info (outer_switch, inner_switch);
          switch_in_bb->put(bb, new_info);
          continue;
        }
      (*info)->add_inner_switch(inner_switch);
    }
}

namespace
{
  const pass_data pass_data_flatten_switch =
  {
    GIMPLE_PASS,          /* type */
    "flattenswitch",      /* name */
    OPTGROUP_NONE,        /* optinfo_flags */
    TV_TREE_FLATTEN_SWITCH,  /* tv_id */
    ( PROP_cfg | PROP_ssa ), /* properties_required */
    0,                    /* properties_provided */
    0,                    /* properties_destroyed */
    0,                    /* todo_flags_start */
    ( TODO_update_ssa | TODO_cleanup_cfg )      /* todo_flags_finish */
  };

  class pass_flatten_switch : public gimple_opt_pass
  {
  public:
    pass_flatten_switch(gcc::context *ctxt)
        : gimple_opt_pass(pass_data_flatten_switch, ctxt)
    {
    }

    unsigned int execute(function *) final override;

  }; // class pass_flatten_switch

  unsigned int
  pass_flatten_switch::execute(function *fun)
  {
    hash_map<basic_block, switch_info *> switches_in_bbs;

    basic_block bb;
    FOR_EACH_BB_FN (bb, fun)
      find_nested_switches (&switches_in_bbs, bb);


    if (switches_in_bbs.is_empty ())
      return 0;

    int *rpo = XNEWVEC (int, n_basic_blocks_for_fn (fun));

    unsigned n = pre_and_rev_post_order_compute_fn (fun, NULL, rpo, false);

    auto_bitmap seen_bbs;
    for (int i = n - 1; i >= 0; --i)
      {
        basic_block bb = BASIC_BLOCK_FOR_FN (fun, rpo[i]);
        if (bitmap_bit_p (seen_bbs, bb->index))
          continue;

        bitmap_set_bit (seen_bbs, bb->index);
        switch_info **info = switches_in_bbs.get (bb);
        if (info == NULL)
          continue;
        merge_nested_switches(fun, *info);
      }


    free (rpo);


    for (hash_map<basic_block, switch_info *>::iterator it
        = switches_in_bbs.begin (); it != switches_in_bbs.end (); ++it)
      delete (*it).second;

    free_dominance_info (CDI_DOMINATORS);
    dump_function_to_file (fun->decl, stderr, TDF_DETAILS);
    return 0;


  }

} // anon namespace

gimple_opt_pass *
make_pass_flatten_switch(gcc::context *ctxt)
{
  return new pass_flatten_switch(ctxt);
}
