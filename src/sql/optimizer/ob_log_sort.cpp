/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX SQL_OPT
#include "sql/optimizer/ob_log_sort.h"
#include "ob_optimizer_context.h"
#include "ob_opt_est_cost.h"
#include "ob_optimizer_util.h"
#include "sql/optimizer/ob_log_plan.h"
#include "ob_log_exchange.h"
#include "sql/rewrite/ob_transform_utils.h"
#include "sql/optimizer/ob_join_order.h"
#include "share/ob_order_perserving_encoder.h"
#include "common/ob_smart_call.h"
using namespace oceanbase::sql;
using namespace oceanbase::common;

int ObLogSort::set_sort_keys(const common::ObIArray<OrderItem> &order_keys)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sort_keys_.assign(order_keys))) {
    LOG_WARN("failed to set sort keys", K(ret));
  } else { /* do nothing */ }
  return ret;
}

int ObLogSort::create_encode_sortkey_expr(const common::ObIArray<OrderItem> &order_keys) {
  int ret = OB_SUCCESS;
  ObOpRawExpr* encode_expr = NULL;
  if (OB_ISNULL(get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(get_plan()), K(ret));
  } else if (OB_ISNULL(get_plan()->get_optimizer_context().get_exec_ctx())){
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(get_plan()), K(ret));
  } else {
    int64_t ecd_pos = 0;

    // Prefix sort and hash-based sort both can combine with encode sort.
    // And prefix sort is prior to hash-based sort(part sort).
    if (is_prefix_sort() || is_part_sort()) {
      int64_t orig_pos = is_prefix_sort() ? get_prefix_pos() : get_part_cnt();
      for (int64_t i = 0; OB_SUCC(ret) && i < orig_pos; ++i) {
        if (OB_FAIL(encode_sortkeys_.push_back(order_keys.at(i)))) {
          LOG_WARN("failed to add encodekey", K(ret));
        } else {
          ecd_pos++;
        }
      }
    } else {
      ecd_pos = 0;
    }
    ObRawExprFactory &expr_factory = get_plan()->get_optimizer_context().get_expr_factory();
    ObExecContext* exec_ctx = get_plan()->get_optimizer_context().get_exec_ctx();
    OrderItem encode_sortkey;
    if (OB_FAIL(ObSQLUtils::create_encode_sortkey_expr(
        expr_factory, exec_ctx, order_keys, ecd_pos, encode_sortkey))) {
      LOG_WARN("failed to create encode sortkey expr", K(ret));
    } else if (OB_FAIL(encode_sortkeys_.push_back(encode_sortkey))) {
      LOG_WARN("failed to push back encode sortkey", K(ret));
    } else { /* do nothing*/ }
  }
  return ret;
}

int ObLogSort::create_hash_sortkey(const common::ObIArray<OrderItem> &order_keys)
{
  int ret = OB_SUCCESS;
  ObOpRawExpr *hash_expr = NULL;
  ObRawExprFactory &expr_factory = get_plan()->get_optimizer_context().get_expr_factory();
  ObExecContext *exec_ctx = get_plan()->get_optimizer_context().get_exec_ctx();
  if (OB_FAIL(expr_factory.create_raw_expr(T_FUN_SYS_HASH, hash_expr))) {
    LOG_WARN("failed to create raw expr", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < part_cnt_; ++i) {
      if (OB_FAIL(hash_expr->add_param_expr(order_keys.at(i).expr_))) {
        LOG_WARN("failed to add param expr", K(ret));
      }
    }
  }
  if (OB_FAIL(ret)) {
    //do nothing
  } else if (OB_FAIL(hash_expr->formalize(exec_ctx->get_my_session()))) {
    LOG_WARN("failed to formalize expr", K(ret));
  } else {
    hash_sortkey_.expr_ = hash_expr;
    hash_sortkey_.order_type_ = default_asc_direction();
  }
  return ret;
}

int ObLogSort::get_sort_exprs(common::ObIArray<ObRawExpr*> &sort_exprs)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < sort_keys_.count(); ++i) {
    if (OB_FAIL(sort_exprs.push_back(sort_keys_.at(i).expr_))) {
      LOG_WARN("push back order key expr failed", K(ret));
    }
  }
  return ret;
}

