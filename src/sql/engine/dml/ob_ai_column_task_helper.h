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

#pragma once

#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "lib/string/ob_sql_string.h"
#include "lib/utility/ob_print_utils.h"
#include <strings.h>
#include <ctype.h>

namespace oceanbase
{
namespace sql
{
namespace aicolumn
{

enum ObAiColumnDmlEvent
{
  AI_COL_EVENT_INSERT = 0,
  AI_COL_EVENT_UPDATE = 1,
  AI_COL_EVENT_DELETE = 2
};

// only user tables created at runtime are handled
static inline bool ob_ai_user_table(const uint64_t table_id)
{
  return table_id >= 500000;
}

static inline int64_t ob_ai_next_request_id()
{
  static int64_t base = 0;
  if (0 == base) {
    base = common::ObTimeUtility::current_time() * 1000;
  }
  return __sync_fetch_and_add(&base, 1);
}

static inline int ob_ai_quote_append(ObSqlString &sql, const common::ObString &value)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("'", 1))) {
  } else {
    for (int32_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
      const char *ptr = value.ptr() + i;
      if ('\'' == *ptr) {
        if (OB_FAIL(sql.append("''", 2))) {
        }
      } else if (OB_FAIL(sql.append(ptr, 1))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(sql.append("'", 1))) {
    }
  }
  return ret;
}

// ai tables keep hidden aux columns named "__ai_state_xxx"
static inline bool ob_ai_table_has_ai_column(const share::schema::ObTableSchema &table)
{
  bool has = false;
  for (int64_t i = 0; !has && i < table.get_column_count(); ++i) {
    const share::schema::ObColumnSchemaV2 *column = table.get_column_schema_by_idx(i);
    if (OB_NOT_NULL(column) && column->is_hidden()) {
      const common::ObString &name = column->get_column_name_str();
      has = (name.length() >= 7 && 0 == strncasecmp(name.ptr(), "__ai_st", 7));
    }
  }
  return has;
}

// build a rowkey predicate like "`id` = 2" from the evaluated dml row exprs
static inline int ob_ai_build_rowkey_predicate(ObEvalCtx &eval_ctx,
                                               const share::schema::ObTableSchema &table,
                                               const ObDMLBaseCtDef &dml_ctdef,
                                               const common::ObIArray<ObExpr *> &row,
                                               const common::ObObjPrintParams &print_params,
                                               common::ObIAllocator &alloc,
                                               common::ObString &predicate)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  const ObRowkeyInfo &rowkey_info = table.get_rowkey_info();
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_info.get_size(); ++i) {
    uint64_t column_id = common::OB_INVALID_ID;
    const share::schema::ObColumnSchemaV2 *column = NULL;
    int64_t idx = -1;
    common::ObDatum *datum = NULL;
    if (OB_FAIL(rowkey_info.get_column_id(i, column_id))) {
    } else if (OB_ISNULL(column = table.get_column_schema(column_id))) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      for (int64_t j = 0; OB_SUCC(ret) && j < dml_ctdef.column_ids_.count(); ++j) {
        if (column_id == dml_ctdef.column_ids_.at(j)) {
          idx = j;
          break;
        }
      }
      if (OB_SUCC(ret) && (idx < 0 || idx >= row.count())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_INFO("ai column task can not find rowkey expr", K(ret), K(column_id), K(idx),
                 K(row.count()), K(dml_ctdef.column_ids_));
      } else if (OB_SUCC(ret) && OB_FAIL(row.at(idx)->eval(eval_ctx, datum))) {
      } else if (OB_SUCC(ret) && OB_ISNULL(datum)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_SUCC(ret)
                 && OB_FAIL(sql.append_fmt("%s`%.*s` = ",
                                           0 == i ? "" : " and ",
                                           column->get_column_name_str().length(),
                                           column->get_column_name_str().ptr()))) {
      } else if (OB_SUCC(ret) && datum->is_null()) {
        if (OB_FAIL(sql.append("NULL", 4))) {
        }
      } else if (OB_SUCC(ret)) {
        ObObj obj;
        char *buf = NULL;
        int64_t buf_len = 0;
        int64_t buf_pos = 0;
        if (OB_FAIL(datum->to_obj(obj, row.at(idx)->obj_meta_))) {
        } else if (OB_FAIL(obj.print_sql_literal(buf, buf_len, buf_pos, alloc, print_params))) {
        } else if (OB_FAIL(sql.append(buf, buf_pos))) {
        }
      }
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(ob_write_string(alloc, sql.string(), predicate))) {
  }
  return ret;
}

