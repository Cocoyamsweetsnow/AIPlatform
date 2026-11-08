#!/usr/bin/env bash
# 鉴权端到端验收：注册/登录/JWT/API Key/跨用户隔离/撤销/刷新。
#
# 用法：./scripts/test_auth.sh
# 前置：build/aiplatform 已用 -DAIPLATFORM_WITH_POSTGRES=ON -DAIPLATFORM_WITH_AUTH=ON 编译。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PG_PORT="${AIP_TEST_PG_PORT:-55437}"
APP_PORT="${AIP_TEST_APP_PORT:-8099}"
PG_NAME="${AIP_TEST_PG_CONTAINER:-aip-pg-auth-test}"
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
AIP_TOKEN='admin-static-token' \
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

# /v1/config 要告诉前端：账号体系可用，同时静态 Token 也可用；只能暴露能力，不能暴露密钥。
CONFIG_JSON="$(curl -sS "$BASE/v1/config")"
python3 - "$CONFIG_JSON" <<'PYCHECK'
import json, sys
config = json.loads(sys.argv[1])
auth = config.get("auth") or {}
assert auth.get("user_accounts") is True, auth
assert auth.get("password_login") is True, auth
assert auth.get("registration") is True, auth
assert auth.get("static_token") is True, auth
assert config.get("auth_required") is True, config
assert "secret" not in json.dumps(config).lower(), config
PYCHECK

STATUS="$(curl -sS -o /dev/null -w '%{http_code}' -H 'Content-Type: application/json' \
  -d '{"message":"no token"}' "$BASE/v1/chat/completions")"
[[ "$STATUS" == "401" ]] || { echo "FAIL: 无 token 应为 401，实际 $STATUS" >&2; exit 1; }

REGISTER="$(curl -sS -H 'Content-Type: application/json' \
  -d '{"email":"alice@example.com","password":"password123","display_name":"Alice"}' \
  "$BASE/v1/auth/register")"
ACCESS="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["access_token"])' <<<"$REGISTER")"
REFRESH="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["refresh_token"])' <<<"$REGISTER")"
[[ -n "$ACCESS" && -n "$REFRESH" ]] || { echo "FAIL: 注册未返回 token" >&2; exit 1; }

ME="$(curl -sS -H "Authorization: Bearer $ACCESS" "$BASE/v1/auth/me")"
echo "$ME" | grep -q 'alice@example.com' || { echo "FAIL: /me 不正确: $ME" >&2; exit 1; }

SID="$(curl -sS -D - -o /dev/null -H "Authorization: Bearer $ACCESS" \
  -H 'Content-Type: application/json' -d '{"message":"我是 Alice"}' \
  "$BASE/v1/chat/completions" | tr -d '\r' | awk -F': ' 'tolower($1)=="x-session-id"{print $2}')"
[[ -n "$SID" ]] || { echo "FAIL: 未拿到 X-Session-Id" >&2; exit 1; }

BOB="$(curl -sS -H 'Content-Type: application/json' \
  -d '{"email":"bob@example.com","password":"password123"}' "$BASE/v1/auth/register")"
BOB_ACCESS="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["access_token"])' <<<"$BOB")"
STATUS="$(curl -sS -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $BOB_ACCESS" \
  "$BASE/v1/sessions/$SID/messages")"
[[ "$STATUS" == "404" ]] || { echo "FAIL: 跨用户历史应为 404，实际 $STATUS" >&2; exit 1; }

KEY_JSON="$(curl -sS -H "Authorization: Bearer $ACCESS" -H 'Content-Type: application/json' \
  -d '{"name":"ci"}' "$BASE/v1/auth/api-keys")"
API_KEY="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["key"])' <<<"$KEY_JSON")"
KEY_ID="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])' <<<"$KEY_JSON")"
[[ "$API_KEY" == aip_* ]] || { echo "FAIL: API Key 格式不正确" >&2; exit 1; }

STATUS="$(curl -sS -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $API_KEY" \
  -H 'Content-Type: application/json' -d '{"message":"api key chat","stream":false}' \
  "$BASE/v1/chat/completions")"
[[ "$STATUS" == "200" ]] || { echo "FAIL: API Key 对话应为 200，实际 $STATUS" >&2; exit 1; }

curl -sS -H "Authorization: Bearer $ACCESS" "$BASE/v1/auth/api-keys" | grep -q '"name":"ci"' \
  || { echo "FAIL: API Key 列表缺少 ci" >&2; exit 1; }

curl -sS -H "Authorization: Bearer $ACCESS" -X DELETE "$BASE/v1/auth/api-keys/$KEY_ID" >/dev/null
STATUS="$(curl -sS -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $API_KEY" \
  -H 'Content-Type: application/json' -d '{"message":"revoked"}' "$BASE/v1/chat/completions")"
[[ "$STATUS" == "401" ]] || { echo "FAIL: 撤销后 API Key 应为 401，实际 $STATUS" >&2; exit 1; }

curl -sS -H "Authorization: Bearer $REFRESH" -H 'Content-Type: application/json' -d '{}' \
  "$BASE/v1/auth/refresh" | grep -q '"access_token"' \
  || { echo "FAIL: refresh 未返回新 access_token" >&2; exit 1; }

STATUS="$(curl -sS -o /dev/null -w '%{http_code}' -H 'Content-Type: application/json' \
  -d '{"email":"alice@example.com","password":"password123"}' "$BASE/v1/auth/register")"
[[ "$STATUS" == "409" ]] || { echo "FAIL: 重复注册应为 409，实际 $STATUS" >&2; exit 1; }

STATUS="$(curl -sS -o /dev/null -w '%{http_code}' -H 'Content-Type: application/json' \
  -d '{"email":"alice@example.com","password":"wrong-password"}' "$BASE/v1/auth/login")"
[[ "$STATUS" == "401" ]] || { echo "FAIL: 错误密码应为 401，实际 $STATUS" >&2; exit 1; }

STATUS="$(curl -sS -o /dev/null -w '%{http_code}' -H 'Authorization: Bearer admin-static-token' \
  -H 'Content-Type: application/json' -d '{"message":"admin chat","stream":false}' \
  "$BASE/v1/chat/completions")"
[[ "$STATUS" == "200" ]] || { echo "FAIL: 静态管理员对话应为 200，实际 $STATUS" >&2; exit 1; }

STATUS="$(curl -sS -o /dev/null -w '%{http_code}' -H 'Authorization: Bearer admin-static-token' \
  "$BASE/v1/auth/me")"
[[ "$STATUS" == "401" ]] || { echo "FAIL: 静态 admin /me 应为 401，实际 $STATUS" >&2; exit 1; }

echo "auth OK: register/login/JWT/API key/ownership/revoke/refresh all passed"