int ObLogSort::get_op_exprs(ObIArray<ObRawExpr*> &all_exprs)
{
  int ret = OB_SUCCESS;
  if (NULL != topn_expr_ && OB_FAIL(all_exprs.push_back(topn_expr_))) {
    LOG_WARN("failed to push back expr", K(ret));
  } else if (NULL != topk_limit_expr_ && OB_FAIL(all_exprs.push_back(topk_limit_expr_))) {
    LOG_WARN("failed to push back expr", K(ret));
  } else if (NULL != topk_offset_expr_ && OB_FAIL(all_exprs.push_back(topk_offset_expr_))) {
    LOG_WARN("failed to push back expr", K(ret));
  } else if (OB_ISNULL(get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(get_plan()), K(ret));
  } else if (GCONF._enable_newsort
      && ObSQLUtils::check_can_encode_sortkey(sort_keys_)
      && OB_FAIL(create_encode_sortkey_expr(sort_keys_))) {
    LOG_WARN("failed to create encode sortkey expr", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < sort_keys_.count(); i++) {
      if (OB_ISNULL(sort_keys_.at(i).expr_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret));
      } else if (OB_FAIL(all_exprs.push_back(sort_keys_.at(i).expr_))) {
        LOG_WARN("failed to push back exprs", K(ret));
      } else { /*do nothing*/ }
    }

    for (int64_t i = 0; OB_SUCC(ret) && i < encode_sortkeys_.count(); i++) {
      if (OB_ISNULL(encode_sortkeys_.at(i).expr_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret), K(i));
      } else if (OB_FAIL(all_exprs.push_back(encode_sortkeys_.at(i).expr_))) {
        LOG_WARN("failed to push back expr", K(ret));
      } else { /*do nothing*/ }
    }

    if (OB_SUCC(ret)) {
      if (part_cnt_ > 0) {
        if (OB_ISNULL(hash_sortkey_.expr_)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get unexpected null", K(ret));
        } else if (OB_FAIL(all_exprs.push_back(hash_sortkey_.expr_))) {
          LOG_WARN("failed to push back expr", K(ret));
        }
      }
      if (OB_FAIL(ObLogicalOperator::get_op_exprs(all_exprs))) {
        LOG_WARN("failed to get op exprs", K(ret));
      } else { /*do nothing*/ }
    }
  }
  return ret;
}

uint64_t ObLogSort::hash(uint64_t seed) const
{
  bool is_topn = NULL != topn_expr_;
  seed = do_hash(is_topn, seed);
  seed = ObLogicalOperator::hash(seed);

  return seed;
}

int ObLogSort::print_my_plan_annotation(char *buf,
                                        int64_t &buf_len,
                                        int64_t &pos,
                                        ExplainType type)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(BUF_PRINTF(", "))) {
    LOG_WARN("BUF_PRINTF fails", K(ret));
  } else {
    ObSEArray<OrderItem, 1> sort_keys;
    if (NULL != get_hash_sortkey().expr_
        && OB_FAIL(sort_keys.push_back(get_hash_sortkey()))) {
      LOG_WARN("failed to push back sortkeys", K(ret));
    } else if (OB_FAIL(append(sort_keys, get_sort_keys()))) {
      LOG_WARN("failed to append sortkeys", K(ret));
    } else {
      EXPLAIN_PRINT_SORT_ITEMS(sort_keys, type);
    }
    if (OB_SUCC(ret) && NULL != topn_expr_) {
      ObRawExpr *topn = topn_expr_;
      BUF_PRINTF(", ");
      EXPLAIN_PRINT_EXPR(topn, type);
    }
    ObRawExpr *limit = topk_limit_expr_;
    if (OB_SUCC(ret) && NULL != limit) {
      if (OB_FAIL(BUF_PRINTF(", minimum_row_count:%ld top_precision:%ld ",
                             minimum_row_count_, topk_precision_))) {
        LOG_WARN("BUF_PRINTF fails", K(ret));
      } else {
        ObRawExpr *offset = topk_offset_expr_;
        BUF_PRINTF(", ");
        EXPLAIN_PRINT_EXPR(limit, type);
        BUF_PRINTF(", ");
        EXPLAIN_PRINT_EXPR(offset, type);
      }
    } else { /* Do nothing */ }

    if (OB_SUCC(ret) && prefix_pos_> 0) {
      BUF_PRINTF(", prefix_pos(");
      if (OB_FAIL(BUF_PRINTF("%ld)", prefix_pos_))) {
        LOG_WARN("BUF_PRINTF fails", K(ret), K(prefix_pos_));
      }
    }
    if (OB_SUCC(ret) && is_local_merge_sort_) {
      BUF_PRINTF(", local merge sort");
    }
    if (OB_SUCC(ret) &&
        (EXPLAIN_EXTENDED == type
          || EXPLAIN_EXTENDED_NOADDR == type
          || EXPLAIN_PLANREGRESS == type)
          && enable_encode_sortkey_opt()) {
      BUF_PRINTF(", encoded");
    }
    if (OB_SUCC(ret) && is_fetch_with_ties_) {
      BUF_PRINTF(", with_ties(true)");
    }
  }
  return ret;
}

