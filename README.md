# AIPlatform

用 C++20 写的 AI 对话平台后端骨架：**HTTP API + 流式（SSE）对话 + 可插拔模型网关**。
配套的完整开发路线图见 [docs/ROADMAP.md](docs/ROADMAP.md)。

这个仓库解决的是「从 0 到第一个能跑通、能演示、能接真实模型」的那一段，
所有代码都围绕一个目标：**让流式对话链路先正确地跑起来**。

## 已实现（阶段 0~2）

- ✅ HTTP 服务：`/healthz`、`/v1/chat/completions`、`/v1/sessions/{id}/messages`
- ✅ SSE 流式输出，OpenAI 兼容的 chunk 格式（前端工具链可直接复用）
- ✅ 模型网关：`Provider` 抽象 + OpenAI 兼容实现 + 离线 `mock` 实现
- ✅ 上游 SSE 增量解析、非流式自动降级、上游错误映射
- ✅ 客户端断开 → 取消上游请求（不浪费 token）
- ✅ 多轮上下文（内存会话存储，带 LRU 淘汰）
- ✅ Bearer Token 鉴权、CORS、请求日志、统一错误码
- ✅ 优雅退出（SIGTERM）、单元测试、Docker 多阶段构建

## 快速开始

> 完整的编译 / 运行 / 镜像构建 / 排错手册见 [docs/BUILD.md](docs/BUILD.md)。

```bash
# 依赖：CMake ≥ 3.20、支持 C++20 的编译器、libcurl 开发包
# Ubuntu: sudo apt install build-essential cmake pkg-config libcurl4-openssl-dev

cmake -S . -B build -DAIPLATFORM_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure

# 离线模式（mock 模型，不需要 API key），默认监听 :8080
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

接真实模型（任何 OpenAI 兼容接口）：

```bash
export AIP_PROVIDER=openai
export AIP_BASE_URL=https://api.deepseek.com/v1
export AIP_API_KEY=sk-xxxxxxxx
export AIP_MODEL=deepseek-chat
./build/aiplatform
```

完整配置项见 [config/config.example.env](config/config.example.env)。

## API

### `POST /v1/chat/completions`

```jsonc
{
  "message": "你好",           // 必填；也兼容 OpenAI 的 messages 数组（取最后一条 user）
  "session_id": "s_xxx",       // 可选；为空则新建会话，ID 通过响应头 X-Session-Id 返回
  "model": "deepseek-chat",    // 可选
  "stream": true               // 默认 true
}
```

响应：

- `stream: true` → `text/event-stream`，每个 chunk 是 OpenAI 兼容的 `chat.completion.chunk`，
  结束时发送 `data: [DONE]`；上游出错时发送 `event: error` 再 `[DONE]`。
- `stream: false` → 一个 `chat.completion` JSON，含 `usage`。

### `GET /v1/sessions/{id}/messages`

返回该会话最近 100 条消息。

### `GET /healthz`

`{"status":"ok","sessions":3,"model":"deepseek-chat","provider":"mock"}`

### 错误格式

```json
{ "error": { "type": "invalid_request", "message": "message 不能为空" } }
```

| HTTP | type | 场景 |
|---|---|---|
| 400 | `invalid_request` / `invalid_json` | 参数缺失、JSON 非法 |
| 401 | `unauthorized` | 缺少或错误的 Bearer Token |
| 413 | `payload_too_large` | 单条消息超过 32KB |
| 502 | `upstream_error` | 上游模型报错 |
| 504 | `timeout` | 上游超时 |

## 验证

```bash
./scripts/demo.sh                    # 冒烟测试（健康检查 / 流式 / 多轮 / 历史 / 错误码）
python3 scripts/fake_upstream.py &   # 假上游，验证 OpenAI 协议与错误路径
```

## 目录结构

```
src/
├── main.cpp                  组装依赖、信号处理
├── config.{h,cpp}            环境变量配置
├── common/log.h              极简日志（后续换 spdlog）
├── util/id.{h,cpp}           随机 ID / 时间戳
├── llm/                      模型网关
│   ├── types.h               Provider / Stream / Event 接口
│   ├── event_queue.h         生产者-消费者队列
│   ├── threaded_stream.h     后台线程 + 队列模板
│   ├── openai_provider.cpp   OpenAI 兼容 + SSE 解析 + 取消
│   └── mock_provider.cpp     离线假模型
├── chat/                     业务层（上下文、会话存储）
└── http/server.{h,cpp}       路由、鉴权、SSE 编码
```

## 设计要点

**为什么用「后台线程 + 阻塞队列」而不是回调？**
HTTP 框架的写接口本身就是拉取式（`DataSink`），拉取语义既能适配同步框架，也能适配协程框架。

**为什么默认 `AIP_PROVIDER=mock`？**
让前端开发、CI、演示都不依赖网络和 API key。真实项目里它就是集成测试的 stub。

**为什么用 cpp-httplib 而不是 Drogon？**
起步阶段编译快（秒级）、依赖少（无）。但它**每连接一线程**，每个 SSE 长连接独占一个线程，
并发上限 = 线程池大小（本项目设为 64）。要支撑数百上千并发长连接，需换异步框架，
详见 [docs/ROADMAP.md](docs/ROADMAP.md) 阶段 2 与阶段 6。

> **2026-11-02 更新**：已决定迁移到 Drogon（提前执行路线图的 D1），**POC 已通过**
> （200 路 SSE / 300 路空闲连接实测，见 [docs/POC-DROGON.md](docs/POC-DROGON.md) 第 5 节），
> 决策记录见 [docs/adr/0001-http-framework.md](docs/adr/0001-http-framework.md)。
> 主线代码在 `src/http/` 重写完成前仍是 httplib；Drogon 要求 **≥ 1.9.14**。

## 下一步

路线图里列了从阶段 3 到阶段 7 的详细步骤：PostgreSQL 持久化 → 多租户与配额 →
前端 → 可观测性与压测 → 上线 → RAG / 工具调用 / 本地推理。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/ROADMAP.md](docs/ROADMAP.md) | 从零到上线的 8 个阶段、架构、DDL、踩坑清单 |
| [docs/DECISIONS.md](docs/DECISIONS.md) | 架构选型决策清单（待拍板表格） |
| [docs/BUILD.md](docs/BUILD.md) | 编译、运行、镜像构建、排错手册 |
| [docs/GIT.md](docs/GIT.md) | Git 工作流、提交规范、发布与事故处理 |

## 说明

- `third_party/` 里是两个单头文件库（cpp-httplib 0.18.3、nlohmann/json 3.11.3），
  直接随仓库分发，避免依赖管理工具带来的首次构建开销。
  `third_party/nlohmann/json.hpp` 是个转发头，让 `#include <nlohmann/json.hpp>` 命中仓库内版本
  （此前能编过是因为构建机恰好装了系统包，换机器就挂）。
- 会话存储目前是**进程内内存**，重启即丢——这是有意的阶段划分，不是 bug。
