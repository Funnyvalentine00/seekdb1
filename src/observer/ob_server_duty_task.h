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

#ifndef OB_SERVER_DUTY_TASK_H
#define OB_SERVER_DUTY_TASK_H
#include <stdint.h>
#include "lib/task/ob_timer.h"
#include "lib/allocator/page_arena.h"
#include "lib/string/ob_string.h"

namespace oceanbase {
namespace common {
class ObMySQLTransaction;
}
namespace observer {

struct AiColumnTaskItem
{
  AiColumnTaskItem()
    : table_id_(0),
      column_id_(0),
      request_id_(-1),
      retry_count_(0),
      max_retries_(0),
      result_type_(0),
      processed_(false)
  {}
  uint64_t table_id_;
  uint64_t column_id_;
  int64_t request_id_;
  int64_t retry_count_;
  int64_t max_retries_;
  int64_t result_type_;
  common::ObString rowkey_predicate_;
  common::ObString model_;
  common::ObString system_prompt_;
  common::ObString prompt_expr_;
  bool processed_;
};

struct AiColumnMeta
{
  AiColumnMeta() : column_id_(0), column_name_() {}
  uint64_t column_id_;
  common::ObString column_name_;
};

class ObServerDutyTask
    : private common::ObTimerTask
{
  static constexpr int64_t SCHEDULE_PERIOD = 10 * 1000L * 1000L;
public:
  int schedule(common::ObTimer &timer);
  ObServerDutyTask();
private:
  void runTimerTask() override;
  void update_runtime_settings();

private:
  // Apply context-memory limits from the runtime configuration.
  int update_ctx_memory_throttle();
  common::ObArenaAllocator allocator_;
};

class ObSqlMemoryTimerTask : private common::ObTimerTask
{
public:
  int schedule(common::ObTimer &timer);
private:
  void runTimerTask() override;
private:
  static constexpr int64_t SCHEDULE_PERIOD = 3 * 1000L * 1000L;
};

// Consumes oceanbase.__all_ai_column_task rows in background: calls the
// configured ai model, publishes the grouped result, retries failed tasks.
class ObAiColumnWorkerTask : private common::ObTimerTask
{
public:
  ObAiColumnWorkerTask();
  int schedule(common::ObTimer &timer);
private:
  void runTimerTask() override;
  int do_work();
  int load_pending_tasks(common::ObIArray<AiColumnTaskItem> &tasks);
  int resolve_table_name(const uint64_t table_id,
                         common::ObString &db_name,
                         common::ObString &table_name,
                         bool &found);
  int load_column_metas(const uint64_t table_id,
                        common::ObIArray<AiColumnMeta> &metas);
  const AiColumnMeta *find_column_meta(const common::ObIArray<AiColumnMeta> &metas,
                                       const uint64_t column_id);
  int eval_prompt(const common::ObString &db_name,
                  const common::ObString &table_name,
                  const common::ObString &rowkey_predicate,
                  const common::ObString &prompt_expr,
                  common::ObString &prompt);
  int call_model(const AiColumnTaskItem &item,
                 const common::ObString &prompt,
                 common::ObString &value);
  int finish_transaction(common::ObMySQLTransaction &trans);
  int delete_group_tasks(common::ObMySQLTransaction &trans,
                         const uint64_t table_id,
                         const int64_t request_id,
                         const common::ObString &rowkey_predicate,
                         const uint64_t column_id,
                         const bool one_column);
  int publish_group(const uint64_t table_id,
                    const int64_t request_id,
                    const common::ObString &db_name,
                    const common::ObString &table_name,
                    const common::ObString &rowkey_predicate,
                    const common::ObIArray<AiColumnTaskItem *> &group,
                    const common::ObIArray<AiColumnMeta> &metas,
                    const common::ObIArray<common::ObString> &values,
                    const common::ObIArray<int> &states);
  int retry_failed_group(const uint64_t table_id,
                         const int64_t request_id,
                         const common::ObString &rowkey_predicate,
                         const common::ObIArray<AiColumnTaskItem *> &group,
                         const common::ObIArray<int> &states);
  int process_group(const common::ObIArray<AiColumnTaskItem *> &group);
private:
  static constexpr int64_t SCHEDULE_PERIOD = 100 * 1000L;
  common::ObArenaAllocator allocator_;
};

}  // observer
}  // oceanbase

#endif /* OB_SERVER_DUTY_TASK_H */
