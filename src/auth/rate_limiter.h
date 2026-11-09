// 登录/注册/找回密码的固定窗口限流（PostgreSQL 实现，多副本共享计数）。
//
// 为什么用 DB 而不是进程内：登录爆破在多副本下会被放大 N 倍；PG 已在部署里，
// 不必为这个功能额外引入 Redis。频率不高，一次 UPSERT 足够。
#pragma once

#include <atomic>
#include <string>

#include "storage/db.h"

namespace auth {

struct RateLimitDecision {
  bool allowed = true;
  int retry_after_s = 1;
};

class RateLimiter {
 public:
  virtual ~RateLimiter() = default;
  /// limit <= 0 表示不限；window_s 建议 60。
  virtual RateLimitDecision check(const std::string& bucket, int limit, int window_s) = 0;
};

class PgRateLimiter final : public RateLimiter {
 public:
  explicit PgRateLimiter(storage::DbPool& db) : db_(db) {}

  RateLimitDecision check(const std::string& bucket, int limit, int window_s) override;

 private:
  storage::DbPool& db_;
  std::atomic<int> calls_{0};
};

}  // namespace auth