int ObLogSort::inner_replace_generated_agg_expr(
        const ObIArray<std::pair<ObRawExpr *, ObRawExpr *> > &to_replace_exprs)
{
  int ret = OB_SUCCESS;
  int64_t N = sort_keys_.count();
  for(int64_t i = 0; OB_SUCC(ret) && i < N; ++i) {
    OrderItem &cur_order_item = sort_keys_.at(i);
    if (OB_FAIL(replace_expr_action(to_replace_exprs, cur_order_item.expr_))) {
      LOG_WARN("failed to resolve ref params in sort key ", K(cur_order_item), K(ret));
    } else { /* Do nothing */ }
  }
  for(int64_t i = 0; OB_SUCC(ret) && i < encode_sortkeys_.count(); ++i) {
    OrderItem &cur_order_item = encode_sortkeys_.at(i);
    if (OB_FAIL(replace_expr_action(to_replace_exprs, cur_order_item.expr_))) {
      LOG_WARN("failed to resolve ref params in sort key ", K(cur_order_item), K(ret));
    } else { /* Do nothing */ }
  }
  if (OB_SUCC(ret) && part_cnt_ > 0) {
    if (OB_FAIL(replace_expr_action(to_replace_exprs, hash_sortkey_.expr_))) {
      LOG_WARN("failed to resolve ref params of hash sortkey", K(hash_sortkey_), K(ret));
    } else { /* Do nothing */ }
  }
  return ret;
}

const char *ObLogSort::get_name() const
{
  const char *ret = NULL;
  if (NULL != topn_expr_) {
    ret = "TOP-N SORT";
  } else if (NULL == topk_limit_expr_ && prefix_pos_ <= 0 && part_cnt_ > 0) {
    ret = "PARTITION SORT";
  }
  return NULL != ret ? ret : log_op_def::get_op_name(type_);
}

int ObLogSort::est_width()
{
  int ret = OB_SUCCESS;
  double width = 0.0;
  ObSEArray<ObRawExpr*, 16> output_exprs;
  ObLogicalOperator *child = NULL;
  if (OB_ISNULL(get_plan()) ||
      OB_ISNULL(child = get_child(ObLogicalOperator::first_child))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid plan", K(ret));
  } else if (!get_plan()->get_candidate_plans().is_final_sort_) {
    width = child->get_width();
    set_width(width);
    LOG_TRACE("est width for non-final sort", K(output_exprs), K(width));
  } else if (OB_FAIL(get_sort_output_exprs(output_exprs))) {
    LOG_WARN("failed to get sort output exprs", K(ret));
  } else if (OB_FAIL(ObOptEstCost::estimate_width_for_exprs(get_plan()->get_basic_table_metas(),
                                                            get_plan()->get_selectivity_ctx(),
                                                            output_exprs,
                                                            width))) {
    LOG_WARN("failed to estimate width for output orderby exprs", K(ret));
  } else {
    set_width(width);
    LOG_TRACE("est width for final sort", K(output_exprs), K(width));
  }
  return ret;
}

int ObLogSort::get_sort_output_exprs(ObIArray<ObRawExpr *> &output_exprs)
{
  int ret = OB_SUCCESS;
  ObLogPlan *plan = NULL;
  ObSEArray<ObRawExpr*, 16> candi_exprs;
  ObSEArray<ObRawExpr*, 16> extracted_col_aggr_winfunc_exprs;
  if (OB_ISNULL(plan = get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid input", K(ret));
  } else if (OB_FAIL(append_array_no_dup(candi_exprs, plan->get_select_item_exprs_for_width_est()))) {
    LOG_WARN("failed to add into output exprs", K(ret));
  } else if (OB_FAIL(ObRawExprUtils::extract_col_aggr_winfunc_exprs(candi_exprs,
                                                                    extracted_col_aggr_winfunc_exprs))) {
  } else if (OB_FAIL(append_array_no_dup(output_exprs, extracted_col_aggr_winfunc_exprs))) {
    LOG_WARN("failed to add into output exprs", K(ret));
  } else {/*do nothing*/}
  return ret;
}

int ObLogSort::est_cost()
{
  int ret = OB_SUCCESS;
  double sort_cost = 0.0;
  double double_topn_count = -1;
  ObLogicalOperator *child = get_child(ObLogicalOperator::first_child);
  if (OB_ISNULL(child)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(child), K(ret));
  } else if (OB_FAIL(inner_est_cost(child->get_card(), double_topn_count, sort_cost))) {
    LOG_WARN("failed to est sort cost", K(ret));
  } else {
    set_op_cost(sort_cost);
    set_cost(child->get_cost() + sort_cost);
    if (double_topn_count >= 0) {
      set_card(double_topn_count);
    } else {
      set_card(child->get_card());
    }
    LOG_TRACE("cost for sort operator", K(sort_cost), K(get_cost()),
              K(get_card()));
  }
  return ret;
}

