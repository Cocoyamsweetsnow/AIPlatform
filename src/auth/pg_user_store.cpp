#include "auth/pg_user_store.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace auth {
namespace {

constexpr const char* kUserSelect =
    "SELECT id, email, display_name, role, password_hash, (disabled_at IS NOT NULL)"
    " FROM users";

UserRecord read_user(const storage::Result& result, int row) {
  UserRecord user;
  user.id = result.int64(row, 0);
  user.email = result.value(row, 1);
  user.display_name = result.value(row, 2);
  user.role = result.value(row, 3);
  user.password_hash = result.value(row, 4);
  user.disabled = result.value(row, 5) == "t";
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
        " RETURNING id, email, display_name, role, password_hash, (disabled_at IS NOT NULL)",
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

}  // namespace auth
