# 用 C++ 从零做一个 AI 对话平台：详细路线图

> 目标读者：有 C++ 基础、想做出**能上线**的对话平台（类 ChatGPT 的 Web 服务）的工程师。
> 本文把「从空目录到上线」拆成 8 个阶段，每个阶段都有**要做什么、验收标准、预计工期、常见坑**。
> 仓库里已经包含阶段 0~2 的可运行骨架（`src/`），clone 下来就能跑。

---

## 0. 动手之前：先定三件事

技术选型不是拍脑袋，它由三个问题决定：

| 问题 | 选项 | 对方案的影响 |
|---|---|---|
| 平台形态？ | Web 服务（B/S）/ 桌面客户端 / 命令行 | 决定要不要写前端、要不要处理 SSE 与 CORS |
| 模型从哪来？ | 调云端 API / 自建推理 / 两者都要 | 决定网关复杂度：要不要做多 provider、负载均衡、GPU 调度 |
| 目标规模？ | 内部工具（<50 人）/ 对外产品（万级用户） | 决定要不要上 PostgreSQL、Redis、多租户、计费 |

**推荐默认组合（本文按此展开）**：
Web 服务 + 云端 OpenAI 兼容 API（DeepSeek/Qwen/OpenAI）+ 单机起步、按需扩展。

### 为什么用 C++，以及它的代价

适合用 C++ 的理由：

- **吞吐与内存可控**：同样 4C8G，C++ 网关能扛的连接数通常是脚本语言的数倍；
- **和推理栈同语言**：llama.cpp、TensorRT-LLM、vLLM 的 C++ 前端、tokenizer 都是 C++ 生态；
- **交付物简单**：一个静态链接的二进制 + 一个配置文件就能部署，没有运行时依赖地狱。

必须接受的代价：

- 没有 Django/Rails 那种「ORM + 迁移 + 后台管理」一条龙，**这些要自己搭**；
- 前端、运营后台仍然建议用 TypeScript（别用 C++ 写 UI）；
- 生态库质量参差，选型要看维护活跃度，别选半年没提交的库。

> 结论：**C++ 负责「网关 + 推理服务 + 高性能路径」，前端和运营后台交给 TS。** 这不是妥协，是分工。

---

## 1. 技术选型

| 层次 | 推荐 | 备选 / 何时换 |
|---|---|---|
| 语言标准 | **C++20**，GCC 13+ 或 Clang 17+ | GCC 11 可用，但缺 `std::format`、`std::expected` |
| 构建 | **CMake ≥ 3.20 + Ninja** | Bazel（大团队、多语言 monorepo） |
| 依赖管理 | **vcpkg manifest 模式** / Conan 2 | 头文件库直接 `FetchContent` / 提交进 `third_party/`（本骨架的做法，零网络依赖） |
| HTTP 服务 | **cpp-httplib**（起步）→ **Drogon**（生产） | uWebSockets（极致性能）、Boost.Beast（完全可控但心智负担大） |
| JSON | **nlohmann/json**（易用） | simdjson（解析 >100MB/s 的场景） |
| 日志 | **spdlog** | 骨架里的极简 logger；生产建议 spdlog 异步模式 |
| 数据库 | **PostgreSQL 16 + libpqxx** | MySQL（已有运维体系）、SQLite（单机小工具） |
| 缓存/限流 | **Redis 7 + redis-plus-plus** | 单机可用进程内 LRU + 令牌桶 |
| 鉴权 | **JWT（jwt-cpp）+ Argon2id** | OAuth2/OIDC（接企业 SSO 时） |
| 上游 HTTP 客户端 | **libcurl** | Boost.Beast（需要 HTTP/2、连接池定制时） |
| 测试 | **GoogleTest** + CTest | Catch2（更轻） |
| 压测 | **k6** / wrk | 自写 C++ 压测器（测极限 QPS 时） |
| 部署 | **Docker 多阶段构建 + Nginx** | Kubernetes（多副本、弹性伸缩） |

### 两个关键取舍

**① HTTP 框架：httplib 起步，Drogon 上线**

`cpp-httplib` 是单头文件、零依赖、1500 行就能跑通，非常适合验证想法。但它是**每连接一线程**模型：

