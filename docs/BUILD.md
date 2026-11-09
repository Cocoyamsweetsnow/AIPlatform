# 编译、运行与镜像构建（操作手册）

> 本文件只写「你要执行的命令」，代码与设计说明见 [ROADMAP.md](ROADMAP.md)。
> 所有命令都在仓库根目录 `AIPlatform/` 下执行。

---

## 1. 编译（本机，Ubuntu 22.04 / 24.04）

### 1.1 安装依赖

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libcurl4-openssl-dev \
                    libpq5 libargon2-1 libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev
```

> PostgreSQL 客户端头文件已经随仓库分发在 `third_party/libpq/include`，
> 这里只装运行库 `libpq5` 即可；不需要 `libpq-dev`，也不需要 root 权限去装开发包。
> 完全不想引入 PostgreSQL 时用 `-DAIPLATFORM_WITH_POSTGRES=OFF` 构建纯内存版本；
> 不想引入用户鉴权时用 `-DAIPLATFORM_WITH_AUTH=OFF`（只保留静态 `AIP_TOKEN`）。
> Argon2 头文件同样随仓库分发在 `third_party/argon2/include`，运行时只需 `libargon2-1`。

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

# 单元测试（约 1 秒）；pg_tests 没设置 AIP_TEST_PG_DSN 时会自动 skip
ctest --test-dir build --output-on-failure
```

产物：

| 路径 | 说明 |
|---|---|
| `build/aiplatform` | 服务端二进制（**内置 Web UI**） |
| `build/unit_tests` | 单元测试（开了 `-DAIPLATFORM_BUILD_TESTS=ON` 才有） |
| `build/pg_tests` | PostgreSQL 集成测试（需要 `AIP_TEST_PG_DSN`，否则 skip） |
| `build/auth_tests` | 鉴权原语与 AuthService 单测（不需要数据库） |
| `build/usage_tests` | 用量记录与 token 估算单测 |
| `build/quota_tests` | 配额/并发/每日 token 单测 |
| `build/idempotency_tests` | 幂等状态机/重放流/TurnObserver 单测 |
| `build/context_tests` | token 预算上下文裁剪单测 |
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
| `找不到 libpq` / `libpq.so.5: cannot open shared object` | 装 `libpq5`；或 `-DAIPLATFORM_WITH_POSTGRES=OFF` 构建纯内存版本 |
| 启动时报 `连接 PostgreSQL 失败` | 检查 `AIP_DB_DSN`、数据库是否已起；compose 里是 `postgres:5432`，宿主机是 `127.0.0.1:55432` |
| `找不到 libargon2` / `libargon2.so.1: cannot open shared object` | 装 `libargon2-1`；或 `-DAIPLATFORM_WITH_AUTH=OFF` 只用静态 Token |
| 运行时 `error while loading shared libraries: libdrogon.so` | 装完 Drogon 没执行 `sudo ldconfig` |

---

## 2. 运行

### 2.0 一键启动（推荐）

```bash
./scripts/run.sh                         # 加载 .env，自动拉起 PostgreSQL，真实模型 + 持久化
./scripts/run.sh --mock                  # 强制 mock，不消耗 API Key
./scripts/run.sh --memory               # 不连数据库；自动关闭 JWT/账号体系
./scripts/run.sh --mock --port 8081     # 离线 + 换端口
./scripts/run.sh --binary build-drogon/aiplatform   # Drogon 主线二进制
```

脚本负责：加载 `.env`、确保 compose 里的 PostgreSQL 就绪、做 P0 配置的启动前校验、
`exec` 目标二进制。它不会停止数据库、不会删数据卷；`Ctrl-C` 停止应用。
8080 被占用时会提示原因和处理方式。

- 默认二进制是 `build/aiplatform`；Drogon 主线用 `--binary build-drogon/aiplatform`
  或设置 `AIP_BINARY`。
- `--memory` 会临时忽略 `AIP_JWT_SECRET` / `AIP_AUTH_REQUIRED` /
  `AIP_EMAIL_VERIFICATION_REQUIRED` / `AIP_AUTH_COOKIE_MODE`，因为用户体系依赖 PostgreSQL。
