# v4 不利指标归因计划

> For agentic workers: use superpowers:executing-plans in this session.

**Goal:** 找到 Linux RSS、build_ik_content、tokenize_beng 不利读数的来源，决定是否需要隔离 H2/H3/H7，保持原验收门禁。
**Architecture:** 先复用封存的 v3/fixed 二进制，读取 /proc 并关联原官方脚本阶段日志；阶段定位后才选择单变量消融。未确认根因前不改产品代码。
**Tech Stack:** Python /proc 采样、官方 Bash benchmark、既有 Linux 编译缓存。
**Spec:** AGENT_ENGINEERING_STANDARD.md、LINUX_V4_ACCEPTANCE_2026-11-06.md、用户“OK批准”。

## Global Constraints

- 不推送；2914仅在全面验收通过后切换；不改其他工作树。
- 两并发构建、单实例2G预算，沿用已记录的空间和可用RAM阈值。
- 不改官方SQL、数据量、计时或金样；原始失败长期保留。
- 基线7accd1ace8ee5d035b3ef8aaf426c46b1d0a595e，候选194150497aca9a69e395317bd6d5a64f26340a61。

## Review Focus

- 启动即存在的RSS差异与查询新增内存不能混算；采集启动、build、tokenize、query阶段。
- file-backed RSS与匿名内存应分开；采集smaps_rollup。
- 同二进制/时序波动不能归因给优化；采用v3/fixed/fixed/v3顺序，保留原六对结果。
- 采样开销会干扰墙钟；诊断读数不替代正式验收。
- 中断、后台进程、旧结果覆盖：独立tag，检查进程，finally只停止并清理自有实例。

## Task 1：P0/P1 分阶段定位

- [x] 复核Git、操作系统、工具链/依赖已有记录、SSH用户、资源、任务及两份二进制SHA。
- [ ] 创建外部trace_phases.py，复用verify_linux.py的start/bench/stop；每0.2秒读取RSS/CPU/阶段，smaps_rollup每秒一次。
- [ ] 固定四次顺序v3/fixed/fixed/v3，官方参数与四条hits不变；记录CPU tick、anonymous/file-backed内存与输入SHA。
- [ ] 判断差异是否出现在检索前；建立阶段Checkpoint、原始JSON与总结。

## Task 2：按证据选择最小实验

- [ ] 如果新增匿名RSS在query阶段稳定出现，隔离H3或H2相关分配；如果差异在启动/build阶段已出现，先比较映射/异步后台阶段，避免无因果依据重编译。
- [ ] 若需代码消融，先写出单一假设和预期观测，封存反向补丁，复用缓存2并发构建；语义差分通过后做相同基准。
- [ ] 任何新候选必须重跑边界/完整语义、三套回归、平衡A/B；未过门禁不保留为可交付优化。

## Checkpoint 与回滚

P0：d57a170e5，产品候选194150497；只存在两个macOS构建补丁。Linux RUN 9.5GB，data为空，无残留tmux/数据库；D盘140.44GiB、可用RAM14.25GiB。

风险：诊断轮额外采样可能影响墙钟，故不能替代正式成绩。外部脚本失败时仅终止其子进程并保存日志。产品源码不变则无需代码回滚；若做消融，按记录的补丁恢复并核对SHA。保留计划与证据，不删除研究数据。
