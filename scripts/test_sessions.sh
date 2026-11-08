#!/usr/bin/env bash
# 会话管理端到端验收：列表/改名/软删除/跨用户 404/history limit。
#
# 用法：./scripts/test_sessions.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PG_PORT="${AIP_TEST_PG_PORT:-55440}"
APP_PORT="${AIP_TEST_APP_PORT:-8102}"
PG_NAME="${AIP_TEST_PG_CONTAINER:-aip-pg-sessions-test}"
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
AIP_RPM_LIMIT=1000 \
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
  -d '{"email":"sessions@example.com","password":"password123"}' "$BASE/v1/auth/register")"
TOKEN="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["access_token"])' <<<"$REGISTER")"

create_session() {
  curl -sS -D - -o /dev/null -H "Authorization: Bearer $TOKEN" \
    -H 'Content-Type: application/json' -d "{\"message\":\"$1\",\"stream\":false}" \
    "$BASE/v1/chat/completions" | tr -d '\r' | awk -F': ' 'tolower($1)=="x-session-id"{print $2}'
}
SID1="$(create_session first)"
SID2="$(create_session second)"
[[ -n "$SID1" && -n "$SID2" ]] || { echo "FAIL: 未创建会话" >&2; exit 1; }

LIST="$(curl -sS -H "Authorization: Bearer $TOKEN" "$BASE/v1/sessions?limit=10")"
echo "$LIST" | grep -q "\"id\":\"$SID1\"" || { echo "FAIL: 列表缺少 SID1" >&2; exit 1; }
echo "$LIST" | grep -q "\"id\":\"$SID2\"" || { echo "FAIL: 列表缺少 SID2" >&2; exit 1; }

PATCHED="$(curl -sS -X PATCH -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' -d '{"title":"重命名后的会话"}' \
  "$BASE/v1/sessions/$SID1")"
echo "$PATCHED" | grep -q '重命名后的会话' || { echo "FAIL: 改名未生效: $PATCHED" >&2; exit 1; }

DELETED="$(curl -sS -X DELETE -H "Authorization: Bearer $TOKEN" "$BASE/v1/sessions/$SID2")"
echo "$DELETED" | grep -q '"deleted":true' || { echo "FAIL: 删除未生效: $DELETED" >&2; exit 1; }

LIST2="$(curl -sS -H "Authorization: Bearer $TOKEN" "$BASE/v1/sessions?limit=10")"
echo "$LIST2" | grep -q "\"id\":\"$SID2\"" && { echo "FAIL: 删除后列表仍出现 SID2" >&2; exit 1; }

BOB="$(curl -sS -H 'Content-Type: application/json' \
  -d '{"email":"sessions-bob@example.com","password":"password123"}' "$BASE/v1/auth/register")"
BOB_TOKEN="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["access_token"])' <<<"$BOB")"
for method in GET PATCH DELETE; do
  if [[ "$method" == "GET" ]]; then
    STATUS="$(curl -sS -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $BOB_TOKEN" \
      "$BASE/v1/sessions/$SID1/messages")"
  elif [[ "$method" == "PATCH" ]]; then
    STATUS="$(curl -sS -o /dev/null -w '%{http_code}' -X PATCH -H "Authorization: Bearer $BOB_TOKEN" \
      -H 'Content-Type: application/json' -d '{"title":"hijack"}' "$BASE/v1/sessions/$SID1")"
  else
    STATUS="$(curl -sS -o /dev/null -w '%{http_code}' -X DELETE -H "Authorization: Bearer $BOB_TOKEN" \
      "$BASE/v1/sessions/$SID1")"
  fi
  [[ "$STATUS" == "404" ]] || { echo "FAIL: 跨用户 $method 应为 404，实际 $STATUS" >&2; exit 1; }
done

HISTORY="$(curl -sS -H "Authorization: Bearer $TOKEN" "$BASE/v1/sessions/$SID1/messages?limit=1")"
python3 - "$HISTORY" <<'PY'
import json, sys
data = json.loads(sys.argv[1])
assert len(data["messages"]) == 1, data
assert data["messages"][0]["role"] == "assistant", data
PY

echo "sessions OK: list/rename/delete/ownership/history limit"
