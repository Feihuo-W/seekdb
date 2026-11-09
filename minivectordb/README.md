# MiniVectorDB (2026-competition-round2)

单机、单进程、单客户端的轻量向量数据库，按加赛题一实现：基础 SQL、float32 向量精确
TopK、持久化、事务与崩溃恢复。存储、执行器、事务与索引均为自行实现，不依赖现成数据库
或向量索引库。

## 构建与运行

```bash
# 本地构建
cmake -S minivectordb -B minivectordb/build -DCMAKE_BUILD_TYPE=Release
cmake --build minivectordb/build -j

# 运行（前台，stdin/stdout 协议；数据目录默认 /data）
./minivectordb/build/minivectordb --data /data

# 容器
docker build -t minivectordb .
docker run --rm -i -v minivectordb-data:/data --cpus=2 --memory=2g minivectordb
```

- 语言：C++17，无第三方运行时依赖；`Dockerfile` 在镜像内从源码构建。
- 数据目录：`/data`（可用 `--data <dir>` 或 `MINIVECTORDB_DATA` 覆盖）。
- 日志与就绪信息只写 stderr；stdout 只输出评测结果。
- 正常关闭：stdin EOF、`--restart` 指令或 SIGTERM/SIGINT，均丢弃未提交事务。

## 输入输出协议

每一行输入是一条完整 SQL（末尾分号可有可无；一行多条以 `;` 分隔时逐条执行）。
每条 SQL 输出一行紧凑 JSON：

| 语句 | 输出 |
| --- | --- |
| 查询 | `{"columns":[...],"rows":[[...],...]}` |
| INSERT/UPDATE/DELETE | `{"affected_rows":N}` |
| CREATE TABLE / CREATE VECTOR INDEX / BEGIN / COMMIT / ROLLBACK | `{}` |
| 失败 | `{"error":"<code>"}` |

驱动指令行以 `--` 开头、不产生输出：

- `--restart`：回滚未提交事务并退出，由驱动用同一数据目录重新启动；
- `--mode exact|ann`：切换向量检索模式（`ann` 当前以精确扫描实现，召回率 1.0）。

错误码：`syntax_error`、`unsupported_sql`、`duplicate_key`、`dimension_mismatch`、
`invalid_vector`、`value_out_of_range`、`value_too_long`、`type_mismatch`、
`unknown_table`、`unknown_column`、`table_exists`、`duplicate_column`、
`primary_key_required`、`missing_column_value`、`column_count_mismatch`、
`transaction_state_error`、`io_error`。

## SQL 支持范围

```sql
CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(3));
INSERT INTO docs VALUES (1, '甲', '[0,0]'), (2, '乙', '[3,4]');
SELECT id, category FROM docs WHERE id >= 1 AND category = '甲';
UPDATE docs SET category = '丙' WHERE id = 1;
DELETE FROM docs WHERE id = 2;
SELECT id, L2_DISTANCE(embedding, '[0,0]') AS distance
  FROM docs WHERE category = '甲' ORDER BY distance LIMIT 10;
CREATE VECTOR INDEX docs_vec ON docs(embedding);
BEGIN; UPDATE docs SET category = '丁' WHERE id = 1; ROLLBACK;
```

- 表：一个 BIGINT 主键，每表最多一个 VECTOR(n) 列；`VARCHAR(n)` 按 Unicode 码点数限长，
  比较按码点顺序（UTF-8 字节序一致）。
- 查询：`SELECT *` 或普通列投影、`= < <= > >=` 与 `AND`、多列 `ORDER BY [ASC|DESC]`、
  `LIMIT`；过滤在 TopK 之前生效，距离并列按主键升序。
- 向量：存储为 float32；拒绝 NaN、Infinity、float32 溢出和维度不符；`L2_DISTANCE`
  返回欧氏距离。
- 不要求（返回 `unsupported_sql`）：NULL、JOIN、聚合、子查询、ALTER TABLE、外键、
  嵌套事务、主键更新。

## 设计与实现

- 记录布局（`/data/wal.log`、`/data/snapshot.dat`）：自描述记录
  `magic|len|crc32|type|txn` + payload；PUT 记录携带表 id、主键和整行编码
  （BIGINT 8B、VARCHAR 长度前缀、VECTOR 512/4B×dim）。文件以 mmap 访问，主键到偏移的
  映射常驻内存（约 40B/行），行数据按需读取，扫描不整表载入内存。
- 事务：单显式事务；修改先写入内存 overlay（读己之写），COMMIT 时连续追加
  BEGIN/记录/COMMIT 并 `fdatasync`，成功后才对外返回；ROLLBACK 与语句失败只丢弃
  overlay。自动提交等价于单语句事务。
- 恢复：启动时先加载 snapshot，再顺序重放 WAL，仅应用带 COMMIT 的事务；遇到
  CRC/长度不完整的尾部记录即截断逻辑末端，因此恢复途中再次被杀可重复恢复。
  读取阶段不写文件，恢复幂等。
- 压缩：WAL 超过 128 MiB 或正常关闭且超过 16 MiB 时，把当前可见行重写为
  `snapshot.tmp`，`fsync` 后原子 `rename`，再截断并收缩 WAL 文件（回到 16 MiB 空白
  映射）；崩溃发生在任一窗口都可由“快照 + 完整 WAL 重放”得到一致状态。
- 向量检索：精确扫描 + AVX2/FMA 距离计算；TopK 先用 float32 距离保留 `K + max(3K,64)`
  个候选，再用 double 复核距离、按主键打破并列并截断到 K。`ann` 模式当前同样返回精确
  结果（Recall@K=1.0）。索引创建信息随 schema 持久化，增删改始终作用于当前可见记录。

## 已知限制

- 未实现真正的近似索引（ANN 用精确扫描替代），召回率满足要求，但大数据查询成本为
  线性扫描。
- 单连接、单显式事务；不支持 NULL/JOIN/聚合/子查询/ALTER/主键更新。
- 主键到偏移的映射常驻内存，收益是随机点查与扫描寻址快，代价是行数极大时映射本身
  仍占用内存。

## 本地自测

```bash
python3 minivectordb/tools/run_cases.py            # 运行 mysqltest 公开样例
python3 minivectordb/tools/run_cases.py --filter vector
python3 minivectordb/tools/fuzz_diff.py --seeds 10 --steps 200   # 与独立参考模型对拍
python3 minivectordb/tools/fuzz_diff.py --crash --seeds 10 --steps 200  # 随机强杀（含恢复期）
```
