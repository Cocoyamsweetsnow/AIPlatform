# 0005 请求幂等与会话管理 API

- 状态：**已落地（M4）**
- 日期：2026-11-08
- 决策者：项目负责人（AI 按 roadmap 阶段 3/4 实施）
- 影响范围：`src/chat/idempotency.*`、`src/chat/pg_idempotency_store.*`、`src/chat/replay_stream.h`、`src/http/`、`src/main.cpp`、`schema request_dedup`

## 背景

网络抖动/客户端重试会让同一个用户请求执行两次：重复调用上游、重复扣费、重复写会话。
同时前端/第三方客户端需要会话列表、改名、删除，而不只是“按 ID 查最近 100 条”。

## 决定

### 请求幂等

1. `POST /v1/chat/completions` 新增可选 `request_id`（1~128 字符）。
2. 作用域是 `Principal::scope()`（用户或静态 Token），同一 scope 内 `request_id` 唯一。
3. 状态机在 `request_dedup` 表：
   - 第一次 `INSERT ... status='in_progress'` → 执行；
   - 已有 `in_progress` → `409 request_in_progress` + `Retry-After: 1`；
   - `completed` → 用保存的 `assistant_message_id` 重放；
   - `failed` → 重置为 `in_progress`，允许重试；
   - `in_progress` 超过 `AIP_IDEMPOTENCY_TTL_S`（默认 180s）视为进程崩溃，允许重抢。
4. 完成回调：
   - `RecordingStream` 在 `Done/End/cancel/error` 时通过 `TurnObserver` 写 completed/failed；
   - 成功写 `session_id + assistant_message_id`，失败写 error type/message。
5. 重放：
   - 非流式：返回保存的 assistant 内容，`idempotent_replay=true`；
   - 流式：`ReplayStream` 把完整内容重新编码成 SSE（role/delta/final/[DONE]），不再调用上游；
   - 重放不消耗配额、不写新的 usage。
6. 没有 request_id 时行为与 M3 完全一致。

### 会话管理 API

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/v1/sessions?limit=&offset=` | 当前用户会话列表；`limit` 1~100，`offset` 0~100000，返回 `has_more` |
| PATCH | `/v1/sessions/{id}` | `{ "title": "..." }`，1~200 字节 |
| DELETE | `/v1/sessions/{id}` | 软删除（`deleted_at`），返回 `{deleted:true,id}` |
| GET | `/v1/sessions/{id}/messages?limit=` | 最近 N 条原始消息（不再做上下文裁剪） |

- 普通用户只能操作自己的会话；跨用户/不存在统一 404，避免枚举。
- admin（静态 `AIP_TOKEN`）可以列出/修改所有会话。
- `SessionStore::history` 只返回原始消息，上下文裁剪移到 `ChatService`；
  这样 `limit=1` 也能返回最后一条 assistant，而不是被裁成空。

## 后果

**正面**

- 客户端重试不会重复调用上游/重复计费；
- 流式重放保持 OpenAI chunk 格式，现有前端无感；
- 崩溃留下的 `in_progress` 会在 TTL 后自动恢复；
- 会话管理 API 让前端/第三方不必只依赖本地 localStorage。

**限制**

- 幂等键是 `(scope, request_id)`，不校验请求体；同一个 request_id 换 message 会重放旧结果；
- 失败重试可能留下上次的部分 assistant 消息（未做清理/标记）；
- `request_dedup` 没有定期清理任务，长期需要按 `updated_at` 归档/删除；
- 多副本依赖 PostgreSQL 唯一约束，没有额外分布式锁；
- 会话删除是软删除，消息仍保留在库里（审计/恢复），暂未提供恢复 API；
- 列表用 offset 分页，深页性能一般；数据量大后改 cursor。

## 验收记录（2026-11-08）

| 用例 | 结果 |
|---|---|
| `idempotency_tests` | 内存幂等状态机、重放流、完成/取消回调全部通过 |
| `pg_tests` | PG claim/complete/fail/stale TTL、消息按 ID 读取通过 |
| `scripts/test_idempotency.sh` | 非流式/流式重放、in_progress 409 + Retry-After、failed 重试全部通过 |
| `scripts/test_sessions.sh` | 列表/改名/软删除/跨用户 404/history limit 全部通过 |
| Drogon 容器 | 编译 + 6/6 tests；运行时非流式重放、列表、改名通过 |
