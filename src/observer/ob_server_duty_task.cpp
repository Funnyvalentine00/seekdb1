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

#define USING_LOG_PREFIX SERVER
#include "config_bridge.h"
#include "share/config/ob_config_helper.h"
#include "ob_server_duty_task.h"
#include "share/rc/ob_server_runtime.h"
#include "sql/engine/ob_sql_memory_manager.h"
#include "observer/omt/ob_server_runtime.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/ai_service/ob_ai_service_struct.h"
#include <string.h>

using namespace oceanbase::common;

namespace oceanbase {
using namespace share;
using namespace share::schema;
using namespace sql;
namespace observer {

ObServerDutyTask::ObServerDutyTask()
  : allocator_(ObModIds::OB_DUTY_TASK)
{
}

void ObServerDutyTask::runTimerTask()
{
  allocator_.reset_remain_one_page();
  update_runtime_settings();
}

int ObServerDutyTask::schedule(common::ObTimer &timer)
{
  return timer.schedule(*this, SCHEDULE_PERIOD, true);
}

void ObServerDutyTask::update_runtime_settings()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(update_ctx_memory_throttle())) {
  }
}

int ObServerDutyTask::update_ctx_memory_throttle()
{
  int ret = OB_SUCCESS;
  {
    ObCtxMemoryLimitChecker checker;
    uint64_t ctx_id = 0;
    int64_t limit = 0;
    ObMallocAllocator *alloc = ObMallocAllocator::get_instance();
    rust::String limits = config::_ctx_memory_limit();
    if (!checker.check(limits.c_str(), ctx_id, limit)) {
      // do nothing
    } else {
      if (limits.empty()) {
        ctx_id = ObCtxIds::MAX_CTX_ID;
        limit = INT64_MAX; // empty str means no limit, and not care ctx_id.
      }
      for (int i = 0; i < ObCtxIds::MAX_CTX_ID; i++) {
        if (ObCtxIds::WORK_AREA == i ||
            ObCtxIds::META_OBJ_CTX_ID == i) {
          continue;
        }
        auto allocator = alloc->get_ctx_allocator(i);
        if (OB_NOT_NULL(allocator)) {
          if (OB_FAIL(allocator->set_limit(ctx_id == i ? limit : INT64_MAX))) {
          }
        }
      }
    }
  }
  return ret;
}

//////////////////////////////////////////////////////////////////////////////////////////

int ObSqlMemoryTimerTask::schedule(common::ObTimer &timer)
{
  return timer.schedule(*this, SCHEDULE_PERIOD, true);
}

void ObSqlMemoryTimerTask::runTimerTask()
{
  int ret = OB_SUCCESS;
  {
    SERVER_MODULE_SCOPE {
      ObSqlMemoryManager *sql_mem_mgr = ::oceanbase::share::server_service<::oceanbase::sql::ObSqlMemoryManager>();
      if (OB_UNLIKELY(nullptr == sql_mem_mgr)) {
      } else if (OB_FAIL(sql_mem_mgr->calculate_global_bound_size())) {
      }
    }
  }
}

//////////////////////////////////////////////////////////////////////////////////////////
namespace {

enum AiTaskState
{
  AI_TASK_SKIPPED = 0,
  AI_TASK_SUCCEEDED = 1,
  AI_TASK_FAILED = 2
};

bool ai_str_equal(const ObString &left, const ObString &right)
{
  return left.length() == right.length()
      && (0 == left.length() || 0 == ::memcmp(left.ptr(), right.ptr(), left.length()));
}

bool ai_same_group(const AiColumnTaskItem &left, const AiColumnTaskItem &right)
{
  return left.table_id_ == right.table_id_
      && left.request_id_ == right.request_id_
      && ai_str_equal(left.rowkey_predicate_, right.rowkey_predicate_);
}

int ai_append_quoted(ObSqlString &sql, const ObString &value)
{
  int ret = OB_SUCCESS;
  const char quote = '\'';
  if (OB_FAIL(sql.append(&quote, 1))) {
  } else {
    for (int32_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
      const char c = value.ptr()[i];
      if ('\'' == c) {
        if (OB_FAIL(sql.append("''", 2))) {
        }
      } else if (OB_FAIL(sql.append(&c, 1))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(sql.append(&quote, 1))) {
    }
  }
  return ret;
}

int ai_append_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("`", 1))) {
  } else if (OB_FAIL(sql.append(name.ptr(), name.length()))) {
  } else if (OB_FAIL(sql.append("`", 1))) {
  }
  return ret;
}

