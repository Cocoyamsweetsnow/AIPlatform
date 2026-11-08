#include "usage/pg_usage_sink.h"

#include <algorithm>
#include <string>

namespace usage {

void PgUsageSink::record(const UsageEvent& event) {
  db_.exec(
      "INSERT INTO usage_records"
      " (user_id, session_id, request_id, model, prompt_tokens, completion_tokens,"
      "  latency_ms, ttft_ms)"
      " VALUES (NULLIF($1::bigint, 0), $2, $3, $4, $5, $6, $7, $8)",
      {std::to_string(event.user_id), event.session_id, event.request_id, event.model,
       std::to_string(std::max(0, event.prompt_tokens)),
       std::to_string(std::max(0, event.completion_tokens)),
       std::to_string(std::max(0, event.latency_ms)),
       std::to_string(event.ttft_ms)});
}

UsageSummary PgUsageSink::summary_today(std::int64_t user_id) {
  const storage::Result result = db_.exec(
      "SELECT count(*), COALESCE(sum(prompt_tokens), 0), COALESCE(sum(completion_tokens), 0)"
      " FROM usage_records"
      " WHERE (($1::bigint = 0 AND user_id IS NULL) OR user_id = $1::bigint)"
      "   AND created_at >= date_trunc('day', now())",
      {std::to_string(user_id)});

  UsageSummary summary;
  if (result.rows() == 0) return summary;
  summary.requests = result.int64(0, 0);
  summary.prompt_tokens = result.int64(0, 1);
  summary.completion_tokens = result.int64(0, 2);
  return summary;
}

}  // namespace usage
