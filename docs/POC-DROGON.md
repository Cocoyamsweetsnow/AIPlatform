# Drogon SSE POC（决策 0001 的验证步骤）

> 决策背景见 [0001-http-framework.md](adr/0001-http-framework.md)。
> 本文只回答三件事：**怎么验、看到什么算通过、不通过怎么办**。

POC 代码：`poc/drogon_sse.cpp`　压测脚本：`scripts/sse_load.py`

---

## 0. 这个 POC 验证什么

| 问题 | 怎么验 | 通过标准 |
|---|---|---|
| 阻塞拉取的 `llm::Stream` 能否桥接到 Drogon 推送式异步流 | `curl -N /sse` 看是否逐帧输出 | 逐帧到达，最后有 `[DONE]` |
| 客户端断开后能否及时取消上游（省 token） | `timeout 1 curl -N` 后看日志 | 5 秒内出现 `client gone, upstream cancelled` |
| **连接本身还占不占线程** | 300 路 `/idle` 挂 3 秒，采样线程数 | 线程数基本不随 N 增长（< 50） |
| 每路流的真实线程/内存单价 | 200 路 `/sse` | 0 失败；线程数 ≈ 2N（预期，见 3.4） |

> **先说清楚 `/sse` 的 2 个线程从哪来**：一个是 POC 的泵线程（把「阻塞拉取」桥接到「推送流」），
> 一个是 `mock_provider` 内部的读线程（真实环境里就是 libcurl 那根线程）。**换 Drogon 消不掉这两个**，
> 因为上游是阻塞的。Drogon 消掉的是「连接线程」——用 `/idle` 单独验证。
> 这是本 POC 最重要的一个认知，也是决定「要不要现在就把上游改异步」的依据。

**不验证**：鉴权、CORS、限流、优雅退出 drain、真实模型首字延迟。这些是迁移阶段的事，POC 只回答「流式链路能不能成立」。

POC 复用 `src/llm/mock_provider.cpp`（内部就是 `ThreadedStream` + `EventQueue`），所以走的是真实链路，不是假的 sleep 循环。

---

## 1. 装 Drogon（≥ 1.9.14）

> **版本要求**：`HttpResponse::newAsyncStreamResponse` 在 **1.9.0 里还没有**（Debian trixie 的
> `libdrogon-dev` 正是 1.9.0）。本 POC 在 **v1.9.14** 上实测通过。
> 另外一个坑：Debian 那个包的 `DrogonConfig.cmake` 把 pg/sqlite/mysql/brotli/hiredis/yaml 全列成
> 硬依赖，装了也不用得上，建议直接源码编、把 ORM 关掉。

### 路线 A：宿主机源码编译（推荐，版本可控）

本机是 Ubuntu 22.04 / GCC 11.4 / 4 核。协程需要 C++20，本 POC 不用协程。

```bash
# 1) 依赖，约 1 分钟（关掉 ORM 后只需要这四个 -dev）
sudo apt-get update && sudo apt-get install -y \
  build-essential cmake libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev

# 2) 编 Drogon + trantor，4 核 -j2 实测约 7 分钟
git clone --depth 1 --branch v1.9.14 --recursive \
  https://github.com/drogonframework/drogon /tmp/drogon
cmake -S /tmp/drogon -B /tmp/drogon/build -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_CTL=OFF \
  -DBUILD_POSTGRESQL=OFF -DBUILD_MYSQL=OFF -DBUILD_SQLITE=OFF -DBUILD_REDIS=OFF \
  -DUSE_SUBMODULE=ON
cmake --build /tmp/drogon/build -j2

# 3) 安装（默认 /usr/local），约 1 分钟
sudo cmake --install /tmp/drogon/build
sudo ldconfig
```

**预期**：`/usr/local/lib/cmake/Drogon/DrogonConfig.cmake` 存在，`ldconfig -p | grep drogon` 有输出。
**失败先看**：缺 `uuid-dev` / `libjsoncpp-dev`；或内存不足（本机 3.7G，`-j2` 实测峰值 ~1.5G，够用）。

### 路线 B：容器里源码编译（不需要宿主 sudo）

