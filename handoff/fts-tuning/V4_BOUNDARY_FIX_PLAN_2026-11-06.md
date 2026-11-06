# v4 集成边界修正与验收计划

状态：Linux 构建、语义/边界、三套回归、A/A、六对 A/B 和补充 Tokenize 覆盖已执行完成；验收门禁未通过，仍不推送、不切换2914。最新报告：LINUX_V4_ACCEPTANCE_2026-11-06.md。

目标：保留 H2/H3/H7 在 fdb8363/v3 底座上的收益，恢复 v3 支持的 MATCH 边界；不修改其他赛道或本机构建补丁。

- [x] 封存原 v4 SHA f9716d9f 和独立 v3 SHA 27016ae8。
- [x] 先复现：仅投影 MATCH>0、>256 tokens、tiny partition 均 v3 成功、原 v4 失败。
- [x] 只在 driving scan 开启评分省略；doc-agg child 只为需要评分的查询建立；无评分长查询走动态 DaaT。
- [x] 完整构建，v3/fixed 9 组边界逐字节一致，含255/256/257/1024词和prepared short-long-short。
- [x] 原有 projection/score-controls/tokenize 语义输出、错误信息与返回状态完全一致。
- [x] 三套回归与精确基线对照；临时fixture适配和平台限制单独报告。
- [x] 六对平衡完整 A/B、服务端 instructions/cycles、RSS 与精确 hits；build_ik_content仍未过门禁。
- [x] 修正与证据本地提交，排除 file_sql 本机构建补丁；不推送。
- [x] Linux 两版完整构建与 SHA/源码身份核对；两并发、复用缓存，无新增依赖安装。
- [x] Linux 三套回归、A/A、六对 A/B、9组边界和22条 Tokenize 差分执行完；全量样本和失败已保留。
- [ ] Linux硬门禁：build_ik_content -1.50%、tokenize_beng -2.14%、峰值RSS +2.67%；FTS两版13/15。首次AI v3 4/5，隔离复测两版5/5，偶发根因未证明。
- [ ] 根据用户明确选择，在验收后将2914切换修正版并保持运行。

资源边界：无评分长查询使用 DaaT 后内存随 tokens×batch_size 增长。测试覆盖至1024词，不能宣称任意长度的资源成本不变。

下一轮优先定位 RSS / build_ik_content 及 Tokenize 噪声；不重复构建当前两份已保存二进制，不把阶段执行完成记为项目验收完成。