int ai_append_result_value(ObSqlString &sql, const ObString &value, const int64_t result_type)
{
  int ret = OB_SUCCESS;
  int32_t begin = 0;
  int32_t end = value.length();
  while (begin < end && ' ' == value.ptr()[begin]) {
    ++begin;
  }
  while (end > begin && ' ' == value.ptr()[end - 1]) {
    --end;
  }
  bool numeric = false;
  int64_t number = 0;
  if (ob_is_integer_type(static_cast<ObObjType>(result_type))
      && end > begin && end - begin < 64) {
    char buf[64];
    MEMCPY(buf, value.ptr() + begin, end - begin);
    buf[end - begin] = '\0';
    char *parse_end = nullptr;
    errno = 0;
    const long long parsed = ::strtoll(buf, &parse_end, 10);
    if (0 == errno && nullptr != parse_end && '\0' == *parse_end) {
      numeric = true;
      number = static_cast<int64_t>(parsed);
    }
  }
  if (OB_FAIL(ret)) {
  } else if (numeric) {
    if (OB_FAIL(sql.append_fmt("%ld", number))) {
    }
  } else if (OB_FAIL(ai_append_quoted(sql, value))) {
  }
  return ret;
}

}  // anonymous namespace

int ObAiColumnWorkerTask::load_pending_tasks(common::ObIArray<AiColumnTaskItem> &tasks)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(sql.append("select table_id, column_id, rowkey_predicate, request_id,"
                                " retry_count, max_retries,"
                                " ifnull(ai_model_name, '') as ai_model_name,"
                                " ifnull(system_prompt, '') as system_prompt,"
                                " ifnull(prompt_expr, '') as prompt_expr,"
                                " ifnull(result_type, 0) as result_type"
                                " from oceanbase.__all_ai_column_task"
                                " where eligible_ts <= now(6)"
                                " order by gmt_create limit 20"))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      common::sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        LOG_INFO("ai column worker scan failed", KR(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_INFO("ai column worker scan has no result", KR(ret), K(sql));
      } else {
        while (OB_SUCC(ret) && OB_SUCC(result->next())) {
          AiColumnTaskItem item;
          int64_t table_id_value = 0;
          int64_t column_id_value = 0;
          ObString rowkey;
          ObString model;
          ObString system_prompt;
          ObString prompt_expr;
          if (OB_FAIL(result->get_int("table_id", table_id_value)) ||
              OB_FAIL(result->get_int("column_id", column_id_value)) ||
              OB_FAIL(result->get_int("request_id", item.request_id_)) ||
              OB_FAIL(result->get_int("retry_count", item.retry_count_)) ||
              OB_FAIL(result->get_int("max_retries", item.max_retries_)) ||
              OB_FAIL(result->get_int("result_type", item.result_type_))) {
          } else if (OB_FAIL(result->get_varchar("rowkey_predicate", rowkey))) {
          } else if (OB_FAIL(result->get_varchar("ai_model_name", model))) {
          } else if (OB_FAIL(result->get_varchar("system_prompt", system_prompt))) {
          } else if (OB_FAIL(result->get_varchar("prompt_expr", prompt_expr))) {
          } else if (FALSE_IT(item.table_id_ = static_cast<uint64_t>(table_id_value))) {
          } else if (FALSE_IT(item.column_id_ = static_cast<uint64_t>(column_id_value))) {
          } else if (OB_FAIL(ob_write_string(allocator_, rowkey, item.rowkey_predicate_))) {
          } else if (OB_FAIL(ob_write_string(allocator_, model, item.model_))) {
          } else if (OB_FAIL(ob_write_string(allocator_, system_prompt, item.system_prompt_))) {
          } else if (OB_FAIL(ob_write_string(allocator_, prompt_expr, item.prompt_expr_))) {
          } else if (OB_FAIL(tasks.push_back(item))) {
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        } else if (OB_SUCCESS != ret) {
          LOG_INFO("ai column worker scan row failed", KR(ret));
        }
      }
    }
  }
  return ret;
}

