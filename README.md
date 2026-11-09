# AIPlatform

用 C++20 写的 AI 对话平台后端骨架：**HTTP API + 流式（SSE）对话 + 可插拔模型网关**。
配套的完整开发路线图见 [docs/ROADMAP.md](docs/ROADMAP.md)。

这个仓库解决的是「从 0 到第一个能跑通、能演示、能接真实模型」的那一段，
所有代码都围绕一个目标：**让流式对话链路先正确地跑起来**。
阶段 3 的 PostgreSQL 持久化、阶段 4 的用户鉴权（JWT/API Key）与配额/用量已经落地，
见 [adr/0002](docs/adr/0002-postgresql-persistence.md)、[adr/0003](docs/adr/0003-auth.md)、
[adr/0004](docs/adr/0004-quota-usage.md)。

## 已实现（阶段 0~2 + 持久化 + 用户鉴权 + 内置演示前端）

- ✅ HTTP 服务（**Drogon**，连接不占线程）：`/healthz`、`/v1/config`、`/v1/chat/completions`、`/v1/sessions/{id}/messages`
- ✅ 内置 Web UI（零构建单页，编译进二进制）：流式对话、Markdown / 代码块、会话本地持久化、明暗主题、Token 设置
- ✅ **Web 登录 / 注册**：邮箱密码注册登录、JWT access/refresh 自动刷新、退出登录；本地会话按账号隔离
- ✅ SSE 流式输出，OpenAI 兼容的 chunk 格式（前端工具链可直接复用）
- ✅ OpenAI 兼容扩展：完整 `messages` 上下文、`/v1/models`、请求级
  `temperature/max_tokens/top_p/stop/seed/response_format/stream_options`
- ✅ 模型网关：`Provider` 抽象 + OpenAI 兼容实现 + 离线 `mock` 实现
- ✅ 上游 SSE 增量解析、非流式自动降级、上游错误映射
- ✅ 上游失败重试（仅首 token 前；408/409/425/429/5xx + 连接类错误，指数退避 + 抖动）
- ✅ 多 API Key 轮转（`AIP_API_KEYS`，重试自动换 key）
- ✅ 每 Key 熔断/冷却、RPM/TPM、Key 可用数指标
- ✅ 客户端断开 → 取消上游请求（不浪费 token）
- ✅ 多轮上下文（内存 LRU 或 PostgreSQL 会话存储）
- ✅ **PostgreSQL 持久化（M1）**：`AIP_STORAGE=postgres` 时会话/消息落库，重启不丢；
  迁移自动执行、连接池、软删除、按用户隔离的会话归属
- ✅ 鉴权：静态 `AIP_TOKEN`（admin）/ JWT（HS256，access 15m + refresh 7d）/ API Key（SHA-256 哈希、可撤销）
- ✅ 账号安全：refresh 轮换与吊销、改密踢下线、邮箱验证、找回密码、登录/注册限流、恒定时间假哈希
- ✅ 多租户会话归属：普通用户只能访问自己的会话；admin 可访问全部
- ✅ 配额限流：每用户 RPM / 并发流 / 每日 token；超出返回 429 + `Retry-After`
- ✅ 多副本配额（PostgreSQL 原子预留/lease）、usage 异步可靠写
- ✅ 用量流水：`usage_records` 记录 prompt/completion tokens、TTFT、latency
- ✅ 请求幂等：`request_id` 重放非流式/流式结果，`in_progress` 409，崩溃 TTL 自动恢复
- ✅ 会话管理：列表/改名/软删除；历史支持 `limit`，跨用户 404
- ✅ token 预算上下文：按估算 token 裁剪 system + 历史 + user，超预算 413 `context_too_long`
- ✅ CORS、结构化请求日志、`X-Request-Id`、统一错误码
- ✅ 可观测性：`/metrics`（Prometheus）、`/readyz`、TTFT/token/upstream 指标
- ✅ 流数量准入（超出 `AIP_MAX_STREAMS` 直接 503）、SSE 心跳
- ✅ 优雅退出（SIGTERM → 停止接新请求 → 等在途流收尾 → 退出）
- ✅ 单元测试、Docker 多阶段构建、CI（含 Drogon 镜像冒烟）

> 回退：把 `-DAIPLATFORM_HTTP=httplib` 切到 cpp-httplib 实现，API 契约完全一致，
> 代价是每连接一线程、并发流上限 = 线程池大小（见 [docs/adr/0001](docs/adr/0001-http-framework.md)）。

## 快速开始

> 完整的编译 / 运行 / 镜像构建 / 排错手册见 [docs/BUILD.md](docs/BUILD.md)。

