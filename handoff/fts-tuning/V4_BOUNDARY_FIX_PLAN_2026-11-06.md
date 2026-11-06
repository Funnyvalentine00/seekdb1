# v4 集成边界修正与验收计划

状态：用户已授权恢复Linux验收，仍不推送。SSH用户名改为xiazihan、端口2222；资源检查与写入/fsync已恢复。采用2并发构建、单实例2G内存预算，完成每轮后回收自有数据目录。

目标：保留 H2/H3/H7 在 fdb8363/v3 底座上的收益，恢复 v3 支持的 MATCH 边界；不修改其他赛道或本机构建补丁。

- [x] 封存原 v4 SHA f9716d9f 和独立 v3 SHA 27016ae8。
- [x] 先复现：仅投影 MATCH>0、>256 tokens、tiny partition 均 v3 成功、原 v4 失败。
- [x] 只在 driving scan 开启评分省略；doc-agg child 只为需要评分的查询建立；无评分长查询走动态 DaaT。
- [x] 完整构建，v3/fixed 9 组边界逐字节一致，含255/256/257/1024词和prepared short-long-short。
- [x] 原有 projection/score-controls/tokenize 语义输出、错误信息与返回状态完全一致。
- [x] 三套回归与精确基线对照；临时fixture适配和平台限制单独报告。
- [x] 六对平衡完整 A/B、服务端 instructions/cycles、RSS 与精确 hits；build_ik_content仍未过门禁。
- [x] 修正与证据本地提交，排除 file_sql 本机构建补丁；不推送。
- [ ] Linux完整构建、三套回归与平衡A/B，确认build_ik_content门禁及File SQL。legion SSH已恢复，但Linux构建在链接阶段遇EIO，写入报EROFS，等待文件系统恢复后从可信产物重新开始。
- [ ] 根据用户明确选择，在验收后将2914切换修正版并保持运行。

资源边界：无评分长查询使用 DaaT 后内存随 tokens×batch_size 增长。测试覆盖至1024词，不能宣称任意长度的资源成本不变。