int ObLogSort::re_est_cost(EstimateCostInfo &param, double &card, double &cost)
{
  int ret = OB_SUCCESS;
  double child_card = 0.0;
  double child_cost = 0.0;
  double double_topn_count = -1;
  double sort_cost = 0.0;
  card = get_card();
  if (param.need_row_count_ >=0 && param.need_row_count_ < card) {
    card = param.need_row_count_;
  }
  ObLogicalOperator *child = get_child(ObLogicalOperator::first_child);
  if (OB_ISNULL(child)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret));
  } else if (OB_FALSE_IT(param.need_row_count_ = -1)) {
    //limit N在sort算子被阻塞
  } else if (OB_FAIL(SMART_CALL(child->re_est_cost(param, child_card, child_cost)))) {
    LOG_WARN("failed to re est cost", K(ret));
  } else if (OB_FAIL(inner_est_cost(child_card, double_topn_count, sort_cost))) {
    LOG_WARN("failed to est sort cost", K(ret));
  } else {
    cost = child_cost + sort_cost;
    card = child_card < card ? child_card : card;
    if (double_topn_count >= 0 && card > double_topn_count) {
      card = double_topn_count;
    }
    if (param.override_) {
      set_op_cost(sort_cost);
      set_cost(cost);
      set_card(card);
    }
  }
  return ret;
}

int ObLogSort::inner_est_cost(double child_card, double &double_topn_count, double &op_cost)
{
  int ret = OB_SUCCESS;
  int64_t parallel = 0;
  int64_t topn_count = -1;
  bool is_null_value = false;
  double_topn_count = -1;
  ObLogicalOperator *child = get_child(ObLogicalOperator::first_child);
  if (OB_ISNULL(child) || OB_ISNULL(get_stmt()) ||
      OB_ISNULL(get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(child), K(get_stmt()),
        K(get_plan()), K(ret));
  } else if (OB_UNLIKELY((parallel = get_parallel()) < 1)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected parallel degree", K(parallel), K(ret));
  } else if (NULL != topn_expr_ &&
             OB_FAIL(ObTransformUtils::get_limit_value(topn_expr_,
                                                       get_stmt(),
                                                       get_plan()->get_optimizer_context().get_params(),
                                                       get_plan()->get_optimizer_context().get_exec_ctx(),
                                                       &get_plan()->get_optimizer_context().get_allocator(),
                                                       topn_count,
                                                       is_null_value))) {
    LOG_WARN("failed to get value", K(ret));
  } else {
    if (NULL != topn_expr_) {
      double_topn_count = std::min(static_cast<double>(topn_count), child_card);
    }
    get_plan()->get_selectivity_ctx().init_op_ctx(&child->get_output_equal_sets(), child_card);
    ObOptimizerContext &opt_ctx = get_plan()->get_optimizer_context();
    ObSortCostInfo cost_info(child_card / parallel,
                             child->get_width(),
                             get_prefix_pos(),
                             get_sort_keys(),
                             is_local_merge_sort_,
                             &get_plan()->get_update_table_metas(),
                             &get_plan()->get_selectivity_ctx(),
                             double_topn_count,
                             part_cnt_);
    if (OB_FAIL(ObOptEstCost::cost_sort(cost_info, op_cost, opt_ctx.get_cost_model_type()))) {
      LOG_WARN("failed to calc cost", K(ret), K(child->get_type()));
    }
  }
  return ret;
}

int ObLogSort::compute_op_ordering()
{
  int ret = OB_SUCCESS;
  common::ObSEArray<OrderItem, 1> op_ordering;
  if (part_cnt_ > 0 && OB_FAIL(op_ordering.push_back(hash_sortkey_))) {
    LOG_WARN("failed to push back hash sortkey", K(ret));
  } else if (OB_FAIL(append(op_ordering, sort_keys_))) {
    LOG_WARN("failed to append sort keys", K(ret));
  } else if (OB_FAIL(set_op_ordering(op_ordering))) {
    LOG_WARN("failed to set op ordering", K(ret));
  } else {
    is_local_order_ = false;
  }
  return ret;
}

int ObLogSort::generate_link_sql_post(GenLinkStmtPostContext &link_ctx)
{
  UNUSED(link_ctx);
  int ret = OB_SUCCESS;
  if (0 == dblink_id_) {
    // do nothing
  } else if (!startup_exprs_.empty()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("set operator have startup filters when reverse spell dblink sql", K(ret));
  }
  return ret;
}

