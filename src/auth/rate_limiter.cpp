#include "auth/pg_rate_limiter.h"

#include <algorithm>
#include <string>

namespace auth {

RateLimitDecision PgRateLimiter::check(const std::string& bucket, int limit, int window_s) {
  RateLimitDecision decision;
  if (limit <= 0) return decision;

  const int window = std::clamp(window_s, 1, 3600);
  const storage::Result result = db_.exec(
      "INSERT INTO rate_limits (bucket, window_start, count)"
      " VALUES ($1, to_timestamp(floor(extract(epoch from now()) / $2::int) * $2::int), 1)"
      " ON CONFLICT (bucket, window_start)"
      " DO UPDATE SET count = rate_limits.count + 1"
      " RETURNING count,"
      "   GREATEST(1, CEIL(EXTRACT(EPOCH FROM"
      "     (window_start + make_interval(secs => $2::int)) - now())))::int",
      {bucket, std::to_string(window)});

  if (result.rows() > 0) {
    const long long count = result.int64(0, 0);
    if (count > limit) {
      decision.allowed = false;
      decision.retry_after_s = std::max(1, static_cast<int>(result.int64(0, 1)));
    }
  }

  // 低频清理，避免表无限增长；失败不影响本次判定。
  if ((calls_.fetch_add(1) % 1000) == 0) {
    try {
      db_.exec("DELETE FROM rate_limits WHERE window_start < now() - interval '1 day'");
    } catch (const std::exception&) {
      // 清理是尽力而为。
    }
  }
  return decision;
}

}  // namespace auth
