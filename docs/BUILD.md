# 编译、运行与镜像构建（操作手册）

> 本文件只写「你要执行的命令」，代码与设计说明见 [ROADMAP.md](ROADMAP.md)。
> 所有命令都在仓库根目录 `AIPlatform/` 下执行。

---

## 1. 编译（本机，Ubuntu 22.04 / 24.04）

### 1.1 安装依赖

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libcurl4-openssl-dev
```

> 本项目只用两个单头文件库（已在 `third_party/` 内），**不需要** vcpkg / conan，不联网也能编译。
> GCC 11 及以上即可（代码没用到 `std::format` 这类 GCC 13 才有的特性）。

### 1.2 编译 + 测试

```bash
# 我上次编译留下的 build/ 可以删掉，重新来过更干净
rm -rf build

cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DAIPLATFORM_BUILD_TESTS=ON

cmake --build build -j"$(nproc)"

# 单元测试（7 项，约 0.4 秒）
ctest --test-dir build --output-on-failure
```

产物：

| 路径 | 说明 |
|---|---|
| `build/aiplatform` | 服务端二进制 |
| `build/unit_tests` | 单元测试（开了 `-DAIPLATFORM_BUILD_TESTS=ON` 才有） |

### 1.3 编译报错对照

| 报错 | 原因 / 处理 |
|---|---|
| `Could NOT find CURL` | 缺 `libcurl4-openssl-dev`，或 `pkg-config` 没装 |
| `#include <curl/curl.h>` 找不到 | 同上 |
| `error: 'std::span' ...` | 编译器太老，`g++ --version` 需 ≥ 11 |
| `undefined reference to curl_easy_init` | 链接顺序/静态库问题，重跑一次 `cmake -S . -B build` |

---

## 2. 运行

### 2.1 离线模式（mock 模型，不需要 API Key）

```bash
./build/aiplatform
# 日志里会出现: HTTP 服务启动: http://0.0.0.0:8080

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

---

## 3. Docker 镜像构建

### 3.1 构建与运行

```bash
# 构建（多阶段，最终镜像只含二进制 + libcurl）
docker build -t aiplatform:dev .

# 运行
cp config/config.example.env .env
# 编辑 .env，至少填 AIP_API_KEY（想先用 mock 就不用改）
docker run --rm -p 8080:8080 --env-file .env aiplatform:dev

# 或者用 compose
docker compose --env-file .env up -d --build
docker compose logs -f api
```

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

> 我在这台机器上试过构建，卡在 build 阶段的 `apt-get update`（拉索引就花了十几分钟），
> 属于网络问题，不是 Dockerfile 的问题。正常网络下约 2 分钟出镜像。

### 3.3 镜像验证

```bash
docker run --rm aiplatform:dev --help 2>/dev/null || true
docker run -d --name aip-test -p 8080:8080 aiplatform:dev
curl -sS http://127.0.0.1:8080/healthz     # {"status":"ok",...}
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
| 编译、跑测试、启动服务 | 人（本文件第 1、2 节） |
| 拉镜像、构建镜像、部署 | 人（本文件第 3、4 节） |
| 配置 Git 身份、push、填 API Key | 人 |
| 给出报错日志后定位问题 | AI |

判断标准：**如果一条命令失败会浪费你 5 分钟以上，或者要联网/要凭据/要花钱，就交给人做。**
AI 交付代码时会标注「是否经过编译验证」，并附上需要你执行的验证命令。