```bash
# 依赖：CMake ≥ 3.20、支持 C++20 的编译器、libcurl 开发包、Drogon ≥ 1.9.14
# Ubuntu: sudo apt install build-essential cmake pkg-config libcurl4-openssl-dev
# Drogon 需源码编译（约 7~10 分钟），步骤见 docs/BUILD.md 第 1.1 节；
# 没有 Drogon 的机器可以先用回退后端：
#   cmake -S . -B build -DAIPLATFORM_HTTP=httplib

cmake -S . -B build -DAIPLATFORM_BUILD_TESTS=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure

# 离线模式（mock 模型，不需要 API key），默认监听 :8080
./build/aiplatform

# 想用 .env 里的真实模型 + PostgreSQL（自动拉起 compose 里的 postgres）：
#   ./scripts/run.sh
# 只想要内存模式：./scripts/run.sh --memory
```

需要持久化时（会话/消息落 PostgreSQL，重启不丢）：

```bash
docker compose up -d postgres          # 只起数据库，映射到 55432
AIP_STORAGE=postgres \
AIP_DB_DSN='postgresql://aiplatform:aiplatform@127.0.0.1:55432/aiplatform?connect_timeout=5' \
./build/aiplatform
```

另开一个终端：

```bash
curl -N http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"message":"你好，介绍一下你自己"}'
```

你会看到逐字输出的 SSE：

```
data: {"id":"chatcmpl-...","object":"chat.completion.chunk","choices":[{"delta":{"role":"assistant","content":""},"index":0}]}

data: {"id":"chatcmpl-...","object":"chat.completion.chunk","choices":[{"delta":{"content":"这是 "},"index":0}]}

...

data: [DONE]
```

### 用户鉴权（可选，需要 PostgreSQL）

```bash
# 生产必须显式设置 >=32 字节；多副本要一致
export AIP_JWT_SECRET="$(openssl rand -hex 32)"
AIP_STORAGE=postgres \
AIP_DB_DSN='postgresql://aiplatform:aiplatform@127.0.0.1:55432/aiplatform?connect_timeout=5' \
./build/aiplatform
```

```bash
# 注册：返回 access_token / refresh_token；密码 8~128 字节
curl -sS http://127.0.0.1:8080/v1/auth/register \
  -H 'Content-Type: application/json' \
  -d '{"email":"alice@example.com","password":"password123","display_name":"Alice"}'

# 带 JWT 对话
curl -N http://127.0.0.1:8080/v1/chat/completions \
  -H "Authorization: Bearer $ACCESS_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"message":"你好"}'

# 创建 API Key（明文只返回一次）
curl -sS http://127.0.0.1:8080/v1/auth/api-keys \
  -H "Authorization: Bearer $ACCESS_TOKEN" \
  -H 'Content-Type: application/json' -d '{"name":"cli"}'
```

- `AIP_AUTH_REQUIRED=1` 强制鉴权；`AIP_TOKEN` 仍可作为**管理员**静态 Token。
- 未设置 `AIP_TOKEN` / `AIP_JWT_SECRET` / `AIP_AUTH_REQUIRED` 时是本地匿名管理员模式，
  只适合内网/演示。
- JWT 需要 `AIP_STORAGE=postgres`（用户存 PostgreSQL）；memory 模式只支持静态 Token。

### Web 界面登录

启用账号体系后，打开 `http://127.0.0.1:8080/`，左下角会出现 **登录 / 注册**：

```bash
AIP_STORAGE=postgres \
AIP_DB_DSN='postgresql://aiplatform:aiplatform@127.0.0.1:55432/aiplatform?connect_timeout=5' \
AIP_JWT_SECRET="$(openssl rand -hex 32)" \
AIP_AUTH_REQUIRED=1 \
./build/aiplatform
```

- 账号模式下，未登录会先显示**全屏登录 / 注册页**，登录成功后才进入对话；手动 Token 不能绕过账号登录。
- 登录成功后，页面把 access/refresh token 保存在浏览器 `localStorage`；access 过期时自动 refresh 一次，失败会回到登录框。
- 本地会话按 `user_id` 隔离：未登录会话存在 `.anon` 作用域，退出登录后仍可看到；登录后的会话存在该账号作用域。
- 未启用账号体系（memory 模式、没配 `AIP_JWT_SECRET`）时，页面回退到「填写 Token」，静态 `AIP_TOKEN` / API Key 仍可用。
- `/v1/config` 的 `auth.user_accounts` 告诉前端是否展示登录入口；真正的鉴权仍由后端逐请求校验。

> 当前是演示级存储：token 在 `localStorage`，生产必须走 HTTPS，并建议改成 httpOnly cookie / BFF 代理。
> 云端会话列表同步还没接：目前登录主要解决账号身份、会话隔离和跨刷新保持登录。

