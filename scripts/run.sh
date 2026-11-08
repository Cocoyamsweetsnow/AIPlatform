#!/usr/bin/env bash
# 本地一键启动：加载 .env → 确保 PostgreSQL 就绪 → 运行 build/aiplatform。
#
# 用法：
#   ./scripts/run.sh                 # .env 里的真实模型 + PostgreSQL（会话持久化）
#   ./scripts/run.sh --memory        # 不连数据库，会话重启即丢
#   ./scripts/run.sh --port 8081     # 覆盖 AIP_PORT
#   ./scripts/run.sh --help
#
# 前置：已编译 build/aiplatform。PostgreSQL 使用 docker compose 里的 postgres 服务
# （宿主机 55432）；脚本只启动数据库，不会停止它，也不会删除数据卷。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

STORAGE="postgres"
PORT=""

usage() {
  cat <<'EOF'
本地一键启动：加载 .env → 确保 PostgreSQL 就绪 → 运行 build/aiplatform。

用法：
  ./scripts/run.sh                 # .env 里的真实模型 + PostgreSQL（会话持久化）
  ./scripts/run.sh --memory        # 不连数据库，会话重启即丢
  ./scripts/run.sh --port 8081     # 覆盖 AIP_PORT
  ./scripts/run.sh --help

前置：已编译 build/aiplatform。PostgreSQL 使用 docker compose 里的 postgres 服务
（宿主机 55432）；脚本只启动数据库，不会停止它，也不会删除数据卷。
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --memory) STORAGE="memory"; shift ;;
    --port) PORT="${2:?--port 需要一个端口号}"; shift 2 ;;
    --port=*) PORT="${1#*=}"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "未知参数: $1（试试 --help）" >&2; exit 2 ;;
  esac
done

if [[ ! -x build/aiplatform ]]; then
  echo "FAIL: 没找到可执行的 build/aiplatform" >&2
  echo "先编译：cmake -S . -B build -DAIPLATFORM_BUILD_TESTS=ON -DAIPLATFORM_HTTP=httplib" >&2
  echo "        cmake --build build -j2" >&2
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

if [[ -n "$PORT" ]]; then
  export AIP_PORT="$PORT"
fi
AIP_PORT="${AIP_PORT:-8080}"
if ! [[ "$AIP_PORT" =~ ^[0-9]+$ ]]; then
  echo "FAIL: 端口不是数字: $AIP_PORT" >&2
  exit 1
fi

if [[ "$STORAGE" == "postgres" ]]; then
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
else
  export AIP_STORAGE=memory
  unset AIP_DB_DSN || true
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

echo "==> provider=${AIP_PROVIDER:-mock} model=${AIP_MODEL:-default} storage=${AIP_STORAGE} port=${AIP_PORT}"
echo "==> http://127.0.0.1:${AIP_PORT}/   （Ctrl-C 停止）"
exec ./build/aiplatform