int ObAiColumnWorkerTask::resolve_table_name(const uint64_t table_id,
                                             common::ObString &db_name,
                                             common::ObString &table_name,
                                             bool &found)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  found = false;
  if (OB_FAIL(sql.append_fmt("select d.database_name as database_name,"
                             " t.table_name as table_name"
                             " from oceanbase.__all_table t"
                             " join oceanbase.__all_database d on d.database_id = t.database_id"
                             " where t.table_id = %lu limit 1", table_id))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      common::sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        const int tmp_ret = result->next();
        if (OB_SUCCESS == tmp_ret) {
          ObString db;
          ObString table;
          if (OB_FAIL(result->get_varchar("database_name", db))) {
          } else if (OB_FAIL(result->get_varchar("table_name", table))) {
          } else if (OB_FAIL(ob_write_string(allocator_, db, db_name))) {
          } else if (OB_FAIL(ob_write_string(allocator_, table, table_name))) {
          } else {
            found = true;
          }
        } else if (OB_ITER_END != tmp_ret) {
          ret = tmp_ret;
        }
      }
    }
  }
  return ret;
}

int ObAiColumnWorkerTask::load_column_metas(const uint64_t table_id,
                                            common::ObIArray<AiColumnMeta> &metas)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.append_fmt("select column_id, column_name from oceanbase.__all_ai_column"
                             " where table_id = %lu", table_id))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      common::sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        while (OB_SUCC(ret) && OB_SUCC(result->next())) {
          AiColumnMeta meta;
          int64_t column_id = 0;
          ObString column_name;
          if (OB_FAIL(result->get_int("column_id", column_id))) {
          } else if (OB_FAIL(result->get_varchar("column_name", column_name))) {
          } else {
            meta.column_id_ = static_cast<uint64_t>(column_id);
            if (OB_FAIL(ob_write_string(allocator_, column_name, meta.column_name_))) {
            } else if (OB_FAIL(metas.push_back(meta))) {
            }
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

const AiColumnMeta *ObAiColumnWorkerTask::find_column_meta(const common::ObIArray<AiColumnMeta> &metas,
                                                           const uint64_t column_id)
{
  const AiColumnMeta *meta = NULL;
  for (int64_t i = 0; NULL == meta && i < metas.count(); ++i) {
    if (metas.at(i).column_id_ == column_id) {
      meta = &metas.at(i);
    }
  }
  return meta;
}

int ObAiColumnWorkerTask::eval_prompt(const common::ObString &db_name,
                                      const common::ObString &table_name,
                                      const common::ObString &rowkey_predicate,
                                      const common::ObString &prompt_expr,
                                      common::ObString &prompt)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.append("select ifnull(("))) {
  } else if (OB_FAIL(sql.append(prompt_expr.ptr(), prompt_expr.length()))) {
  } else if (OB_FAIL(sql.append("), '') as ai_prompt from "))) {
  } else if (OB_FAIL(ai_append_identifier(sql, db_name))) {
  } else if (OB_FAIL(sql.append(".", 1))) {
  } else if (OB_FAIL(ai_append_identifier(sql, table_name))) {
  } else if (OB_FAIL(sql.append(" where "))) {
  } else if (OB_FAIL(sql.append(rowkey_predicate.ptr(), rowkey_predicate.length()))) {
  } else if (OB_FAIL(sql.append(" limit 1"))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      common::sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        const int tmp_ret = result->next();
        if (OB_SUCCESS == tmp_ret) {
          ObString value;
          if (OB_FAIL(result->get_varchar("ai_prompt", value))) {
          } else if (OB_FAIL(ob_write_string(allocator_, value, prompt))) {
          }
        } else if (OB_ITER_END == tmp_ret) {
          ret = OB_ITER_END;
        } else {
          ret = tmp_ret;
        }
      }
    }
  }
  return ret;
}