// extract the ai prompt expression (content argument) from the normalized expression
static inline int ob_ai_extract_prompt(const common::ObString &normalized,
                                       common::ObString &prompt)
{
  int ret = OB_SUCCESS;
  const char *ptr = normalized.ptr();
  const int32_t len = normalized.length();
  int32_t fn_pos = -1;
  for (int32_t i = 0; OB_SUCC(ret) && i + 12 <= len; ++i) {
    if (0 == strncasecmp(ptr + i, "ai_complete(", 12)) {
      fn_pos = i + 12;
      break;
    } else if (0 == strncasecmp(ptr + i, "ai_embed(", 9)) {
      fn_pos = i + 9;
      break;
    }
  }
  if (fn_pos < 0) {
    prompt = normalized;
  } else {
    int32_t content_start = -1;
    int32_t depth = 0;
    for (int32_t i = fn_pos; OB_SUCC(ret) && i < len; ++i) {
      const char c = ptr[i];
      if ('(' == c) {
        ++depth;
      } else if (')' == c) {
        if (0 == depth) {
          break;
        }
        --depth;
      } else if (',' == c && 0 == depth) {
        content_start = i + 1;
        break;
      }
    }
    if (content_start < 0) {
      prompt = normalized;
    } else {
      while (content_start < len && ' ' == ptr[content_start]) {
        ++content_start;
      }
      int32_t content_end = len;
      depth = 0;
      for (int32_t i = content_start; i < len; ++i) {
        const char c = ptr[i];
        if ('(' == c) {
          ++depth;
        } else if (')' == c) {
          if (0 == depth) {
            content_end = i;
            break;
          }
          --depth;
        }
      }
      while (content_end > content_start && ' ' == ptr[content_end - 1]) {
        --content_end;
      }
      prompt.assign_ptr(ptr + content_start, content_end - content_start);
    }
  }
  return ret;
}

static inline int ob_ai_exec_sql(common::sqlclient::ObISQLConnection *conn,
                                  const ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  if (OB_ISNULL(conn)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(conn->execute_write(sql.string(), affected_rows))) {
    LOG_INFO("failed to execute ai column inner sql", K(ret), K(sql.string()));
  }
  return ret;
}

struct ObAiTaskColumnMeta
{
  ObAiTaskColumnMeta()
    : column_id_(common::OB_INVALID_ID),
      max_retries_(0),
      result_type_(0),
      model_(),
      system_prompt_(),
      prompt_expr_()
  {}
  uint64_t column_id_;
  int64_t max_retries_;
  int64_t result_type_;
  common::ObString model_;
  common::ObString system_prompt_;
  common::ObString prompt_expr_;
};

static inline bool ob_ai_prompt_contains_column(const common::ObString &prompt,
                                                const common::ObString &name)
{
  bool found = false;
  if (!prompt.empty() && !name.empty()) {
    const char *p = prompt.ptr();
    const int32_t len = prompt.length();
    for (int32_t i = 0; !found && i + name.length() <= len; ++i) {
      if (0 == strncasecmp(p + i, name.ptr(), name.length())) {
        const int32_t before = i - 1;
        const int32_t after = i + name.length();
        const bool left_ok = before < 0
            || (0 == isalnum(static_cast<unsigned char>(p[before])) && '_' != p[before]);
        const bool right_ok = after >= len
            || (0 == isalnum(static_cast<unsigned char>(p[after])) && '_' != p[after]);
        found = left_ok && right_ok;
      }
    }
  }
  return found;
}

static inline bool ob_ai_column_affected(const share::schema::ObTableSchema &table,
                                         const ObAiTaskColumnMeta &meta,
                                         uint64_t column_id,
                                         const common::ObIArray<uint64_t> &updated_column_ids)
{
  bool affected = false;
  const ObRowkeyInfo &rowkey_info = table.get_rowkey_info();
  for (int64_t i = 0; !affected && i < updated_column_ids.count(); ++i) {
    const uint64_t updated_id = updated_column_ids.at(i);
    // writing the ai column itself (e.g. the background worker publishing a
    // result) must never re-invalidate the column and create a new task.
    if (updated_id == column_id) {
      continue;
    }
    for (int64_t k = 0; !affected && k < rowkey_info.get_size(); ++k) {
      uint64_t rowkey_column_id = common::OB_INVALID_ID;
      if (OB_SUCCESS == rowkey_info.get_column_id(k, rowkey_column_id)) {
        affected = (rowkey_column_id == updated_id);
      }
    }
    if (!affected) {
      const share::schema::ObColumnSchemaV2 *updated_column =
          table.get_column_schema(updated_id);
      if (OB_NOT_NULL(updated_column)) {
        affected = ob_ai_prompt_contains_column(meta.prompt_expr_,
                                                 updated_column->get_column_name_str());
      }
    }
  }
  return affected;
}

