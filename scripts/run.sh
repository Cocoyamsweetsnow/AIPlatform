#!/usr/bin/env bash
# 本地一键启动：加载 .env → 确保 PostgreSQL 就绪 → 运行 aiplatform。
#
# 用法：
#   ./scripts/run.sh                       # .env 里的真实模型 + PostgreSQL（会话持久化）
#   ./scripts/run.sh --memory              # 内存模式；自动关闭 JWT/账号体系
#   ./scripts/run.sh --mock                # 强制离线 mock 模型（不消耗 API）
#   ./scripts/run.sh --port 8081           # 覆盖 AIP_PORT
#   ./scripts/run.sh --binary build-drogon/aiplatform
#   ./scripts/run.sh --help
#
# 前置：已编译目标二进制。默认找 build/aiplatform；Drogon 主线可传
#       --binary build-drogon/aiplatform（或设置 AIP_BINARY）。
# PostgreSQL 使用 docker compose 里的 postgres 服务（宿主机 55432）；
# 脚本只启动数据库，不会停止它，也不会删除数据卷。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

STORAGE="postgres"
PORT=""
BINARY="${AIP_BINARY:-build/aiplatform}"
FORCE_MOCK=0

usage() {
  cat <<'EOF'
本地一键启动：加载 .env → 确保 PostgreSQL 就绪 → 运行 aiplatform。

用法：
  ./scripts/run.sh                       # .env 里的真实模型 + PostgreSQL（会话持久化）
  ./scripts/run.sh --memory              # 内存模式；自动关闭 JWT/账号体系
  ./scripts/run.sh --mock                # 强制离线 mock 模型（不消耗 API）
  ./scripts/run.sh --port 8081           # 覆盖 AIP_PORT
  ./scripts/run.sh --binary build-drogon/aiplatform
  ./scripts/run.sh --help

前置：已编译目标二进制。默认 build/aiplatform；Drogon 主线用
      --binary build-drogon/aiplatform（或设置 AIP_BINARY）。
PostgreSQL 使用 docker compose 里的 postgres 服务（宿主机 55432）；
脚本只启动数据库，不会停止它，也不会删除数据卷。

P0 新增配置的启动行为：
  - --memory 会 unset AIP_JWT_SECRET/AIP_AUTH_REQUIRED 等账号体系变量，
    因为 JWT/邮箱验证/Cookie 模式都依赖 PostgreSQL；
  - AIP_AUTH_COOKIE_MODE=1 未设置 AIP_PUBLIC_BASE_URL 时，自动用当前端口补一个；
  - AIP_AUTH_REQUIRED=1 但既没有 JWT secret 也没有静态 Token 时直接报错；
  - AIP_EMAIL_VERIFICATION_REQUIRED=1 但没配 SMTP 时提示“只写日志”。
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --memory) STORAGE="memory"; shift ;;
    --mock) FORCE_MOCK=1; shift ;;
    --port) PORT="${2:?--port 需要一个端口号}"; shift 2 ;;
    --port=*) PORT="${1#*=}"; shift ;;
    --binary) BINARY="${2:?--binary 需要一个路径}"; shift 2 ;;
    --binary=*) BINARY="${1#*=}"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "未知参数: $1（试试 --help）" >&2; exit 2 ;;
  esac
done

if [[ ! -x "$BINARY" ]]; then
  echo "FAIL: 没找到可执行的 $BINARY" >&2
  echo "httplib 回退构建：cmake -S . -B build -DAIPLATFORM_HTTP=httplib -DAIPLATFORM_BUILD_TESTS=ON" >&2
  echo "Drogon 主线构建：  cmake -S . -B build-drogon -DAIPLATFORM_BUILD_TESTS=ON" >&2
  echo "                    cmake --build <build-dir> -j2" >&2
  exit 1
fi

if [[ -f .env ]]; then
  set -a
  # shellcheck disable=SC1091
  . ./.env
  set +a
else
  echo "WARN: 没有 .env，使用默认 mock + memory" >&2
  STORAGE="memory"
fi

if [[ "$FORCE_MOCK" == "1" ]]; then
  export AIP_PROVIDER=mock
fi
if [[ -n "$PORT" ]]; then
  export AIP_PORT="$PORT"
fi
AIP_PORT="${AIP_PORT:-8080}"
if ! [[ "$AIP_PORT" =~ ^[0-9]+$ ]]; then
  echo "FAIL: 端口不是数字: $AIP_PORT" >&2
  exit 1
fi

if [[ "$STORAGE" == "memory" ]]; then
  # memory 模式没有 users 表，JWT/邮箱验证/Cookie 模式都会让 Config::from_env 拒绝启动。
  if [[ -n "${AIP_JWT_SECRET:-}" || "${AIP_AUTH_REQUIRED:-false}" == "1" ||
        "${AIP_EMAIL_VERIFICATION_REQUIRED:-false}" == "1" ||
        "${AIP_AUTH_COOKIE_MODE:-false}" == "1" ]]; then
    echo "WARN: memory 模式不支持 JWT/账号体系，临时忽略 AIP_JWT_SECRET、AIP_AUTH_REQUIRED、"
    echo "      AIP_EMAIL_VERIFICATION_REQUIRED、AIP_AUTH_COOKIE_MODE；要测账号请去掉 --memory。"
  fi
  unset AIP_JWT_SECRET AIP_AUTH_REQUIRED AIP_EMAIL_VERIFICATION_REQUIRED \
        AIP_AUTH_COOKIE_MODE AIP_SMTP_URL || true
  if [[ -n "${AIP_TOKEN:-}" ]]; then
    echo "WARN: memory 模式保留 AIP_TOKEN 静态鉴权；不需要请 unset AIP_TOKEN 后重跑。"
  fi
  export AIP_STORAGE=memory
  unset AIP_DB_DSN || true
