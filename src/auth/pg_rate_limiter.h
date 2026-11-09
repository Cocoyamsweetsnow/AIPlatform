// RateLimiter 的 PostgreSQL 实现（固定窗口，多副本共享计数）。
#pragma once

#include <atomic>
#include <string>

#include "auth/rate_limiter.h"
#include "storage/db.h"

namespace auth {

class PgRateLimiter final : public RateLimiter {
 public:
  explicit PgRateLimiter(storage::DbPool& db) : db_(db) {}

  RateLimitDecision check(const std::string& bucket, int limit, int window_s) override;

 private:
  storage::DbPool& db_;
  std::atomic<int> calls_{0};
};

}  // namespace auth
