#include "chat/pg_idempotency_store.h"

#include <optional>
#include <string>

namespace chat {
namespace {

DedupStatus to_status(const std::string& value) {
  if (value == "completed") return DedupStatus::Completed;
  if (value == "failed") return DedupStatus::Failed;
  return DedupStatus::InProgress;
}

DedupRecord read_record(const storage::Result& result) {
  DedupRecord record;
  record.status = to_status(result.value(0, 0));
  record.session_id = result.optional(0, 1).value_or("");
  record.assistant_message_id = result.is_null(0, 2) ? 0 : result.int64(0, 2);
  record.error_type = result.optional(0, 3).value_or("");
  record.error_message = result.optional(0, 4).value_or("");
  return record;
}

}  // namespace

DedupRecord PgIdempotencyStore::claim(const std::string& scope,
                                      const std::string& request_id) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    const storage::Result inserted = db_.exec(
        "INSERT INTO request_dedup (scope, request_id, status)"
        " VALUES ($1, $2, 'in_progress')"
        " ON CONFLICT (scope, request_id) DO NOTHING",
        {scope, request_id});
    if (inserted.affected_rows() > 0) {
      DedupRecord record;
      record.status = DedupStatus::Claimed;
      return record;
    }

    const std::string ttl = std::to_string(stale_after_s_);
    const storage::Result existing = db_.exec(
        "SELECT status, session_id, assistant_message_id, error_type, error_message,"
        "       (status = 'in_progress' AND"
        "        updated_at < now() - ($3::int * interval '1 second')) AS stale"
        " FROM request_dedup WHERE scope = $1 AND request_id = $2",
        {scope, request_id, ttl});
    if (existing.rows() == 0) continue;  // 并发删除，重试一次

    DedupRecord record = read_record(existing);
    const bool stale = existing.value(0, 5) == "t";
    if (record.status == DedupStatus::Failed ||
        (record.status == DedupStatus::InProgress && stale)) {
      const storage::Result reset = db_.exec(
          "UPDATE request_dedup"
          " SET status = 'in_progress', error_type = NULL, error_message = NULL,"
          "     updated_at = now()"
          " WHERE scope = $1 AND request_id = $2"
          "   AND (status = 'failed' OR"
          "        (status = 'in_progress' AND"
          "         updated_at < now() - ($3::int * interval '1 second')))",
          {scope, request_id, ttl});
      if (reset.affected_rows() > 0) {
        DedupRecord claimed;
        claimed.status = DedupStatus::Claimed;
        return claimed;
      }
      continue;
    }
    return record;
  }

  DedupRecord busy;
  busy.status = DedupStatus::InProgress;
  return busy;
}

void PgIdempotencyStore::complete(const std::string& scope, const std::string& request_id,
                                  const std::string& session_id,
                                  std::int64_t assistant_message_id) {
  db_.exec(
      "INSERT INTO request_dedup"
      " (scope, request_id, session_id, status, assistant_message_id)"
      " VALUES ($1, $2, $3, 'completed', $4)"
      " ON CONFLICT (scope, request_id) DO UPDATE"
      " SET session_id = EXCLUDED.session_id,"
      "     status = 'completed',"
      "     assistant_message_id = EXCLUDED.assistant_message_id,"
      "     error_type = NULL,"
      "     error_message = NULL,"
      "     updated_at = now()",
      {scope, request_id, session_id, std::to_string(assistant_message_id)});
}

void PgIdempotencyStore::fail(const std::string& scope, const std::string& request_id,
                              const std::string& error_type,
                              const std::string& error_message) {
  db_.exec(
      "INSERT INTO request_dedup"
      " (scope, request_id, status, error_type, error_message)"
      " VALUES ($1, $2, 'failed', $3, $4)"
      " ON CONFLICT (scope, request_id) DO UPDATE"
      " SET status = 'failed',"
      "     error_type = EXCLUDED.error_type,"
      "     error_message = EXCLUDED.error_message,"
      "     updated_at = now()",
      {scope, request_id, error_type, error_message});
}

void PgIdempotencyStore::remove(const std::string& scope, const std::string& request_id) {
  db_.exec("DELETE FROM request_dedup WHERE scope = $1 AND request_id = $2",
           {scope, request_id});
}

}  // namespace chat
