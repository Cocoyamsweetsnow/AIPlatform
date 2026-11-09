#include "auth/pg_user_store.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace auth {
namespace {

constexpr const char* kUserSelect =
    "SELECT id, email, display_name, role, password_hash, (disabled_at IS NOT NULL),"
    "       (email_verified_at IS NOT NULL), token_version"
    " FROM users";

UserRecord read_user(const storage::Result& result, int row) {
  UserRecord user;
  user.id = result.int64(row, 0);
  user.email = result.value(row, 1);
  user.display_name = result.value(row, 2);
  user.role = result.value(row, 3);
  user.password_hash = result.value(row, 4);
  user.disabled = result.value(row, 5) == "t";
  user.email_verified = result.value(row, 6) == "t";
  user.token_version = static_cast<int>(result.int64(row, 7));
  return user;
}

}  // namespace

std::optional<UserRecord> PgUserStore::find_by_email(const std::string& email) {
  const std::string sql = std::string(kUserSelect) + " WHERE lower(email) = lower($1)";
  const storage::Result result = db_.exec(sql, {email});
  if (result.rows() == 0) return std::nullopt;
  return read_user(result, 0);
}

std::optional<UserRecord> PgUserStore::find_by_id(std::int64_t user_id) {
  const std::string sql = std::string(kUserSelect) + " WHERE id = $1";
  const storage::Result result = db_.exec(sql, {std::to_string(user_id)});
  if (result.rows() == 0) return std::nullopt;
  return read_user(result, 0);
}

std::optional<UserRecord> PgUserStore::create(const std::string& email,
                                              const std::string& password_hash,
                                              const std::string& display_name) {
  try {
    const storage::Result result = db_.exec(
        "INSERT INTO users (email, password_hash, display_name) VALUES ($1, $2, $3)"
        " RETURNING id, email, display_name, role, password_hash, (disabled_at IS NOT NULL),"
        "           (email_verified_at IS NOT NULL), token_version",
        {email, password_hash, display_name});
    if (result.rows() == 0) return std::nullopt;
    return read_user(result, 0);
  } catch (const storage::DbError& e) {
    if (e.sqlstate() == "23505") return std::nullopt;  // 邮箱已存在
    throw;
  }
}

std::optional<UserRecord> PgUserStore::find_by_api_key_hash(const std::string& key_hash) {
  const storage::Result updated = db_.exec(
      "UPDATE api_keys SET last_used_at = now()"
      " WHERE key_hash = $1 AND revoked_at IS NULL"
      " RETURNING user_id",
      {key_hash});
  if (updated.rows() == 0) return std::nullopt;

  const std::int64_t user_id = updated.int64(0, 0);
  auto user = find_by_id(user_id);
  if (!user.has_value() || user->disabled) return std::nullopt;
  return user;
}

std::int64_t PgUserStore::create_api_key(std::int64_t user_id, const std::string& name,
                                         const std::string& prefix,
                                         const std::string& key_hash) {
  const storage::Result result = db_.exec(
      "INSERT INTO api_keys (user_id, name, key_prefix, key_hash)"
      " VALUES ($1, $2, $3, $4) RETURNING id",
      {std::to_string(user_id), name, prefix, key_hash});
  return result.int64(0, 0);
}

std::vector<ApiKeyInfo> PgUserStore::list_api_keys(std::int64_t user_id) {
  const storage::Result result = db_.exec(
      "SELECT id, name, key_prefix,"
      "       EXTRACT(EPOCH FROM created_at)::bigint,"
      "       COALESCE(EXTRACT(EPOCH FROM last_used_at)::bigint, 0)"
      " FROM api_keys"
      " WHERE user_id = $1 AND revoked_at IS NULL"
      " ORDER BY id DESC",
      {std::to_string(user_id)});

  std::vector<ApiKeyInfo> out;
  out.reserve(static_cast<std::size_t>(result.rows()));
  for (int row = 0; row < result.rows(); ++row) {
    ApiKeyInfo info;
    info.id = result.int64(row, 0);
    info.name = result.value(row, 1);
    info.prefix = result.value(row, 2);
    info.created_at = result.int64(row, 3);
    info.last_used_at = result.int64(row, 4);
    out.push_back(std::move(info));
  }
  return out;
}

