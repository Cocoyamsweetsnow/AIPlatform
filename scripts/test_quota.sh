#!/usr/bin/env bash
# 配额端到端验收：RPM 429 + Retry-After + usage_records 落库。
#
# 用法：./scripts/test_quota.sh
# 前置：build/aiplatform 已用 PostgreSQL + AUTH 编译。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PG_PORT="${AIP_TEST_PG_PORT:-55438}"
APP_PORT="${AIP_TEST_APP_PORT:-8100}"
PG_NAME="${AIP_TEST_PG_CONTAINER:-aip-pg-quota-test}"
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
  if is_ours; then docker rm -f "$PG_NAME" >/dev/null 2>&1 || true; fi
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
  exit 1
fi

AIP_PORT="$APP_PORT" \
AIP_PROVIDER=mock \
AIP_WEB=false \
AIP_STORAGE=postgres \
AIP_DB_DSN="$DSN" \
AIP_JWT_SECRET='0123456789abcdef0123456789abcdef' \
AIP_AUTH_REQUIRED=1 \
AIP_RPM_LIMIT=2 \
AIP_DAILY_TOKEN_LIMIT=1000000 \
AIP_MAX_CONCURRENT_PER_USER=2 \
AIP_MAX_TOKENS=16 \
AIP_LOG_LEVEL=warn \
  "$ROOT/build/aiplatform" >>"$LOG_DIR/app.log" 2>&1 &
APP_PID=$!

for _ in $(seq 1 60); do
  if curl -fsS "http://127.0.0.1:${APP_PORT}/healthz" >/dev/null 2>&1; then break; fi
  sleep 0.2
done
if ! curl -fsS "http://127.0.0.1:${APP_PORT}/healthz" >/dev/null 2>&1; then
  echo "FAIL: 服务未就绪" >&2
  tail -40 "$LOG_DIR/app.log" >&2 || true
  exit 1
fi

BASE="http://127.0.0.1:${APP_PORT}"
REGISTER="$(curl -sS -H 'Content-Type: application/json' \
  -d '{"email":"quota@example.com","password":"password123"}' "$BASE/v1/auth/register")"
TOKEN="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["access_token"])' <<<"$REGISTER")"
[[ -n "$TOKEN" ]] || { echo "FAIL: 注册未返回 token" >&2; exit 1; }

for i in 1 2; do
  STATUS="$(curl -sS -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $TOKEN" \
    -H 'Content-Type: application/json' -d '{"message":"quota test","stream":false}' \
    "$BASE/v1/chat/completions")"
  [[ "$STATUS" == "200" ]] || { echo "FAIL: 第 $i 次请求应为 200，实际 $STATUS" >&2; exit 1; }
  sleep 0.2
done

STATUS="$(curl -sS -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' -d '{"message":"too many","stream":false}' \
  "$BASE/v1/chat/completions")"
[[ "$STATUS" == "429" ]] || { echo "FAIL: 第 3 次请求应为 429，实际 $STATUS" >&2; exit 1; }

HEADERS="$(curl -sS -D - -o /dev/null -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' -d '{"message":"retry","stream":false}' \
  "$BASE/v1/chat/completions" | tr -d '\r')"
echo "$HEADERS" | grep -qi '^Retry-After: [0-9]' \
  || { echo "FAIL: 429 响应缺少 Retry-After" >&2; exit 1; }

COUNT="$(docker exec "$PG_NAME" psql -U aiplatform -d aiplatform_test -tAc \
  'select count(*) from usage_records;' | tr -d ' ')"
[[ "$COUNT" -ge 2 ]] || { echo "FAIL: usage_records 数量不足（$COUNT）" >&2; exit 1; }

# 重启服务，把每日 token 配额降到 50；已有用量 40，下一次预留 ~26 必然超额。
kill -TERM "$APP_PID"
wait "$APP_PID" 2>/dev/null || true
APP_PID=""

AIP_PORT="$APP_PORT" \
AIP_PROVIDER=mock \
AIP_WEB=false \
AIP_STORAGE=postgres \
AIP_DB_DSN="$DSN" \
AIP_JWT_SECRET='0123456789abcdef0123456789abcdef' \
AIP_AUTH_REQUIRED=1 \
AIP_RPM_LIMIT=1000 \
AIP_DAILY_TOKEN_LIMIT=50 \
AIP_MAX_CONCURRENT_PER_USER=2 \
AIP_MAX_TOKENS=16 \
AIP_LOG_LEVEL=warn \
  "$ROOT/build/aiplatform" >>"$LOG_DIR/app.log" 2>&1 &
APP_PID=$!

for _ in $(seq 1 60); do
  if curl -fsS "http://127.0.0.1:${APP_PORT}/healthz" >/dev/null 2>&1; then break; fi
  sleep 0.2
done

DAILY_BODY="$(curl -sS -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"message":"daily quota","stream":false}' "$BASE/v1/chat/completions")"
echo "$DAILY_BODY" | grep -q 'quota_exceeded' \
  || { echo "FAIL: 每日 token 配额未生效: $DAILY_BODY" >&2; exit 1; }

echo "quota OK: rpm=2, third=429, Retry-After present, daily quota enforced, usage_records=$COUNT"