```bash
# 1) 构建镜像（约 8 分钟，-j2）
docker run --name drogon-build -v "$PWD":/src -w /src debian:trixie-slim bash -c '
  apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates \
    libcurl4-openssl-dev libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev && \
  git clone --depth 1 --branch v1.9.14 --recursive \
    https://github.com/drogonframework/drogon /opt/drogon && \
  cmake -S /opt/drogon -B /opt/drogon/build -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_CTL=OFF \
    -DBUILD_POSTGRESQL=OFF -DBUILD_MYSQL=OFF -DBUILD_SQLITE=OFF -DBUILD_REDIS=OFF \
    -DUSE_SUBMODULE=ON && \
  cmake --build /opt/drogon/build -j2 && \
  cmake --install /opt/drogon/build && ldconfig && \
  cmake -S . -B build-poc -DAIPLATFORM_BUILD_POC=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo && \
  cmake --build build-poc -j2'

# 2) 固化成镜像，之后可以反复起（不需要重装依赖）
docker commit drogon-build poc-drogon-img
docker rm drogon-build

# 3) 起服务
docker run -d --name poc-drogon -p 8081:8081 -v "$PWD":/src -w /src \
  poc-drogon-img ./build-poc/poc_drogon_sse 8081
```

注意：容器里编的二进制**只在同镜像里能跑**（依赖 /opt/drogon 下的 .so），不要拷回宿主机。
若宿主机同时装了 Debian 的 `libdrogon-dev`，配置时用 `-DDrogon_DIR=/usr/local/lib/cmake/Drogon` 指定版本。

---

## 2. 编译 POC

```bash
cmake -S . -B build-poc -DAIPLATFORM_BUILD_POC=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DDrogon_DIR=/usr/local/lib/cmake/Drogon     # 只在系统里存在多个 Drogon 时需要
cmake --build build-poc -j2
```

预期：生成 `build-poc/poc_drogon_sse`，约 30 秒（Drogon 装好之后）。
失败先看：`find_package(Drogon)` 找不到 → 检查 `DrogonConfig.cmake` 是否在 `/usr/local/lib/cmake/Drogon`；运行时找不到 `.so` → `sudo ldconfig`。

POC 默认监听 **8081**，不影响 `./build/aiplatform` 的 8080。

---

## 3. 四个验收用例

### 3.1 流式输出

```bash
./build-poc/poc_drogon_sse 8081
# 另一个终端：
curl -sN 'http://127.0.0.1:8081/sse?text=你好' | head -4
```

预期（`-N` 必须加，否则 curl 会缓冲，看不出流式）：

```
data: {"choices":[...],"created":...,"id":"chatcmpl-poc-1","model":"mock","object":"chat.completion.chunk"}

data: {"choices":[{"delta":{"content":"这是 "},...}],...}
```

**关注点**：是「一格一格」到达，还是一次性刷出来。后者说明中间某层在缓冲（Drogon 自己不会，看是否被反向代理缓冲）。

### 3.2 非流式（验证「不能阻塞 IO 线程」）

```bash
curl -s http://127.0.0.1:8081/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"message":"你好","stream":false}'
```

预期：`{"answer":"这是 mock 模型的流式回复。你说的：你好",...}`。

这一条验证的是 POC 里「工作线程拉完 + `queueInLoop` 回调」的写法。如果直接照搬 `server.cpp` 在 handler 里跑阻塞循环，4 个 IO 线程会被 4 个请求全部占死——**这是迁移时最容易漏的坑**。

### 3.3 断连取消（最容易被忽略）

```bash
timeout 1 curl -sN 'http://127.0.0.1:8081/sse?text=慢慢说' > /dev/null
```

预期：约 1 秒后服务端 stderr 出现：

```
[poc] client gone, upstream cancelled: chatcmpl-poc-N
```

**计时**：如果这条日志出现在 5 秒之后，说明 Drogon 要等到下一次 `send()` 或心跳才发现断连。生产心跳是 15s，那就等于**断开后最多 15 秒还在烧 token**。这是决定「要不要额外做 close 回调」的依据，请把实际时延记到本文末尾。

### 3.4a 真实链路：200 路 `/sse`

```bash
python3 scripts/sse_load.py -n 200 --pid "$(pgrep -f poc_drogon_sse)"
```

实测（2026-11-02，4 核容器）：

```
成功     : 200   失败: 0
TTFT     : p50 27ms  p95 34ms  max 39ms
帧数     : 13 frame/流
线程数   : 峰值 405（= 2×200 + Drogon 自身 5）
RSS      : 峰值 19.8 MB
```

