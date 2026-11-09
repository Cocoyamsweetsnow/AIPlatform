// 配额：每用户 RPM / 并发流 / 每日 token。
//
// 两种实现：
//   - QuotaService：进程内，memory 模式/单测；
//   - PgQuotaService：PostgreSQL，多副本一致（见 quota/pg_quota.h）。
//
// HTTP 层只依赖 QuotaController，不关心后端。
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "auth/principal.h"

namespace quota {

struct QuotaConfig {
  int rpm_limit = 60;              // 0 = 不限
  int daily_token_limit = 1000000; // 0 = 不限
  int max_concurrent = 4;          // 0 = 不限
};

struct QuotaDecision {
  bool allowed = true;
  std::string type = "rate_limited";
  std::string message;
  int retry_after_s = 1;
};

/// 并发/预留名额 RAII；析构时归还。可移动，不可拷贝。
class QuotaGuard {
 public:
  QuotaGuard() = default;
  explicit QuotaGuard(std::function<void()> release);
  ~QuotaGuard();

  QuotaGuard(const QuotaGuard&) = delete;
  QuotaGuard& operator=(const QuotaGuard&) = delete;
  QuotaGuard(QuotaGuard&& other) noexcept;
  QuotaGuard& operator=(QuotaGuard&& other) noexcept;

  void release();

 private:
  std::function<void()> release_;
};

struct QuotaAcquired {
  QuotaDecision decision;
  QuotaGuard guard;  // decision.allowed 为 false 时是 inactive guard
};

class QuotaController {
 public:
  virtual ~QuotaController() = default;

  /// daily_used_tokens 只在 requires_daily_usage()=true 的实现里使用。
  virtual QuotaAcquired try_acquire(const auth::Principal& principal,
                                    int estimated_tokens,
                                    long long daily_used_tokens) = 0;

  /// true：HTTP 层需要先查 usage_records 当天汇总；PG 实现自己原子查。
  virtual bool requires_daily_usage() const { return true; }
};

class QuotaService final : public QuotaController {
 public:
  explicit QuotaService(QuotaConfig config);

  QuotaAcquired try_acquire(const auth::Principal& principal, int estimated_tokens,
                            long long daily_used_tokens) override;

  QuotaConfig config() const { return config_; }

 private:
  friend class QuotaGuard;
  void release(std::int64_t user_id);

  struct Bucket {
    double tokens = 0.0;
    std::chrono::steady_clock::time_point last_refill;
  };

  QuotaConfig config_;
  std::mutex mutex_;
  std::unordered_map<std::int64_t, Bucket> buckets_;
  std::unordered_map<std::int64_t, int> concurrent_;
};

}  // namespace quota