如果浏览器里还留着旧版登录态，页面可能直接进入对话：点左下角「退出登录」，或在 DevTools 执行
`localStorage.removeItem('aiplatform.auth.v1')` 后刷新。旧的手动 Token 不会绕过账号登录闸门。

### 配额与用量

```bash
export AIP_RPM_LIMIT=60                    # 每用户每分钟请求数，0 = 不限
export AIP_DAILY_TOKEN_LIMIT=1000000       # 每用户每日 token，0 = 不限
export AIP_MAX_CONCURRENT_PER_USER=4       # 每用户并发流，0 = 不限
```

- 超限返回 `429`，带 `Retry-After` 秒数；每日配额用 `type=quota_exceeded`。
- 每次完成/取消的请求写 `usage_records`：`user_id/session_id/model/prompt_tokens/
  completion_tokens/latency_ms/ttft_ms`。
- `AIP_STORAGE=postgres` 时配额走 **PostgreSQL 原子预留**（`quota_reservations` /
  `quota_stream_leases`）：RPM 用 `rate_limits` 固定窗口、多副本共享；每日 token 用
  advisory lock + 预留避免先查后写超卖；并发流用带 TTL 的 lease，进程崩溃自动回收。
- memory 模式仍是进程内限流，只适合单实例/演示。
- 每日 token 的 usage 流水是异步写的：流结束到流水落库之间有短暂延迟，
  极端并发下可能多放行少量请求；要求严格账实一致时，把 `AIP_USAGE_ASYNC=false`
  （预留）或改用同步写入（见 ADR 0008）。
- 静态管理员和未绑定用户的请求不受限；普通 JWT/API Key 用户按上面的配置限流。

### 上下文预算

```bash
export AIP_MAX_CONTEXT_TOKENS=8192   # 模型上下文总预算
export AIP_MAX_TOKENS=1024           # 其中给 completion 预留
```

`ChatService` 按 `system + 最近历史 + 本轮 user` 估算 token，装不下就裁掉更早的历史；
system + user 都放不下时直接返回 `413 context_too_long`，不调用上游。
估算不是 tokenizer，长中文/代码会有偏差；`AIP_MAX_TOKENS` 必须小于
`AIP_MAX_CONTEXT_TOKENS`。

接真实模型（任何 OpenAI 兼容接口）：

```bash
export AIP_PROVIDER=openai
export AIP_BASE_URL=https://api.deepseek.com/v1
export AIP_API_KEY=sk-xxxxxxxx
export AIP_MODEL=deepseek-chat
# 可选：多 key 轮转（重试时自动换下一个），以及首 token 前重试次数
# export AIP_API_KEYS=sk-aaa,sk-bbb
# export AIP_MAX_RETRIES=2
./build/aiplatform
```

完整配置项见 [config/config.example.env](config/config.example.env)。

## Web 界面

启动服务后打开 **<http://127.0.0.1:8080/>**，不需要 npm / Node：

- 流式逐字输出，支持 Markdown、代码块高亮与复制、表格、引用
- 会话自动保存在浏览器 `localStorage`，刷新 / 重开页面可恢复
- 明暗主题、移动端布局、`Enter` 发送、`Shift+Enter` 换行
- 服务端启用 `AIP_TOKEN` 时，页面会提示填写 Bearer Token（只存本机）
- 顶部状态灯来自 `/healthz`；设置里能看到后端类型、模型与版本
- 请求失败 / 鉴权失败 / 用户停止都会在消息里给出明确状态，可一键重试

实现是**零构建**的：`web/` 下就是 HTML / CSS / JS 源码，CMake 在构建期把整个目录
嵌入二进制（[cmake/embed_web.cmake](cmake/embed_web.cmake)），部署产物仍然只有一个可执行文件。
改完前端后重新 `cmake --build build -j2` 即可，不需要另起静态服务器。

```bash
# 只想提供 API：不暴露 / 与 /assets/*
AIP_WEB=false ./build/aiplatform
```

> 正式产品仍建议按 [docs/DECISIONS.md](docs/DECISIONS.md) 的 D6 复用 Open WebUI / LobeChat；
> 这个内置页面定位是「演示 + 联调 + 无网络依赖的兜底」。

前端启动时读取 `/v1/config`（公开接口，不含密钥）：

