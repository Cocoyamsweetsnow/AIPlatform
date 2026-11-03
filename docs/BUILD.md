# 编译、运行与镜像构建（操作手册）

> 本文件只写「你要执行的命令」，代码与设计说明见 [ROADMAP.md](ROADMAP.md)。
> 所有命令都在仓库根目录 `AIPlatform/` 下执行。

---

## 1. 编译（本机，Ubuntu 22.04 / 24.04）

### 1.1 安装依赖

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libcurl4-openssl-dev \
                    libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev
```

#### 装 Drogon（主线，**≥ 1.9.14**）

Ubuntu 22.04 官方源没有 Drogon；Debian trixie 只有 1.9.0，而
`HttpResponse::newAsyncStreamResponse`（SSE 的基础）在 1.9.0 里还不存在。
所以源码编译，约 7~10 分钟：

```bash
git clone --depth 1 --branch v1.9.14 --recursive \
  https://github.com/drogonframework/drogon /tmp/drogon

cmake -S /tmp/drogon -B /tmp/drogon/build -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_CTL=OFF \
  -DBUILD_POSTGRESQL=OFF -DBUILD_MYSQL=OFF -DBUILD_SQLITE=OFF -DBUILD_REDIS=OFF \
  -DUSE_SUBMODULE=ON
cmake --build /tmp/drogon/build -j2      # 4 核 3.7G 机器上 -j2 峰值约 1.5G
sudo cmake --install /tmp/drogon/build
sudo ldconfig
```

验证：`ls /usr/local/lib/cmake/Drogon/DrogonConfig.cmake` 存在。

#### 不想装 Drogon：切回 httplib 回退后端

```bash
cmake -S . -B build -DAIPLATFORM_HTTP=httplib -DAIPLATFORM_BUILD_TESTS=ON
```

两个后端的 API 契约完全一致（`scripts/demo.sh` 必须全绿），区别见
[adr/0001-http-framework.md](adr/0001-http-framework.md)：httplib 每连接一线程，
并发流上限 = 线程池（64），没有准入控制与 drain。

> 其余依赖都是仓库内的单头文件库（`third_party/`），**不需要** vcpkg / conan。
> GCC 11 及以上即可（代码没用到 `std::format` 这类 GCC 13 才有的特性）。

### 1.2 编译 + 测试

```bash
# 我上次编译留下的 build/ 可以删掉，重新来过更干净
rm -rf build

cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DAIPLATFORM_BUILD_TESTS=ON \
      -DAIPLATFORM_HTTP=drogon        # 默认值；没装 Drogon 时改成 httplib

cmake --build build -j2

# 单元测试（9 项，约 0.4 秒）
ctest --test-dir build --output-on-failure
```

产物：

| 路径 | 说明 |
|---|---|
| `build/aiplatform` | 服务端二进制（**内置 Web UI**） |
| `build/unit_tests` | 单元测试（开了 `-DAIPLATFORM_BUILD_TESTS=ON` 才有） |
| `build/generated/web_assets.cpp` | 由 `web/` 自动生成的嵌入资源，不用手改 |

前端不需要 npm / node：`cmake --build` 时会执行
[cmake/embed_web.cmake](../cmake/embed_web.cmake)，把 `web/` 下的 HTML/CSS/JS 生成成
C++ raw string 编进二进制。改完 `web/` 重新 `cmake --build build -j2` 即可。

### 1.3 编译报错对照

| 报错 | 原因 / 处理 |
|---|---|
| `Could NOT find CURL` | 缺 `libcurl4-openssl-dev`，或 `pkg-config` 没装 |
| `#include <curl/curl.h>` 找不到 | 同上 |
| `Could not find a package configuration file provided by "Drogon"` | 没装 Drogon：按 1.1 编译安装，或加 `-DAIPLATFORM_HTTP=httplib` |
| `'newAsyncStreamResponse' is not a member of 'drogon::HttpResponse'` | Drogon 版本太低（< 1.9.14），重装 |
| `error: 'std::span' ...` | 编译器太老，`g++ --version` 需 ≥ 11 |
| `undefined reference to curl_easy_init` | 链接顺序/静态库问题，重跑一次 `cmake -S . -B build` |
| 运行时 `error while loading shared libraries: libdrogon.so` | 装完 Drogon 没执行 `sudo ldconfig` |

---

## 2. 运行

### 2.1 离线模式（mock 模型，不需要 API Key）

```bash
./build/aiplatform
# 日志里会出现: HTTP 服务启动: http://0.0.0.0:8080

# 浏览器打开内置对话页（无需 npm / 静态服务器）
#   http://127.0.0.1:8080/
# 只想提供 API：AIP_WEB=false ./build/aiplatform

# 另开终端
curl -N http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"message":"你好"}'
```

### 2.2 接真实模型

```bash
export AIP_PROVIDER=openai
export AIP_BASE_URL=https://api.deepseek.com/v1
export AIP_API_KEY=sk-xxxxxxxx
export AIP_MODEL=deepseek-chat
./build/aiplatform
```

其他常见上游：

| 服务 | AIP_BASE_URL | AIP_MODEL |
|---|---|---|
| DeepSeek | `https://api.deepseek.com/v1` | `deepseek-chat` |
| 通义千问 | `https://dashscope.aliyuncs.com/compatible-mode/v1` | `qwen-plus` |
| 本地 vLLM | `http://127.0.0.1:8000/v1` | 你部署的模型名 |
| Ollama | `http://127.0.0.1:11434/v1` | `qwen2.5:7b` |

全部配置项见 [../config/config.example.env](../config/config.example.env)。