> 每个 SSE 长连接会独占一个线程。默认线程池 16 个，就意味着**最多 16 路并发流**。
> 本次骨架已把线程池调到 64（`CPPHTTPLIB_THREAD_POOL_COUNT`），够内部工具和演示用；
> 要支撑几百上千并发长连接，必须换异步框架（Drogon 的 reactor 模型 / uWebSockets 的事件循环）。

**② 模型接入：先做「OpenAI 兼容」一个协议**

DeepSeek、Qwen（DashScope 兼容模式）、Kimi、vLLM、Ollama、OneAPI 全都提供 OpenAI 兼容接口。
先只实现这一个协议，能覆盖 90% 的场景；等真的要接 Anthropic/Google 原生协议时，
再在 `llm::Provider` 接口下加实现（骨架里已经是这个结构，加一个文件即可）。

---

## 2. 架构：分层与数据流

```
┌─────────────────────────────────────────────────────────────┐
│  浏览器 / App / 第三方客户端                                  │
└───────────────┬─────────────────────────────────────────────┘
                │ HTTPS + SSE
┌───────────────▼─────────────────────────────────────────────┐
│  Nginx（TLS 终止、限流、proxy_buffering off）                 │
└───────────────┬─────────────────────────────────────────────┘
                │ HTTP/1.1 chunked
┌───────────────▼─────────────────────────────────────────────┐
│  http::ApiServer        ← 路由 / 鉴权 / 参数校验 / SSE 编码    │
├─────────────────────────────────────────────────────────────┤
│  chat::ChatService      ← 拼上下文 / 调模型 / 落库 / 计费      │
├─────────────────────────────────────────────────────────────┤
│  llm::Provider          ← 模型网关（openai / mock / llama.cpp）│
│    └─ llm::Stream       ← 统一「事件流」抽象：Delta/Done/Error │
├─────────────────────────────────────────────────────────────┤
│  chat::SessionStore     ← 会话存储（内存 → PostgreSQL）        │
└───────────────┬─────────────────────────────────────────────┘
                │ HTTPS（libcurl，增量读）
┌───────────────▼─────────────────────────────────────────────┐
│  上游模型服务（DeepSeek / vLLM / ...）                        │
└─────────────────────────────────────────────────────────────┘
```

### 一次对话请求的生命周期

1. Nginx 收到 `POST /v1/chat/completions`，转发到 C++ 服务；
2. `pre_routing_handler` 校验 `Authorization`，写入 CORS 头；
3. 解析 JSON，校验 `message` 非空、长度上限；
4. `ChatService::start()`：取会话历史 → 拼 `system + history + user` → 调 Provider；
5. Provider 在**后台线程**用 libcurl 发起上游请求，逐行解析上游 SSE；
6. 每个 token 作为 `Event{Delta}` 进入阻塞队列；
7. HTTP 线程从队列取事件，编码成 SSE `data: {...}\n\n` 写入 socket；
8. 上游发 `[DONE]` → 产生 `Event{Done}` → 追加 `data: [DONE]` 并结束响应；
9. `RecordingStream` 在结束时把完整回复写回会话存储；
10. 若客户端中途断开，`sink.write` 失败 → 触发 `cancel()` → curl 的写回调返回 0 → 上游连接被断开（**省钱的关键**）。

### 目录结构（与仓库一致）

```
AIPlatform/
├── CMakeLists.txt
├── Dockerfile / docker-compose.yml / deploy/nginx.conf
├── config/config.example.env
├── docs/ROADMAP.md              ← 本文
├── scripts/                     ← 冒烟测试 + 假上游
├── src/
│   ├── main.cpp                 ← 组装依赖、信号处理
│   ├── config.{h,cpp}           ← 环境变量配置
│   ├── common/log.h             ← 日志（后续换 spdlog）
│   ├── util/id.{h,cpp}
│   ├── llm/
│   │   ├── types.h              ← Provider / Stream / Event 接口
│   │   ├── event_queue.h        ← 生产者-消费者队列
│   │   ├── threaded_stream.h    ← 后台线程 + 队列的通用实现
│   │   ├── openai_provider.cpp  ← OpenAI 兼容 + SSE 解析 + 取消
│   │   └── mock_provider.cpp    ← 离线假模型（测试/CI 必需）
│   ├── chat/
│   │   ├── session_store.{h,cpp}
│   │   └── chat_service.{h,cpp}
│   └── http/server.{h,cpp}
└── tests/unit_tests.cpp
```