```json
{
  "app": "AIPlatform", "version": "0.1.0", "api": "openai-compatible",
  "http": "drogon", "provider": "mock", "model": "deepseek-chat",
  "storage": "memory", "auth_required": false, "web": true,
  "auth": {
    "user_accounts": false, "password_login": false, "registration": false,
    "static_token": false, "email_verification_required": false,
    "password_reset": true, "refresh_rotation": true,
    "access_ttl_s": 900, "refresh_ttl_s": 604800
  },
  "max_message_bytes": 32768, "max_streams": 512
}
```

## API

### 鉴权接口

| 方法 | 路径 | 鉴权 | 说明 |
|---|---|---|---|
| POST | `/v1/auth/register` | 公开 | 邮箱 + 密码注册，返回 JWT 对与用户信息 |
| POST | `/v1/auth/login` | 公开 | 登录，返回 JWT 对 |
| POST | `/v1/auth/refresh` | 公开（需 refresh token） | 用 refresh token 换新 access/refresh |
| GET | `/v1/auth/me` | 用户 | 当前用户资料；静态管理员返回 401 |
| GET | `/v1/auth/api-keys` | 用户 | 当前用户的 API Key 列表（不含明文/哈希） |
| POST | `/v1/auth/api-keys` | 用户 | 创建 API Key，`key` 明文只返回一次 |
| DELETE | `/v1/auth/api-keys/{id}` | 用户 | 撤销 API Key |
| POST | `/v1/auth/logout` | 公开（需 refresh token） | 吊销 refresh token（幂等） |
| POST | `/v1/auth/verify-email` | 公开 | 消费邮箱验证 token，返回 JWT 对 |
| POST | `/v1/auth/password/forgot` | 公开 | 发送重置密码邮件；始终 202，避免枚举 |
| POST | `/v1/auth/password/reset` | 公开 | 消费一次性 token 重置密码 |
| POST | `/v1/auth/password/change` | 用户 | 校验旧密码后改密，返回新 JWT 对 |

```jsonc
// POST /v1/auth/register
{ "email": "alice@example.com", "password": "password123", "display_name": "Alice" }

// 200
{
  "token_type": "Bearer",
  "access_token": "eyJ...",
  "refresh_token": "eyJ...",
  "expires_in": 900,
  "user": { "id": 1, "email": "alice@example.com", "display_name": "Alice", "role": "user" }
}
```

鉴权头统一为 `Authorization: Bearer <JWT | API Key | AIP_TOKEN>`。
普通用户的会话互相隔离；跨用户访问历史返回 404（避免枚举）。

**账号安全（M6）**

- **refresh 轮换**：每次 `/v1/auth/refresh` 都会吊销旧 refresh token 并签发新 token；
  旧 token 重放返回 401。退出登录会吊销当前 refresh token。
- **改密/重置踢下线**：`token_version` 自增，旧 access token 立即失效；
  所有 refresh token 同时吊销，客户端需要重新登录（改密接口会返回新 JWT 对）。
- **邮箱验证**：`AIP_EMAIL_VERIFICATION_REQUIRED=1` 时，注册返回
  `201 {"verification_required":true}`，验证前登录返回 `403 email_not_verified`；
  邮件链接指向内置的 `/account.html?mode=verify&token=...` 落地页。
- **找回密码**：`/v1/auth/password/forgot` 生成一次性 token（数据库只存 SHA-256），
  重置链接指向 `/account.html?mode=reset&token=...`；token 只能使用一次。
- **限流**：登录/注册/找回密码按 IP + 邮箱固定窗口计数（PostgreSQL `rate_limits` 表，
  多副本共享），超限返回 429 + `Retry-After`。
- **枚举防护**：登录时用户不存在也执行一次 Argon2 假哈希；找回密码对不存在的邮箱
  也返回 202。
- **邮件配置**：生产设置 `AIP_SMTP_URL`；未配置时验证/重置链接只写日志，仅适合本地。
- **Web 端**：token 仍存 `localStorage`（演示级）。生产建议改用 httpOnly cookie + BFF，
  或在 Nginx/BFF 层做 CSRF 防护；`/v1/config` 的 `auth` 字段会告诉前端是否开启验证/重置。

### `POST /v1/chat/completions`

```jsonc
{
  "message": "你好",           // 必填；也兼容 OpenAI 的 messages 数组（取最后一条 user）
  "session_id": "s_xxx",       // 可选；为空则新建会话，ID 通过响应头 X-Session-Id 返回
  "model": "deepseek-chat",    // 可选
  "stream": true,              // 默认 true
  "request_id": "req_xxx"      // 可选；同 scope 内唯一，重复请求重放原结果
}
```

**OpenAI 兼容字段**（2026-11 扩展）：

- 同时支持 `message`（本平台简化字段）和 OpenAI 原生 `messages` 数组。传 `messages`
  时它被视为**权威上下文**，服务端不再叠加历史（适合 Open WebUI / LobeChat 这类每次带
  全量历史的客户端）；服务端只落本轮 `user` 与最终 `assistant` 消息。
