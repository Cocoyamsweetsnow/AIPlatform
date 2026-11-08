#include "quota/quota.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace quota {

QuotaGuard::QuotaGuard(QuotaService* service, std::int64_t user_id, bool active)
    : service_(service), user_id_(user_id), active_(active) {}

QuotaGuard::~QuotaGuard() { release(); }

QuotaGuard::QuotaGuard(QuotaGuard&& other) noexcept
    : service_(other.service_), user_id_(other.user_id_), active_(other.active_) {
  other.service_ = nullptr;
  other.user_id_ = 0;
  other.active_ = false;
}

QuotaGuard& QuotaGuard::operator=(QuotaGuard&& other) noexcept {
  if (this != &other) {
    release();
    service_ = other.service_;
    user_id_ = other.user_id_;
    active_ = other.active_;
    other.service_ = nullptr;
    other.user_id_ = 0;
    other.active_ = false;
  }
  return *this;
}

void QuotaGuard::release() {
  if (active_ && service_ != nullptr) {
    service_->release(user_id_);
  }
  service_ = nullptr;
  user_id_ = 0;
  active_ = false;
}

QuotaService::QuotaService(QuotaConfig config) : config_(config) {}

QuotaService::Acquired QuotaService::try_acquire(const auth::Principal& principal,
                                                 int estimated_tokens,
                                                 long long daily_used_tokens) {
  Acquired result;
  if (principal.is_admin || principal.user_id <= 0) {
    return result;
  }

  const long long estimated = std::max(0, estimated_tokens);
  if (config_.daily_token_limit > 0 &&
      daily_used_tokens + estimated > config_.daily_token_limit) {
    result.decision.allowed = false;
    result.decision.type = "quota_exceeded";
    result.decision.message = "今日 token 配额已用尽，请明天再试或联系管理员";
    result.decision.retry_after_s = 3600;
    return result;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const auto now = std::chrono::steady_clock::now();

  const auto concurrent_it = concurrent_.find(principal.user_id);
  const int concurrent = concurrent_it == concurrent_.end() ? 0 : concurrent_it->second;
  if (config_.max_concurrent > 0 && concurrent >= config_.max_concurrent) {
    result.decision.allowed = false;
    result.decision.type = "rate_limited";
    result.decision.message = "并发请求过多，请稍后重试";
    result.decision.retry_after_s = 1;
    return result;
  }

  if (config_.rpm_limit > 0) {
    auto& bucket = buckets_[principal.user_id];
    if (bucket.last_refill.time_since_epoch().count() == 0) {
      bucket.tokens = static_cast<double>(config_.rpm_limit);
      bucket.last_refill = now;
    } else {
      const double elapsed =
          std::chrono::duration_cast<std::chrono::duration<double>>(now - bucket.last_refill)
              .count();
      const double rate = static_cast<double>(config_.rpm_limit) / 60.0;
      bucket.tokens = std::min(static_cast<double>(config_.rpm_limit),
                               bucket.tokens + elapsed * rate);
      bucket.last_refill = now;
    }

    if (bucket.tokens < 1.0) {
      const double rate = static_cast<double>(config_.rpm_limit) / 60.0;
      const double wait = rate > 0.0 ? (1.0 - bucket.tokens) / rate : 60.0;
      result.decision.allowed = false;
      result.decision.type = "rate_limited";
      result.decision.message = "请求过于频繁，请稍后重试";
      result.decision.retry_after_s = std::max(1, static_cast<int>(std::ceil(wait)));
      return result;
    }
    bucket.tokens -= 1.0;
  }

  ++concurrent_[principal.user_id];
  result.guard = QuotaGuard(this, principal.user_id, /*active=*/true);
  return result;
}

void QuotaService::release(std::int64_t user_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = concurrent_.find(user_id);
  if (it == concurrent_.end()) return;
  if (--it->second <= 0) concurrent_.erase(it);
}

}  // namespace quota