else
  if ! command -v docker >/dev/null 2>&1; then
    echo "FAIL: 需要 Docker 启动 PostgreSQL；没有 Docker 时用 --memory" >&2
    exit 1
  fi
  echo "==> 确保 PostgreSQL 就绪（compose postgres，宿主机 55432）"
  if ! docker compose up -d postgres >/dev/null 2>&1; then
    echo "FAIL: 启动 PostgreSQL 失败，执行 docker compose up -d postgres 看报错" >&2
    exit 1
  fi
  ready=0
  for _ in $(seq 1 30); do
    if docker compose exec -T postgres pg_isready -U aiplatform -d aiplatform >/dev/null 2>&1; then
      ready=1
      break
    fi
    sleep 1
  done
  if [[ "$ready" != 1 ]]; then
    echo "FAIL: PostgreSQL 30s 内未就绪，查看：docker compose logs postgres" >&2
    exit 1
  fi
  export AIP_STORAGE=postgres
  export AIP_DB_DSN="${AIP_DB_DSN:-postgresql://aiplatform:${POSTGRES_PASSWORD:-aiplatform}@127.0.0.1:${AIP_DB_HOST_PORT:-55432}/aiplatform?connect_timeout=5}"
fi

# ---- P0 配置的启动前校验（不打印密钥）----
if [[ "${AIP_AUTH_COOKIE_MODE:-false}" == "1" ]]; then
  if [[ -z "${AIP_JWT_SECRET:-}" ]]; then
    echo "FAIL: AIP_AUTH_COOKIE_MODE=1 需要 AIP_JWT_SECRET（用户体系）" >&2
    exit 1
  fi
  if [[ -z "${AIP_PUBLIC_BASE_URL:-}" ]]; then
    export AIP_PUBLIC_BASE_URL="http://127.0.0.1:${AIP_PORT}"
    echo "==> AIP_PUBLIC_BASE_URL 未设置，Cookie 模式自动使用 ${AIP_PUBLIC_BASE_URL}"
  fi
  if [[ "${AIP_AUTH_COOKIE_SECURE:-false}" == "1" &&
        "${AIP_PUBLIC_BASE_URL}" != https://* ]]; then
    echo "WARN: AIP_AUTH_COOKIE_SECURE=1 但 AIP_PUBLIC_BASE_URL 不是 https；浏览器可能拒收 Cookie。" >&2
  fi
fi
if [[ "${AIP_AUTH_REQUIRED:-false}" == "1" &&
      -z "${AIP_JWT_SECRET:-}" && -z "${AIP_TOKEN:-}" ]]; then
  echo "FAIL: AIP_AUTH_REQUIRED=1 时必须配置 AIP_JWT_SECRET 或 AIP_TOKEN" >&2
  exit 1
fi
if [[ "${AIP_EMAIL_VERIFICATION_REQUIRED:-false}" == "1" &&
      -z "${AIP_SMTP_URL:-}" ]]; then
  echo "WARN: 邮箱验证已开启但未配置 AIP_SMTP_URL，验证邮件只会写日志。" >&2
fi

if ss -ltnH "sport = :${AIP_PORT}" 2>/dev/null | grep -q .; then
  echo "FAIL: 端口 ${AIP_PORT} 已被占用" >&2
  if docker ps --format '{{.Names}}' 2>/dev/null | grep -qx 'aip-app'; then
    echo "提示: 演示容器 aip-app 正在占用该端口，先 docker stop aip-app（数据库和历史不受影响）" >&2
  else
    echo "换端口: ./scripts/run.sh --port $((AIP_PORT + 1))" >&2
  fi
  exit 1
fi

AUTH_SUMMARY="anonymous"
if [[ -n "${AIP_JWT_SECRET:-}" || -n "${AIP_TOKEN:-}" ||
      "${AIP_AUTH_REQUIRED:-false}" == "1" ]]; then
  AUTH_SUMMARY="enabled"
  if [[ "${AIP_AUTH_COOKIE_MODE:-false}" == "1" ]]; then
    AUTH_SUMMARY="enabled+cookie"
  fi
fi
MODEL_SUMMARY="${AIP_MODELS:-${AIP_MODEL:-default}}"
echo "==> binary=${BINARY} provider=${AIP_PROVIDER:-mock} models=${MODEL_SUMMARY} storage=${AIP_STORAGE} auth=${AUTH_SUMMARY} port=${AIP_PORT}"
echo "==> UI:      http://127.0.0.1:${AIP_PORT}/"
echo "==> metrics: http://127.0.0.1:${AIP_PORT}/metrics"
echo "==> ready:   http://127.0.0.1:${AIP_PORT}/readyz"
echo "==> Ctrl-C 停止"
exec "$BINARY"
