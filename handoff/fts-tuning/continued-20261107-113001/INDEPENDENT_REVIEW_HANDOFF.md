# 长查询分配独立审查交接

- 封存参考 Linux SHA256 `9544ca38c4d6cc336adb079f362eff1fa220739d58ce70bc2d0f1d948d5ade32`。源参考 `194150497`。原有 V4 优化仍由该版本承载。
- 最小复现使用 `fixture.json.gz` 中的 `tokens_1024` 夹具。用同一 prepared 语句按短→长→短顺序执行。原始 GDB 事件和 SQL 输出在 `allocation-short-first.tar.gz`。字面量与长参数先执行对照在 `allocation-followup.tar.gz`。
- 重点看 `src/storage/retrieval/ob_text_retrieval_token_iter.cpp` 中 init、save_docids、advance_to、get_curr_id、reset。每个 scoreless 词项仍持有 B 个 double 和 B 个 doc-id 元素。GDB 没有调用被调试程序的成员函数。
- 复算器将短→长→短的首尾单词项与中间 1024 个实例分开，验证中间只有一个 doc-id 缓冲触发非空填充。应审查可能绕开该观察点的存取路径，再决定延迟分配方案。
- 原生 macOS 指令与 WSL 连接方式两种新校准都未通过。结论只适用于本轮窗口。暂不把精确的分配计数当作正式性能验收。
- 第一次 GDB 测试的 tiny partition 夹具因已有同名库提前退出。第二次使用新的独占库，旧日志保留。三次 GDB 实例均正常关闭。
- 计划门禁下没有本轮产品 diff 需要审查。这里提供后续独立审查所需的最小复现和证据。
