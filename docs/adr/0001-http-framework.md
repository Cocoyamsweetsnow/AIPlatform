# 0001 采用 Drogon 作为 HTTP 服务框架

- 状态：**已接受**（POC 已通过，实测数据见 [docs/POC-DROGON.md](../POC-DROGON.md) 第 5 节）
- 日期：2026-11-02
- 决策者：项目负责人（AI 建议）
- 影响范围：`src/http/`、`main.cpp`、`CMakeLists.txt`、`Dockerfile`；业务层 `llm/` `chat/` 不动

## 背景

本项目是 C++20 的 AI 对话后端，核心链路是 **SSE 长连接**：一次对话从首字到结束，连接要保持几秒到几分钟。

阶段 0~2 用 cpp-httplib 跑通，代价是它**每连接一线程**：

- `CPPHTTPLIB_THREAD_POOL_COUNT=64` 是硬上限，64 路并发流之后新连接排队；
- 每路流实际占 **2 个线程**（连接线程 + `ThreadedStream` 的上游读线程）；
- 线程只在「拉」的时候有用，等 token 的时间全在阻塞。

路线图 `docs/ROADMAP.md` 阶段 1 原本就规划了 **httplib 起步 → Drogon 生产**。本 ADR 不是推翻原决策，而是把迁移提前：仓库当前 0 个 commit、0 个用户、HTTP 层 393 行，是迁移成本最低的时点；再往后，鉴权、配额、请求 ID、指标都会长在 `server.cpp` 里，要重写两遍。

## 考虑过的方案

| 方案 | 优点 | 代价 | 结论 |
|---|---|---|---|
| **cpp-httplib（现状）** | 单头文件、零依赖、编译秒级 | 每连接一线程，并发流上限 = 线程池大小 | 保留为回退项 |
| **Drogon 1.9.x** | reactor + 异步流、C++20 协程、filter、WebSocket、生产级 | 依赖重、首次编译分钟级、Ubuntu 22.04 无包 | **本 ADR 选择** |
| uWebSockets | 吞吐/内存最优 | API 底层、生态小、文档少 | 不到万级连接不考虑 |
| Boost.Beast | 完全可控 | 协议细节全自己写，bug 面大 | 否决 |

关键的增量信息：Drogon 只解决「连接不占线程」，**不解决「上游不占线程」**。`ThreadedStream` 仍然是每路一个线程；而且把「阻塞拉取」桥接到「推送流」还需要一个泵线程，所以 **POC 的 `/sse` 路径是 2 线程/流**（`/idle` 路径才是 0 线程/连接）。这意味着迁移后的并发上限从「64 路」变成「你能开多少线程」（几百到 1~2k），不是无限的。

降线程分三步走，本 ADR 只做第一步：

1. **迁移 HTTP 层**（本 ADR）：连接不占线程，SSE 仍 2 线程/流；
2. 把 `llm::Stream` 从「拉」改成「推」（生产者线程直接 send）：**1 线程/流**；
3. 上游改非阻塞（`curl_multi` 或 Drogon 异步客户端 + 协程）：**0 线程/流**。

第 2、3 步不在本次范围内，等 POC 与压测数据出来再单独决策。

## 验证结果（POC，2026-11-02）

环境：Drogon **v1.9.14**（源码编，ORM 全关）、Debian trixie 容器、g++ 14.2、4 核。

| 场景 | 结果 |
|---|---|
| 200 路 `/sse`（真实 `llm::Stream` 链路） | 200/200 成功，TTFT p95 34ms，线程峰值 405，RSS 19.8MB |
| 300 路 `/idle`（只有连接、没有上游） | 300/300 成功，线程峰值 **5**，RSS 20.5MB |
| 断连取消 | **1.03s** 后发现客户端断开并取消上游 |
| 非流式（工作线程 + `queueInLoop`） | 正常返回，未阻塞 IO 线程 |

两个必须记住的边界：

- **版本下限**：`newAsyncStreamResponse` 在 1.9.0 里不存在（Debian trixie 的 `libdrogon-dev` 就是 1.9.0），
  必须 **≥ 1.9.14**（或至少验证过含该 API 的版本）；且 Debian 包的 `DrogonConfig.cmake` 会把
  pg/sqlite/mysql/brotli/hiredis/yaml 全当硬依赖，建议源码编并关掉 ORM。
- **断连时延取决于下一次 `send()`**：mock 每 25ms 一个 delta 时是 1.03s；真实模型两个 token 之间
  可能停几秒，最坏 = 一个心跳周期（POC 设 5s，生产建议 15s）。

## 决定

**迁移到 Drogon，但分两步走：**

1. **POC 先行**（`poc/drogon_sse.cpp`）：只验证三件事——阻塞流能否桥接推送流、断连能否及时取消上游、200 路并发的线程/内存。通过标准见 `docs/POC-DROGON.md`。
2. **POC 通过后**，重写 `src/http/server.{h,cpp}`，保持 API 契约不变（`scripts/demo.sh` 必须全绿），业务层一行不改。

**明确不采用的部分：**

- **不用 Drogon ORM**：数据访问仍是 `libpqxx` + 手写 SQL（见 D3），Drogon 只当 HTTP 层。
- **不换 JSON 库**：Drogon 依赖 jsoncpp，但业务序列化继续用 nlohmann；只在 Drogon 的 `newHttpJsonResponse` 边界上做转换（或干脆不用它，自己 `setBody`）。
- **不引入 Drogon 的 controller 代码生成**：三个端点，手写 handler 更直观。

## 后果

**正面**

- 连接不再占线程，SSE 并发从 64 提升到「线程数允许的量级」，内存曲线从线性变近平；
- 后续阶段的中间件（鉴权、限流、请求 ID、CORS）用 `registerPreRoutingAdvice` / filter 实现，比现在手写 pre-routing 干净；
- 自带 SIGTERM/SIGINT 优雅退出、CORS 助手、WebSocket，阶段 5/6 少写不少代码；
- 业务层零改动，迁移可以被 `scripts/demo.sh` 完整回归。

**负面**

- 依赖变重：Drogon + trantor + jsoncpp + uuid + openssl + zlib；Ubuntu 22.04 官方源没有现成包，需要源码编（4 核约 10 分钟）；
- 镜像基础从 `debian:bookworm-slim` 换成 trixie（有 `libdrogon-dev 1.9.0`），或加一个源码编译 stage；
- 每个 TU 都要吃 Drogon 头文件，`server.cpp` 的单次重编从 2 秒变几十秒；
- SSE 从「拉」变「推」：失去 `sink.write` 的天然背压，慢客户端要靠自己限速；
- 断连检测依赖下一次 `send()` 失败，最坏晚一个心跳周期才取消上游（POC 要量化这个时延）；
- **迁移本身不降低每路流的线程数**（仍 2 个），它只解除「64 路」这个连接层上限；数量级提升要等上面的第 2/3 步。

**触发重新评估的条件**

- POC 不通过（`ResponseStream::send` 不能跨线程 / 断连无法在可接受时延内取消 / 内存不符合预期）→ 回退 httplib，或改评估 uWebSockets；
- 压测显示目标并发 < 100 路 → 迁移收益不足以覆盖依赖与构建成本，保持 httplib；
- Drogon 在目标交付环境（客户机房、国产化 OS）装不上 → 走回退项。

## 参考

- Drogon `HttpResponse::newStreamResponse` / `newAsyncStreamResponse` 语义：<https://github.com/drogonframework/drogon/blob/master/lib/inc/drogon/HttpResponse.h>
- Debian trixie `libdrogon-dev 1.9.0`：<https://packages.debian.org/trixie/libdrogon-dev>
