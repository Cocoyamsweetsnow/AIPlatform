# 0002 会话与消息持久化到 PostgreSQL

- 状态：**已落地（M1）**
- 日期：2026-11-08
- 决策者：项目负责人（AI 按 D2 推荐项执行）
- 影响范围：`src/storage/`、`src/chat/`、`src/main.cpp`、`src/http/`、`CMakeLists.txt`、`Dockerfile`、`docker-compose.yml`

## 背景

阶段 0~2 的 `SessionStore` 是纯进程内 LRU：重启丢会话，多副本无法共享，
也无法承载用户、用量、幂等等需要事务的数据。P0 生产化清单的第一项就是持久化。

`docs/DECISIONS.md` 的 D2 仍列着三个选项（PostgreSQL / MySQL / SQLite），
但推荐项是 PostgreSQL，原因是后续 RAG 可以直接用 pgvector，不必再引入向量库。

## 考虑过的方案

| 方案 | 优点 | 代价 | 结论 |
|---|---|---|---|
| **PostgreSQL 16** | 事务、JSONB、pgvector、成熟运维；与路线图一致 | 需要多一个服务；本机原本没有 libpq 开发头 | **选择** |
| MySQL 8 | 本机已有服务和客户端头文件 | 无 pgvector，RAG 要另上向量库；本机 root 凭据不可用 | 备选，未采用 |
| SQLite | 零运维、单文件 | 单写者、多副本要换；不支持 pgvector | 仅适合演示/单机，未采用 |

环境约束的处理：

- 本机只有 `libpq.so.5`，没有 `libpq-dev`。把 PostgreSQL 16 的
  `libpq-fe.h` / `postgres_ext.h` 随仓库放到 `third_party/libpq/include`，
  CMake 直接链接 `libpq.so.5`；目标机器仍只需装 `libpq5`。
- 本地/CI 用 `postgres:16` 容器；CI 用 GitHub Actions service。
- 保留 `MemorySessionStore` 作为默认模式和紧急回退：没有数据库时
  `AIP_STORAGE=memory` 仍可启动、演示、跑单测。

## 决定

1. 新增 `chat::SessionStore` 抽象接口，两个实现：
   - `MemorySessionStore`：默认，行为与旧实现一致（LRU + 单会话上限）；
   - `PgSessionStore`：生产，会话/消息落 PostgreSQL，软删除。
2. 新增 `storage::DbPool`（libpq 连接池，Lease + Transaction）和
   `storage::run_migrations`（版本化迁移 + advisory lock）。
3. Schema 一次性建好 P0 需要的基础表：
   `users`、`api_keys`、`sessions`、`messages`、`usage_records`、`request_dedup`。
   M2~M4 只加代码，不再改这几张表的结构。
4. 配置：
   - `AIP_STORAGE=memory|postgres`（默认 `memory`）；
   - `AIP_DB_DSN`、`AIP_DB_POOL_SIZE`、`AIP_DB_CONNECT_TIMEOUT_S`；
   - `docker-compose.yml` 默认把 `api` 指向 compose 内的 `postgres`。
5. Drogon 主线把「会话读 + 用户消息写 + 建流」放到独立的 `db_workers`，
   IO 线程不再等 PostgreSQL；assistant 消息在 SSE 泵线程落库。
   httplib 回退每个连接本来就有独立线程，保持同步实现。
6. 不引入 ORM：SQL 手写、参数化；迁移 SQL 编进二进制，部署不需要额外目录。

## 后果

**正面**

- 进程重启后会话与消息仍在（`scripts/test_persistence.sh` 验证）；
- 迁移可重复执行，多实例启动用 `pg_advisory_lock` 串行化；
- 会话有 `user_id`、`title`、`model`、`created_at`、`updated_at`、`deleted_at`，
  为 M2 多租户 / M4 会话管理 API 留好字段；
- `usage_records` / `request_dedup` 已建表，M3/M4 不需要再做破坏性迁移；
- 默认 memory 模式仍然零依赖，CI 的 httplib 单元测试不需要数据库。

**负面 / 仍需注意**

- `sessions` 目前仍是 `TEXT` 主键（沿用 `s_xxx` 客户端会话 ID），不是 UUID；
- `session_count()` 在 `/healthz` 里每次 `count(*)`，十万级会话时应改成缓存/估算；
- 连接池是固定大小，DB 故障时请求会 503（`storage_unavailable`），不会自动降级到内存；
- 没有读写分离、没有 PgBouncer；多副本共享一个库，配额/幂等的原子性要在 M3/M4 再验证；
- 迁移目前是 C++ 字符串数组，迁移多了应改成 CMake 嵌入的 `.sql` 文件。

## 验收记录（2026-11-08）

| 用例 | 结果 |
|---|---|
| `ctest`（memory 单测 + pg 集成测试） | 2/2 passed |
| `scripts/test_pg.sh`（临时 PG + pg_tests） | passed（迁移重复执行、CRUD、归属、软删除） |
| `scripts/test_persistence.sh`（杀进程重启） | passed（session 4 条消息重启后仍在） |
| `demo.sh`（httplib + postgres） | 全绿 |
| Drogon 1.9.14 容器内编译 + `demo.sh` + PG | 全绿（`http=drogon`、`storage=postgres`） |