---

## 3. 分阶段实施

### 阶段 0：环境与骨架（0.5 周）

**做什么**

```bash
# Ubuntu 22.04 上装较新的编译器（GCC 11 缺 std::format）
sudo add-apt-repository ppa:ubuntu-toolchain-r/test -y
sudo apt update && sudo apt install -y g++-13 cmake ninja-build pkg-config libcurl4-openssl-dev
sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 100

# 或者直接跳进容器，避免污染宿主机
docker run -it --rm -v "$PWD":/src -w /src gcc:13 bash
```

- 建仓库骨架：`CMakeLists.txt` + `src/` + `tests/` + `third_party/`
- 先只实现 `GET /healthz`，能编译、能启动、能 curl 通
- 接 CI：`cmake -S . -B build && cmake --build build -j && ctest --test-dir build`

**验收**

- [ ] `./build/aiplatform` 启动后 `curl localhost:8080/healthz` 返回 200
- [ ] `cmake --build` 零 warning（`-Wall -Wextra -Wpedantic`）
- [ ] CI 一分钟内跑完

**坑**：不要在这一步引入 vcpkg/Drogon 等重依赖，否则第一次编译就要 20 分钟，挫败感极强。
先让「改一行代码 → 3 秒出二进制」的循环转起来。

---

### 阶段 1：对话 API 与错误约定（0.5 周）

**做什么**

1. 定义对外协议（就按 OpenAI 兼容来，前端工具链直接复用）：

```jsonc
POST /v1/chat/completions
{
  "message": "你好",            // 本平台简化字段
  "session_id": "s_xxx",        // 可选，空则新建
  "model": "deepseek-chat",     // 可选
  "stream": true                // 默认 true
}
```

2. 统一错误格式，**并写进文档**（前端要靠它做提示）：

```json
{ "error": { "type": "invalid_request", "message": "message 不能为空" } }
```

| 场景 | HTTP | type |
|---|---|---|
| 参数缺失/非法 | 400 | `invalid_request` |
| JSON 解析失败 | 400 | `invalid_json` |
| 未带 token | 401 | `unauthorized` |
| 超出配额 | 429 | `rate_limited` |
| 上游模型报错 | 502 | `upstream_error` |
| 上游超时 | 504 | `timeout` |

3. 加上：请求体大小限制（1MB）、`message` 长度限制（32KB）、CORS、请求日志。

**验收**

- [ ] `scripts/demo.sh` 第 5 节四条错误路径全部命中预期状态码
- [ ] 日志能按 `method path status` 检索

---

### 阶段 2：模型网关（核心，1~1.5 周）

这是整个平台技术含量最高、也最容易翻车的部分。骨架已实现，但你要理解每一处为什么这么写。

**2.1 抽象出 Provider 接口**

```cpp
class Provider {
 public:
  virtual std::string name() const = 0;
  virtual std::unique_ptr<Stream> chat_stream(const ChatRequest& req) = 0;
};

class Stream {                        // 阻塞拉取语义
  virtual Pull next(Event& out, std::chrono::milliseconds timeout) = 0;
  virtual void cancel() = 0;
};
```

为什么要「阻塞拉取 + 独立生产线程」而不是回调？因为 HTTP 框架的写接口（httplib 的 `DataSink`）
本身就是「你来拉我就写」的形态，拉取语义能同时适配同步框架和协程式框架。

**2.2 SSE 解析的三个细节**

- libcurl 的 `WRITEFUNCTION` 给的是**任意切分**的网络包，必须自己按 `\n` 组行；
- 只处理 `data:` 开头的行，`: ping`、`event:`、`id:` 一律忽略；
- `[DONE]` 是字符串不是 JSON，别丢进 JSON 解析器。

**2.3 不能省的四个设置**

