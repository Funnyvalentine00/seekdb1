# V5 Experiment Log

## P0 — state protection

- Parent=f5fe975382f4b151f7181a70d7f7e05ff9b234fd，产品194150497；本地tag checkpoint/fts-v4-before-v5-20261107。
- 隔离feat/fts-opt-v5；原两个macOS改动SHA及二进制diff封存在v5-20261107/protected-state.json/protected-macos.patch，原工作树不动。
- 历史H2/H7收益、H3三处修正、parser/map/hash/bsearch/IK/BMW/storage负例已经接管；不重复已否定的机制。
- 当前步骤：AA-V5-01待执行；E01方案和用例先准备，禁止与基准并行编译。

## E01 — word aggregation one lookup

- Parent=f5fe975382f4b151f7181a70d7f7e05ff9b234fd（产品194150497）；BEST_VALIDATED完整门禁尚未建立。
- Hypothesis：当前groupby仍get_refactored+set_refactored；历史v2有单查找收益，当前版本需独立再验证。仅移植带末次拼写修正的updater，不批量cherry-pick。
- File=src/storage/fts/ob_fts_stop_word.cpp。风险=collation等价词拼写与词频、arena归属；先用基线建立重复/重音/全半角/长文本/分数/并发差分预期。
- Expected：减少hash/桶锁路径；幅度未知，不以调用次数声明收益。
- Rollback：只恢复本轮单文件的保存原始字节并SHA验证；不reset、不动既有优化/其他工作区。
- Build/Test/Benchmark/Memory/Decision/Commit：PENDING。
