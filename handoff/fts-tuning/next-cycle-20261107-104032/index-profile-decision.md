# 建索引输出表达式候选判定

保留二进制 `9544ca38…ade32` 上，暖 `build_ik_content` 诊断耗时 4.0738 s。同步采样共 329 个 `cpu-clock:u` 用户态样本，近似事件计数 3.39 s。进程 CPU 时间和 DDL 端到端时间有不同口径，不能相减来推导等待比例。

`ObTableScanOp::fill_generated_fts_cols` 的累计样本占比为 0.30%。`fetch_next_fts_index_rows` 的 28.27% 包含分词及取行等更多工作，不能归给表达式查找。符号表未单独显示 `get_output_fts_col_expr_by_type`，内联解码关闭。采样仅支持当前证据不足的结论，不能建立严格的端到端收益上界。P3-B 本轮跳过。

直接硬件 instructions 事件显示 `<not supported>`。使用既有软件采样工具完成诊断。完整采样、解码命令、输出和失败记录已归档。
