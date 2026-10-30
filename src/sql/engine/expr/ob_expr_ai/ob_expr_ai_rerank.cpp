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
#include "ob_expr_ai_rerank.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"
 
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_RERANK, 
                    N_AI_RERANK, 
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIRerank::~ObExprAIRerank() 
{
}

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_tc(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_tc(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (doc_key_type == ObJsonType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {  
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_query = nullptr;
  ObDatum *arg_documents = nullptr;
  ObDatum *arg_doc_key = nullptr;
  if (OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_query, arg_documents, arg_doc_key))) {
  } else if (arg_model_id->is_null() || arg_query->is_null() || arg_documents->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query or documents is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_RERANK));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString query = arg_query->get_string();
    ObArray<ObString> header_array;
    ObJsonArray *document_array = nullptr;
    ObIJsonBase *j_base = nullptr;
    bool is_null_result = false;
    // 第 4 个参数缺省或显式为 NULL 时都表示“没有 doc key”，此时 documents 为字符串数组
    bool has_doc_key = (nullptr != arg_doc_key && !arg_doc_key->is_null());
    ObString doc_key;
    if (has_doc_key) {
      doc_key = arg_doc_key->get_string();
    }
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, temp_allocator, DOCUMENTS_IDX,
          j_base, is_null_result))) {
    } else if (OB_ISNULL(j_base)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (j_base->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is not array");
    } else if (OB_ISNULL(document_array = static_cast<ObJsonArray *>(j_base))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (document_array->element_count() == 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is empty");
    } else if (has_doc_key && doc_key.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key is empty");
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < document_array->element_count(); i++) {
        ObJsonNode *doc_node = document_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (has_doc_key && doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else if (!has_doc_key && doc_node->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not string");
        }
      }
    }
    
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else if (OB_ISNULL(info)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(info))) {
    } else if (OB_FAIL(ObAIFuncUtils::get_header(temp_allocator, *info, *endpoint_info, header_array))) {
    }

    // 对象模式先按 doc key 抽出待排序的字符串，之后两种模式共用同一套分批 + index 还原逻辑
    ObJsonArray *query_document_array = document_array;
    ObJsonArray *extracted_document_array = nullptr;
    if (OB_SUCC(ret) && has_doc_key) {
      if (OB_FAIL(get_doc_array_from_documents_array_with_key(temp_allocator, document_array,
                                                             doc_key, extracted_document_array))) {
      } else {
        query_document_array = extracted_document_array;
      }
    }

    ObArray<ObJsonNode *> model_results;
    if (OB_SUCC(ret)) {
      const int64_t doc_count = query_document_array->element_count();
      const int64_t batch_size = 20; // max batch size for rerank
      for (int64_t start = 0; OB_SUCC(ret) && start < doc_count; start += batch_size) {
        int64_t end = start + batch_size;
        if (end > doc_count) {
          end = doc_count;
        }
        ObJsonArray *batch_document_array = nullptr;
        ObJsonArray *batch_result_array = nullptr;
        if (OB_FAIL(construct_batch_document_array(temp_allocator, query_document_array,
                                                   start, end, batch_document_array))) {
        } else if (OB_FAIL(inner_eval_ai_rerank(temp_allocator, *info, *endpoint_info,
                                                header_array, query,
                                                batch_document_array, batch_result_array))) {
        } else if (OB_FAIL(append_rerank_batch_result(batch_result_array,
                                                      end - start, start, model_results))) {
        }
      }
    }

    if (OB_SUCC(ret)) {
      ObJsonArray *sorted_result_array = nullptr;
      ObJsonArray *sorted_document_array = nullptr;
      if (OB_FAIL(sort_rerank_result_array(temp_allocator, model_results, sorted_result_array))) {
      } else if (has_doc_key && OB_FAIL(sort_document_array_by_model_result(
                     temp_allocator, document_array, sorted_result_array, sorted_document_array))) {
      } else {
        ObJsonArray *output_array = has_doc_key ? sorted_document_array : sorted_result_array;
        ObString raw_str;
        if (OB_FAIL(output_array->get_raw_binary(raw_str, &temp_allocator))) {
        } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::get_doc_array_from_documents_array_with_key(ObIAllocator &allocator, ObJsonArray *document_object_array, ObString &doc_key, ObJsonArray *&doc_array)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(document_object_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (doc_key.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonArray *doc_array_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, doc_array_obj))) {
    } else {
      int64_t count = document_object_array->element_count();
      ObJsonNode *doc_node = nullptr;
      ObJsonNode *doc_value_node = nullptr;
      ObJsonObject *doc_obj = nullptr;
      for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
        doc_node = document_object_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is null");
        } else if (doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else {
          doc_obj = static_cast<ObJsonObject *>(doc_node);
          doc_value_node = doc_obj->get_value(doc_key);
          if (OB_ISNULL(doc_value_node)) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is null");
          } else if (doc_value_node->json_type() != ObJsonNodeType::J_STRING) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is not string");
          } else if (OB_FAIL(doc_array_obj->append(doc_value_node))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      doc_array = doc_array_obj;
    }
  }
  return ret;
}

