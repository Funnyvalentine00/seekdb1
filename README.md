# MiniVectorDB

从零实现的单机轻量向量数据库，用于 2026 第六届 OceanBase 数据库大赛加赛。
仅使用 C++17 标准库与 POSIX 系统调用，不依赖任何现成存储引擎或向量索引库。

## 构建与运行

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/minivectordb --data-dir=/data
```

进程从 stdin 逐条读取 SQL（以 `;` 结束，可跨行），向 stdout 输出一行 JSON：

- 查询：`{"columns":[...],"rows":[[...],...]}`
- 增删改：`{"affected_rows":N}`
- 其他成功语句：`{}`
- 错误：`{"error":"<NAME>","message":"..."}`

日志只写 stderr。收到 EOF / `exit` / `quit` 时回滚未提交事务并关闭。

## 支持的 SQL 子集

- `CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(n), v VECTOR(d))`
- `CREATE VECTOR INDEX idx ON t(v)` / `DROP INDEX idx ON t`
- `INSERT INTO t [(cols)] VALUES (...), (...)`
- `UPDATE t SET col = value [, ...] [WHERE ...]`
- `DELETE FROM t [WHERE ...]`
- `SELECT * | expr, ... FROM t [WHERE ...] [ORDER BY expr [ASC|DESC], ...] [LIMIT n]`
- `L2_DISTANCE(vector_col, '[x,y,...]')`，返回欧氏距离
- `BEGIN` / `COMMIT` / `ROLLBACK`（单显式事务，读己之写）
- `SET vector_mode = 'exact'|'ann'`

未列出的语句返回 `NOT_SUPPORTED`，不会静默改写语义。

## 设计要点

- **存储**：每表定长槽文件 + 追加式 WAL。槽内包含 valid 标志、主键与各列定长编码
  （VARCHAR 预留 `4*n+4` 字节，VECTOR 64 字节对齐以利于 SIMD）。
- **持久化**：每个提交把整批行镜像写入 WAL 帧（长度 + CRC32 + 结束魔数）并 `fdatasync`，
  成功响应在落盘之后发出；随后原位应用到 mmap 的槽文件。WAL 超过阈值时先 `msync` 再截断。
- **恢复**：启动时扫描槽文件重建主键目录，再按顺序重放 WAL；不完整帧按 CRC/魔数丢弃并截断，
  重放幂等，可在恢复途中再次强杀后重复恢复。
- **事务**：内存暂存按表隔离（槽位更新/删除 + 新行），提交时折叠为最少操作写入 WAL；
  回滚直接丢弃暂存。DML 语句先整体校验再落暂存，保证语句失败不留部分修改。
- **查询**：无过滤 TopK 走专用扫描（AVX2/FMA 运行时派发、标量兜底），有过滤时先按条件
  筛选候选集再计算距离，不会"先全表取 K 再过滤"。距离并列按主键升序，普通 ORDER BY 并列同理。
- **索引**：`CREATE VECTOR INDEX` 记录元数据；精确扫描始终返回精确结果，因此索引维护
  （建索引后插入/更新/删除）不影响正确性，Recall@K 恒为 1。
- **资源**：小表 mmap 常驻，大表按页流式访问；不为全表复制额外内存，峰值 RSS 约 10 MiB
  （10k×128 维基准）。

## 本地验证

```bash
python3 tests/run_cases.py mysqltest/cases --binary ./build/minivectordb   # 官方公开用例 5/5
python3 tests/run_cases.py tests/cases --binary ./build/minivectordb       # 自建语义用例 10/10
python3 tests/fuzz.py --seed 1 --rounds 60                                  # 随机差分测试
python3 tests/bench.py --binary ./build/minivectordb                        # 10k×128 性能基线
python3 tests/bigdata.py --n 200000 --dim 32                                # 大数据量/重启恢复
```

## Docker

```bash
docker build -t minivectordb .
docker run -i --rm -v "$PWD/data:/data" minivectordb
```

镜像为多阶段构建，运行层只依赖 glibc。