- `messages` 支持 `system/user/assistant/tool` 文本消息；最后一条必须是 `user`。
  图片/多模态 part 和 `tool_calls` 暂不支持，会返回明确的 `unsupported_content` /
  `unsupported_tool`，而不是静默忽略。
- 请求级参数：`temperature`（0~2）、`max_tokens`（1~`AIP_MAX_CONTEXT_TOKENS-64`）、
  `top_p`（0~1）、`stop`（字符串或最多 4 项数组）、`presence_penalty` /
  `frequency_penalty`（-2~2）、`seed`、`response_format`（`text` / `json_object`）、
  `user`。未传时用服务端默认值。
- `stream_options.include_usage=false` 时最后一个 chunk 不带 `usage`；服务端仍会按
  估算值写用量流水。
- `model` 必须是 `AIP_MODELS` 允许列表中的模型；默认只有 `AIP_MODEL`。

响应：

- `stream: true` → `text/event-stream`，每个 chunk 是 OpenAI 兼容的 `chat.completion.chunk`，
  结束时发送 `data: [DONE]`；上游出错时发送 `event: error` 再 `[DONE]`。
  在途流达到 `AIP_MAX_STREAMS` 时直接返回 503（`too_many_streams`），不会排队。
- `stream: false` → 一个 `chat.completion` JSON，含 `usage`（在 WorkerPool 里跑，不占 IO 线程）。

### 模型列表

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/v1/models` | OpenAI 兼容模型列表，返回 `{"object":"list","data":[...]}` |
| GET | `/v1/models/{id}` | 单个模型信息；不在 `AIP_MODELS` 中返回 404 |

`AIP_MODELS=deepseek-chat,deepseek-reasoner` 控制允许列表；默认只有 `AIP_MODEL`。
对话请求里的 `model` 必须在允许列表中，否则返回 400 `model_not_found`。

### 会话管理

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/v1/sessions?limit=20&offset=0` | 当前用户会话列表，返回 `has_more` |
| PATCH | `/v1/sessions/{id}` | `{ "title": "新标题" }` |
| DELETE | `/v1/sessions/{id}` | 软删除 |
| GET | `/v1/sessions/{id}/messages?limit=100` | 最近 N 条原始消息 |

普通用户只能访问自己的会话；跨用户/不存在都返回 404。静态 `AIP_TOKEN` 可访问全部。

### `GET /v1/sessions/{id}/messages`

返回该会话最近 100 条消息。

### `GET /healthz`

```jsonc
// 正常
{"status":"ok","sessions":3,"active_streams":0,"max_streams":512,
 "model":"deepseek-chat","provider":"mock","storage":"postgres","storage_ok":true,
 "web":true,"http":"drogon"}
// 正在退出（仍返回 200，避免 liveness 探针误杀；LB 应看 draining 摘流量）
{"status":"draining","draining":true,...}
// 存储不可用（仍返回 200，storage_ok=false；就绪探针请改用 /readyz）
{"status":"degraded","storage":"postgres","storage_ok":false,...}
```

### `GET /readyz`

严格就绪探针：存储不可用或服务正在退出时返回 **503**，其余返回
`{"status":"ready","storage_ok":true}`。K8s readinessProbe / LB 应看这个接口，
`/healthz` 保留给 liveness（避免数据库短暂抖动把进程杀掉）。

### `GET /metrics`

Prometheus text exposition（`text/plain; version=0.0.4`）。默认开启，可用
`AIP_METRICS_ENABLED=false` 关闭；设置 `AIP_METRICS_TOKEN` 后需要
`Authorization: Bearer <token>`。核心指标：

| 指标 | 说明 |
|---|---|
| `aip_http_requests_total{method,route,status}` | HTTP 请求量；`route` 已归一化，避免会话 ID 打爆基数 |
| `aip_http_request_duration_seconds` | HTTP 时延直方图 |
| `aip_chat_requests_total{stream,outcome}` | 对话完成 / 取消 / 错误 |
| `aip_chat_ttft_seconds` | 首 token 延迟直方图 |
| `aip_chat_tokens_total{model,type}` | prompt / completion token 用量 |
| `aip_active_streams` | 当前在途 SSE 流 |
| `aip_quota_rejections_total{type}` | 配额/限流拒绝 |
| `aip_upstream_requests_total{model,outcome}` | 上游尝试结果（后续接 Key 熔断） |
| `aip_ready` | 就绪状态 0/1 |

### 结构化日志与 request_id