```cpp
CURLOPT_CONNECTTIMEOUT, 10        // 连不上要快速失败
CURLOPT_TIMEOUT, 120              // 总超时兜底
CURLOPT_LOW_SPEED_LIMIT, 1        // 上游「假死」（不吐字不断开）检测
CURLOPT_LOW_SPEED_TIME, 60
CURLOPT_NOSIGNAL, 1               // 多线程下必须，否则 curl 用信号做 DNS 超时会崩
```

**2.4 客户端断开 → 取消上游（省钱）**

```cpp
// curl 写回调里检查取消标志，返回 0 即中断传输
if (self->queue_->cancelled()) return 0;
```

httplib 侧：`sink.write()` 返回 false（对端已关闭）→ 返回 false 结束 provider →
resource_releaser 里 `stream->cancel()`。

**2.5 非流式降级**

上游被代理、或上游本来就返回非流式 JSON 时，不能直接报错。骨架里做了形态嗅探：
首包是 `data:` 就走 SSE，是 `{` 就整体解析成一次性回复。

**2.6 重试与多 key（生产必备，骨架未做）**

- 429/5xx/网络错误：指数退避 + 抖动，最多 2 次，**只在还没吐出第一个 token 时重试**（否则前端会收到重复内容）；
- 多 key 轮转：`std::atomic<size_t>` 取模，配合每 key 的失败计数熔断；
- 幂等：客户端传 `request_id`，服务端在 60s 内去重。

**验收**

- [ ] 首字延迟（TTFT）< 800ms（同地域、DeepSeek）
- [ ] 上游 401/429/断网，前端都能收到明确的 `event: error` 并正常收尾
- [ ] 客户端 `Ctrl-C` 后，上游连接在 1s 内断开（看上游侧日志或 `ss -tnp`）
- [ ] 连续 100 次请求无内存增长（`valgrind --leak-check=full` 或 ASan）

---

### 阶段 3：持久化（1 周）

内存存储重启即丢，必须在「能演示」之后立刻补上。

**3.1 建表（PostgreSQL 16）**

```sql
CREATE TABLE users (
  id            BIGSERIAL PRIMARY KEY,
  email         TEXT UNIQUE NOT NULL,
  password_hash TEXT NOT NULL,              -- Argon2id
  display_name  TEXT NOT NULL DEFAULT '',
  created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
  disabled_at   TIMESTAMPTZ
);

CREATE TABLE sessions (
  id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  user_id    BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  title      TEXT NOT NULL DEFAULT '新会话',
  model      TEXT NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_sessions_user_updated ON sessions(user_id, updated_at DESC);

CREATE TABLE messages (
  id          BIGSERIAL PRIMARY KEY,
  session_id  UUID NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
  role        TEXT NOT NULL CHECK (role IN ('system','user','assistant','tool')),
  content     TEXT NOT NULL,
  token_count INT  NOT NULL DEFAULT 0,
  created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_messages_session_created ON messages(session_id, created_at);

-- 用量流水：计费与审计的唯一真相来源
CREATE TABLE usage_records (
  id                BIGSERIAL PRIMARY KEY,
  user_id           BIGINT NOT NULL REFERENCES users(id),
  session_id        UUID,
  model             TEXT NOT NULL,
  prompt_tokens     INT NOT NULL,
  completion_tokens INT NOT NULL,
  latency_ms        INT NOT NULL,
  created_at        TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_usage_user_time ON usage_records(user_id, created_at DESC);
```

**3.2 迁移管理**

别手敲 SQL。用 goose / flyway（Go/Java 写的独立二进制），或者自己在 C++ 里写个
`aiplatform migrate` 子命令：读 `migrations/*.sql`，在 `schema_migrations` 表里记录版本号。
**迁移文件只增不改**，这是上线后能回滚的前提。

**3.3 仓储层替换**

保持 `SessionStore` 的接口不变，加一个 `PgSessionStore` 实现：

- 读路径：会话历史走「查最近 N 条」的覆盖索引；
- 写路径：**不要在 HTTP 线程里同步写库**——SSE 流式请求可能持续几十秒，
  把落库丢给线程池（或 Redis Stream）异步做；
