# V5 next steps

当前恢复位置：P0保护点已建立；AA-V5-01四次校准运行中（tmux fts-v5-aa）；E01尚未修改源码。先核对tmux/退出标记/新结果tag，禁止重复启动。原节点RUN /home/xiazihan/benchmark/fts-v4-integration-20261106，既有fixed/v3二进制保持。

结束必须生成独立审查交接报告，包含相关源码、最小复现、profile、失败记录和待裁决问题；不自行切换模型。

AA-V5-01已完成；AA-V5-CONTROLLED-01运行中（tmux fts-v5-caa），新tag v5-caa*。没有编译同时运行。

当前：AA与CPU-AA均结束，基线e01-before-check.json已生成；E01产品源仅本轮cpp补丁未提交，节点构建tmux fts-v5-e01-build。下步先检查v5/e01-build.complete.json+SHA，再候选check_candidate_v2.py e01 e01-after（另日志），通过差分后最多3对AB。不要重跑build_candidate.py（源已应用补丁）。

E01三对已完成并判不晋升。用户提前停止条件4已触发，新实验停止。本地E01源码已精确恢复；下一步检查远端恢复和最终reference复核（固定原9544...二进制），然后封存报告/独立审查交接。不得启动E02，不得追加E01正式AB。

## 最终交付 / STOP

新实验停止；E01已回退且全部数据保留；最终参考验证完成。待文档和证据提交后本任务结束，不再启动新实验、构建或基准。当前任务剩余仅工件校验、提交和最终报告。后续模型先读INDEPENDENT_REVIEW_HANDOFF.md、V4_STATE.md、BUDGET.json和FINAL_STATE.json，不重建整个聊天，不直接重放一次性驱动。