- `AIP_LOG_FORMAT=json` 时所有日志都是一行 JSON；默认即 `json`，可切回 `text`。
- 每个请求都有 `X-Request-Id`：客户端可传（1~128 位字母/数字/`-_.`），否则服务端生成；
  响应头原样返回，日志字段 `request_id` 可用于全链路检索。
- 请求结束打 `event=http_request`；流结束打 `event=chat_stream`，包含
  `request_id/user_id/session_id/model/outcome/duration_ms/ttft_ms/prompt_tokens/completion_tokens`。
- 不记录用户消息正文与 `Authorization` 头。

### `GET /v1/config`

前端启动用的公开配置：是否要求 Token、模型名、消息大小上限等。**不含** `api_key`、
`base_url`、system prompt。消息读取上限由 `/v1/config` 返回，前端据此在发送前提示，
服务端仍会独立校验。

### 内置静态资源

`GET /`、`/index.html`、`/assets/*`、`/favicon.svg` 由二进制内的资源表直接返回，
不读文件系统。启用鉴权（`AIP_TOKEN`/`AIP_JWT_SECRET`/`AIP_AUTH_REQUIRED`）时这些路径仍然公开
（否则页面本身打不开），
`/v1/chat/completions` 和会话历史接口照常鉴权；`AIP_WEB=false` 时不再暴露。

### 错误格式

```json
{ "error": { "type": "invalid_request", "message": "message 不能为空" } }
```

| HTTP | type | 场景 |
|---|---|---|
| 400 | `invalid_request` / `invalid_json` | 参数缺失、JSON 非法 |
| 401 | `unauthorized` / `invalid_credentials` | 缺少/错误 Bearer Token、登录失败 |
| 403 | `account_disabled` | 账号被禁用 |
| 409 | `email_exists` | 重复注册 |
| 409 | `request_in_progress` / `idempotency_conflict` | 同 request_id 正在执行/重放不可用 |
| 429 | `rate_limited` / `quota_exceeded` | RPM/并发超限、每日 token 配额用尽 |
| 413 | `payload_too_large` / `context_too_long` | 单条消息超过 32KB、上下文超预算 |
| 502 | `upstream_error` | 上游模型报错 |
| 503 | `storage_unavailable` / `auth_unavailable` / `server_busy` | 数据库/鉴权不可用、队列满 |
| 504 | `timeout` | 上游超时 |

### 上游重试与多 Key

- 只在「还没有向客户端吐第一个 token」时重试；已经吐字后失败直接发 `event: error`，避免前端看到重复内容。
- 可重试：HTTP 408/409/425/429/5xx，以及连接/收包/发包类 curl 错误；401/403/400 等参数或鉴权错误不重试，总超时也不重试（避免把 120s 等待放大成 6 分钟）。
- 退避：`AIP_RETRY_BASE_MS` 起步（默认 300ms）按 2 倍递增，单次最多 5s，另加 <100ms 抖动；`AIP_MAX_RETRIES=0` 关闭。
- 多 Key：`AIP_API_KEYS=a,b,c` 按请求轮转，重试时自动换下一个。
- Key 治理：429/5xx/网络错误累计到 `AIP_KEY_FAILURE_THRESHOLD` 后进入冷却，
  冷却按 `AIP_KEY_COOLDOWN_S` 指数退避、封顶 `AIP_KEY_COOLDOWN_MAX_S`；429 立即冷却；
  多 Key 下 401/403 会换 Key 再试。每 Key 还支持 `AIP_KEY_RPM_LIMIT` /
  `AIP_KEY_TPM_LIMIT`，所有 Key 都不可用时快速失败，不再排队等上游。
  指标见 `aip_upstream_key_cooldowns_total`、`aip_upstream_keys_available`。
- 每次尝试仍受 `AIP_TIMEOUT_S` 限制；重试只增加次数，不共享总超时。
- 重试会放大上游成本，默认 2 次是折中；成本敏感可设 0~1。

## 验证

```bash
./scripts/demo.sh                    # 冒烟测试（健康检查 / 流式 / 多轮 / 历史 / 错误码）
./scripts/test_retry.sh              # 验证首 token 前重试（前 2 次 429，第 3 次成功）
./scripts/test_pg.sh                 # 临时 PostgreSQL + pg 集成测试（迁移/CRUD/归属/软删除）
./scripts/test_persistence.sh        # 杀进程重启，验证会话历史仍在
./scripts/test_auth.sh              # 注册/登录/JWT/API Key/跨用户隔离/撤销/刷新
./scripts/test_quota.sh             # RPM 429/Retry-After/每日配额/usage 流水
./scripts/test_idempotency.sh        # 非流式/流式重放、in_progress 409、failed 重试
./scripts/test_sessions.sh           # 列表/改名/软删除/跨用户/history limit
python3 scripts/fake_upstream.py &   # 假上游，验证 OpenAI 协议与错误路径
```