int ObAiColumnWorkerTask::call_model(const AiColumnTaskItem &item,
                                     const common::ObString &prompt,
                                     common::ObString &value)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator;
  ObAIFuncExprInfo *info = NULL;
  share::ObAiModelEndpointInfo endpoint_info;
  ObString model = item.model_;
  ObString system_prompt = item.system_prompt_;
  ObString user_prompt = prompt;
  ObString result;
  if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model, info))) {
  } else {
    query::ObIAiEndpointResolver *resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    if (OB_ISNULL(resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(resolver->resolve_by_model_name(model, allocator, endpoint_info))) {
    } else {
      ObAIFuncModel model_caller(allocator, *info, endpoint_info);
      if (OB_FAIL(model_caller.call_completion_with_system(system_prompt, user_prompt, NULL, result))) {
      } else if (OB_FAIL(ob_write_string(allocator_, result, value))) {
      }
    }
  }
  return ret;
}

int ObAiColumnWorkerTask::finish_transaction(common::ObMySQLTransaction &trans)
{
  int ret = OB_SUCCESS;
  if (trans.is_started()) {
    int tmp_ret = trans.end(true);
    if (OB_SUCCESS != tmp_ret) {
      ret = tmp_ret;
    }
  }
  return ret;
}

int ObAiColumnWorkerTask::delete_group_tasks(common::ObMySQLTransaction &trans,
                                             const uint64_t table_id,
                                             const int64_t request_id,
                                             const common::ObString &rowkey_predicate,
                                             const uint64_t column_id,
                                             const bool one_column)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.append_fmt("delete from oceanbase.__all_ai_column_task"
                             " where table_id = %lu and request_id = %ld",
                             table_id, request_id))) {
  } else if (one_column && OB_FAIL(sql.append_fmt(" and column_id = %lu", column_id))) {
  } else if (OB_FAIL(sql.append(" and rowkey_predicate = "))) {
  } else if (OB_FAIL(ai_append_quoted(sql, rowkey_predicate))) {
  } else {
    int64_t affected_rows = 0;
    if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
      LOG_INFO("failed to delete ai column tasks", KR(ret), K(sql));
    }
  }
  return ret;
}

int ObAiColumnWorkerTask::publish_group(const uint64_t table_id,
                                        const int64_t request_id,
                                        const common::ObString &db_name,
                                        const common::ObString &table_name,
                                        const common::ObString &rowkey_predicate,
                                        const common::ObIArray<AiColumnTaskItem *> &group,
                                        const common::ObIArray<AiColumnMeta> &metas,
                                        const common::ObIArray<common::ObString> &values,
                                        const common::ObIArray<int> &states)
{
  int ret = OB_SUCCESS;
  ObSqlString update_sql;
  bool first = true;
  if (OB_FAIL(update_sql.append("update "))) {
  } else if (OB_FAIL(ai_append_identifier(update_sql, db_name))) {
  } else if (OB_FAIL(update_sql.append(".", 1))) {
  } else if (OB_FAIL(ai_append_identifier(update_sql, table_name))) {
  } else if (OB_FAIL(update_sql.append(" set "))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
    if (AI_TASK_SUCCEEDED != states.at(i)) {
      continue;
    }
    const AiColumnMeta *meta = find_column_meta(metas, group.at(i)->column_id_);
    if (OB_ISNULL(meta)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(update_sql.append(first ? "" : ", ", first ? 0 : 2))) {
    } else if (OB_FAIL(ai_append_identifier(update_sql, meta->column_name_))) {
    } else if (OB_FAIL(update_sql.append(" = "))) {
    } else if (OB_FAIL(ai_append_result_value(update_sql, values.at(i),
                                              group.at(i)->result_type_))) {
    } else {
      first = false;
    }
  }
  if (OB_SUCC(ret) && !first) {
    if (OB_FAIL(update_sql.append(" where "))) {
    } else if (OB_FAIL(update_sql.append(rowkey_predicate.ptr(), rowkey_predicate.length()))) {
    } else if (OB_FAIL(update_sql.append(" and exists (select 1 from oceanbase.__all_ai_column_task"
                                         " where table_id = "))) {
    } else if (OB_FAIL(update_sql.append_fmt("%lu and request_id = %ld and rowkey_predicate = ",
                                             table_id, request_id))) {
    } else if (OB_FAIL(ai_append_quoted(update_sql, rowkey_predicate))) {
    } else if (OB_FAIL(update_sql.append(")", 1))) {
    }
  }
  if (OB_SUCC(ret) && !first) {
    ObMySQLTransaction trans;
    if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
    } else {
      int64_t affected_rows = 0;
      if (OB_FAIL(trans.write(update_sql.ptr(), affected_rows))) {
        LOG_INFO("failed to publish ai column result", KR(ret), K(update_sql));
      } else if (OB_FAIL(delete_group_tasks(trans, table_id, request_id,
                                            rowkey_predicate, 0, false))) {
      } else if (OB_FAIL(finish_transaction(trans))) {
      }
      if (trans.is_started()) {
        trans.end(false);
      }
    }
  } else if (OB_SUCC(ret)) {
    ObMySQLTransaction trans;
    if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
    } else {
      if (OB_FAIL(delete_group_tasks(trans, table_id, request_id,
                                     rowkey_predicate, 0, false))) {
      } else if (OB_FAIL(finish_transaction(trans))) {
      }
      if (trans.is_started()) {
        trans.end(false);
      }
    }
  }
  return ret;
}