int ObExprAIRerank::sort_document_array_by_model_result(ObIAllocator &allocator, ObJsonArray *document_array, ObJsonArray *model_result_array, ObJsonArray *&sorted_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *sorted_document_array_obj = nullptr;
  ObString index_key(INDEX_KEY);
  ObString score_key(SCORE_KEY);
  if (OB_ISNULL(document_array) || OB_ISNULL(model_result_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, sorted_document_array_obj))) {
  } else {
    int64_t count = model_result_array->element_count();
    for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
      ObJsonNode *result_node = model_result_array->get_value(i);
      ObJsonObject *model_result_obj = nullptr;
      ObJsonNode *index_node = nullptr;
      ObJsonNode *score_node = nullptr;
      if (OB_ISNULL(result_node) || ObJsonNodeType::J_OBJECT != result_node->json_type()) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_ISNULL(model_result_obj = static_cast<ObJsonObject *>(result_node))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_ISNULL(index_node = model_result_obj->get_value(index_key))
                 || OB_ISNULL(score_node = model_result_obj->get_value(score_key))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        int64_t index = index_node->get_int();
        ObJsonNode *origin_node = nullptr;
        ObJsonObject *new_obj = nullptr;
        if (index < 0 || index >= document_array->element_count()) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("ai_rerank, result index is out of range", K(ret), K(index),
                   "document_count", document_array->element_count());
        } else if (OB_ISNULL(origin_node = document_array->get_value(index))) {
          ret = OB_ERR_UNEXPECTED;
        } else if (ObJsonNodeType::J_OBJECT != origin_node->json_type()) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_ISNULL(new_obj = static_cast<ObJsonObject *>(origin_node->clone(&allocator, true)))) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(new_obj->add(MODEL_SCORE_KEY, score_node))) {
        } else if (OB_FAIL(sorted_document_array_obj->append(new_obj))) {
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    sorted_document_array = sorted_document_array_obj;
  }
  return ret;
}