- 热点：最近会话与限流计数放 Redis，PG 只存冷数据。

**验收**

- [ ] 重启服务，会话与消息仍在
- [ ] `EXPLAIN ANALYZE` 确认历史查询走 `idx_messages_session_created`
- [ ] 并发 50 路对话时 DB 连接池不被打爆（`pg_stat_activity` 观察）

---

### 阶段 4：账号、多租户、配额（1~1.5 周）

**4.1 认证**

- 注册：邮箱 + Argon2id（`libargon2`，别用 bcrypt 的新项目了）；
- 登录：签发 JWT（`jwt-cpp`），access token 15 分钟 + refresh token 7 天；
- 对外 API：单独的 `api_keys` 表，存 `sha256(key)` 前缀 + 哈希，明文只展示一次；
- 企业场景接 OIDC（Keycloak / 飞书 / 企业微信）。

**4.2 限流（三层）**

| 层 | 粒度 | 实现 |
|---|---|---|
| 网关 | 全局 QPS | Nginx `limit_req` |
| 用户 | 每分钟请求数 / 每日 token 数 | Redis 令牌桶（Lua 脚本保证原子性） |
| 上游 | 每 key 的 RPM/TPM | 进程内计数 + 熔断 |

令牌桶 Lua 的核心是「一次 EVAL 里读-算-写」，避免并发下超卖。

**4.3 计费**

不要在 C++ 里做金额计算（浮点 + 多币种 = 事故）。记录 **token 流水**，
金额在报表层按单价换算；单价配置化，改价不改代码。

**验收**

- [ ] 100 并发打同一用户，配额不超卖（对比 Redis 计数与 `usage_records` 汇总）
- [ ] token 过期、篡改、越权访问他人会话，全部 401/403

---

### 阶段 5：前端（1~2 周）

**不要自己从零写。** 两条路：

1. **直接复用**：Open WebUI / LobeChat / ChatGPT-Next-Web，把「OpenAI 接口地址」指向你的服务即可（它们支持自定义 base URL）；
2. **自研**：Next.js + Tailwind + shadcn/ui，核心只有三个页面：登录、会话列表、对话页。

**SSE 消费的关键代码**（不用 `EventSource`，因为它不能带 `Authorization` 头）：

```ts
const res = await fetch("/v1/chat/completions", {
  method: "POST",
  headers: { "Content-Type": "application/json", Authorization: `Bearer ${token}` },
  body: JSON.stringify({ message, session_id: sid, stream: true }),
});

const reader = res.body!.getReader();
const decoder = new TextDecoder();
let buffer = "";

while (true) {
  const { done, value } = await reader.read();
  if (done) break;
  buffer += decoder.decode(value, { stream: true });

  // 按空行切分 SSE 帧；注意网络包可能在任意位置断开
  let idx: number;
  while ((idx = buffer.indexOf("\n\n")) >= 0) {
    const frame = buffer.slice(0, idx);
    buffer = buffer.slice(idx + 2);
    for (const line of frame.split("\n")) {
      if (!line.startsWith("data: ")) continue;
      const data = line.slice(6);
      if (data === "[DONE]") return;
      const chunk = JSON.parse(data);
      appendText(chunk.choices?.[0]?.delta?.content ?? "");
    }
  }
}
```

**验收**

- [ ] 首字 < 1.5s（含网络）；文字逐字出现而不是「转圈 3 秒后整段蹦出」
- [ ] Markdown + 代码高亮（`react-markdown` + `shiki`）
- [ ] 断网重连、刷新页面能恢复会话
- [ ] 移动端可用的输入框（回车发送 / Shift+回车换行）

---

### 阶段 6：可观测、压测、上线（1~1.5 周）

**6.1 指标（Prometheus）**

| 指标 | 为什么重要 |
|---|---|
| `chat_ttft_seconds`（直方图） | 用户体感的核心指标 |
| `chat_tokens_total{model,type}` | 成本 |
| `upstream_request_duration_seconds` | 区分「我们慢」还是「上游慢」 |
| `active_streams` | 判断线程/连接是否耗尽 |
| `http_requests_total{status}` | 错误率 |

**6.2 日志**