static inline int ob_ai_delete_task(common::sqlclient::ObISQLConnection *conn,
                                    uint64_t table_id,
                                    const common::ObString &rowkey_predicate)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.append("delete from oceanbase.__all_ai_column_task where table_id = "))) {
  } else if (OB_FAIL(sql.append_fmt("%lu and rowkey_predicate = ", table_id))) {
  } else if (OB_FAIL(ob_ai_quote_append(sql, rowkey_predicate))) {
  } else if (OB_FAIL(ob_ai_exec_sql(conn, sql))) {
  }
  return ret;
}

// create / invalidate / drop the ai task rows of one changed row.
// All statements run on the session transaction through the given inner connection.
static inline int ob_ai_process_row(ObExecContext &ctx,
                                    common::sqlclient::ObISQLConnection *conn,
                                    uint64_t table_id,
                                    const common::ObString &old_rowkey_predicate,
                                    const common::ObString &new_rowkey_predicate,
                                    int64_t dml_event,
                                    const common::ObIArray<uint64_t> *updated_column_ids)
{
  int ret = OB_SUCCESS;
  ObSqlCtx *sql_ctx = ctx.get_sql_ctx();
  share::schema::ObSchemaGetterGuard *guard =
      NULL == sql_ctx ? NULL : sql_ctx->schema_guard_;
  const share::schema::ObTableSchema *table = NULL;
  const share::schema::ObDatabaseSchema *db_schema = NULL;
  ObSqlString target_table;
  if (OB_ISNULL(conn)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(guard)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_INFO("invalid argument for ai column task", K(ret));
  } else if (OB_FAIL(guard->get_table_schema(table_id, table)) || OB_ISNULL(table)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(guard->get_database_schema(table->get_database_id(), db_schema)) ||
             OB_ISNULL(db_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(target_table.append("`")) ||
             OB_FAIL(target_table.append(db_schema->get_database_name_str())) ||
             OB_FAIL(target_table.append("`.`")) ||
             OB_FAIL(target_table.append(table->get_table_name_str())) ||
             OB_FAIL(target_table.append("`"))) {
    LOG_INFO("failed to build target table name", K(ret));
  }

  // the row level task rows of a removed row must not survive
  if (OB_SUCC(ret) && AI_COL_EVENT_DELETE == dml_event) {
    if (OB_FAIL(ob_ai_delete_task(conn, table_id, old_rowkey_predicate))) {
    }
  }

  common::ObSEArray<ObAiTaskColumnMeta, 8> metas;
  common::ObSEArray<bool, 8> affected_list;
  if (OB_FAIL(ret) || AI_COL_EVENT_DELETE == dml_event) {
  } else {
    LOG_INFO("ai task trace start", K(table_id), K(dml_event), K(updated_column_ids));
    ObSqlString sql;
    if (OB_FAIL(sql.append("select column_id, max_retries, ai_model_name, normalized_expr"
                           " from oceanbase.__all_ai_column where table_id = "))) {
    } else if (OB_FAIL(sql.append_fmt("%lu", table_id))) {
    } else {
      SMART_VAR(common::ObMySQLProxy::MySQLResult, result) {
        if (OB_FAIL(conn->execute_read(sql.string(), result))) {
          LOG_INFO("failed to read ai column metadata", K(ret));
        } else if (OB_ISNULL(result.get_result())) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          common::sqlclient::ObMySQLResult *res = result.get_result();
          while (OB_SUCC(ret) && OB_SUCC(res->next())) {
            ObAiTaskColumnMeta meta;
            common::ObString str;
            int64_t int_value = 0;
            if (OB_FAIL(res->get_int("column_id", int_value))) {
            } else if (FALSE_IT(meta.column_id_ = static_cast<uint64_t>(int_value))) {
            } else if (OB_FAIL(res->get_int("max_retries", meta.max_retries_))) {
            } else if (OB_FAIL(res->get_varchar("ai_model_name", str)) ||
                       OB_FAIL(ob_write_string(ctx.get_allocator(), str, meta.model_))) {
            } else if (OB_FAIL(res->get_varchar("normalized_expr", str)) ||
                       OB_FAIL(ob_write_string(ctx.get_allocator(), str, meta.system_prompt_))) {
            } else if (OB_FAIL(ob_ai_extract_prompt(meta.system_prompt_, meta.prompt_expr_))) {
            } else {
              const share::schema::ObColumnSchemaV2 *column =
                  table->get_column_schema(meta.column_id_);
              meta.result_type_ = OB_NOT_NULL(column)
                  ? static_cast<int64_t>(column->get_data_type()) : 0;
              if (OB_FAIL(metas.push_back(meta))) {
              }
            }
          }
          if (OB_ITER_END == ret) {
            ret = OB_SUCCESS;
          }
        }
      }
    }
  }
  int64_t affected_cnt = 0;
  if (OB_SUCC(ret) && !metas.empty()) {
    for (int64_t i = 0; OB_SUCC(ret) && i < metas.count(); ++i) {
      bool affected = true;
      if (AI_COL_EVENT_UPDATE == dml_event && OB_NOT_NULL(updated_column_ids)) {
        affected = ob_ai_column_affected(*table, metas.at(i), metas.at(i).column_id_,
                                         *updated_column_ids);
      }
      if (OB_FAIL(affected_list.push_back(affected))) {
      } else if (affected) {
        ++affected_cnt;
      }
    }
  }
  if (OB_SUCC(ret) && AI_COL_EVENT_DELETE != dml_event) {
    // when the rowkey changed, the task rows of the old rowkey are stale
    if (AI_COL_EVENT_UPDATE == dml_event
        && 0 != old_rowkey_predicate.compare(new_rowkey_predicate)
        && !old_rowkey_predicate.empty()
        && OB_FAIL(ob_ai_delete_task(conn, table_id, old_rowkey_predicate))) {
    } else if (affected_cnt <= 0) {
      // the update does not touch any ai column, keep the current result
    } else {
      // invalidate the ai result columns, insert does not need it(null by default)
      if (AI_COL_EVENT_UPDATE == dml_event) {
        ObSqlString sql;
        bool first = true;
        if (OB_FAIL(sql.append("update "))) {
        } else if (OB_FAIL(sql.append(target_table.string()))) {
        } else if (OB_FAIL(sql.append(" set "))) {
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < metas.count(); ++i) {
          if (!affected_list.at(i)) {
            continue;
          }
          const share::schema::ObColumnSchemaV2 *column =
              table->get_column_schema(metas.at(i).column_id_);
          if (OB_ISNULL(column)) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(sql.append(first ? "`" : ", `")) ||
                     OB_FAIL(sql.append(column->get_column_name_str())) ||
                     OB_FAIL(sql.append("` = NULL"))) {
          } else {
            first = false;
          }
        }
        if (OB_SUCC(ret) && !first) {
          if (OB_FAIL(sql.append(" where "))) {
          } else if (OB_FAIL(sql.append(new_rowkey_predicate))) {
          } else if (OB_FAIL(ob_ai_exec_sql(conn, sql))) {
          }
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(ob_ai_delete_task(conn, table_id, new_rowkey_predicate))) {
      }
      // create the task rows, one request id for the whole row
      const int64_t request_id = ob_ai_next_request_id();
      for (int64_t i = 0; OB_SUCC(ret) && i < metas.count(); ++i) {
        if (!affected_list.at(i)) {
          continue;
        }
        const ObAiTaskColumnMeta &meta = metas.at(i);
        ObSqlString system_prompt;
        const bool is_int = common::ob_is_integer_type(
            static_cast<common::ObObjType>(meta.result_type_));
        ObSqlString task_sql;
        if (is_int && OB_FAIL(system_prompt.append("Return the result as a base-10 integer. "))) {
        } else if (OB_FAIL(system_prompt.append(meta.system_prompt_))) {
        } else if (OB_FAIL(task_sql.append("insert into oceanbase.__all_ai_column_task"
                     "(table_id, column_id, rowkey_predicate, request_id, state, retry_count,"
                     " max_retries, eligible_ts, update_ts, ai_model_name, system_prompt,"
                     " prompt_expr, result_type, gmt_create, gmt_modified) values("))) {
        } else if (OB_FAIL(task_sql.append_fmt("%lu, %lu, ", table_id, meta.column_id_))) {
        } else if (OB_FAIL(ob_ai_quote_append(task_sql, new_rowkey_predicate))) {
        } else if (OB_FAIL(task_sql.append_fmt(", %ld, 0, 0, %ld, now(6), now(6), ",
                                               request_id, meta.max_retries_))) {
        } else if (OB_FAIL(ob_ai_quote_append(task_sql, meta.model_))) {
        } else if (OB_FAIL(task_sql.append(", "))) {
        } else if (OB_FAIL(ob_ai_quote_append(task_sql, system_prompt.string()))) {
        } else if (OB_FAIL(task_sql.append(", "))) {
        } else if (OB_FAIL(ob_ai_quote_append(task_sql, meta.prompt_expr_))) {
        } else if (OB_FAIL(task_sql.append_fmt(", %ld, now(6), now(6))", meta.result_type_))) {
        } else if (OB_FAIL(ob_ai_exec_sql(conn, task_sql))) {
        } else {
          LOG_INFO("ai task trace inserted", K(task_sql.string()));
        }
      }
    }
  }
  return ret;
}

}  // namespace aicolumn
}  // namespace sql
}  // namespace oceanbase
