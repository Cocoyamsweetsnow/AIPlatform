#include "quota/pg_quota.h"

#include <algorithm>
#include <string>
#include <utility>

#include "common/log.h"
#include "util/id.h"

namespace quota {

PgQuotaService::PgQuotaService(storage::DbPool& db, QuotaConfig config, int lease_ttl_s)
    : db_(db), config_(config), lease_ttl_s_(std::max(60, lease_ttl_s)) {}

QuotaDecision PgQuotaService::check_rpm(const std::string& user_id) {
  QuotaDecision decision;
  if (config_.rpm_limit <= 0) return decision;

  const storage::Result result = db_.exec(
      "INSERT INTO rate_limits (bucket, window_start, count)"
      " VALUES ($1, to_timestamp(floor(extract(epoch from now()) / 60) * 60), 1)"
      " ON CONFLICT (bucket, window_start)"
      " DO UPDATE SET count = rate_limits.count + 1"
      " RETURNING count,"
      "   GREATEST(1, CEIL(EXTRACT(EPOCH FROM"
      "     (window_start + interval '60 seconds') - now())))::int",
      {"quota:rpm:" + user_id});
  if (result.rows() == 0) return decision;

  const long long count = result.int64(0, 0);
  if (count > config_.rpm_limit) {
    decision.allowed = false;
    decision.type = "rate_limited";
    decision.message = "请求过于频繁，请稍后重试";
    decision.retry_after_s = std::max(1, static_cast<int>(result.int64(0, 1)));
  }
  return decision;
}

QuotaAcquired PgQuotaService::try_acquire(const auth::Principal& principal,
                                          int estimated_tokens,
                                          long long /*daily_used_tokens*/) {
  QuotaAcquired result;
  if (principal.is_admin || principal.user_id <= 0) return result;

  const std::string user_id = std::to_string(principal.user_id);
  result.decision = check_rpm(user_id);
  if (!result.decision.allowed) return result;

  const bool need_daily = config_.daily_token_limit > 0;
  const bool need_concurrent = config_.max_concurrent > 0;
  if (!need_daily && !need_concurrent) return result;

  const int estimated = std::max(0, estimated_tokens);
  const std::string reservation_id = need_daily ? util::random_id("q_") : std::string{};
  const std::string lease_id = need_concurrent ? util::random_id("l_") : std::string{};

  storage::Transaction tx(db_.acquire());
  // 同一个用户的配额检查串行化；hashtext 避免 user_id 超出 int32 溢出。
  tx.exec("SELECT pg_advisory_xact_lock(hashtext('quota:' || $1))", {user_id});
  tx.exec("DELETE FROM quota_stream_leases WHERE user_id = $1 AND expires_at <= now()",
          {user_id});
  tx.exec("DELETE FROM quota_reservations WHERE user_id = $1 AND expires_at <= now()",
          {user_id});

  if (need_concurrent) {
    const storage::Result active = tx.exec(
        "SELECT count(*) FROM quota_stream_leases"
        " WHERE user_id = $1 AND expires_at > now()",
        {user_id});
    const long long count = active.rows() > 0 ? active.int64(0, 0) : 0;
    if (count >= config_.max_concurrent) {
      result.decision.allowed = false;
      result.decision.type = "rate_limited";
      result.decision.message = "并发请求过多，请稍后重试";
      result.decision.retry_after_s = 1;
      return result;  // Transaction 析构自动 ROLLBACK
    }
  }

  if (need_daily) {
    const storage::Result reserved = tx.exec(
        "SELECT COALESCE(sum(tokens), 0) FROM quota_reservations"
        " WHERE user_id = $1 AND expires_at > now()",
        {user_id});
    const storage::Result used = tx.exec(
        "SELECT COALESCE(sum(prompt_tokens + completion_tokens), 0) FROM usage_records"
        " WHERE user_id = $1 AND created_at >= date_trunc('day', now())",
        {user_id});
    const long long reserved_tokens = reserved.rows() > 0 ? reserved.int64(0, 0) : 0;
    const long long used_tokens = used.rows() > 0 ? used.int64(0, 0) : 0;
    if (used_tokens + reserved_tokens + estimated > config_.daily_token_limit) {
      result.decision.allowed = false;
      result.decision.type = "quota_exceeded";
      result.decision.message = "今日 token 配额已用尽，请明天再试或联系管理员";
      result.decision.retry_after_s = 3600;
      return result;  // ROLLBACK
    }
    tx.exec(
        "INSERT INTO quota_reservations (id, user_id, tokens, expires_at)"
        " VALUES ($1, $2, $3, now() + make_interval(secs => $4::int))",
        {reservation_id, user_id, std::to_string(estimated), std::to_string(lease_ttl_s_)});
  }

  if (need_concurrent) {
    tx.exec(
        "INSERT INTO quota_stream_leases (id, user_id, expires_at)"
        " VALUES ($1, $2, now() + make_interval(secs => $3::int))",
        {lease_id, user_id, std::to_string(lease_ttl_s_)});
  }
  tx.commit();

  storage::DbPool* db = &db_;
  result.guard = QuotaGuard([db, reservation_id, lease_id] {
    try {
      if (!reservation_id.empty()) {
        db->exec("DELETE FROM quota_reservations WHERE id = $1", {reservation_id});
      }
      if (!lease_id.empty()) {
        db->exec("DELETE FROM quota_stream_leases WHERE id = $1", {lease_id});
      }
    } catch (const std::exception& e) {
      platform::log_warn(std::string("释放配额预留失败: ") + e.what());
    }
  });
  return result;
}

}  // namespace quota