int ObAiColumnWorkerTask::retry_failed_group(const uint64_t table_id,
                                             const int64_t request_id,
                                             const common::ObString &rowkey_predicate,
                                             const common::ObIArray<AiColumnTaskItem *> &group,
                                             const common::ObIArray<int> &states)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
      AiColumnTaskItem *item = group.at(i);
      if (AI_TASK_SUCCEEDED == states.at(i)) {
        // the successful task is retried together with the failed ones
        continue;
      }
      if (AI_TASK_FAILED == states.at(i) && item->retry_count_ < item->max_retries_) {
        ObSqlString sql;
        if (OB_FAIL(sql.append_fmt("update oceanbase.__all_ai_column_task set state = 3,"
                                   " retry_count = retry_count + 1,"
                                   " eligible_ts = now(6) + interval 1 second,"
                                   " update_ts = now(6), gmt_modified = now(6)"
                                   " where table_id = %lu and column_id = %lu"
                                   " and request_id = %ld and rowkey_predicate = ",
                                   table_id, item->column_id_, request_id))) {
        } else if (OB_FAIL(ai_append_quoted(sql, rowkey_predicate))) {
        } else {
          int64_t affected_rows = 0;
          if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
            LOG_INFO("failed to update ai column task retry", KR(ret), K(sql));
          }
        }
      } else if (OB_FAIL(delete_group_tasks(trans, table_id, request_id,
                                            rowkey_predicate, item->column_id_, true))) {
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(finish_transaction(trans))) {
      }
    }
    if (trans.is_started()) {
      trans.end(false);
    }
  }
  return ret;
}

