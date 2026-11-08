#!/usr/bin/env bash
# 验证「首 token 前重试」：假上游前 2 次返回 429，第 3 次正常返回 SSE。
# 用法: ./scripts/test_retry.sh [fake_port] [app_port]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FAKE_PORT="${1:-9099}"
APP_PORT="${2:-8081}"
LOG_DIR="$(mktemp -d)"
FAKE_PID=""
APP_PID=""

cleanup() {
  if [[ -n "$APP_PID" ]]; then kill "$APP_PID" 2>/dev/null || true; fi
  if [[ -n "$FAKE_PID" ]]; then kill "$FAKE_PID" 2>/dev/null || true; fi
  wait 2>/dev/null || true
  rm -rf "$LOG_DIR"
}
trap cleanup EXIT

FAKE_FAIL_FIRST=2 python3 "$ROOT/scripts/fake_upstream.py" "$FAKE_PORT" \
  >"$LOG_DIR/fake.log" 2>&1 &
FAKE_PID=$!

for _ in $(seq 1 50); do
  if (exec 3<>"/dev/tcp/127.0.0.1/$FAKE_PORT") 2>/dev/null; then
    exec 3>&-
    break
  fi
  sleep 0.1
done

AIP_PROVIDER=openai \
AIP_BASE_URL="http://127.0.0.1:$FAKE_PORT" \
AIP_API_KEY=good-key \
AIP_MAX_RETRIES=2 \
AIP_RETRY_BASE_MS=50 \
AIP_PORT="$APP_PORT" \
AIP_WEB=false \
AIP_TOKEN= \
"$ROOT/build/aiplatform" >"$LOG_DIR/app.log" 2>&1 &
APP_PID=$!

for _ in $(seq 1 50); do
  if curl -fsS "http://127.0.0.1:$APP_PORT/healthz" >/dev/null 2>&1; then break; fi
  sleep 0.1
done

BODY="$(curl -sS -N "http://127.0.0.1:$APP_PORT/v1/chat/completions" \
  -H 'Content-Type: application/json' -d '{"message":"你好"}')"

if ! grep -q '上游' <<<"$BODY"; then
  echo "FAIL: 没有收到正常回复" >&2
  echo "$BODY" >&2
  echo "--- app.log ---" >&2
  cat "$LOG_DIR/app.log" >&2
  exit 1
fi

ATTEMPTS="$(grep -c 'POST /chat/completions' "$LOG_DIR/fake.log" || true)"
if [[ "$ATTEMPTS" != "3" ]]; then
  echo "FAIL: 期望 3 次上游请求（2 次 429 + 1 次成功），实际 $ATTEMPTS" >&2
  cat "$LOG_DIR/fake.log" >&2
  exit 1
fi

echo "retry smoke OK: 2 次 429 后第 3 次成功，SSE 内容正常"
