# Query 第三单点：merge 比较器二进制 doc-id 快路径（保留）

日期：2026-11-03　｜　分支：`feat/fts-opt-v3`　｜　累计候选：fill_merge_heap + get_iter + 本单点

## 改动

`ObSRMergeCmp` 在 init 时识别 `CS_TYPE_BINARY + string` 的 doc id，`cmp()` 直接 `memcmp + 长度差`，结果归一化为 -1/0/1；null 或非 binary 仍走原 `null_first_cmp_` 回退。仅 `ob_sparse_daat_iter.{h,cpp}`，未改 `advance_to` 算法、project、BMW 拓扑。

二进制 collation 的通用比较本就等价于 `memcmp + 长度差`，null 语义保留；排序与 `equal_with_next_` 逻辑不变。

## 指令数（三次候选重复，相对基线 A/B 中位数）

| Workload | A/A 3× 门槛 | 改善 |
|---|---:|---:|
| `query_cn` | 0.738% | **+8.085%** |
| `query_beng` | 0.365% | **+11.363%** |
| `query_mixed` | 0.309% | **+11.125%** |
| `query_limit` | 0.298% | **+7.166%** |
| `tokenize_ik` | 0.194% | +0.264% |
| `tokenize_beng` | 1.835% | +0.298% |

hits 全对：`8001 / 11000 / 7332 / 20`；FTS 回归 13/15（与基线相同的两个已知失败）。

## 墙钟（三对交替 + 一对完整负载，四份报告中位数）

| 指标 | baseline | candidate | 改善 |
|---|---:|---:|---:|
| `query_cn_avg_ms` | 0.8388 | 0.7900 | **+5.83%** |
| `query_beng_avg_ms` | 1.2294 | 1.1391 | **+7.34%** |
| `query_mixed_avg_ms` | 0.9366 | 0.8698 | **+7.13%** |
| `query_limit_avg_ms` | 1.0547 | 1.0076 | **+4.46%** |
| `tokenize_ik_avg_ms` | 0.0746 | 0.0751 | −0.67% |
| `tokenize_beng_avg_ms` | 0.0484 | 0.0485 | −0.21% |
| `build_ik_all_sec` | 2.353 | 2.352 | +0.04% |
| `build_ik_content_sec` | 1.741 | 1.744 | −0.17% |
| `build_beng_en_sec` | 1.032 | 1.035 | −0.29% |

- 三对交替中 Query 四项每一对都为正（`+0.82%~+8.09%`）。
- `build_beng_en_sec` 单对显示 −10.70%，因该对的 baseline 出现 0.935s 异常快样本；同二进制基线完整报告中位数为 1.032s，A/A 范围 9.8%，候选 1.035s 相对历史基线为 −0.29%。
- RSS（三对）：baseline 318,091 KB，candidate 315,472 KB（**−0.83%**）。

## 结论

官方九项指标无超过 1% 的退化；Query 指令与墙钟均显著改善，hits 与回归无新增失败。本单点保留，与 `fill_merge_heap`、`get_iter` 一起构成当前累计候选（未提交、未推送）。

## 产物

- 二进制：`~/seekdb-work/saved-binaries/seekdb-qopt-bincmp`
- SHA256：`89b018a03d229dd0e748104f7c094028e5f6c1d9fe6f81a6fce8aa0c4fc1ff06`
- 指令证据：[`qopt-bincmp-instr.json`](qopt-bincmp-instr.json)
- 墙钟证据：[`qopt-bincmp-wall-evidence.json`](qopt-bincmp-wall-evidence.json)
- 合并九指标证据：[`qopt-bincmp-combined-evidence.json`](qopt-bincmp-combined-evidence.json)
- A/A 噪声证据：[`wall-aa-noise-evidence.json`](wall-aa-noise-evidence.json)

## 限制

- 本机 macOS 测量，官方 Linux CI 未运行。
- `select1` 对照四报告中位数 −1.00%，属控制项噪声（A/A 范围 5.81%），不计入官方九指标。
- build 三项来自单对完整负载，需以官方 CI 复核。

## 工作区

`ob_sparse_daat_iter.{h,cpp}`、`ob_sparse_bmw_iter.h` 三处候选（未提交）；两处 File SQL 改动未触碰。2883 测试实例已停止。
