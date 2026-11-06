# DaaT 整数 doc-id 比较快路径：负结果与回滚（2026-11-06）

## 结论

不保留。以 H2 stage 2（`f8819ff6d`）为基线，整数比较快路径在 query_cn/beng/mixed 上仅 +0.13%/+0.24%/+0.31%（接近噪声），而 **query_limit −1.48%**（三轮一致），触发任一指标退化 >1% 的回滚门禁。源码已回滚，H2 仍是当前候选。

## 假设与改动

目标：`query_limit` 的 `ObTextRetrievalDaaTTokenIter::advance_to` 占 4.2% self，`ObSRMergeCmp::cmp` 占 1.8%，BIGINT doc-id 仍走 `cmp_func_` 函数指针。尝试在以下两处增加整数快路径：

- `src/storage/retrieval/ob_text_retrieval_token_iter.{h,cpp}`：`advance_to` 的比较 lambda 在非 null/ext 且类型为 `ObIntType`/`ObUInt64Type` 时直接比较 `get_int()`/`get_uint()`；保留 binary 与 `cmp_func_` 回退。
- `src/storage/retrieval/ob_sparse_daat_iter.{h,cpp}`：`ObSRMergeCmp::cmp` 同样增加 int64/uint64 直接比较；null/ext 与字符串仍走原路径。

改动仅新增 private bool 与分支，未改算法、未改输出类型。完整补丁：`results/daat-intcmp-20261106/intcmp.patch`，SHA256 `351020ebbcdbafafdb736392a919ae10811397cf51c8c00c8e9917533aea89d3`。

## 测量

服务端指令数（`proc_pid_rusage.ri_instructions`），3 轮交替 × 5 reps × 200 SQL；候选二进制 SHA256 `c2c64dfaef3a39fd8a00c44680aacca445b01c986c6f30acbdf3748944db1bd3`。

| 指标 | H2 stage 2 中位数 | 整数比较中位数 | 改善 |
|---|---:|---:|---:|
| query_cn | 19,793,959 | 19,768,584 | +0.13% |
| query_beng | 29,591,911 | 29,521,494 | +0.24% |
| query_mixed | 21,186,917 | 21,120,407 | +0.31% |
| query_limit | 22,873,366 | 23,212,251 | **−1.48%** |

`query_limit` 三轮均约 −1.48%，不是单轮噪声；其余三项的“正收益”小于同版轮间波动量级，不能定案。

## 正确性

差分 SQL 与 clean/阶段 2 对照：原 1734 行 + 多 token/长扫描 345 行，**逐字节一致**（SHA256 `539253c0…` / `72a2e26f…`）。说明回滚原因是性能而非语义；未跑 fts_index/Linux（性能门禁先失败）。

## 解释与不要重做

- 快路径把 null/ext 检查放到热路径最前，且 `ObSRMergeCmp::cmp` 现在对所有类型都先取两个 datum；对 query_limit 的高频比较，新增分支反而超过函数指针调用的收益。
- `advance_to` 的 binary 快路径已覆盖字符串；BIGINT 的 `cmp_func_` 本身已足够便宜，直接比较没有可靠收益。
- 该方向与已保留的 binary `memcmp` 快路径不同，但结论是负的；没有新机制前不要重做“整数比较快路径”或仅调整分支顺序来追 query_limit。

## 证据

- 指令 A/B JSON：`handoff/fts-tuning/daat-intcmp-negative-evidence.json`。
- 原始数据：`~/seekdb-work/results/daat-intcmp-20261106/`（`instr-runs.json`、`instr-summary.json`、`intcmp.patch`、`base_*.out`、`cand_*.out`）。
- 二进制：`~/seekdb-work/saved-binaries/seekdb-daat-intcmp-20261106`（仅复现，不采用）。
- Linux：本实验未上节点；H2 的 Linux 验证因节点 SSH 链路问题暂挂。