## 目录结构

```
web/                          内置 Web UI（零构建，构建期嵌入二进制）
├── index.html
├── favicon.svg
└── assets/{app.css,app.js}
cmake/embed_web.cmake         生成 web_assets.cpp，把 web/ 变成 C++ raw string
src/
├── main.cpp                  组装依赖（信号处理由 HTTP 后端自己装）
├── config.{h,cpp}            环境变量配置
├── common/log.h              极简日志（后续换 spdlog）
├── util/id.{h,cpp}           随机 ID / 时间戳
├── web/assets.h              嵌入资源的查找接口（实现由 CMake 生成）
├── llm/                      模型网关
│   ├── types.h               Provider / Stream / Event 接口
│   ├── key_ring.h            多 API Key 轮转（原子游标）
│   ├── event_queue.h         生产者-消费者队列
│   ├── threaded_stream.h     后台线程 + 队列模板
│   ├── openai_provider.cpp   OpenAI 兼容 + SSE 解析 + 取消
│   └── mock_provider.cpp     离线假模型
├── auth/                     用户鉴权
│   ├── principal.h           请求身份（user_id / admin / auth_type）
│   ├── types.h               User / ApiKeyInfo / TokenPair
│   ├── password_hasher.*     Argon2id
│   ├── jwt.*                 HS256 JWT（access/refresh）
│   ├── api_key.*             API Key 生成/哈希/常量时间比较
│   ├── auth_service.*        注册/登录/刷新/Bearer 解析/API Key 管理
│   ├── user_store.h          用户存储接口
│   ├── pg_user_store.*       PostgreSQL 实现
│   └── errors.h              AuthError（400/401/403/409）
├── storage/                  PostgreSQL 基础设施
│   ├── db.{h,cpp}            连接池 / Lease / Transaction / Result
│   └── migrations.{h,cpp}    版本化迁移 + advisory lock
├── quota/                    进程内配额（RPM / 并发 / 每日 token）
│   └── quota.{h,cpp}         QuotaService + RAII QuotaGuard
├── usage/                    用量流水
│   ├── usage.h               UsageSink 接口
│   └── pg_usage_sink.*       PostgreSQL 实现 + 当天汇总
├── chat/                     业务层（上下文、会话存储）
│   ├── session_store.{h,cpp} SessionStore 接口 + 内存 LRU 实现
│   ├── pg_session_store.*    PostgreSQL 实现
│   ├── chat_service.{h,cpp}  拼上下文 / 调 Provider / 落库装饰器
│   ├── token_estimate.*      token 估算
│   ├── context_builder.*     token 预算上下文裁剪
│   ├── idempotency.*         幂等存储接口 + 内存实现 + TurnObserver
│   ├── pg_idempotency_store.* PostgreSQL request_dedup 实现
│   ├── replay_stream.h       幂等重放流
│   └── errors.h              Forbidden / SessionNotFound
└── http/                     HTTP 层（两个后端，API 契约一致）
    ├── server.h              对外接口（PIMPL，不暴露框架类型）
    ├── public_config.h       /v1/config 的公开字段
    ├── server_drogon.cpp     主线：Drogon + 泵线程 + 准入 + drain + 内置 UI
    └── server_httplib.cpp    回退：cpp-httplib（-DAIPLATFORM_HTTP=httplib）
```

`third_party/libpq/` 是随仓库分发的 PostgreSQL 客户端头文件；运行时只需要 `libpq5`。


## 设计要点

**为什么用「后台线程 + 阻塞队列」而不是回调？**
`llm::Stream` 是拉取语义，既能适配同步框架，也能适配协程式框架。Drogon 的推送式
`ResponseStream` 由每路流自己的泵线程桥接（`server_drogon.cpp` 的 `SsePump`）。

**为什么默认 `AIP_PROVIDER=mock`？**
让前端开发、CI、演示都不依赖网络和 API key。真实项目里它就是集成测试的 stub。

**为什么迁移到 Drogon？**
cpp-httplib 每连接一线程，每个 SSE 长连接独占一个线程，并发上限 = 线程池大小（64）。
Drogon 用 reactor，**连接本身不占线程**：实测 300 路挂起连接只多 5 个线程。
迁移代价与边界见 [docs/adr/0001-http-framework.md](docs/adr/0001-http-framework.md)。