**线程数 ≈ 2N 是预期结果，不是失败**：每路流 = 泵线程 + mock 的上游读线程。
这条测的是「能不能稳定扛住 200 路」和「每路的线程/内存单价」，用来给「要不要把上游改异步」提供数字。

### 3.4b 纯连接：300 路 `/idle` 挂 3 秒

```bash
python3 scripts/sse_load.py -n 300 --path /idle --hold 3 --pid "$(pgrep -f poc_drogon_sse)"
```

实测：

```
成功     : 300   失败: 0
TTFT     : p50 13ms  p95 19ms
线程数   : 峰值 5（不随 N 增长）
RSS      : 峰值 20.5 MB
```

**这条才是 Drogon 相对 httplib 的真实收益**：httplib 下 300 个挂着的连接会占满 64 线程池、第 65 个开始排队；
Drogon 下 300 个连接只有 5 个线程。注意 POC 的 `/idle` 不回收已断开的流（会留下极少量内存），跑完重启进程即可。

> 容器内跑压测时，`--pid` 要用容器内的 PID（通常是 1）：`docker exec poc-drogon python3 /src/scripts/sse_load.py -n 300 --pid 1`。

---

## 4. 通过 / 不通过

**通过** → 我把 ADR 状态改成「已接受」，然后按小步迁移 `src/http/server.cpp`（业务层不动）。

**不通过** → 对照处理：

| 症状 | 可能原因 | 改法 |
|---|---|---|
| 高并发下崩溃 / 数据错乱 | `ResponseStream::send` 不能跨线程调用 | 不直接 send：每流绑定 `drogon::app().getIOLoop(i)`，用 `queueInLoop` 发送 |
| 断开后很久才取消上游 | 没有 close 回调，只能等 send 失败 | 缩短心跳；或查当前 Drogon 版本是否暴露连接关闭回调 |
| RSS 随流数线性上涨 | 推式流没有阻塞背压，慢客户端积压 | 限制单流在途字节 / 单实例并发上限 |
| 首字节被掐断 | trantor kickoff timeout | POC 已设 `disableKickoffTimeout=true`；仍触发就查 trantor 版本 |
| 编不过 | 缺依赖 / C++ 标准不一致 | 先单独编一个 hello-drogon 排除环境问题 |

---

## 5. 实测结果（2026-11-02）

环境：Debian trixie 容器 / g++ 14.2 / 4 核 / Drogon **v1.9.14**（源码编，ORM 全关）。

| 项 | 实测 |
|---|---|
| Drogon 版本 | v1.9.14 |
| 200 路 `/sse`：成功/失败 | 200 / 0 |
| 200 路 `/sse`：线程数峰值 | **405**（≈ 2N + 5） |
| 200 路 `/sse`：TTFT p95 | 34 ms（服务端本地，mock 每 25ms 一个 delta） |
| 200 路 `/sse`：RSS 峰值 | 19.8 MB |
| 300 路 `/idle`：成功/失败、线程数峰值 | 300 / 0，**5 线程** |
| 300 路 `/idle`：RSS 峰值 | 20.5 MB |
| 断连 → 取消时延 | **1.03 s**（mock 每 25ms 发一次，下一次 send 立刻失败） |

**结论：POC 通过。** 两个关键结论：

1. `newAsyncStreamResponse` + 工作线程 `send()` 在 200 路下稳定，没有崩溃或数据错乱；
2. 「连接不占线程」成立（300 路连接只多 5 个线程），但「每路活跃流仍占 2 个线程」也成立——
   所以迁移只解除 64 路连接上限，不降低单流线程数，后续要按 ADR 里的第 2/3 步继续降。

**断连时延的边界**：1.03s 是因为 mock 每 25ms 就有一个 delta，下一次 `send()` 立刻发现连接已断。
真实模型在两个 token 之间可能停几秒，此时取消会推迟到下一个 token 或心跳（POC 设 5s，生产建议 15s）。
也就是说：**最坏情况 = 心跳周期**，这个要写进迁移后的 SLA 说明。

---

## 6. 清理

```bash
rm -rf build-poc
# 如果不再需要：sudo rm -rf /usr/local/lib/cmake/Drogon /usr/local/lib/libdrogon* /usr/local/include/drogon
```