int ObAiColumnWorkerTask::process_group(const common::ObIArray<AiColumnTaskItem *> &group)
{
  int ret = OB_SUCCESS;
  if (group.empty()) {
  } else {
    const uint64_t table_id = group.at(0)->table_id_;
    const int64_t request_id = group.at(0)->request_id_;
    const ObString rowkey_predicate = group.at(0)->rowkey_predicate_;
    ObString db_name;
    ObString table_name;
    bool table_found = false;
    if (OB_FAIL(resolve_table_name(table_id, db_name, table_name, table_found))) {
    } else if (!table_found) {
      ObMySQLTransaction trans;
      if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
      } else {
        if (OB_FAIL(delete_group_tasks(trans, table_id, request_id,
                                       rowkey_predicate, 0, false))) {
        } else if (OB_FAIL(finish_transaction(trans))) {
        }
        if (trans.is_started()) {
          trans.end(false);
        }
      }
    } else {
      ObSEArray<AiColumnMeta, 8> metas;
      if (OB_FAIL(load_column_metas(table_id, metas))) {
      } else {
        bool failed = false;
        bool succeeded = false;
        bool row_missing = false;
        ObSEArray<common::ObString, 8> values;
        ObSEArray<int, 8> states;
        for (int64_t i = 0; OB_SUCC(ret) && !row_missing && i < group.count(); ++i) {
          AiColumnTaskItem *item = group.at(i);
          const AiColumnMeta *meta = find_column_meta(metas, item->column_id_);
          ObString value;
          int state = AI_TASK_SKIPPED;
          if (OB_ISNULL(meta)) {
            // the ai column metadata is gone, drop the orphan task
          } else {
            const int eval_ret = eval_prompt(db_name, table_name, rowkey_predicate,
                                             item->prompt_expr_, value);
            if (OB_ITER_END == eval_ret) {
              row_missing = true;
            } else if (OB_SUCCESS != eval_ret) {
              state = AI_TASK_FAILED;
              failed = true;
              value.reset();
              LOG_INFO("failed to evaluate ai column prompt", KR(eval_ret), K(table_id),
                       K(item->column_id_));
            } else {
              ObString model_result;
              const int model_ret = call_model(*item, value, model_result);
              if (OB_SUCCESS == model_ret) {
                state = AI_TASK_SUCCEEDED;
                succeeded = true;
                value = model_result;
              } else {
                state = AI_TASK_FAILED;
                failed = true;
                value.reset();
                LOG_INFO("failed to call ai model for column", KR(model_ret), K(table_id),
                         K(item->column_id_), K(item->model_));
              }
            }
          }
          if (OB_SUCC(ret) && !row_missing) {
            if (OB_FAIL(states.push_back(state))) {
            } else if (OB_FAIL(values.push_back(value))) {
            }
          }
        }
        if (OB_SUCC(ret)) {
          if (row_missing) {
            ObMySQLTransaction trans;
            if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
            } else {
              if (OB_FAIL(delete_group_tasks(trans, table_id, request_id,
                                             rowkey_predicate, 0, false))) {
              } else if (OB_FAIL(finish_transaction(trans))) {
              }
              if (trans.is_started()) {
                trans.end(false);
              }
            }
          } else if (failed) {
            if (OB_FAIL(retry_failed_group(table_id, request_id, rowkey_predicate,
                                           group, states))) {
            }
          } else if (succeeded) {
            if (OB_FAIL(publish_group(table_id, request_id, db_name, table_name,
                                      rowkey_predicate, group, metas, values, states))) {
            }
          } else if (OB_FAIL(retry_failed_group(table_id, request_id, rowkey_predicate,
                                                group, states))) {
          }
        }
      }
    }
  }
  return ret;
}

int ObAiColumnWorkerTask::do_work()
{
  int ret = OB_SUCCESS;
  allocator_.reset();
  ObMultiVersionSchemaService &schema_service = ObMultiVersionSchemaService::get_instance();
  if (!schema_service.is_runtime_schema_ready()) {
    // wait until the runtime schema is ready
  } else {
    ObSEArray<AiColumnTaskItem, 16> tasks;
    if (OB_FAIL(load_pending_tasks(tasks))) {
      LOG_INFO("failed to load ai column tasks", KR(ret));
    } else if (!tasks.empty()) {
      for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
        if (tasks.at(i).processed_) {
          continue;
        }
        ObSEArray<AiColumnTaskItem *, 8> group;
        tasks.at(i).processed_ = true;
        if (OB_FAIL(group.push_back(&tasks.at(i)))) {
        } else {
          for (int64_t j = i + 1; OB_SUCC(ret) && j < tasks.count(); ++j) {
            if (!tasks.at(j).processed_ && ai_same_group(tasks.at(i), tasks.at(j))) {
              tasks.at(j).processed_ = true;
              if (OB_FAIL(group.push_back(&tasks.at(j)))) {
              }
            }
          }
        }
        if (OB_SUCC(ret)) {
          const int group_ret = process_group(group);
          if (OB_SUCCESS != group_ret) {
            LOG_INFO("failed to process ai column task group", KR(group_ret),
                     K(tasks.at(i).table_id_), K(tasks.at(i).request_id_));
          }
        }
      }
    }
  }
  return ret;
}

ObAiColumnWorkerTask::ObAiColumnWorkerTask()
  : allocator_(ObModIds::OB_DUTY_TASK)
{
}

int ObAiColumnWorkerTask::schedule(common::ObTimer &timer)
{
  const int ret = timer.schedule(*this, SCHEDULE_PERIOD, true);
  LOG_INFO("schedule ai column worker task", KR(ret), K(SCHEDULE_PERIOD));
  return ret;
}

void ObAiColumnWorkerTask::runTimerTask()
{
  const int ret = do_work();
  if (OB_SUCCESS != ret) {
    LOG_INFO("ai column worker failed", KR(ret));
  }
}

}  // observer
}  // oceanbase
