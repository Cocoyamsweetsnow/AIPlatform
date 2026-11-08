#!/usr/bin/env bash
# 端到端持久化验收：起临时 PostgreSQL + 服务，发一轮对话，杀进程重启，历史仍在。
#
# 用法：./scripts/test_persistence.sh
# 前置：build/aiplatform 已用 -DAIPLATFORM_WITH_POSTGRES=ON 编译。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PG_PORT="${AIP_TEST_PG_PORT:-55434}"
APP_PORT="${AIP_TEST_APP_PORT:-8095}"
PG_NAME="${AIP_TEST_PG_CONTAINER:-aip-pg-persist-test}"
DSN="postgresql://aiplatform:aiplatform@127.0.0.1:${PG_PORT}/aiplatform_test"
LOG_DIR="$(mktemp -d)"
APP_PID=""

is_ours() {
  docker inspect "$PG_NAME" >/dev/null 2>&1 || return 1
  local label
  label="$(docker inspect -f '{{index .Config.Labels "aiplatform.test"}}' "$PG_NAME" 2>/dev/null || true)"
  [[ "$label" == "1" ]]
}

cleanup() {
  if [[ -n "$APP_PID" ]]; then
    kill -TERM "$APP_PID" 2>/dev/null || true
    wait "$APP_PID" 2>/dev/null || true
  fi
  if is_ours; then
    docker rm -f "$PG_NAME" >/dev/null 2>&1 || true
  fi
  rm -rf "$LOG_DIR"
}
trap cleanup EXIT

if docker inspect "$PG_NAME" >/dev/null 2>&1 && ! is_ours; then
  echo "FAIL: 容器 $PG_NAME 已存在且不是本脚本创建的，拒绝删除" >&2
  exit 1
fi
if ! docker inspect "$PG_NAME" >/dev/null 2>&1; then
  docker run -d --rm --name "$PG_NAME" --label aiplatform.test=1 \
    -e POSTGRES_USER=aiplatform \
    -e POSTGRES_PASSWORD=aiplatform \
    -e POSTGRES_DB=aiplatform_test \
    -p "${PG_PORT}:5432" postgres:16 >/dev/null
fi

for _ in $(seq 1 60); do
  if docker exec "$PG_NAME" psql -U aiplatform -d aiplatform_test -c 'select 1' >/dev/null 2>&1; then
    break
  fi
  sleep 1
done
if ! docker exec "$PG_NAME" psql -U aiplatform -d aiplatform_test -c 'select 1' >/dev/null 2>&1; then
  echo "FAIL: PostgreSQL 未在 60s 内就绪" >&2
  docker logs "$PG_NAME" 2>&1 | tail -30 >&2 || true
  exit 1
fi

start_app() {
  AIP_PORT="$APP_PORT" \
  AIP_PROVIDER=mock \
  AIP_WEB=false \
  AIP_STORAGE=postgres \
  AIP_DB_DSN="$DSN" \
  AIP_LOG_LEVEL=warn \
    "$ROOT/build/aiplatform" >>"$LOG_DIR/app.log" 2>&1 &
  APP_PID=$!
}

wait_health() {
  for _ in $(seq 1 50); do
    if curl -fsS "http://127.0.0.1:${APP_PORT}/healthz" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.2
  done
  echo "FAIL: 服务未在 10s 内就绪" >&2
  tail -40 "$LOG_DIR/app.log" >&2 || true
  return 1
}

start_app
wait_health

SID="$(curl -sS -D - -o /dev/null \
  -H 'Content-Type: application/json' \
  -d '{"message":"持久化冒烟：记住 42"}' \
  "http://127.0.0.1:${APP_PORT}/v1/chat/completions" \
  | tr -d '\r' | awk -F': ' 'tolower($1)=="x-session-id"{print $2}')"
if [[ -z "$SID" ]]; then
  echo "FAIL: 未拿到 X-Session-Id" >&2
  exit 1
fi

REPLY="$(curl -sS \
  -H 'Content-Type: application/json' \
  -d "{\"message\":\"42 是什么？\",\"session_id\":\"$SID\",\"stream\":false}" \
  "http://127.0.0.1:${APP_PORT}/v1/chat/completions")"
echo "$REPLY" | grep -q '42' || {
  echo "FAIL: 非流式回复中没有 42: $REPLY" >&2
  exit 1
}

# 重启进程（不是重启容器），验证数据真的在 PostgreSQL 里。
kill -TERM "$APP_PID"
wait "$APP_PID" 2>/dev/null || true
APP_PID=""
start_app
wait_health

HISTORY="$(curl -sS "http://127.0.0.1:${APP_PORT}/v1/sessions/${SID}/messages")"
if ! echo "$HISTORY" | grep -q '持久化冒烟'; then
  echo "FAIL: 重启后历史丢失: $HISTORY" >&2
  exit 1
fi

COUNT="$(python3 -c 'import json,sys; print(len(json.load(sys.stdin)["messages"]))' <<<"$HISTORY")"
if [[ "$COUNT" -lt 2 ]]; then
  echo "FAIL: 重启后消息数不足（$COUNT）: $HISTORY" >&2
  exit 1
fi

echo "persistence OK: session=$SID messages=$COUNT"
