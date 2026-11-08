#!/usr/bin/env bash
# 幂等端到端验收：非流式/流式重放、in_progress 409、failed 可重试。
#
# 用法：./scripts/test_idempotency.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PG_PORT="${AIP_TEST_PG_PORT:-55439}"
APP_PORT="${AIP_TEST_APP_PORT:-8101}"
PG_NAME="${AIP_TEST_PG_CONTAINER:-aip-pg-idem-test}"
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
  -d '{"email":"idem@example.com","password":"password123"}' "$BASE/v1/auth/register")"
TOKEN="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["access_token"])' <<<"$REGISTER")"
USER_ID="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["user"]["id"])' <<<"$REGISTER")"
SCOPE="u:${USER_ID}"

# 非流式：第一次执行，第二次重放
for n in 1 2; do
  curl -sS -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
    -d '{"message":"幂等非流式","stream":false,"request_id":"req-nonstream"}' \
    "$BASE/v1/chat/completions" >"$LOG_DIR/nonstream$n.json"
done
python3 - "$LOG_DIR/nonstream1.json" "$LOG_DIR/nonstream2.json" <<'PY'
import json, sys
a = json.load(open(sys.argv[1]))
b = json.load(open(sys.argv[2]))
assert a["session_id"] == b["session_id"], (a, b)
assert a["choices"][0]["message"]["content"] == b["choices"][0]["message"]["content"]
assert a.get("idempotent_replay") is False
assert b.get("idempotent_replay") is True
PY

# 流式：第一次执行，第二次重放，拼接后的文本一致
for n in 1 2; do
  curl -sS -N -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
    -d '{"message":"幂等流式","stream":true,"request_id":"req-stream"}' \
    "$BASE/v1/chat/completions" >"$LOG_DIR/stream$n.txt"
done
python3 - "$LOG_DIR/stream1.txt" "$LOG_DIR/stream2.txt" <<'PY'
import json, sys
def text(path):
    out = []
    for line in open(path):
        if not line.startswith("data: "):
            continue
        payload = line[6:].strip()
        if payload == "[DONE]":
            continue
        try:
            obj = json.loads(payload)
        except Exception:
            continue
        out.append(obj.get("choices", [{}])[0].get("delta", {}).get("content", "") or "")
    return "".join(out)
a = text(sys.argv[1])
b = text(sys.argv[2])
assert a and a == b, (a, b)
PY

# in_progress：手工占位，重复请求应 409 + Retry-After
docker exec "$PG_NAME" psql -U aiplatform -d aiplatform_test -c \
  "insert into request_dedup(scope,request_id,status) values('$SCOPE','req-busy','in_progress');" \
  >/dev/null
HEADERS="$(curl -sS -D - -o /dev/null -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' -d '{"message":"busy","stream":false,"request_id":"req-busy"}' \
  "$BASE/v1/chat/completions" | tr -d '\r')"
echo "$HEADERS" | grep -q '^HTTP/1.1 409' || { echo "FAIL: in_progress 应返回 409" >&2; exit 1; }
echo "$HEADERS" | grep -qi '^Retry-After: 1' || { echo "FAIL: 409 缺少 Retry-After" >&2; exit 1; }
docker exec "$PG_NAME" psql -U aiplatform -d aiplatform_test -c \
  "delete from request_dedup where request_id='req-busy';" >/dev/null

# failed：重置为 in_progress 后允许重试成功
docker exec "$PG_NAME" psql -U aiplatform -d aiplatform_test -c \
  "insert into request_dedup(scope,request_id,status,error_type,error_message) values('$SCOPE','req-failed','failed','upstream_error','temp');" \
  >/dev/null
STATUS="$(curl -sS -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' -d '{"message":"retry","stream":false,"request_id":"req-failed"}' \
  "$BASE/v1/chat/completions")"
[[ "$STATUS" == "200" ]] || { echo "FAIL: failed 重试应为 200，实际 $STATUS" >&2; exit 1; }
DB_STATUS="$(docker exec "$PG_NAME" psql -U aiplatform -d aiplatform_test -tAc \
  "select status from request_dedup where scope='$SCOPE' and request_id='req-failed';" | tr -d ' ')"
[[ "$DB_STATUS" == "completed" ]] || { echo "FAIL: retry 后状态应为 completed，实际 $DB_STATUS" >&2; exit 1; }

echo "idempotency OK: nonstream/stream replay, in_progress 409, failed retry"