### 2.3 冒烟测试（一次性验证全链路）

```bash
./scripts/demo.sh http://127.0.0.1:8080
```

会依次验证：健康检查 → 流式对话 → 多轮上下文 → 会话历史 → 错误码（400/404）。

### 2.4 并发冒烟（秒级，不是正式压测）

```bash
# 200 路真实 SSE（服务在容器里时，--pid 用容器内的 1）
python3 scripts/sse_load.py -n 200 \
  --base http://127.0.0.1:8080 \
  --path /v1/chat/completions --body '{"message":"压测"}'
```

判据：`失败 = 0`；线程数峰值 ≈ 2×N（每路流仍是泵线程 + 上游读线程）。
正式的 k6 / wrk 长时压测请由人执行。

### 2.5 验证优雅退出（drain）

```bash
./build/aiplatform &          # 另开一个终端跑 200 路冒烟
kill -TERM %1
# 日志应出现：收到退出信号 → 在途流已全部结束，退出 → 服务已退出
```

若在途流超过 `AIP_SHUTDOWN_DRAIN_S`（默认 30s），会打印「退出超时」并强制关闭。

---

## 3. Docker 镜像构建

### 3.1 构建与运行

```bash
# 主线（Drogon）：工具链一层、Drogon 一层、应用一层、运行层只带 .so + 二进制
# 首次约 10 分钟（编 Drogon），之后改业务代码只需 1 分钟（Drogon 层命中缓存）
docker build -t aiplatform:dev .

# 机器核多可以加速编译层（默认 -j2，避免打满小机器）
docker build -t aiplatform:dev --build-arg BUILD_JOBS=4 .

# 回退后端（httplib）：完全不编 Drogon，镜像更小，功能和主线一致
docker build --target runtime-httplib -t aiplatform:httplib .

# 运行
cp config/config.example.env .env
# 编辑 .env，至少填 AIP_API_KEY（想先用 mock 就不用改）
docker run --rm -p 8080:8080 --env-file .env aiplatform:dev
# 浏览器打开 http://127.0.0.1:8080/ —— Web UI 已随二进制嵌入镜像，无需额外静态目录

# 或者用 compose
docker compose --env-file .env up -d --build
docker compose logs -f api
```

> 两个 target 的区别：`--target runtime-httplib` 不会被 Drogon 那层拖住，
> 适合装不上 Drogon 的交付环境。
> **改了 Drogon 版本**才需要重建 Drogon 层：`--build-arg DROGON_VERSION=v1.9.15`。
> 运行层有一条 `ldd` 门禁：缺任何 `.so` 会让镜像构建直接失败，而不是部署后才发现。

### 3.2 国内网络加速（构建慢 / 卡在 apt 时）

**a) Docker Hub 镜像加速**（拉基础镜像快）：

```bash
sudo mkdir -p /etc/docker
sudo tee /etc/docker/daemon.json <<'EOF'
{
  "registry-mirrors": ["https://docker.mirrors.ustc.edu.cn", "https://hub-mirror.c.163.com"]
}
EOF
sudo systemctl daemon-reload && sudo systemctl restart docker
```

**b) 构建阶段 apt 换源**（Debian 官方源慢时）：

```bash
# 生成一份临时 Dockerfile，把 deb.debian.org 换成国内镜像
sed 's|deb.debian.org|mirrors.aliyun.com|g' Dockerfile > Dockerfile.cn
docker build -f Dockerfile.cn -t aiplatform:dev .
rm -f Dockerfile.cn        # 不要提交这份临时文件
```

> **本机实测（2026-11-02）**：到 `deb.debian.org` 约 110 KB/s，单次 `apt-get update`
> 拉 8.8MB 索引就要 80 秒以上；再加上 `git clone` Drogon 和编译，这台机器上完整构建
> 不现实（镜像构建未在本机完成验证）。网络正常时首次约 10 分钟、后续约 1 分钟。
> 换源或换网络后，用 3.3 验证。

### 3.3 镜像验证

```bash
docker run -d --name aip-test -p 8080:8080 aiplatform:dev
curl -sS http://127.0.0.1:8080/healthz     # {"status":"ok",...,"http":"drogon"}
./scripts/demo.sh http://127.0.0.1:8080   # 全链路冒烟
docker logs aip-test
docker rm -f aip-test
```

---

## 4. 生产部署要点

Nginx 反代配置见 [../deploy/nginx.conf](../deploy/nginx.conf)，**关键是 `proxy_buffering off`**，
否则前端不会逐字输出。部署检查清单见 [ROADMAP.md](ROADMAP.md) 阶段 6。

---

## 5. 我（AI）与你（人）的分工约定

**完整约定见仓库根目录的 [AGENTS.md](../AGENTS.md)**，这里只列摘要：

| 事项 | 谁来做 |
|---|---|
| 写代码、改设计、补文档 | AI |
| 编译、跑测试、起服务做冒烟（≤200 连接 / ≤10 秒） | AI（本文件第 1、2 节） |
| root / sudo 相关（装 Drogon、改系统配置）、填 API Key | 人 |
| 正式压测（k6/wrk 长时）、构建并发布镜像、部署 | 人（本文件第 3、4 节） |
| 架构 / 成本拍板，配置 Git 身份、push | 人 |
| 给出报错日志后定位问题 | AI |

判断标准：**要 root/凭据、跑很久、或会改变线上/他人可见状态 → 先问；
编译、拉代码、本地秒级冒烟 → AI 直接做，但别把 CPU 占满（默认 `-j2`，最多 `-j3`）。**
AI 交付代码时会标注「是否经过编译验证」，并附上需要你执行的验证命令。