int ObExprAIRerank::construct_batch_document_array(ObIAllocator &allocator, ObJsonArray *document_array, int64_t start_idx, int64_t end_idx, ObJsonArray *&batch_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *result_array = nullptr;
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    for (int64_t i = start_idx; i < end_idx; i++) {
      if (OB_FAIL(result_array->append(document_array->get_value(i)))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    batch_document_array = result_array;
  }
  return ret;
}

int ObExprAIRerank::inner_eval_ai_rerank(ObIAllocator &allocator, 
                                          const ObAIFuncExprInfo &info, 
                                          const share::ObAiModelEndpointInfo &endpoint_info,
                                          ObArray<ObString> &header_array, 
                                          ObString &query, 
                                          ObJsonArray *document_array, 
                                          ObJsonArray *&result_array)
{
  INIT_SUCC(ret);
  ObJsonArray *res = nullptr;
  ObJsonObject *config_json = nullptr;
  ObJsonObject *body = nullptr;
  ObJsonObject *http_response = nullptr;
  ObIJsonBase *response = nullptr;
  ObAIFuncClient ai_client;
  if (OB_FAIL(ObAIFuncUtils::get_rerank_body(allocator, info, endpoint_info, query, document_array, config_json, body))) {
  } else if (OB_FAIL(ai_client.send_post(allocator, endpoint_info.get_url(), header_array, body, http_response))) {
  } else if (OB_FAIL(ObAIFuncUtils::parse_rerank_output(allocator, endpoint_info, http_response, response))) {
  } else if (response->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(res = static_cast<ObJsonArray *>(response))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    result_array = res;
  }
  return ret;
}

int ObExprAIRerank::append_rerank_batch_result(ObJsonArray *batch_result_array,
                                               int64_t expected_count,
                                               int64_t base_index,
                                               ObArray<ObJsonNode *> &model_results)
{
  INIT_SUCC(ret);
  ObString index_key(INDEX_KEY);
  ObString score_key(SCORE_KEY);
  if (OB_ISNULL(batch_result_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (expected_count <= 0 || batch_result_array->element_count() != expected_count) {
    ret = OB_INVALID_DATA;
    LOG_WARN("ai_rerank, result count is not equal to document count", K(ret),
             K(expected_count), "actual_count", batch_result_array->element_count());
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < expected_count; i++) {
      ObJsonNode *result_node = batch_result_array->get_value(i);
      ObJsonObject *result_obj = nullptr;
      ObJsonNode *index_node = nullptr;
      ObJsonNode *score_node = nullptr;
      if (OB_ISNULL(result_node) || ObJsonNodeType::J_OBJECT != result_node->json_type()) {
        ret = OB_INVALID_DATA;
        LOG_WARN("ai_rerank, result is not object", K(ret), K(i));
      } else if (OB_ISNULL(result_obj = static_cast<ObJsonObject *>(result_node))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_ISNULL(index_node = result_obj->get_value(index_key))
                 || OB_ISNULL(score_node = result_obj->get_value(score_key))) {
        ret = OB_INVALID_DATA;
        LOG_WARN("ai_rerank, result is missing index or relevance_score", K(ret), K(i));
      } else if (ObJsonNodeType::J_INT != index_node->json_type()
                 && ObJsonNodeType::J_UINT != index_node->json_type()) {
        ret = OB_INVALID_DATA;
        LOG_WARN("ai_rerank, result index is not integer", K(ret), K(i));
      } else if (!share::ObJsonHelper::is_number_type(score_node)) {
        ret = OB_INVALID_DATA;
        LOG_WARN("ai_rerank, result relevance_score is not number", K(ret), K(i));
      } else {
        int64_t index = index_node->get_int();
        bool duplicated = false;
        if (index < 0 || index >= expected_count) {
          ret = OB_INVALID_DATA;
          LOG_WARN("ai_rerank, result index is out of batch range", K(ret), K(index), K(expected_count));
        } else {
          // 批内 index 必须唯一，否则无法还原原始顺序
          for (int64_t j = 0; !duplicated && j < i; j++) {
            ObJsonNode *prev_node = batch_result_array->get_value(j);
            if (OB_NOT_NULL(prev_node) && ObJsonNodeType::J_OBJECT == prev_node->json_type()) {
              ObJsonNode *prev_index = static_cast<ObJsonObject *>(prev_node)->get_value(index_key);
              if (OB_NOT_NULL(prev_index) && prev_index->get_int() == index) {
                duplicated = true;
              }
            }
          }
          if (duplicated) {
            ret = OB_INVALID_DATA;
            LOG_WARN("ai_rerank, result index is duplicated", K(ret), K(index));
          } else {
            // 批内 index 加上批起始偏移，还原成相对整个 documents 数组的下标
            int64_t global_index = index + base_index;
            if (ObJsonNodeType::J_INT == index_node->json_type()) {
              static_cast<ObJsonInt *>(index_node)->set_value(global_index);
            } else {
              static_cast<ObJsonUint *>(index_node)->set_value(static_cast<uint64_t>(global_index));
            }
            if (OB_FAIL(model_results.push_back(result_node))) {
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::sort_rerank_result_array(ObIAllocator &allocator,
                                             ObArray<ObJsonNode *> &model_results,
                                             ObJsonArray *&sorted_result_array)
{
  INIT_SUCC(ret);
  ObJsonArray *result_array = nullptr;
  ObString index_key(INDEX_KEY);
  ObString score_key(SCORE_KEY);
  // 插入排序：relevance_score 降序，同分时按原始 index 升序，保证稳定顺序
  for (int64_t i = 1; OB_SUCC(ret) && i < model_results.count(); i++) {
    ObJsonNode *cur_node = model_results.at(i);
    ObJsonObject *cur_obj = nullptr;
    if (OB_ISNULL(cur_node) || OB_ISNULL(cur_obj = static_cast<ObJsonObject *>(cur_node))) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      double cur_score = cur_obj->get_value(score_key)->get_double();
      int64_t cur_index = cur_obj->get_value(index_key)->get_int();
      int64_t j = i - 1;
      for (; j >= 0; j--) {
        ObJsonObject *prev_obj = static_cast<ObJsonObject *>(model_results.at(j));
        double prev_score = prev_obj->get_value(score_key)->get_double();
        int64_t prev_index = prev_obj->get_value(index_key)->get_int();
        if (prev_score < cur_score || (prev_score == cur_score && prev_index > cur_index)) {
          model_results.at(j + 1) = model_results.at(j);
        } else {
          break;
        }
      }
      model_results.at(j + 1) = cur_node;
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < model_results.count(); i++) {
        if (OB_FAIL(result_array->append(model_results.at(i)))) {
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    sorted_result_array = result_array;
  }
  return ret;
}

int ObExprAIRerank::construct_config_json(ObIAllocator &allocator, int64_t top_n, int64_t return_doc, ObJsonObject *&config_json)
{
  INIT_SUCC(ret);
  ObJsonObject *config_obj = nullptr;
  ObJsonInt *top_n_obj = nullptr;
  ObJsonBoolean *return_doc_obj = nullptr;
  bool return_doc_value = true;
  if (return_doc <= 0) {
    return_doc_value = false;
  }
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config_obj))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_boolean(allocator, return_doc_value, return_doc_obj))) {
  } else if (OB_FAIL(config_obj->add("return_documents", return_doc_obj))) {
  } else if (top_n > 0) {
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, top_n, top_n_obj))) {
    } else if (OB_FAIL(config_obj->add("top_n", top_n_obj))) {
    }
  }
  if (OB_SUCC(ret)) {
    config_json = config_obj;
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx, 
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
  //   bool got_data = false;
  //   ObObj const_data;
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
  //   } else {
  //     ObAIFuncExprInfo *info = OB_NEWx(ObAIFuncExprInfo, allocator, *allocator, T_FUN_SYS_AI_RERANK);
  //     if (OB_ISNULL(info)) {
  //       ret = OB_ALLOCATE_MEMORY_FAILED;
  //       LOG_WARN("failed to allocate memory for ai rerank info", K(ret));
  //     } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //       LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //     } else {
  //       rt_expr.extra_info_ = info;
  //     }
  //   }
  // }
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
