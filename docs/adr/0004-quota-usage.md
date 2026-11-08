# 0004 配额、限流与用量流水

- 状态：**已落地（M3）**
- 日期：2026-11-08
- 决策者：项目负责人（AI 按 roadmap 阶段 4 实施）
- 影响范围：`src/quota/`、`src/usage/`、`src/chat/`、`src/http/`、`src/main.cpp`、`src/config.*`、`CMakeLists.txt`

## 背景

M2 之后每个请求都有 `user_id`，但还没有任何成本/滥用控制：一个用户可以在
一分钟内打满上游，也无法知道每天用了多少 token。P0 的第三项是配额、限流和用量。

## 决定

1. **进程内配额（单实例）**：
   - 每用户 RPM 使用令牌桶：容量 = `AIP_RPM_LIMIT`，按 `rpm/60` 每秒回填；
   - 每用户并发流用 `unordered_map<user_id,int>` 计数，`QuotaGuard` RAII 释放；
   - 每日 token 配额用 `usage_records` 当天的 `prompt+completion` 汇总，
     请求开始时用 `estimate_tokens(user_text) + max_tokens + 8` 预留；
   - 0 表示不限；静态管理员 / `user_id=0` 直接放行。
2. **429 契约**：
   ```json
   { "error": { "type": "rate_limited", "message": "请求过于频繁，请稍后重试" } }
   ```
   同时返回 `Retry-After`（秒）；每日配额用 `type=quota_exceeded`，`Retry-After: 3600`。
3. **用量流水**：
   - `RecordingStream` 在完成/取消时写 `usage_records`：user_id、session_id、
     request_id（M4）、model、prompt/completion tokens、latency_ms、ttft_ms；
   - 上游返回 usage 就用上游值；否则用 `chat::estimate_tokens` 估算；
   - 写流水失败只记日志，不影响已经发给客户端的流；
   - `PgUsageSink::summary_today` 供配额预检查。
4. **源码结构**：
   - `quota::QuotaService` / `quota::QuotaGuard`：纯内存，不依赖框架；
   - `usage::UsageSink` 接口 + `usage::PgUsageSink`；
   - `ChatService` 只接收 `UsageSink*` 和 `QuotaGuard`，HTTP 层负责先检查配额。
5. **Drogon / httplib**：
   - Drogon 在 `db_workers` 里查当日用量并检查配额，不阻塞 IO 线程；
   - httplib 回退在连接线程同步执行（该后端本来就是每连接一线程）。

## 后果

**正面**

- 单实例内可以精确限制 RPM/并发，429 + Retry-After 对客户端友好；
- 每次完成的请求都有用量流水，后续计费/报表可以直接聚合；
- `QuotaGuard` 绑定到流对象，客户端断开/上游错误时也会释放并发名额；
- 管理员/匿名本地模式不受限，演示不受影响。

**负面 / 限制**

- **限流是进程内的**：多副本时每个副本各自计数，实际配额会被放大 N 倍；
  上多副本前必须切 Redis 令牌桶或 Nginx/网关限流；
- 每日 token 是“先查后写”，并发请求可能同时通过检查，存在小幅超卖；
  严格配额需要 DB 原子扣减或 Redis；
- 没有 TPM（每分钟 token）限流，只有 RPM + 每日 token；
- token 估算不是 tokenizer，长中文/代码可能偏差；
- `usage_records` 在流结束时同步写库，DB 故障只记日志，流水可能丢；
- 没有按 API Key / 模型维度限流，也没有账单金额计算。

## 验收记录（2026-11-08）

| 用例 | 结果 |
|---|---|
| `quota_tests` | RPM/并发/每日配额/管理员绕过/0=不限 全部通过 |
| `usage_tests` | 完成与取消都落 usage，TTFT/latency/token 字段有效 |
| `scripts/test_pg.sh` | `PgUsageSink` 写入与当天汇总通过 |
| `scripts/test_quota.sh` | RPM=2：前 2 次 200，第 3 次 429 + Retry-After；重启后每日配额 50 触发 `quota_exceeded`；`usage_records=2` |