- `AIP_AUTH_COOKIE_MODE=1` 且未设置 `AIP_PUBLIC_BASE_URL` 时，脚本按当前端口补
  `http://127.0.0.1:<port>`；生产请显式设置同源 HTTPS 地址。
- `AIP_AUTH_REQUIRED=1` 但既无 JWT secret 也无静态 Token 时，脚本直接报错。

需要手动控制每一项配置时，再按 2.1~2.7 的原始命令运行。

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

### 2.2 PostgreSQL 持久化模式

```bash
# 1) 起一个本地 PostgreSQL（compose 里映射到 55432，避免和系统 5432 冲突）
docker compose up -d postgres

# 2) 用持久化模式启动
AIP_STORAGE=postgres \
AIP_DB_DSN='postgresql://aiplatform:aiplatform@127.0.0.1:55432/aiplatform?connect_timeout=5' \
./build/aiplatform
```

启动日志应出现 `storage: postgres` 和数据库迁移日志（v1~v3）。迁移会自动建表
（`users / api_keys / sessions / messages / usage_records / request_dedup /
refresh_tokens / one_time_tokens / rate_limits / quota_reservations /
quota_stream_leases`）；迁移可重复执行，多实例并发启动用 advisory lock 串行化。

验证持久化：

```bash
./scripts/test_pg.sh             # 临时 PG 容器 + pg_tests（迁移/CRUD/归属/软删除）
./scripts/test_persistence.sh    # 杀进程重启，确认历史仍在
```

> `AIP_STORAGE=memory` 仍是默认值；没有数据库时演示和单测不受影响。
> 连接池大小用 `AIP_DB_POOL_SIZE`（默认 8），连接超时用 `AIP_DB_CONNECT_TIMEOUT_S`。

### 2.3 用户鉴权（JWT / API Key）

```bash
export AIP_JWT_SECRET="$(openssl rand -hex 32)"   # >=32 字节，多副本必须一致
AIP_STORAGE=postgres \
AIP_DB_DSN='postgresql://aiplatform:aiplatform@127.0.0.1:55432/aiplatform?connect_timeout=5' \
AIP_AUTH_REQUIRED=1 \
./build/aiplatform

# 注册
curl -sS http://127.0.0.1:8080/v1/auth/register \
  -H 'Content-Type: application/json' \
  -d '{"email":"alice@example.com","password":"password123"}'

# 带 JWT 对话（TOKEN 替换成返回的 access_token）
curl -N http://127.0.0.1:8080/v1/chat/completions \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"message":"你好"}'
```

- `AIP_TOKEN` 仍然有效，视为管理员（可访问所有会话，但不能创建用户 API Key）。
- `AIP_AUTH_REQUIRED=1` 时，`/healthz`、`/v1/config`、内置页面、`/v1/auth/register|login|refresh` 仍公开。
- 用户/JWT 需要 PostgreSQL；memory 模式只支持静态 `AIP_TOKEN`。
- Web UI 登录：同时配置 `AIP_STORAGE=postgres` 和 `AIP_JWT_SECRET` 后，打开 `/` 会先显示
  全屏登录 / 注册页，登录成功后才进入对话；access token 过期自动 refresh 一次。
  没配账号体系时页面回退到 Token 输入。
- 自动化验收：`./scripts/test_auth.sh`（临时 PG + 注册/登录/JWT/API Key/跨用户/撤销/刷新）。

### 2.4 配额与用量

```bash
AIP_RPM_LIMIT=60 AIP_DAILY_TOKEN_LIMIT=1000000 AIP_MAX_CONCURRENT_PER_USER=4 \
AIP_STORAGE=postgres ... ./build/aiplatform
```

- 超限返回 429 + `Retry-After`；每日配额用 `type=quota_exceeded`。
- `usage_records` 记录 prompt/completion tokens、latency、TTFT；当前限流是进程内的，
  多副本需要 Redis/网关限流。
- 自动化验收：`./scripts/test_quota.sh`（RPM 429、Retry-After、每日配额、usage 落库）。

### 2.5 幂等与会话管理

```bash
# 同一个 request_id 第二次请求会重放第一次的结果
curl -sS http://127.0.0.1:8080/v1/chat/completions \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"message":"你好","stream":false,"request_id":"demo-1"}'

# 列表 / 改名 / 软删除
curl -sS -H "Authorization: Bearer $TOKEN" 'http://127.0.0.1:8080/v1/sessions?limit=20'
curl -sS -X PATCH -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"title":"新标题"}' http://127.0.0.1:8080/v1/sessions/<id>
curl -sS -X DELETE -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8080/v1/sessions/<id>
```

