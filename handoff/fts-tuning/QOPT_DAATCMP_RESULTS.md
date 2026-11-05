# Query 第四单点：DaaT token 迭代器二进制比较快路径（保留）

日期：2026-11-03　｜　分支：`feat/fts-opt-v3`　｜　累计四个单点

## 改动

`ObTextRetrievalDaaTTokenIter` 的 doc-id 比较器本就以 `CS_TYPE_BINARY` 初始化；`advance_to()` 内新增二进制 `memcmp + 长度差` 快路径（null 或非 string 类型回退原 `cmp_func_`）。只改比较，不改线性扫描、批加载或 project。`ob_text_retrieval_token_iter.{h,cpp}`。

## 指令数（三次候选重复，相对基线 A/B 中位数）

| Workload | A/A 3× 门槛 | 改善 |
|---|---:|---:|
| `query_cn` | 0.738% | **+8.356%** |
| `query_beng` | 0.365% | **+11.305%** |
| `query_mixed` | 0.309% | **+10.921%** |
| `query_limit` | 0.298% | **+16.040%** |
| `tokenize_ik` | 0.194% | +0.114% |
| `tokenize_beng` | 1.835% | +0.320% |

hits 全对：`8001 / 11000 / 7332 / 20`；FTS 回归 13/15（与基线相同的两个已知失败）。

## 墙钟（三对交替 + 一对完整负载，四份报告中位数）

| 指标 | baseline | candidate | 改善 |
|---|---:|---:|---:|
| `query_cn_avg_ms` | 0.8293 | 0.7807 | **+5.86%** |
| `query_beng_avg_ms` | 1.2094 | 1.1129 | **+7.98%** |
| `query_mixed_avg_ms` | 0.9264 | 0.8584 | **+7.34%** |
| `query_limit_avg_ms` | 1.0335 | 0.9166 | **+11.31%** |
| `tokenize_ik_avg_ms` | 0.0719 | 0.0723 | −0.63% |
| `tokenize_beng_avg_ms` | 0.0461 | 0.0464 | −0.54% |

`query_limit` 三对逐对改善：`+11.34% / +11.19% / +11.59%`。

## build 测量与顺序偏差

六对完整负载（4 对基线先、2 对候选先）的配对差值中位数：

| 指标 | 配对中位数 |
|---|---:|
| `build_ik_all_sec` | −0.07% |
| `build_ik_content_sec` | −0.17% |
| `build_beng_en_sec` | −0.34% |

“基线先、候选后”的 2 对出现 10~12% 的 build 差异；改为“候选先、基线后”后两者持平（0.945 vs 0.944、0.937 vs 0.940），证实是时段/顺序偏差。build 路径不被 retrieval 改动触及。六对配对中位数均在 1% 门禁内。

## RSS

三对交替：baseline 409,845 KB，candidate 392,987 KB（**−4.1%**，baseline 含一个 439 MB 离群；候选更稳定）。

## 结论

官方九项指标无超过 1% 的退化；Query 指令数 `+8.4%~16.0%`，墙钟 `+5.9%~11.3%`，hits 与回归无新增失败。保留本单点。累计四个单点：

1. `fill_merge_heap` 数组裸指针
2. BMW `get_iter` 裸指针
3. `ObSRMergeCmp` binary doc-id 快路径
4. DaaT token iter binary 比较快路径

## 产物

- 二进制：`~/seekdb-work/saved-binaries/seekdb-qopt-daatcmp`
- SHA256：`87ab49f44f3d23dc1553ce8c3ebc9110c3ef84ec5a0d15ea5da9e73b159c4f3e`
- 指令证据：[`qopt-daatcmp-instr.json`](qopt-daatcmp-instr.json)
- 墙钟证据：[`qopt-daatcmp-wall-evidence.json`](qopt-daatcmp-wall-evidence.json)
- 合并九指标证据（含六对 build 配对与反序验证）：[`qopt-daatcmp-combined-evidence.json`](qopt-daatcmp-combined-evidence.json)

## 三套回归补充（2026-11-03 晚）

- `fts_index`：13/15。`basic_dml`（测试变量 `No such row`）与 `partitioned_simple_query`（EXPLAIN bind 标号）与基线二进制逐字相同。
- `ai_column`：4/5。`ai_column_retry_strict` 在 line 108 的严格读取断言失败；用**基线二进制**在独立数据目录 `~/seekdb-work/data/ai-check` 复跑同一用例，**同样在 line 108 失败**，确认是预先存在的行为，不是本轮 FTS 改动引入。其余 4 个用例通过（`worker` 首次失败为 mock 残留 attempt 状态，重启 mock 后通过）。
- `file_sql`：macOS 无法完整执行，该实现读取逻辑为 Linux-only（非 Linux 分支返回 `OB_NOT_SUPPORTED`），需以官方 Linux CI 为准。

## 限制

- 本机 macOS 测量，官方 Linux CI 未运行。
- build 同二进制 A/A 范围约 10%，已用六对配对 + 反序控制处理；最终以 CI 为准。
- `select1` 对照为噪声项，不计入官方九指标。

## 工作区

`ob_sparse_daat_iter.{h,cpp}`、`ob_sparse_bmw_iter.h`、`ob_text_retrieval_token_iter.{h,cpp}` 候选（未提交）；两处 File SQL 改动未触碰。2883 测试实例已停止。
