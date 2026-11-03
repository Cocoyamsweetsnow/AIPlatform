#!/usr/bin/env bash
# 冒烟测试脚本：验证一个正在运行的实例（默认 http://127.0.0.1:8080）。
# 用法: ./scripts/demo.sh [base_url] [token]
set -euo pipefail

BASE="${1:-http://127.0.0.1:8080}"
TOKEN="${2:-}"
AUTH=()
if [[ -n "$TOKEN" ]]; then
  AUTH=(-H "Authorization: Bearer $TOKEN")
fi

echo "== 1. 健康检查"
curl -sS "${BASE}/healthz"; echo

if [[ "${SKIP_UI:-0}" != "1" ]]; then
  echo
  echo "== 2. 内置 Web UI"
  HTML="$(curl -sS "${BASE}/")"
  if [[ "$HTML" != *"<title>AIPlatform</title>"* ]]; then
    echo "FAIL: GET / 没有返回内置页面（AIP_WEB=false？可设 SKIP_UI=1 跳过）" >&2
    exit 1
  fi
  echo "GET / -> $(curl -sS -o /dev/null -w '%{http_code} %{content_type}' "${BASE}/")"
  echo "GET /assets/app.js -> $(curl -sS -o /dev/null -w '%{http_code} %{content_type}' "${BASE}/assets/app.js")"
  echo "GET /v1/config -> $(curl -sS "${BASE}/v1/config")"
fi

echo
echo "== 3. 流式对话（逐字输出）"
curl -sS -N "${BASE}/v1/chat/completions" \
  "${AUTH[@]}" -H 'Content-Type: application/json' \
  -d '{"message":"用三句话介绍一下 C++20 的协程"}'

echo
echo "== 4. 多轮对话（记住上下文）"
SID=$(curl -sS -D - -o /dev/null "${BASE}/v1/chat/completions" \
  "${AUTH[@]}" -H 'Content-Type: application/json' \
  -d '{"message":"记住：我的名字叫小明"}' \
  | tr -d '\r' | awk -F': ' 'tolower($1)=="x-session-id"{print $2}')
echo "session_id=$SID"

curl -sS -N "${BASE}/v1/chat/completions" \
  "${AUTH[@]}" -H 'Content-Type: application/json' \
  -d "{\"message\":\"我叫什么名字？\",\"session_id\":\"$SID\",\"stream\":false}"
echo

echo
echo "== 5. 会话历史"
curl -sS "${BASE}/v1/sessions/${SID}/messages" "${AUTH[@]}" | head -c 600; echo

echo
echo "== 6. 错误处理"
curl -sS -o /dev/null -w '空请求 -> %{http_code}\n' "${BASE}/v1/chat/completions" "${AUTH[@]}" -d '{}'
curl -sS -o /dev/null -w '非法 JSON -> %{http_code}\n' "${BASE}/v1/chat/completions" "${AUTH[@]}" -d 'oops'
curl -sS -o /dev/null -w '字段类型错误 -> %{http_code}\n' "${BASE}/v1/chat/completions" "${AUTH[@]}" \
  -H 'Content-Type: application/json' -d '{"message":"hi","stream":"yes"}'
curl -sS -o /dev/null -w '未知路由 -> %{http_code}\n' "${BASE}/nope"
