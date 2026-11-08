// 进程内配额：每用户 RPM、并发流、每日 token 配额。
//
// 这是单实例实现；多副本部署需要 Redis/网关级限流（后续阶段）。
// 静态管理员 / 未绑定用户（user_id=0）直接放行。
#pragma once

#include <chrono>
#include <cstdint>
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

class QuotaService;

/// 并发名额 RAII；析构时归还。可移动，不可拷贝。
class QuotaGuard {
 public:
  QuotaGuard() = default;
  QuotaGuard(QuotaService* service, std::int64_t user_id, bool active);
  ~QuotaGuard();

  QuotaGuard(const QuotaGuard&) = delete;
  QuotaGuard& operator=(const QuotaGuard&) = delete;
  QuotaGuard(QuotaGuard&& other) noexcept;
  QuotaGuard& operator=(QuotaGuard&& other) noexcept;

  void release();

 private:
  QuotaService* service_ = nullptr;
  std::int64_t user_id_ = 0;
  bool active_ = false;
};

class QuotaService {
 public:
  explicit QuotaService(QuotaConfig config);

  struct Acquired {
    QuotaDecision decision;
    QuotaGuard guard;  // decision.allowed 为 false 时是 inactive guard
  };

  /// daily_used_tokens 由调用方查 usage_records 得到；estimated_tokens 是本次预留。
  Acquired try_acquire(const auth::Principal& principal, int estimated_tokens,
                       long long daily_used_tokens);

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
