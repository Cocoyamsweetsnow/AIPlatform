# AIPlatform

用 C++20 写的 AI 对话平台后端骨架：**HTTP API + 流式（SSE）对话 + 可插拔模型网关**。
配套的完整开发路线图见 [docs/ROADMAP.md](docs/ROADMAP.md)。

这个仓库解决的是「从 0 到第一个能跑通、能演示、能接真实模型」的那一段，
所有代码都围绕一个目标：**让流式对话链路先正确地跑起来**。

## 已实现（阶段 0~2 + 内置演示前端）

- ✅ HTTP 服务（**Drogon**，连接不占线程）：`/healthz`、`/v1/config`、`/v1/chat/completions`、`/v1/sessions/{id}/messages`
- ✅ 内置 Web UI（零构建单页，编译进二进制）：流式对话、Markdown / 代码块、会话本地持久化、明暗主题、Token 设置
- ✅ SSE 流式输出，OpenAI 兼容的 chunk 格式（前端工具链可直接复用）
- ✅ 模型网关：`Provider` 抽象 + OpenAI 兼容实现 + 离线 `mock` 实现
- ✅ 上游 SSE 增量解析、非流式自动降级、上游错误映射
- ✅ 客户端断开 → 取消上游请求（不浪费 token）
- ✅ 多轮上下文（内存会话存储，带 LRU 淘汰）
- ✅ Bearer Token 鉴权、CORS、请求日志、统一错误码
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
  "auth_required": false, "web": true,
  "max_message_bytes": 32768, "max_streams": 512
}
```

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
  在途流达到 `AIP_MAX_STREAMS` 时直接返回 503（`too_many_streams`），不会排队。
- `stream: false` → 一个 `chat.completion` JSON，含 `usage`（在 WorkerPool 里跑，不占 IO 线程）。

### `GET /v1/sessions/{id}/messages`

返回该会话最近 100 条消息。

### `GET /healthz`

```jsonc
// 正常
{"status":"ok","sessions":3,"active_streams":0,"max_streams":512,
 "model":"deepseek-chat","provider":"mock","web":true,"http":"drogon"}
// 正在退出（仍返回 200，避免 liveness 探针误杀；LB 应看 draining 摘流量）
{"status":"draining","draining":true,...}
```

### `GET /v1/config`

前端启动用的公开配置：是否要求 Token、模型名、消息大小上限等。**不含** `api_key`、
`base_url`、system prompt。消息读取上限由 `/v1/config` 返回，前端据此在发送前提示，
服务端仍会独立校验。

### 内置静态资源

`GET /`、`/index.html`、`/assets/*`、`/favicon.svg` 由二进制内的资源表直接返回，
不读文件系统。启用 `AIP_TOKEN` 时这些路径仍然公开（否则页面本身打不开），
`/v1/chat/completions` 和会话历史接口照常鉴权；`AIP_WEB=false` 时不再暴露。

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
│   ├── event_queue.h         生产者-消费者队列
│   ├── threaded_stream.h     后台线程 + 队列模板
│   ├── openai_provider.cpp   OpenAI 兼容 + SSE 解析 + 取消
│   └── mock_provider.cpp     离线假模型
├── chat/                     业务层（上下文、会话存储）
└── http/                     HTTP 层（两个后端，API 契约一致）
    ├── server.h              对外接口（PIMPL，不暴露框架类型）
    ├── public_config.h       /v1/config 的公开字段
    ├── server_drogon.cpp     主线：Drogon + 泵线程 + 准入 + drain + 内置 UI
    └── server_httplib.cpp    回退：cpp-httplib（-DAIPLATFORM_HTTP=httplib）
```

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

路线图里列了从阶段 3 到阶段 7 的详细步骤：PostgreSQL 持久化 → 多租户与配额 →
可观测性与压测 → 上线 → RAG / 工具调用 / 本地推理。

内置单页已经覆盖「演示 + 联调」；生产前端是否复用 Open WebUI / 自研 Next.js，
仍按 [docs/DECISIONS.md](docs/DECISIONS.md) 的 D6 由人拍板。

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
- Web UI 没有 `package.json` / `node_modules`：`web/` 由 CMake 在构建期转成 C++
  raw string（见 [cmake/embed_web.cmake](cmake/embed_web.cmake)），改完前端重新编译即可。
  页面 CSP 只允许同源脚本；模型输出按不可信文本处理（先转义再渲染，链接只放行
  `http(s)` / `mailto`），不使用 `innerHTML` 直接插入原始内容。
