#!/usr/bin/env bash
# PostgreSQL 集成测试：起一个临时 postgres:16 容器，跑 ctest -R pg_tests，然后清理。
#
# 用法：./scripts/test_pg.sh [port]
# 前置：已用 -DAIPLATFORM_BUILD_TESTS=ON -DAIPLATFORM_WITH_POSTGRES=ON 编译。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${1:-55433}"
NAME="${AIP_TEST_PG_CONTAINER:-aip-pg-test}"
DSN="postgresql://aiplatform:aiplatform@127.0.0.1:${PORT}/aiplatform_test"

is_ours() {
  docker inspect "$NAME" >/dev/null 2>&1 || return 1
  local label
  label="$(docker inspect -f '{{index .Config.Labels "aiplatform.test"}}' "$NAME" 2>/dev/null || true)"
  [[ "$label" == "1" ]]
}

cleanup() {
  if is_ours; then
    docker rm -f "$NAME" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

if docker inspect "$NAME" >/dev/null 2>&1 && ! is_ours; then
  echo "FAIL: 容器 $NAME 已存在且不是本脚本创建的，拒绝删除" >&2
  exit 1
fi
if ! docker inspect "$NAME" >/dev/null 2>&1; then
  docker run -d --rm --name "$NAME" --label aiplatform.test=1 \
    -e POSTGRES_USER=aiplatform \
    -e POSTGRES_PASSWORD=aiplatform \
    -e POSTGRES_DB=aiplatform_test \
    -p "${PORT}:5432" postgres:16 >/dev/null
fi

for _ in $(seq 1 60); do
  if docker exec "$NAME" psql -U aiplatform -d aiplatform_test -c 'select 1' >/dev/null 2>&1; then
    break
  fi
  sleep 1
done
if ! docker exec "$NAME" psql -U aiplatform -d aiplatform_test -c 'select 1' >/dev/null 2>&1; then
  echo "FAIL: PostgreSQL 未在 60s 内就绪" >&2
  docker logs "$NAME" 2>&1 | tail -30 >&2 || true
  exit 1
fi

echo "PostgreSQL ready on 127.0.0.1:${PORT}"
AIP_TEST_PG_DSN="$DSN" ctest --test-dir "$ROOT/build" -R pg_tests --output-on-failure
