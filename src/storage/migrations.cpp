#include "storage/migrations.h"

#include <set>
#include <stdexcept>
#include <string>

#include "common/log.h"
#include "storage/db.h"

namespace storage {
namespace {

// 固定的 advisory lock key；同一时刻只允许一个实例跑迁移。
constexpr const char* kMigrationLockKey = "720240101";

struct Migration {
  int version;
  const char* name;
  const char* sql;
};

constexpr Migration kMigrations[] = {
    {1, "initial_persistence_and_auth_schema", R"SQL(
CREATE TABLE users (
  id            BIGSERIAL PRIMARY KEY,
  email         TEXT NOT NULL,
  password_hash TEXT NOT NULL DEFAULT '',
  display_name  TEXT NOT NULL DEFAULT '',
  role          TEXT NOT NULL DEFAULT 'user' CHECK (role IN ('user', 'admin')),
  created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
  disabled_at   TIMESTAMPTZ
);
CREATE UNIQUE INDEX users_email_lower_uniq ON users (lower(email));

CREATE TABLE api_keys (
  id           BIGSERIAL PRIMARY KEY,
  user_id      BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  name         TEXT NOT NULL DEFAULT '',
  key_prefix   TEXT NOT NULL,
  key_hash     TEXT NOT NULL UNIQUE,
  created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
  last_used_at TIMESTAMPTZ,
  revoked_at   TIMESTAMPTZ
);
CREATE INDEX api_keys_user_idx ON api_keys (user_id);

CREATE TABLE sessions (
  id         TEXT PRIMARY KEY,
  user_id    BIGINT REFERENCES users(id) ON DELETE CASCADE,
  title      TEXT NOT NULL DEFAULT '新会话',
  model      TEXT NOT NULL DEFAULT '',
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  deleted_at TIMESTAMPTZ
);
CREATE INDEX sessions_user_updated_idx
  ON sessions (user_id, updated_at DESC)
  WHERE deleted_at IS NULL;
CREATE INDEX sessions_updated_idx
  ON sessions (updated_at DESC)
  WHERE deleted_at IS NULL;

CREATE TABLE messages (
  id          BIGSERIAL PRIMARY KEY,
  session_id  TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
  role        TEXT NOT NULL CHECK (role IN ('system', 'user', 'assistant', 'tool')),
  content     TEXT NOT NULL,
  token_count INT NOT NULL DEFAULT 0,
  created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX messages_session_id_id_idx ON messages (session_id, id);

CREATE TABLE usage_records (
  id                BIGSERIAL PRIMARY KEY,
  user_id           BIGINT REFERENCES users(id) ON DELETE SET NULL,
  session_id        TEXT,
  request_id        TEXT,
  model             TEXT NOT NULL,
  prompt_tokens     INT NOT NULL DEFAULT 0,
  completion_tokens INT NOT NULL DEFAULT 0,
  latency_ms        INT NOT NULL DEFAULT 0,
  ttft_ms           INT NOT NULL DEFAULT -1,
  created_at        TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX usage_records_user_time_idx ON usage_records (user_id, created_at DESC);

CREATE TABLE request_dedup (
  scope                 TEXT NOT NULL,
  request_id            TEXT NOT NULL,
  session_id            TEXT,
  status                TEXT NOT NULL CHECK (status IN ('in_progress', 'completed', 'failed')),
  assistant_message_id  BIGINT,
  error_type            TEXT,
  error_message         TEXT,
  created_at            TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at            TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (scope, request_id)
);
CREATE INDEX request_dedup_status_idx ON request_dedup (status, updated_at);
)SQL"},
    {2, "account_security_and_rate_limits", R"SQL(
ALTER TABLE users ADD COLUMN email_verified_at TIMESTAMPTZ;
ALTER TABLE users ADD COLUMN token_version INT NOT NULL DEFAULT 0;

CREATE TABLE refresh_tokens (
  jti         TEXT PRIMARY KEY,
  user_id     BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  expires_at  TIMESTAMPTZ NOT NULL,
  revoked_at  TIMESTAMPTZ,
  replaced_by TEXT,
  created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX refresh_tokens_user_idx ON refresh_tokens (user_id, expires_at DESC);
CREATE INDEX refresh_tokens_expiry_idx ON refresh_tokens (expires_at);

CREATE TABLE one_time_tokens (
  token_hash TEXT PRIMARY KEY,
  user_id    BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  purpose    TEXT NOT NULL CHECK (purpose IN ('email_verify', 'password_reset')),
  expires_at TIMESTAMPTZ NOT NULL,
  used_at    TIMESTAMPTZ,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX one_time_tokens_user_idx ON one_time_tokens (user_id, purpose);
CREATE INDEX one_time_tokens_expiry_idx ON one_time_tokens (expires_at);

CREATE TABLE rate_limits (
  bucket       TEXT NOT NULL,
  window_start TIMESTAMPTZ NOT NULL,
  count        INT NOT NULL DEFAULT 0,
  PRIMARY KEY (bucket, window_start)
);
CREATE INDEX rate_limits_window_idx ON rate_limits (window_start);
)SQL"},
    {3, "distributed_quota_reservations", R"SQL(
CREATE TABLE quota_reservations (
  id         TEXT PRIMARY KEY,
  user_id    BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  tokens     INT NOT NULL DEFAULT 0,
  expires_at TIMESTAMPTZ NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX quota_reservations_user_idx
  ON quota_reservations (user_id, expires_at);

CREATE TABLE quota_stream_leases (
  id         TEXT PRIMARY KEY,
  user_id    BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  expires_at TIMESTAMPTZ NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX quota_stream_leases_user_idx
  ON quota_stream_leases (user_id, expires_at);
)SQL"},
};

}  // namespace

void run_migrations(DbPool& pool) {
  Lease conn = pool.acquire();
  conn.exec("SELECT pg_advisory_lock($1::bigint)", {kMigrationLockKey});

  try {
    conn.exec_simple(
        "CREATE TABLE IF NOT EXISTS schema_migrations ("
        "  version    INT PRIMARY KEY,"
        "  name       TEXT NOT NULL,"
        "  applied_at TIMESTAMPTZ NOT NULL DEFAULT now()"
        ")");

    auto applied_result = conn.exec("SELECT version FROM schema_migrations");
    std::set<int> applied;
    for (int row = 0; row < applied_result.rows(); ++row) {
      applied.insert(static_cast<int>(applied_result.int64(row, 0)));
    }

    for (const Migration& migration : kMigrations) {
      if (applied.count(migration.version) != 0) continue;

      platform::log_info("应用数据库迁移 v" + std::to_string(migration.version) + ": " +
                         migration.name);
      conn.exec_simple("BEGIN");
      try {
        conn.exec_simple(migration.sql);
        conn.exec("INSERT INTO schema_migrations (version, name) VALUES ($1, $2)",
                  {std::to_string(migration.version), migration.name});
        conn.exec_simple("COMMIT");
      } catch (...) {
        try {
          conn.exec_simple("ROLLBACK");
        } catch (const std::exception& e) {
          platform::log_warn(std::string("迁移 ROLLBACK 失败: ") + e.what());
        }
        throw;
      }
    }
  } catch (...) {
    try {
      conn.exec("SELECT pg_advisory_unlock($1::bigint)", {kMigrationLockKey});
    } catch (const std::exception& e) {
      platform::log_warn(std::string("释放迁移锁失败: ") + e.what());
    }
    throw;
  }

  conn.exec("SELECT pg_advisory_unlock($1::bigint)", {kMigrationLockKey});
}

}  // namespace storage
