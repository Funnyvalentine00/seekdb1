/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_embed.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::call_embed(ObIAllocator &alloc,
                              const ObString &model_id,
                              const ObString &content,
                              int64_t dim,
                              ObString &result)
{
  INIT_SUCC(ret);
  ObJsonInt *dim_json = nullptr;
  ObJsonObject *config = nullptr;
  ObAIFuncExprInfo *info = nullptr;
  share::ObAiModelEndpointInfo resolved_endpoint;
  query::ObIAiEndpointResolver *endpoint_resolver =
      ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
  if (model_id.empty() || content.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
  } else if (dim < 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
  } else if (dim > 0) {
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(alloc, config))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(alloc, dim, dim_json))) {
    } else if (OB_FAIL(config->add("dimensions", dim_json))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(alloc, model_id, info))) {
  } else if (OB_ISNULL(endpoint_resolver)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                 model_id, alloc, resolved_endpoint))) {
  } else {
    ObArray<ObString> contents;
    ObArray<ObString> results;
    if (OB_FAIL(contents.push_back(content))) {
    } else {
      ObAIFuncModel model(alloc, *info, resolved_endpoint);
      if (OB_FAIL(model.call_dense_embedding_vector_v2(contents, config, results))) {
      } else if (results.count() != 1) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        result = results.at(0);
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::set_batch_string_result(const ObExpr &expr, ObEvalCtx &ctx,
                                           ObDatum *res_datums, const int64_t row_idx,
                                           const ObString &result)
{
  int ret = OB_SUCCESS;
  ObTextStringDatumResult output_result(expr.datum_meta_.type_, &expr, &ctx, &res_datums[row_idx]);
  if (OB_FAIL(output_result.init_with_batch_idx(result.length(), row_idx))) {
  } else if (OB_FAIL(output_result.append(result))) {
  } else {
    output_result.set_result();
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_model_id->is_null() || arg_content->is_null()) {
    // NULL 输入与 MySQL 函数语义一致：直接返回 NULL，不报错
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    int64_t dim = 0;
    if (OB_NOT_NULL(arg_dim)) {
      dim = arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        dim = -1;
        res.set_null();
      }
    }
    ObString result;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(call_embed(temp_allocator, arg_model_id->get_string(),
                                  arg_content->get_string(), dim, result))) {
    } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                       const ObBitVector &skip, const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  ObDatum *res_datums = expr.locate_batch_datums(ctx);
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));

  if (OB_UNLIKELY(OB_ISNULL(res_datums))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai_embed, result datum is null", K(ret));
  } else if (OB_UNLIKELY(!expr.is_batch_result())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai_embed, batch func is called on non batch result expr", K(ret));
  } else if (OB_FAIL(expr.args_[MODEL_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else if (OB_FAIL(expr.args_[CONTENT_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else if (expr.arg_cnt_ == 3 && OB_FAIL(expr.args_[DIM_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else {
    const ObDatum *model_datums = expr.args_[MODEL_IDX]->locate_batch_datums(ctx);
    const ObDatum *content_datums = expr.args_[CONTENT_IDX]->locate_batch_datums(ctx);
    const ObDatum *dim_datums = expr.arg_cnt_ == 3 ? expr.args_[DIM_IDX]->locate_batch_datums(ctx) : nullptr;
    // 常量参数在所有行上共用一个 datum
    const bool model_per_row = expr.args_[MODEL_IDX]->is_batch_result();
    const bool content_per_row = expr.args_[CONTENT_IDX]->is_batch_result();
    const bool dim_per_row = expr.arg_cnt_ == 3 && expr.args_[DIM_IDX]->is_batch_result();

    // 第一遍：跳过 NULL 行并收集需要请求的行；同一批 model/dim 不一致时退化为逐行调用
    ObSEArray<int64_t, 32> rows;
    ObString batch_model_id;
    int64_t batch_dim = 0;
    bool same_model_dim = true;
    for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; ++i) {
      if (skip.at(i) || eval_flags.at(i)) {
        continue;
      }
      const ObDatum &model_datum = model_per_row ? model_datums[i] : model_datums[0];
      const ObDatum &content_datum = content_per_row ? content_datums[i] : content_datums[0];
      int64_t dim = 0;
      if (OB_NOT_NULL(dim_datums)) {
        const ObDatum &dim_datum = dim_per_row ? dim_datums[i] : dim_datums[0];
        dim = dim_datum.get_int();
      }
      if (model_datum.is_null() || content_datum.is_null()) {
        // NULL 行不进入请求，也不影响其它行的结果顺序
        res_datums[i].set_null();
        eval_flags.set(i);
      } else if (OB_NOT_NULL(dim_datums) && dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res_datums[i].set_null();
      } else if (model_datum.get_string().empty() || content_datum.get_string().empty()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
        res_datums[i].set_null();
      } else {
        if (rows.empty()) {
          batch_model_id = model_datum.get_string();
          batch_dim = dim;
        } else if (batch_dim != dim
                   || 0 != model_datum.get_string().compare(batch_model_id)) {
          same_model_dim = false;
        }
        if (OB_FAIL(rows.push_back(i))) {
        }
      }
    }

    if (OB_FAIL(ret) || rows.empty()) {
    } else if (!same_model_dim) {
      // model id 或 dimension 逐行不同，无法合并请求，逐行调用保证语义正确
      for (int64_t k = 0; OB_SUCC(ret) && k < rows.count(); ++k) {
        const int64_t i = rows.at(k);
        const ObDatum &model_datum = model_per_row ? model_datums[i] : model_datums[0];
        const ObDatum &content_datum = content_per_row ? content_datums[i] : content_datums[0];
        int64_t dim = 0;
        if (OB_NOT_NULL(dim_datums)) {
          dim = (dim_per_row ? dim_datums[i] : dim_datums[0]).get_int();
        }
        ObString result;
        if (OB_FAIL(call_embed(temp_allocator, model_datum.get_string(),
                               content_datum.get_string(), dim, result))) {
        } else if (OB_FAIL(set_batch_string_result(expr, ctx, res_datums, i, result))) {
        } else {
          eval_flags.set(i);
        }
      }
    } else {
      // 同一 model/dim：按 MAX_ROWS_PER_REQUEST 切片，一次请求合并多行输入
      ObJsonObject *config = nullptr;
      ObJsonInt *dim_json = nullptr;
      ObAIFuncExprInfo *info = nullptr;
      share::ObAiModelEndpointInfo resolved_endpoint;
      query::ObIAiEndpointResolver *endpoint_resolver =
          ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
      if (batch_dim > 0) {
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, batch_dim, dim_json))) {
        } else if (OB_FAIL(config->add("dimensions", dim_json))) {
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, batch_model_id, info))) {
      } else if (OB_ISNULL(endpoint_resolver)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                     batch_model_id, temp_allocator, resolved_endpoint))) {
      } else {
        ObAIFuncModel model(temp_allocator, *info, resolved_endpoint);
        for (int64_t start = 0; OB_SUCC(ret) && start < rows.count();
             start += MAX_ROWS_PER_REQUEST) {
          const int64_t end = MIN(start + MAX_ROWS_PER_REQUEST, rows.count());
          ObArray<ObString> contents;
          ObArray<ObString> results;
          for (int64_t k = start; OB_SUCC(ret) && k < end; ++k) {
            const int64_t i = rows.at(k);
            const ObDatum &content_datum = content_per_row ? content_datums[i] : content_datums[0];
            if (OB_FAIL(contents.push_back(content_datum.get_string()))) {
            }
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(model.call_dense_embedding_vector_v2(contents, config, results))) {
          } else if (results.count() != end - start) {
            // 返回条数必须与请求条数一致，任何一批失败都向整批传播
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("ai_embed, batch result count mismatch", K(ret),
                     K(results.count()), "expected", end - start);
          } else {
            for (int64_t k = 0; OB_SUCC(ret) && k < results.count(); ++k) {
              const int64_t i = rows.at(start + k);
              if (OB_FAIL(set_batch_string_result(expr, ctx, res_datums, i, results.at(k)))) {
              } else {
                eval_flags.set(i);
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
