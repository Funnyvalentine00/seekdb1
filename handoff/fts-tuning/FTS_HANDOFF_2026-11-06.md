# FTS Issue #6 本地交接说明（2026-11-06）

> **最新承接状态（按用户要求暂停，等待新命令；仅本地保存，未推送）**：本地已增加 `194150497`，修复原 v4 的三处 MATCH 边界错误。查询收益经6对A/B确认，但build_ik_content仍未过1%门禁，File SQL待Linux；因此2914仍为原PID31133，尚未按条件切换。详情见 [V4_INTEGRATION_ACCEPTANCE_2026-11-06.md](V4_INTEGRATION_ACCEPTANCE_2026-11-06.md)。以下1–5节是此前交接快照。

本文件用于交接给其他 agent。**所有分支均未推送**；除已注明的 H3 PR #13 外，没有远端动作。

## 1. 保留的优化

| 优化 | 分支 | 代码提交 | 效果（本机 macOS A/B） | 状态 |
|---|---|---|---|---|
| H2 DaaT 批量投影 | `feat/fts-daat-batch-projection` | `9fa0416e5`、`f8819ff6d` | 指令 −5.36/−4.91/−4.62%（cn/beng/mixed）；墙钟 +4.08/+3.52/+4.75%；Linux 节点 +5.87/+5.15/+6.06% | 已验证，fts_index 14/15 |
| H3 命中专用多列 MATCH | `feat/fts-skip-match-score` | `853ccd8ed` | 墙钟 +19.50/+19.93/+20.42%，query 类均 +14.87% | 已验证，fts_index 14/15；PR #13（head `42cb1308e`）已推送待 CI |
| H7 稀疏检索热循环裸数组访问 | `feat/fts-merge-raw-access` | `1e57459dfcec` | 墙钟 +12.19/+9.87/+10.16/+10.28%，类均 +10.62%；稳态指令 −15.50（beng）/−15.05（mixed） | 已验证，hits/差分一致，fts_index 14/15；证据 `a67744d91` |

H7 详情见 `handoff/fts-tuning/RAW_ARRAY_ACCESS_2026-11-06.md` 与 `raw-array-access-evidence.json`。

## 2. 集成分支（未验证）

`feat/fts-opt-v4`，基于 `feat/fts-opt-v3`（`7accd1ace`，fdb8363 基线，含其他 agent 的 `eeb01dd70` 与 `7accd1ace`）+ cherry-pick：

- `e2c867c23` H2 阶段 1
- `bbfe827ee` H2 阶段 2
- `1075e01d5` H3
- `a5c1c3822` H7（与 `eeb01dd70` 的重叠已手工合并，`fill_merge_heap` 采用对方的缓存指针写法）

**重要状态**：

- 该分支在 macOS 构建前必须应用 `fts-tuning-kit/local-macos-build.patch`（file_sql 的 `from_chars` → `fast_float`）；当前工作树已应用、**未提交**，交接时请勿把它提交进分支。
- 已构建候选二进制 `~/seekdb-work/saved-binaries/seekdb-fts-opt-v4-20261106`，SHA256 `f9716d9fe03eadc287a66c9a342bb153a9f23c286a1499abb9489d394cf3269d`。
- **尚未在该基线上做语义校验、A/B 与三套回归**：`hits`/差分/指令/墙钟/`fts_index`/`file_sql`/`ai_column` 全部待跑（脚本在 `~/seekdb-work/results/fts-opt-v4-20261106/`，其中 `run_suite_v4.sh` 已修正 file_sql 需要的 `tools/deploy` cwd）。
- 数据目录 `~/seekdb-work/data/h2-clean-prof-20261106` 已被 v4 首次启动触发**升级/回放**，master 基线旧二进制可能无法再打开它；且 fdb8363 版 v4 二进制在该目录上启动失败（`server_start 12/14`, errcode −5044），**不要再复用该目录**。
- 已改用全新目录 `~/seekdb-work/data/fts-v4-20261106` 启动 v4 实例并保持运行：`127.0.0.1:2914`（PID 31133，2026-11-06 16:24 起，root 空密码）。该实例目前是空库，未加载 `fts_large_bench` 数据；跑压测前需用官方脚本的加载阶段（SKIP_LOAD=0）或手工建表灌数。
- `~/seekdb-work/seekdb1` 工作树（`feat/fts-opt-v3`）属于其他 agent，**不要修改**；其中 file_sql 两个文件有未提交改动。

## 3. 尝试过且未保留的方向

| 方向 | 结论 | 证据 |
|---|---|---|
| H1 IK 一次字符分类 | 负例，已回滚 | `~/seekdb-work/results/` 与 `feat/fts-ik-classify-once` 历史 |
| DaaT 整数比较器 | query_limit 指令 −1.48%，回滚 | `DAAT_INTCMP_NEGATIVE_2026-11-06.md` |
| BMW COUNT-over-limit 改写 | query_limit 指令 +52.3%、墙钟 +35.4%，回滚 | `BMW_COUNT_LIMIT_NEGATIVE_2026-11-06.md` |
| Storage 字符解码 stride | beng/mixed 仅 +0.44/+0.50%，未达认证阈值 | `STORAGE_DECODE_NEGATIVE_2026-11-06.md`（分支 `feat/fts-storage-negative`，`1f5350a46`） |
| Storage 内联小拷贝（≤16B switch） | beng/mixed 指令稳定 −1.13/−1.07%，回滚 | 同上 |
| Tokenize H4/H6 归因 | 成本 <1%，停止 | 历史实验记录 |
| `file_sql` / `ai_column` 回归 | master 基线缺 fdb8363 的 AI/File SQL 代码，无法通过；需在 fdb8363 基线上跑 | 本日 `h7` 结果日志 |

## 4. 关键二进制

| 名称 | SHA256 |
|---|---|
| `seekdb-h3-skipscore-20261106`（H3 基线） | `3edc082d5ff431a1d18da1f3391b561ca7a7b8583c51476313f5a870447d68b7` |
| `seekdb-h7-raw-access-20261106`（master 线 H7） | `79895a3b292486c6b056bd1e15a4f1b5e053d4a69e4c364b571653081864816d` |
| `seekdb-fts-opt-v4-20261106`（fdb8363 合并候选） | `f9716d9fe03eadc287a66c9a342bb153a9f23c286a1499abb9489d394cf3269d` |

## 5. 建议的下一步

1. 在 `feat/fts-opt-v4` 上跑四条 hits 与两条差分（`~/seekdb-work/results/fts-opt-v4-20261106/semantics.sh`）。
2. 构建干净的 `feat/fts-opt-v3` 基线二进制（另建 worktree 或本地分支），做 3 对指令/墙钟 A/B，确认 H2/H3/H7 增量在 fdb8363 基线上成立。
3. 跑 `fts_index`、`file_sql`、`ai_column` 三套回归（`run_suite_v4.sh`，ai_column 需 mock 服务）。
4. 全部通过后再决定推送与 PR；**严禁合入 `feat/seekdb-all`，严禁与 `amd-cagra-submit` 互操作**。