- 结构化 JSON 一行一条，字段：`ts, level, request_id, user_id, session_id, model, ttft_ms, tokens, status`；
- **绝不打印 `Authorization` 头和用户消息全文**（合规红线），只记长度和哈希。

**6.3 压测**

```bash
# 重点测 TTFT 与「并发流数量」，不是 QPS。
# k6 脚本需要自己写（核心是 VU 循环里发 stream:true 请求并统计首字节时间），
# 或用 wrk + 简单的 Lua 脚本。
k6 run -e BASE=http://127.0.0.1:8080 -e VUS=50 ./load.js
```

目标（单机 4C8G，云端 API，单请求约 500 token 输出）：

- 50 并发流：TTFT p95 < 2s，错误率 < 0.1%
- 连接数上限：httplib 线程池 64 → 超过就是排队，**这时必须换异步框架**

**6.4 部署**

```bash
docker compose --env-file .env up -d --build
```

- 多阶段构建（本仓库 `Dockerfile`），镜像不含编译器和源码；
- 非 root 运行（uid 10001）；
- `HEALTHCHECK` 打 `/healthz`；
- 优雅退出：收到 SIGTERM → 停止接收新连接 → 等待在途流结束（最长 30s）→ 退出。
  本骨架已实现信号处理，但**在途流的 drain 还没做**，K8s 滚动更新时会掉几个请求，这是阶段 6 要补的。

**上线检查清单**

- [ ] Nginx：`proxy_buffering off`、`proxy_read_timeout 600s`、HTTP/2 开启
- [ ] 限流、配额、鉴权全部生效
- [ ] 备份：PG 每日全量 + WAL 归档；恢复演练做过一次
- [ ] 告警：5xx 比例 > 1%、TTFT p95 > 5s、上游连续失败
- [ ] 回滚：镜像 tag 保留最近 5 个，DB 迁移向前兼容

---

### 阶段 7：进阶能力（按业务需要，持续迭代）

按投入产出比排序，不要一次全做：

1. **RAG（知识库）**：文档解析（PDF/Word）→ 分块（512 token，重叠 64）→ 向量化（bge-m3 / text-embedding-3）→ pgvector 存储 → 混合检索（向量 + 全文 BM25）→ rerank → 拼进 prompt。**先做「检索质量」再做「花哨 UI」。**
2. **工具调用 / Agent**：function calling → 多轮工具循环 → 接 MCP 生态。注意：工具调用的流式事件（`tool_calls` 分片）比纯文本复杂，要单独处理。
3. **本地推理**：把 `llama.cpp` 包成 `llm::Provider`（同一套 Stream 接口），走 `llama_chat_apply_template` + 回调 → 队列。好处是**离线可用、数据不出内网、边际成本为零**。
4. **多模型路由**：按任务类型/成本/可用性路由；失败自动降级到备用模型。
5. **语音**：ASR（whisper.cpp，C++ 友好）+ TTS，做成「按住说话」。
6. **会话分享、导出、提示词模板库**——这些是产品功能，优先级看用户反馈。

---

## 4. 关键实现细节与踩坑清单

1. **SSE 有三层缓冲**：应用层（别用带缓冲的 ostream 拼响应）→ 反向代理（`proxy_buffering off`）→ 浏览器（`fetch` 流式读取）。任何一层缓冲，用户看到的都是「转圈后整段蹦出」。
2. **别在 IO 线程做阻塞操作**：httplib 一个线程服务一个连接，在 handler 里同步查库 = 整个连接卡住。DB 操作丢线程池。
3. **中文流式要按 UTF-8 码点切分**：把多字节字符切断，前端会显示乱码方块。骨架的 mock provider 演示了正确切法。
4. **`stream_options.include_usage`**：不加这个，流式响应拿不到 token 用量，就没法计费。个别兼容实现不支持，要能降级（`AIP_INCLUDE_USAGE=false`）。
5. **上下文裁剪**：按 token 而不是字符截断；裁剪后不能以 `assistant` 开头（部分上游 400）。
6. **重试只发生在「还没吐字」时**：吐了一半再重试，前端会出现重复文本。
7. **HTTP/2 与 SSE**：HTTP/1.1 下浏览器同域只有 6 个连接，开多个对话页会卡住；上 HTTP/2 解决。
8. **密钥安全**：API key 只在环境变量/密钥管理服务里，不落库、不打日志；支持热轮转。
9. **Prompt 注入**：用户消息永远放在 `user` 角色里，不要把用户输入拼进 system prompt；RAG 检索到的文档也要当成「不可信数据」。
10. **优雅退出**：先停止 accept，再等在途请求；直接 `exit()` 会让前端收到截断的流。
11. **时间同步**：JWT、限流窗口、日志都依赖时钟；容器里配 NTP。
12. **别急着上微服务**：单体 + 清晰分层能撑到几十万用户；过早拆分只会让你在网络上 debug。