bool PgUserStore::revoke_api_key(std::int64_t user_id, std::int64_t key_id) {
  const storage::Result result = db_.exec(
      "UPDATE api_keys SET revoked_at = now()"
      " WHERE id = $1 AND user_id = $2 AND revoked_at IS NULL",
      {std::to_string(key_id), std::to_string(user_id)});
  return result.affected_rows() > 0;
}

bool PgUserStore::mark_email_verified(std::int64_t user_id) {
  const storage::Result result = db_.exec(
      "UPDATE users SET email_verified_at = COALESCE(email_verified_at, now())"
      " WHERE id = $1",
      {std::to_string(user_id)});
  return result.affected_rows() > 0;
}

std::optional<int> PgUserStore::update_password(std::int64_t user_id,
                                                const std::string& password_hash) {
  storage::Transaction tx(db_.acquire());
  const storage::Result result = tx.exec(
      "UPDATE users SET password_hash = $2, token_version = token_version + 1"
      " WHERE id = $1 RETURNING token_version",
      {std::to_string(user_id), password_hash});
  if (result.rows() == 0) return std::nullopt;
  tx.exec(
      "UPDATE refresh_tokens SET revoked_at = now()"
      " WHERE user_id = $1 AND revoked_at IS NULL",
      {std::to_string(user_id)});
  tx.commit();
  return static_cast<int>(result.int64(0, 0));
}

bool PgUserStore::create_refresh_token(std::int64_t user_id, const std::string& jti,
                                       long long expires_at) {
  const storage::Result result = db_.exec(
      "INSERT INTO refresh_tokens (jti, user_id, expires_at)"
      " VALUES ($1, $2, to_timestamp($3::double precision))",
      {jti, std::to_string(user_id), std::to_string(expires_at)});
  return result.affected_rows() > 0;
}

bool PgUserStore::rotate_refresh_token(std::int64_t user_id, const std::string& old_jti,
                                       const std::string& new_jti, long long expires_at) {
  const storage::Result result = db_.exec(
      "WITH revoked AS ("
      "  UPDATE refresh_tokens SET revoked_at = now(), replaced_by = $3"
      "  WHERE jti = $1 AND user_id = $2 AND revoked_at IS NULL AND expires_at > now()"
      "  RETURNING jti"
      ")"
      " INSERT INTO refresh_tokens (jti, user_id, expires_at)"
      " SELECT $3, $2, to_timestamp($4::double precision) FROM revoked"
      " RETURNING jti",
      {old_jti, std::to_string(user_id), new_jti, std::to_string(expires_at)});
  return result.rows() > 0;
}

bool PgUserStore::revoke_refresh_token(std::int64_t user_id, const std::string& jti) {
  const storage::Result result = db_.exec(
      "UPDATE refresh_tokens SET revoked_at = now()"
      " WHERE jti = $1 AND user_id = $2 AND revoked_at IS NULL",
      {jti, std::to_string(user_id)});
  return result.affected_rows() > 0;
}

void PgUserStore::revoke_all_refresh_tokens(std::int64_t user_id) {
  db_.exec(
      "UPDATE refresh_tokens SET revoked_at = now()"
      " WHERE user_id = $1 AND revoked_at IS NULL",
      {std::to_string(user_id)});
}

bool PgUserStore::create_one_time_token(std::int64_t user_id, const std::string& purpose,
                                        const std::string& token_hash,
                                        long long expires_at) {
  const storage::Result result = db_.exec(
      "INSERT INTO one_time_tokens (user_id, purpose, token_hash, expires_at)"
      " VALUES ($1, $2, $3, to_timestamp($4::double precision))",
      {std::to_string(user_id), purpose, token_hash, std::to_string(expires_at)});
  return result.affected_rows() > 0;
}

std::optional<std::int64_t> PgUserStore::consume_one_time_token(
    const std::string& purpose, const std::string& token_hash) {
  const storage::Result result = db_.exec(
      "UPDATE one_time_tokens SET used_at = now()"
      " WHERE token_hash = $1 AND purpose = $2 AND used_at IS NULL AND expires_at > now()"
      " RETURNING user_id",
      {token_hash, purpose});
  if (result.rows() == 0) return std::nullopt;
  return result.int64(0, 0);
}

}  // namespace auth
