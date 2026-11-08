#include "chat/pg_session_store.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "chat/errors.h"
#include "util/id.h"

namespace chat {
namespace {

std::string title_or_default(std::string title) {
  if (title.empty()) return "新会话";
  return title;
}

std::string bool_text(bool value) { return value ? "true" : "false"; }

SessionInfo read_info(const storage::Result& result, int row) {
  SessionInfo info;
  info.id = result.value(row, 0);
  info.user_id = result.int64(row, 1);
  info.title = result.value(row, 2);
  info.model = result.value(row, 3);
  info.created_at = result.int64(row, 4);
  info.updated_at = result.int64(row, 5);
  info.message_count = static_cast<std::size_t>(result.int64(row, 6));
  return info;
}

constexpr const char* kInfoSelect =
    "SELECT s.id, COALESCE(s.user_id, 0), s.title, s.model,"
    "       EXTRACT(EPOCH FROM s.created_at)::bigint,"
    "       EXTRACT(EPOCH FROM s.updated_at)::bigint,"
    "       (SELECT count(*) FROM messages m WHERE m.session_id = s.id)"
    " FROM sessions s";

constexpr const char* kOwnerFilter =
    " AND ($1::boolean OR (s.user_id IS NOT NULL AND s.user_id = $2::bigint))";

}  // namespace

std::string PgSessionStore::create(std::int64_t user_id, const std::string& title,
                                   const std::string& model) {
  for (int attempt = 0; attempt < 8; ++attempt) {
    const std::string id = util::random_id("s_");
    try {
      db_.exec(
          "INSERT INTO sessions (id, user_id, title, model)"
          " VALUES ($1, NULLIF($2::bigint, 0), $3, $4)",
          {id, std::to_string(user_id), title_or_default(title), model});
      return id;
    } catch (const storage::DbError& e) {
      if (e.sqlstate() == "23505") continue;  // 极小概率的主键碰撞，换一个 ID
      throw;
    }
  }
  throw std::runtime_error("生成会话 ID 失败");
}

bool PgSessionStore::ensure(const std::string& session_id, std::int64_t user_id,
                            const std::string& title, const std::string& model) {
  if (session_id.empty()) return false;
  const storage::Result result = db_.exec(
      "INSERT INTO sessions (id, user_id, title, model)"
      " VALUES ($1, NULLIF($2::bigint, 0), $3, $4)"
      " ON CONFLICT (id) DO NOTHING",
      {session_id, std::to_string(user_id), title_or_default(title), model});
  return result.affected_rows() > 0;
}

bool PgSessionStore::exists(const std::string& session_id) const {
  const storage::Result result =
      db_.exec("SELECT 1 FROM sessions WHERE id = $1 AND deleted_at IS NULL", {session_id});
  return result.rows() > 0;
}

bool PgSessionStore::owns(std::int64_t user_id, bool is_admin,
                          const std::string& session_id) const {
  const storage::Result result = db_.exec(
      "SELECT 1 FROM sessions s"
      " WHERE s.id = $1 AND s.deleted_at IS NULL"
      "   AND ($2::boolean OR (s.user_id IS NOT NULL AND s.user_id = $3::bigint))",
      {session_id, bool_text(is_admin), std::to_string(user_id)});
  return result.rows() > 0;
}

std::int64_t PgSessionStore::append(const std::string& session_id,
                                    const llm::Message& message) {
  storage::Transaction tx(db_.acquire());

  const storage::Result updated = tx.exec(
      "UPDATE sessions SET updated_at = now() WHERE id = $1 AND deleted_at IS NULL",
      {session_id});
  if (updated.affected_rows() == 0) throw SessionNotFound(session_id);

  const storage::Result inserted = tx.exec(
      "INSERT INTO messages (session_id, role, content) VALUES ($1, $2, $3) RETURNING id",
      {session_id, message.role, message.content});
  const std::int64_t message_id = inserted.int64(0, 0);
  tx.commit();
  return message_id;
}

std::vector<llm::Message> PgSessionStore::history(const std::string& session_id,
                                                  std::size_t limit) const {
  std::string sql =
      "SELECT m.role, m.content FROM messages m"
      " JOIN sessions s ON s.id = m.session_id"
      " WHERE m.session_id = $1 AND s.deleted_at IS NULL"
      " ORDER BY m.id DESC";
  std::vector<std::optional<std::string>> params{session_id};
  if (limit != 0) {
    sql += " LIMIT $2";
    params.push_back(std::to_string(limit));
  }
  const storage::Result result = db_.exec(sql, params);

  std::vector<llm::Message> out;
  out.reserve(static_cast<std::size_t>(result.rows()));
  for (int row = result.rows() - 1; row >= 0; --row) {
    out.push_back(llm::Message{result.value(row, 0), result.value(row, 1)});
  }
  return out;
}

std::size_t PgSessionStore::message_count(const std::string& session_id) const {
  const storage::Result result = db_.exec(
      "SELECT count(*) FROM messages m JOIN sessions s ON s.id = m.session_id"
      " WHERE m.session_id = $1 AND s.deleted_at IS NULL",
      {session_id});
  return result.rows() == 0 ? 0 : static_cast<std::size_t>(result.int64(0, 0));
}

std::size_t PgSessionStore::session_count() const {
  const storage::Result result =
      db_.exec("SELECT count(*) FROM sessions WHERE deleted_at IS NULL");
  return result.rows() == 0 ? 0 : static_cast<std::size_t>(result.int64(0, 0));
}

std::optional<llm::Message> PgSessionStore::message(std::int64_t message_id) const {
  const storage::Result result =
      db_.exec("SELECT role, content FROM messages WHERE id = $1", {std::to_string(message_id)});
  if (result.rows() == 0) return std::nullopt;
  return llm::Message{result.value(0, 0), result.value(0, 1)};
}

std::optional<SessionInfo> PgSessionStore::get(const std::string& session_id) const {
  const std::string sql = std::string(kInfoSelect) + " WHERE s.id = $1 AND s.deleted_at IS NULL";
  const storage::Result result = db_.exec(sql, {session_id});
  if (result.rows() == 0) return std::nullopt;
  return read_info(result, 0);
}

std::vector<SessionInfo> PgSessionStore::list(std::int64_t user_id, bool is_admin,
                                              std::size_t limit, std::size_t offset) const {
  const std::size_t take = std::min(limit == 0 ? 20 : limit, static_cast<std::size_t>(200));
  const std::string sql = std::string(kInfoSelect) +
                          " WHERE s.deleted_at IS NULL" + kOwnerFilter +
                          " ORDER BY s.updated_at DESC, s.id ASC LIMIT $3 OFFSET $4";
  const storage::Result result =
      db_.exec(sql, {bool_text(is_admin), std::to_string(user_id), std::to_string(take),
                     std::to_string(offset)});

  std::vector<SessionInfo> out;
  out.reserve(static_cast<std::size_t>(result.rows()));
  for (int row = 0; row < result.rows(); ++row) out.push_back(read_info(result, row));
  return out;
}

bool PgSessionStore::rename(std::int64_t user_id, bool is_admin,
                            const std::string& session_id, const std::string& title) {
  const storage::Result result = db_.exec(
      "UPDATE sessions s SET title = $4, updated_at = now()"
      " WHERE s.id = $1 AND s.deleted_at IS NULL"
      "   AND ($2::boolean OR (s.user_id IS NOT NULL AND s.user_id = $3::bigint))",
      {session_id, bool_text(is_admin), std::to_string(user_id), title_or_default(title)});
  return result.affected_rows() > 0;
}

bool PgSessionStore::remove(std::int64_t user_id, bool is_admin,
                            const std::string& session_id) {
  const storage::Result result = db_.exec(
      "UPDATE sessions s SET deleted_at = now(), updated_at = now()"
      " WHERE s.id = $1 AND s.deleted_at IS NULL"
      "   AND ($2::boolean OR (s.user_id IS NOT NULL AND s.user_id = $3::bigint))",
      {session_id, bool_text(is_admin), std::to_string(user_id)});
  return result.affected_rows() > 0;
}

}  // namespace chat
