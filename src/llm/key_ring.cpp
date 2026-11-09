#include "llm/key_ring.h"

#include <algorithm>
#include <utility>

namespace llm {
namespace {

long long minute_of(std::chrono::system_clock::time_point now) {
  return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count() / 60;
}

}  // namespace

KeyRing::KeyRing(std::vector<std::string> keys, KeyRingConfig config)
    : config_(config) {
  keys_.reserve(keys.size());
  for (auto& key : keys) {
    KeyState state;
    state.key = std::move(key);
    keys_.push_back(std::move(state));
  }
}

bool KeyRing::empty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return keys_.empty();
}

std::size_t KeyRing::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return keys_.size();
}

void KeyRing::refresh_window_locked(KeyState& state, long long minute) const {
  if (state.rpm_window_minute != minute) {
    state.rpm_window_minute = minute;
    state.rpm_count = 0;
  }
  if (state.tpm_window_minute != minute) {
    state.tpm_window_minute = minute;
    state.tpm_tokens = 0;
  }
}

bool KeyRing::usable_locked(const KeyState& state,
                            std::chrono::system_clock::time_point now) const {
  if (state.cooldown_until > now) return false;
  if (config_.rpm_limit > 0 && state.rpm_count >= config_.rpm_limit) return false;
  if (config_.tpm_limit > 0 && state.tpm_tokens >= config_.tpm_limit) return false;
  return true;
}

std::optional<KeyLease> KeyRing::acquire() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (keys_.empty()) return std::nullopt;

  const auto now = std::chrono::system_clock::now();
  const long long minute = minute_of(now);

  for (std::size_t offset = 0; offset < keys_.size(); ++offset) {
    const std::size_t index = (cursor_ + offset) % keys_.size();
    KeyState& state = keys_[index];
    refresh_window_locked(state, minute);
    if (!usable_locked(state, now)) continue;

    ++state.rpm_count;
    cursor_ = (index + 1) % keys_.size();
    KeyLease lease;
    lease.index = index;
    lease.key = state.key;
    return lease;
  }
  return std::nullopt;
}

std::string KeyRing::next() {
  auto lease = acquire();
  return lease.has_value() ? lease->key : std::string{};
}

void KeyRing::record_success(std::size_t index) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (index >= keys_.size()) return;
  keys_[index].consecutive_failures = 0;
  keys_[index].cooldown_until = {};
}

bool KeyRing::record_failure(std::size_t index, int http_status, bool retryable) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (index >= keys_.size()) return false;

  const bool key_level_auth = http_status == 401 || http_status == 403;
  const bool rate_limited = http_status == 429;
  if (!retryable && !key_level_auth && !rate_limited) {
    // 400/404/422 是请求本身的问题，不要把 Key 打进冷却。
    return false;
  }

  KeyState& state = keys_[index];
  ++state.consecutive_failures;
  const int threshold = std::max(1, config_.failure_threshold);

  // 429 立即冷却（不必等连续失败到阈值），避免重试风暴继续打同一个 Key。
  if (rate_limited) state.consecutive_failures = std::max(state.consecutive_failures, threshold);
  if (key_level_auth) state.consecutive_failures = std::max(state.consecutive_failures, threshold);

  if (state.consecutive_failures < threshold) return false;

  int exponent = state.consecutive_failures - threshold;
  exponent = std::min(exponent, 10);
  long long cooldown_s = std::max(1, config_.cooldown_s);
  cooldown_s *= (1LL << exponent);
  if (key_level_auth) cooldown_s = std::max<long long>(cooldown_s, 60);
  cooldown_s = std::min<long long>(cooldown_s, std::max(1, config_.cooldown_max_s));

  const auto until = std::chrono::system_clock::now() + std::chrono::seconds(cooldown_s);
  if (until <= state.cooldown_until) return false;
  state.cooldown_until = until;
  return true;
}

void KeyRing::record_usage(std::size_t index, long long prompt_tokens,
                           long long completion_tokens) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (index >= keys_.size()) return;
  const auto now = std::chrono::system_clock::now();
  KeyState& state = keys_[index];
  refresh_window_locked(state, minute_of(now));
  const long long tokens = std::max(0LL, prompt_tokens) + std::max(0LL, completion_tokens);
  state.tpm_tokens += tokens;
}

std::size_t KeyRing::available() const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto now = std::chrono::system_clock::now();
  const long long minute = minute_of(now);
  std::size_t count = 0;
  for (const auto& state : keys_) {
    if (usable_locked(state, now)) ++count;
  }
  return count;
}

long long KeyRing::cooldown_remaining_ms(std::size_t index) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (index >= keys_.size()) return 0;
  const auto now = std::chrono::system_clock::now();
  const auto& until = keys_[index].cooldown_until;
  if (until <= now) return 0;
  return std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count();
}

}  // namespace llm