- 幂等的 `in_progress` 超过 `AIP_IDEMPOTENCY_TTL_S`（默认 180s）会被视为崩溃并允许重试。
- 自动化验收：`./scripts/test_idempotency.sh`、`./scripts/test_sessions.sh`。

### 2.6 上下文预算

```bash
AIP_MAX_CONTEXT_TOKENS=8192 AIP_MAX_TOKENS=1024 ./build/aiplatform
```

- 按估算 token 裁剪 system + 历史 + user；超出返回 413 `context_too_long`。
- `AIP_MAX_TOKENS` 必须小于 `AIP_MAX_CONTEXT_TOKENS`；估算不是 tokenizer。
- 单测：`./build/context_tests`。

### 2.7 接真实模型

```bash
export AIP_PROVIDER=openai
export AIP_BASE_URL=https://api.deepseek.com/v1
export AIP_API_KEY=sk-xxxxxxxx
export AIP_MODEL=deepseek-chat
# 可选：多 key 轮转 + 首 token 前重试（0 = 关闭）
# export AIP_API_KEYS=sk-aaa,sk-bbb
# export AIP_MAX_RETRIES=2
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

### 2.8 冒烟测试（一次性验证全链路）

```bash
./scripts/demo.sh http://127.0.0.1:8080
```

会依次验证：健康检查 → 流式对话 → 多轮上下文 → 会话历史 → 错误码（400/404）。

上游重试单独验证（脚本自己起假上游和服务，使用 9099 / 8081 端口）：

```bash
./scripts/test_retry.sh
```

### 2.9 并发冒烟（秒级，不是正式压测）

```bash
# 200 路真实 SSE（服务在容器里时，--pid 用容器内的 1）
python3 scripts/sse_load.py -n 200 \
  --base http://127.0.0.1:8080 \
  --path /v1/chat/completions --body '{"message":"压测"}'
```

判据：`失败 = 0`；线程数峰值 ≈ 2×N（每路流仍是泵线程 + 上游读线程）。
正式的 k6 / wrk 长时压测请由人执行。

### 2.10 验证优雅退出（drain）

```bash
./build/aiplatform &          # 另开一个终端跑 200 路冒烟
kill -TERM %1
# 日志应出现：收到退出信号 → 在途流已全部结束，退出 → 服务已退出
```

若在途流超过 `AIP_SHUTDOWN_DRAIN_S`（默认 30s），会打印「退出超时」并强制关闭。

---

### 2.11 启动配置变更检查清单

每次新增/重命名环境变量、改变启动前置条件（例如“JWT 必须配 PostgreSQL”）时，
按顺序同步下列位置，并在合并前跑一次 `./scripts/run.sh --help` 和
`./scripts/run.sh --memory --mock`：

1. `src/config.h` / `src/config.cpp`：字段、默认值、校验；
2. `config/config.example.env`：给用户的配置模板与注释；
3. `scripts/run.sh`：默认值、互斥校验、启动提示（本次 P0 已加
   `--mock` / `--binary`、memory 模式关闭 JWT、Cookie 模式 Public URL 校验）；
4. `docker-compose.yml` / `Dockerfile`：容器内需要的环境变量或健康检查；
5. `README.md` / `docs/BUILD.md` / ADR：运行命令与行为变化；
6. 单测/冒烟：至少覆盖默认值、必填校验和一种“错误配置应拒绝启动”的路径。

判断标准：**新人按 README 的命令、用仓库里的 example env，能一次启动成功；
旧 `.env` 升级后不会因为新增前置条件而静默启动失败。**

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

# 或者用 compose（会同时起 postgres，API 默认 AIP_STORAGE=postgres）
docker compose --env-file .env up -d --build
docker compose logs -f api
docker compose logs -f postgres
```

> 单容器 `docker run` 默认还是内存模式；要持久化就用 compose，或者给
> `docker run` 加 `-e AIP_STORAGE=postgres -e AIP_DB_DSN=...` 并自己提供 PostgreSQL。

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
