# FTS Issue #6 施工 Checkpoint

记录日期：2026-11-06（Asia/Shanghai）。最新核对时间：23:04。遵循仓库 `AGENT_ENGINEERING_STANDARD.md` v1.0。

## 施工摘要与范围

已修复 v4 三处 MATCH 边界错误并完成 macOS 对照。当前继续 Linux 验收，采用用户要求的保守资源配置；不推送。此次同时将用户完整施工标准写入仓库并由 AGENTS.md 引用。

## 阶段记录

| 编号 | 时间（+08:00） | Git Commit | 修改文件/范围、功能变化 | 验证结果与已知问题 | 回滚/恢复方式 |
| --- | --- | --- | --- | --- | --- |
| P0 基线 | 2026-11-04 21:04:29（提交时间） | 7accd1ace8ee5d035b3ef8aaf426c46b1d0a595e | v3 精确源码基线，含 tokenize 等已有优化 | macOS 基线二进制与源码身份已封存 | 使用已保存 v3 二进制与其独立数据目录；禁止用旧版本打开已升级的数据目录 |
| P1 集成快照 | 2026-11-06 16:23:06 | aa475c36d（原 v4 交接） | H2/H3/H7 集成、原 v4 服务器状态 | 后续复现三处 MATCH 错误；该快照不代表验收通过 | 保留原 v4 二进制与数据；2914 切换必须满足用户条件 |
| P2 边界修正 | 2026-11-06 16:56:53 | 194150497aca9a69e395317bd6d5a64f26340a61 | ob_log_plan.cpp、ob_das_iter_utils.cpp；新增边界 SQL 复现材料 | 原三类失败复现后修复，9组边界与v3逐字节一致；1024词DaaT有额外RSS成本 | 如需回滚代码，先保留工作树差异，再使用单独 git revert 提交；运行服务则使用保存的二进制和匹配数据，当前未执行回滚 |
| P3 本机集成证据 | 2026-11-06 17:46:32 | 23ad43a61 | handoff 文档、JSON与两份源码导出补丁 | 6对macOS A/B查询收益明确；build_ik_content -2.78%未过门禁；FTS13/15同基线，AI归一化5/5，File SQL平台不支持 | 原始样本、失败、脚本与二进制保留；如需撤销文档，用独立revert，不删除实验记录 |
| P4 Linux全面验收 | 2026-11-06，本轮进行中 | 产品代码仍为194150497；规范文件见本文件所属文档提交 | 恢复后低并发重新构建，源码15项SHA已匹配；串行回归、A/A、6对A/B待完成 | 最后观测基线编译96%、可用RAM12.93GiB，随后SSH断开；尚未确认基线二进制封存，也未确认Linux测试启动。不能标记P4完成 | 先重连检查tmux、build.rc、validation.rc及SHA，不重复启动已有任务；此前失败源码/缓存可由fdb对象+两份patch重建 |
| P5 最终交付 | 未完成 | — | 完整Linux证据和条件满足后的2914切换 | 未全面验收，不推送 | 保留上述检查点与所有原始证据，按用户后续指令推进 |

## 资源与数据保护 Checkpoint

- SSH：`ssh -l xiazihan legion`，端口2222，原密钥；Linux home为`/home/xiazihan`。不修改SSH配置。
- 本轮目录：`/home/xiazihan/benchmark/fts-v4-integration-20261106`。
- 重建前已核对旧saved-binaries目录为空、原构建未成功封存；只清理本轮失效archive/build，保留所有日志、输入、patch、Git历史和其他实验。
- 复用既有依赖与项目Cargo/Rust缓存，不安装/升级系统组件。系统Rust/Cargo 1.75.0是用户提供的基准；项目使用已有隔离工具链，恢复连接后继续核对实际版本，不改变系统默认。
- make、Cargo、lld均2并发；lld的`--threads=2`已在生成的link.txt验证。
- 测试：单实例；memory_budget=2G、vector_memory_limit=512M、datafile_size/maxsize=2G、log_disk_size=2G。两版配置一致，官方数据量和SQL不变。
- 只回收本轮生成且带所有权marker的临时数据目录：先终止并等待精确子进程，保存结果、复现输入和每份内部日志末512KiB，再回收。原始用户数据、其他实验目录、常驻2914目录不在回收范围。
- 每30秒核对资源；Windows D盘至少30GiB、Linux至少15GiB、可用RAM至少3GiB，否则仅停止本轮任务。恢复时D盘约133GiB、RAM约14GiB。
- tmux：`fts-v4-recovered-build`、`fts-v4-recovered-validation`。连接中断后任务状态需要核实，不能推断仍在运行或已成功。
- 两个File SQL macOS构建补丁维持未提交，不混入规范/证据提交。

## 测试报告与验收结论

通过：现有macOS边界9组及语义差分；6对A/B的12份报告hits全部精确。AI5/5为端口归一化后全输出一致，native rc单独保留。

失败：macOS build_ik_content门禁；fts_index两项与基线相同；File SQL在macOS不支持。

未执行/未确认：恢复后的Linux完整构建、三套回归、A/A与六对A/B仍不得计为通过。没有以删用例、改金样或降低标准规避失败。

结论：规范文件落盘完成；FTS项目尚未全面验收。当前产品代码候选194150497，已验证基线7accd1ace；详细原始证据见V4_INTEGRATION_ACCEPTANCE_2026-11-06.md及results目录。

## 下一阶段

恢复2222连通后，首先核对现有任务，继续完成Linux证据；如发现新失败，先复现与精确基线比较再制定最小修复。保持不推送，2914按既有“验收通过后切换”条件处理。
