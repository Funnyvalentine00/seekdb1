# FTS Issue #6 施工 Checkpoint

记录日期：2026-11-06（Asia/Shanghai）。最新核对时间：23:51。遵循仓库 `AGENT_ENGINEERING_STANDARD.md` v1.0。

## 施工摘要与范围

已修复 v4 三处 MATCH 边界错误并完成 macOS 对照。Linux 验收已执行完成，保留所有失败；本轮验收未通过，不推送、不切换2914。已将用户完整施工标准写入仓库并由 AGENTS.md 引用。

## 阶段记录

| 编号 | 时间（+08:00） | Git Commit | 修改文件/范围、功能变化 | 验证结果与已知问题 | 回滚/恢复方式 |
| --- | --- | --- | --- | --- | --- |
| P0 基线 | 2026-11-04 21:04:29（提交时间） | 7accd1ace8ee5d035b3ef8aaf426c46b1d0a595e | v3 精确源码基线，含 tokenize 等已有优化 | macOS 基线二进制与源码身份已封存 | 使用已保存 v3 二进制与其独立数据目录；禁止用旧版本打开已升级的数据目录 |
| P1 集成快照 | 2026-11-06 16:23:06 | aa475c36d（原 v4 交接） | H2/H3/H7 集成、原 v4 服务器状态 | 后续复现三处 MATCH 错误；该快照不代表验收通过 | 保留原 v4 二进制与数据；2914 切换必须满足用户条件 |
| P2 边界修正 | 2026-11-06 16:56:53 | 194150497aca9a69e395317bd6d5a64f26340a61 | ob_log_plan.cpp、ob_das_iter_utils.cpp；新增边界 SQL 复现材料 | 原三类失败复现后修复，9组边界与v3逐字节一致；1024词DaaT有额外RSS成本 | 如需回滚代码，先保留工作树差异，再使用单独 git revert 提交；运行服务则使用保存的二进制和匹配数据，当前未执行回滚 |
| P3 本机集成证据 | 2026-11-06 17:46:32 | 23ad43a61 | handoff 文档、JSON与两份源码导出补丁 | 6对macOS A/B查询收益明确；build_ik_content -2.78%未过门禁；FTS13/15同基线，AI归一化5/5，File SQL平台不支持 | 原始样本、失败、脚本与二进制保留；如需撤销文档，用独立revert，不删除实验记录 |
| P4 Linux验收执行（门禁未过） | 2026-11-06 23:51 | 产品代码194150497；规范e0eb06c82；本报告所属提交封存证据 | Linux两版构建、SHA核对、三套回归、A/A、6对A/B、9组边界、22条Tokenize补测；仅改外部验证驱动和文档 | 查询均值收益口径23.77%；build_ik_content -1.50%、tokenize_beng -2.14%、RSS +2.67%；FTS两版13/15；File SQL均1/1；AI初次4/5与5/5，独立复测均5/5。不能标记全面验收 | 两份Linux二进制及缓存保留；脚本/原始样本/失败日志已取回本地；不要直接重跑已应用补丁的resume_build.sh |
| P5 阶段交付（项目未全面验收） | 2026-11-06，本报告所属提交 | 本地文档/证据提交；产品代码不变 | LINUX_V4_ACCEPTANCE_2026-11-06.md、linux-v4-20261106/、交接/计划/Checkpoint更新 | 阶段证据可重算；2914仍是PID31133的原v4，已知边界错误尚在常驻实例中；未推送 | 文档可独立revert；原始档案长期保留。条件通过前不执行服务切换 |


## 资源与数据保护 Checkpoint

- SSH：`ssh -l xiazihan legion`，端口2222，原密钥；Linux home为`/home/xiazihan`。不修改SSH配置。
- 本轮目录：`/home/xiazihan/benchmark/fts-v4-integration-20261106`。
- 重建前已核对旧saved-binaries目录为空、原构建未成功封存；只清理本轮失效archive/build，保留所有日志、输入、patch、Git历史和其他实验。
- 复用既有依赖与项目Cargo/Rust缓存，不安装/升级系统组件。已核对系统Rust 1.75.0，项目隔离Rust/Cargo 1.98.1符合源码固定版本；clang17.0.6、CMake3.22.1。不改变系统默认。
- make、Cargo、lld均2并发；lld的`--threads=2`已在生成的link.txt验证。
- 测试：单实例；memory_budget=2G、vector_memory_limit=512M、datafile_size/maxsize=2G、log_disk_size=2G。两版配置一致，官方数据量和SQL不变。
- 只回收本轮生成且带所有权marker的临时数据目录：先终止并等待精确子进程，保存结果、复现输入和每份内部日志末512KiB，再回收。原始用户数据、其他实验目录、常驻2914目录不在回收范围。
- 每30秒核对资源；Windows D盘至少30GiB、Linux至少15GiB、可用RAM至少3GiB，否则仅停止本轮任务。恢复时D盘约133GiB、RAM约14GiB；最后D盘140.44GiB、Linux532.90GiB、RAM14.25GiB，门槛未触发。
- 连接中断后先核实了WSL重启、97%处HUP和错误的旧退出标记；修复信号/完成标记后复用缓存完成构建。原HUP红/绿最小复现保留。最终当前用户无tmux会话、2917/2918无监听、自有data目录为空，RUN共9.5GB。
- 两个File SQL macOS构建补丁维持未提交，不混入规范/证据提交。

## 测试报告与验收结论

Linux通过：9组边界每版均成功并与v3差分一致；projection/score-controls一致；Tokenize逐条22条差分一致（21成功，1条NULL错误1235相同）；16份官方报告hits全部精确。

Linux首次三套回归：37通过、5失败、0跳过；FTS两版13/15，File SQL两版1/1，AI v3 4/5、修正版5/5。AI全新实例完整复测另计10通过、0失败，无端口或金样归一化；首次失败不被覆盖。未执行Linux硬件计数器（perf工具不可用）和官方CI，未切换2914。

未通过门禁：build_ik_content -1.50%、tokenize_beng -2.14%、峰值RSS +2.67%，FTS未达到14/15。A/A发现Tokenize波动，但没有因此豁免失败。AI首次偶发失败根因未证实。

**结论：施工规范落盘完成，Linux验收执行完成，FTS项目尚未全面验收。** 候选194150497、精确基线7accd1ace。详细资源、均值/标准差/全部样本、源/二进制身份和复现路径见LINUX_V4_ACCEPTANCE_2026-11-06.md及linux-v4-20261106/。

## 下一阶段

优先隔离H2/H3/H7的RSS与build_ik_content成本，控制负载后复测Tokenize；同步厘清FTS旧fixture兼容要求和AI初始等待偶发问题。维持官方SQL/数据/金样与现有门禁，保留负例；满足条件后才按用户授权切换2914。不推送。
