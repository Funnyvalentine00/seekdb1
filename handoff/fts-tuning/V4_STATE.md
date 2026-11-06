# V4/V5 current state

CURRENT_WORKING_BASELINE=f5fe975382f4b151f7181a70d7f7e05ff9b234fd；产品194150497aca9a69e395317bd6d5a64f26340a61。原工作区feat/fts-opt-v4与两个macOS兼容修改受保护；v3工作区禁止修改。旧0b0eee6 worktree仅历史对照。

当前候选查询收益已实测，全面验收仍未通过。BEST_VALIDATED（完整门禁）尚不能指向v4；当前校准和每项实验before使用194150497的Linux fixed二进制，不恢复旧版本。组件验证与整体验收分开记录。

原始比赛5bce9e0仅历史参照。V5使用独立feat/fts-opt-v5工作树；无推送/PR。固定用户配置模型/推理，不尝试切换。

Linux六对：查询配对改善中位数30.41/30.57/28.91/5.20%；build_ik_content -1.50%、tokenize_beng -2.14%、RSS +2.67%。FTS两版13/15；File SQL1/1；AI独立复测5/5、首次基线失败保留。

## V5任务结束状态

参考产品仍194150497，原v4 HEAD仍f5fe97538。E01未晋升，本地/节点仅本轮stop_word.cpp修改已精确恢复，全部历史优化保持。原两个macOS修改SHA已复核相同；v3工作区未改动。

新实验已停止，预算8实际1。正式AB3对；所有V5数据（18个自有目录）保留，节点RUN约51G，最后D盘134.93GiB/Linux491.37GiB/RAM14.29GiB，无2920监听和残留tmux。

**二进制入口**：继续使用封存的saved-binaries/seekdb-fixed（9544ca38...ade32）。节点src/build_release/src/observer/seekdb仍是E01构建缓存产物，不能当最终参考运行；源码恢复不等于缓存二进制已重建。E01独立封存于v5/saved-binaries/seekdb-e01，仅供复验。
