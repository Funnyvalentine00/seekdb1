# V5 regression state

历史v4：9组边界通过差分；22条Tokenize输出/错误一致（21成功、NULL同1235）；projection/score-controls一致。FTS两版13/15失败basic_dml/partitioned_simple_query；FileSQL两版1/1；AI首轮v3 4/5、fixed5/5，独立全套均5/5。首次失败不覆盖。

V5新实验尚未执行测试，不能记为通过。E01将覆盖等价词末次拼写/频率、停用词、NULL/空串、>4KB文本、中文英文混合、MATCH分数、prepared/长查询/tiny partition、并发和原三套回归。
