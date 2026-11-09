# 0008 多副本配额：PostgreSQL 原子预留 + usage 异步写

- 状态：**已落地（M6/P0）**
- 日期：2026-11-09
- 决策者：项目负责人（AI 按 P0 清单执行）
- 影响范围：`src/quota/`、`src/usage/`、`src/http/`、`src/chat/chat_service.cpp`、`src/storage/migrations.cpp`、`src/config.*`、`src/main.cpp`

## 背景

M3 的配额是进程内的：多副本各自计数，实际配额被放大 N 倍；每日 token
“先查 usage 再写”并发下会超卖；usage 在流结束时同步写库，DB 抖动会丢流水。

## 决策

1. **不引入 Redis，直接用 PostgreSQL 做多副本一致性**
   - 多副本本来就必须共享 PostgreSQL（用户/会话/幂等），再引入 Redis 只为一个限流
     不值当；PG 的 UPSERT + advisory lock 足够支撑当前规模。
   - 如果以后 QPS 高到 DB 成为限流瓶颈，再把 `QuotaController` 的实现换成 Redis，
     HTTP 层不需要改。
2. **RPM 固定窗口**
   - `rate_limits(bucket, window_start, count)`，一条 `INSERT ... ON CONFLICT DO UPDATE`
     原子计数；60 秒窗口，超限 429 + `Retry-After`。
   - 固定窗口在边界允许两倍突发；需要更平滑时换滑动窗口/令牌桶。
3. **每日 token 原子预留**
   - `quota_reservations(id, user_id, tokens, expires_at)`；
   - 事务内 `pg_advisory_xact_lock(hashtext('quota:'||user_id))` 串行化同一用户，
     查 `usage_records` 当天用量 + 未过期预留，再决定是否插入本次预留；
   - 这样并发请求不能同时通过检查；进程崩溃后预留按 TTL 过期。
4. **并发流 lease**
   - `quota_stream_leases(id, user_id, expires_at)`；进入流前插入，流结束/取消删除；
     崩溃后 TTL 回收。lease TTL 用 `AIP_QUOTA_LEASE_S`（默认 900s）。
5. **释放顺序**
   - `RecordingStream` 先写 usage（异步入队也算“已接受”），再释放预留/lease；
     避免预留先删、usage 还没写的空窗。
6. **usage 异步写**
   - `AsyncUsageSink`：有界队列 + 后台线程；失败重试 3 次；队列满时同步兜底；
     析构前 flush。`AIP_USAGE_ASYNC=false` 可切回同步写，严格账实一致。
7. **HTTP 层**
   - `QuotaController` 抽象；`QuotaService`（进程内）/`PgQuotaService`（PG）；
   - `requires_daily_usage()` 让 PG 实现自己原子查询，memory 实现仍由 HTTP 层查汇总。

## 后果

**正面**

- 多副本 RPM/日 token/并发限制共享状态；两个副本实测第 3 次请求 429；
- 每日 token 不再“先查后写”超卖；
- usage 写不再阻塞 SSE 收尾；进程正常退出会尽量写完；
- 预留/lease 都有 TTL，进程崩溃不会永久占额度。

**限制 / 注意**

- usage 异步写有短暂延迟：流结束到流水落库之间，每日配额可能多放行少量请求；
  严格账实一致用 `AIP_USAGE_ASYNC=false`；
- 所有配额请求都多一次/几次 DB 操作；高 QPS 下需要评估 PG 压力或切 Redis；
- 固定窗口有边界突发；
- `quota_reservations`/`quota_stream_leases` 目前靠 TTL + 下次检查清理，
  长期需要定时任务归档；
- 进程崩溃时，已产生但未入队的 usage 仍会丢（异步队列不持久化）。
  要完全不丢，需要 outbox/WAL，成本更高，暂不做。

## 验收记录（2026-11-09）

| 用例 | 结果 |
|---|---|
| `quota_tests` | 进程内 RPM/并发/日配额/管理员绕过全部通过 |
| `usage_tests` | 异步队列 10 条全部落库；前 2 次失败后重试成功 |
| `pg_tests` | 迁移 v3；日 token 预留（60+50>100 拒绝）、并发 lease、RPM 2→第 3 次 429 通过 |
| 双副本 RPM | A 两次 200，B 第三次 429 `rate_limited` |
| 双副本日 token | A/B/A 三次 200，B 第四次 429 `quota_exceeded` |
| usage 落库 | 每次请求 usage 异步写入，日配额检查能看到前序用量 |