> **2026-11-02 更新**：Drogon 迁移**已落地**（`src/http/server_drogon.cpp`），
> 200 路 SSE 实测 200/200 成功、线程峰值 409（= 2×200 + 9）、RSS 26 MB；
> 断连取消、SIGTERM drain（120 路在途全部收尾后退出）、流数量准入均已验证。
> 迁移**没有**降低单流线程数：每路活跃流仍是 2 个线程（泵 + libcurl 读线程），
> 要降到 1 或 0 个需要把 `llm::Stream` 改成推送式或上游改非阻塞，见 ADR「后续两步」。

## 下一步

P0 生产化按 M1~M5 拆分，现已全部落地：M1 持久化、M2 用户/JWT/API Key/归属、
M3 配额限流/usage 流水、M4 请求幂等/会话管理、M5 token 预算上下文。见
[adr/0002](docs/adr/0002-postgresql-persistence.md)、[adr/0003](docs/adr/0003-auth.md)、
[adr/0004](docs/adr/0004-quota-usage.md)、[adr/0005](docs/adr/0005-idempotency-sessions.md)、
[adr/0006](docs/adr/0006-context-budget.md)。

下一阶段是 roadmap 的阶段 6/7：可观测性（metrics/结构化日志）、正式压测、部署、
RAG/工具调用/本地推理。

更完整的长期路线（可观测性、压测、RAG / 工具调用 / 本地推理）见
[docs/ROADMAP.md](docs/ROADMAP.md)。

内置单页已经覆盖「演示 + 联调」；生产前端是否复用 Open WebUI / 自研 Next.js，
仍按 [docs/DECISIONS.md](docs/DECISIONS.md) 的 D6 由人拍板。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/ROADMAP.md](docs/ROADMAP.md) | 从零到上线的 8 个阶段、架构、DDL、踩坑清单 |
| [docs/DECISIONS.md](docs/DECISIONS.md) | 架构选型决策清单（待拍板表格） |
| [docs/adr/0001-http-framework.md](docs/adr/0001-http-framework.md) | 为什么迁移到 Drogon |
| [docs/adr/0002-postgresql-persistence.md](docs/adr/0002-postgresql-persistence.md) | 持久化选型、schema、迁移与验收记录 |
| [docs/adr/0003-auth.md](docs/adr/0003-auth.md) | JWT / API Key / Argon2id 鉴权设计 |
| [docs/adr/0004-quota-usage.md](docs/adr/0004-quota-usage.md) | 配额/限流/用量流水设计 |
| [docs/adr/0005-idempotency-sessions.md](docs/adr/0005-idempotency-sessions.md) | 请求幂等与会话管理 API |
| [docs/adr/0006-context-budget.md](docs/adr/0006-context-budget.md) | token 预算上下文裁剪 |
| [docs/adr/0007-account-security.md](docs/adr/0007-account-security.md) | refresh 轮换/吊销、邮箱验证、找回密码、登录限流 |
| [docs/adr/0008-distributed-quota.md](docs/adr/0008-distributed-quota.md) | 多副本配额原子预留与 usage 异步写 |
| [docs/BUILD.md](docs/BUILD.md) | 编译、运行、镜像构建、排错手册 |
| [docs/GIT.md](docs/GIT.md) | Git 工作流、提交规范、发布与事故处理 |

## 说明

- `third_party/` 里有 cpp-httplib 0.18.3、nlohmann/json 3.11.3 两个单头文件库，
  以及 `libpq/`、`argon2/` 的客户端头文件；直接随仓库分发，避免依赖管理工具带来的首次构建开销。
  `third_party/nlohmann/json.hpp` 是个转发头，让 `#include <nlohmann/json.hpp>` 命中仓库内版本
  （此前能编过是因为构建机恰好装了系统包，换机器就挂）。
- 用户/JWT/API Key 需要 `AIP_STORAGE=postgres`；memory 模式只保留静态 `AIP_TOKEN`。
- 会话存储**默认是进程内内存**，重启即丢；设置 `AIP_STORAGE=postgres` 后会话/消息落
  PostgreSQL，重启不丢。内存模式仍保留给演示、单测和没有数据库的交付环境。
- PostgreSQL 客户端头文件随仓库分发（`third_party/libpq/`），运行时只需要 `libpq5`；
  不想引入 PostgreSQL 可用 `-DAIPLATFORM_WITH_POSTGRES=OFF` 构建纯内存版本。
- Web UI 没有 `package.json` / `node_modules`：`web/` 由 CMake 在构建期转成 C++
  raw string（见 [cmake/embed_web.cmake](cmake/embed_web.cmake)），改完前端重新编译即可。
  页面 CSP 只允许同源脚本；模型输出按不可信文本处理（先转义再渲染，链接只放行
  `http(s)` / `mailto`），不使用 `innerHTML` 直接插入原始内容。
