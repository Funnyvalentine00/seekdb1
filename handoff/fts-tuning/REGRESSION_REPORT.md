# V5 regression state

历史v4：9组边界通过差分；22条Tokenize输出/错误一致（21成功、NULL同1235）；projection/score-controls一致。FTS两版13/15失败basic_dml/partitioned_simple_query；FileSQL两版1/1；AI首轮v3 4/5、fixed5/5，独立全套均5/5。首次失败不覆盖。

V5新实验尚未执行测试，不能记为通过。E01将覆盖等价词末次拼写/频率、停用词、NULL/空串、>4KB文本、中文英文混合、MATCH分数、prepared/长查询/tiny partition、并发和原三套回归。

## V5最终复核

实验前、E01候选、恢复后的最终参考三次完整执行均FTS13/15、FileSQL1/1、AI5/5。总计57通过、6个已知失败、0跳过。basic_dml和partitioned_simple_query的失败主体在三次执行中一致（仅排除文件头/时间）；无新增失败。

三次各9组边界全部成功；22条原Tokenize、12条新聚合语料、DML/6位MATCH分数、prepared及80并发请求的stdout/stderr/返回状态均一致。新增12条语料中的NULL及REPEAT生成LOB两条均为继承错误，不能称12条全部执行成功；完整差分相同。

最终只运行参考版本复核，没有第4对候选AB。官方平台CI与Linux硬件instructions/cycles未执行；逐句CPU诊断采用/proc线程schedstat，出现worker集合变化的样本单独标不可比。内存安全/TSan专用构建未执行；不能把语义差分当成完全内存安全证明。
