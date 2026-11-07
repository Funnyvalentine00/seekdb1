# 公共文件与合并责任

唯一实现方：另一个agent（#2/#6）。本聊天只审查，不再在相同产品文件施工。
这是用户新分工，不再沿用之前“我做CSV/JSONL/DESCRIBE”的拆分。

| 文件或目录 | 涉及任务 | 处理边界 |
| --- | --- | --- |
| sql_parser_mysql_mode.y/.l及生成文件 | File SQL、AI列 | 各任务分支独立记录，队长串行合入；不直接覆盖另一分支 |
| sql_source_inventory.bzl、sql_header_inventory.bzl | File SQL、AI列 | 合并双方新增条目，核查注册完整，不以一方版本覆盖另一方 |
| ob_system_variable_init.json及生成元数据/factory | File SQL、AI列 | 保留双方变量并核查ID；记录生成命令及其输出身份 |
| query/api/query/parser/ob_item_type.h | File SQL等SQL功能 | 显式列出新增AST标签，防止遗漏COPY枚举或冲突 |
| ob_static_engine_cg.cpp、ob_json_table_op.* | File SQL接线 | 必要接线已有用户授权；仍交队长审查范围及回归 |
| ob_select_into_op.*、ob_log_plan.cpp、ob_log_select_into.h | COPY接线 | 同上；不得通过修改传统SELECT INTO语义制造通过 |
| ob_sql.cpp、ob_sql_context.h、ob_transform_utils.cpp、printer | File SQL计划/语义接线 | 同上；当前DESCRIBE安全/类型问题见REVIEW.md |
| storage/fts、share/text_analysis、TOKENIZE及指定DAS路径 | #6 FTS | 仅实现方优化；不要改评分脚本、SQL、数据或金样 |
| handoff/file-sql、handoff/fts-tuning | 实现方交接 | 实现方维护；本聊天复制只读证据，不覆盖原交接目录 |
| coordination/issue-1/20261101 | 本聊天协调 | 本聊天独立维护，不作为功能实现或正式比赛成绩 |

## 合并顺序

issue#1规定FTS→File SQL→AI列。当前没有执行任何合并。
若FTS选择原版，记录“无新增合格优化”即可，不把历史候选当作必合提交。
File SQL提交须包含所需公共接线、完整验收和八节交接后再交队长处理。
AI列及生态题保持原负责人，未授权本聊天接管。

## 环境占用

- 实现方：/opt/codex-seekdb-20261030及build_release，2882新实例。
- 本聊天：Windows协调资料；不向上述native目录同步、构建或切换二进制。
- 原2881数据、历史保护分支、CSV/JSONL存档和所有失败记录继续保留。
- 本轮不建立定时监控，不运行新的SQL/性能测试，不修改对方正在生成的日志。

## 共享构建历史不能追认为干净版本

本聊天此前的describe-first同步清单实际包含13个文件，不只是Windows上
三个未提交产品文件；同步脚本曾对所有登记路径进行比较，而另一任务已在
同一native树开发。原始同步前备份、变更清单和失败编译日志均保留。
这一事实使此前混合构建无法归属于单一可靠提交；不能只补一个枚举就追认
该轮测试身份。实现方后续需在自洽、冻结的最终版本上形成自己的验证记录。