---

## 5. 测试策略

| 层次 | 测什么 | 工具 |
|---|---|---|
| 单元测试 | 队列语义、上下文拼装、SSE 解析、取消幂等 | GoogleTest / 本仓库 `tests/unit_tests.cpp` |
| 契约测试 | 用「假上游」验证协议兼容（本仓库 `scripts/fake_upstream.py`） | 自建 mock server |
| 集成测试 | 真 PG + 真 Redis，跑完整请求 | docker compose + CTest |
| E2E | 前端到后端的流式链路 | Playwright |
| 压测 | TTFT、并发流、内存曲线 | k6 / wrk |

**必须写的三个测试**（骨架已覆盖前两个）：

1. 上游返回 401/429/断连时，前端能收到 `error` 事件并正常结束；
2. 客户端中途断开，上游连接被取消（否则你会为没人看的内容付费）；
3. 100 轮对话后，上下文裁剪正确、token 不超限。

---

## 6. 排期参考

| 规模 | 人力 | MVP（能内部用） | 上线（多租户+计费） |
|---|---|---|---|
| 内部工具 | 1 人全职 | 2~3 周 | — |
| 小型产品 | 1 人 + 前端 0.5 | 4~6 周 | 8~10 周 |
| 对外 SaaS | 2~3 人 | 6 周 | 10~12 周 |

阶段 0~2 是「能不能跑起来」，阶段 3~4 是「能不能给别人用」，阶段 5~6 是「能不能收钱」。
**大部分团队卡在阶段 4**（多租户与配额），因为这部分没有捷径。

---

## 7. 常见误区

- ❌ 一开始就上 Kubernetes / 微服务 / service mesh —— 你先要有一个能跑的单体；
- ❌ 自己写 HTTP 解析、JSON 库、TLS —— 除非你在做框架；
- ❌ 用 C++ 写管理后台 —— 老老实实用 TS；
- ❌ 先做 RAG 再做流式 —— 流式是体感，RAG 是功能，先让用户觉得「快」；
- ❌ 只测「能返回」，不测「断开、超时、重试」—— 线上事故全在这三处；
- ❌ 忽略成本：一个没做取消的流式网关，能让你在一天内烧掉一个月的预算。

---

## 8. 现在就能做的下一步

```bash
# 1. 跑起来（离线 mock，不需要 API key）
cmake -S . -B build -DAIPLATFORM_BUILD_TESTS=ON && cmake --build build -j
./build/aiplatform &
./scripts/demo.sh

# 2. 接真实模型
export AIP_PROVIDER=openai AIP_API_KEY=sk-xxx AIP_MODEL=deepseek-chat
export AIP_BASE_URL=https://api.deepseek.com/v1
./build/aiplatform

# 3. 跑测试
ctest --test-dir build --output-on-failure
```

建议的下一步顺序：

1. 把 `SessionStore` 换成 PostgreSQL 实现（阶段 3）；
2. 加 `POST /v1/auth/register` + `/v1/auth/login`，把 `user_id` 挂到会话上（阶段 4）；
3. 在 `ApiServer` 里加一层「请求 ID + 结构化日志」，为阶段 6 的可观测性打底；
4. 换 Drogon 或 uWebSockets 做一次 200 并发的压测，确认瓶颈后再决定要不要重构。

> 每一步都保持「可运行 + 可回滚」，比一次性设计出一个完美架构重要得多。
