// 多 API Key 轮转 + 冷却/熔断 + 每 Key RPM/TPM。
//
// 设计目标：
//   - 一个 Key 被 429/5xx/网络错误打中后，先冷却一段时间，不要每次请求都撞它；
//   - 冷却结束后半开：下一次 acquire 会重新尝试，成功即清零失败计数；
//   - 所有 Key 都不可用时快速失败，而不是排队等上游；
//   - 单进程状态。多副本各自冷却不算精确，但比「全 Key 一起撞墙」好；
//     需要全局一致时把状态换成 Redis（接口不变）。
#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace llm {

struct KeyRingConfig {
  int rpm_limit = 0;           // 每个 Key 每分钟请求数；0 = 不限
  long long tpm_limit = 0;     // 每个 Key 每分钟 token 数；0 = 不限
  int failure_threshold = 3;   // 连续失败多少次进入冷却
  int cooldown_s = 15;         // 首次冷却秒数
  int cooldown_max_s = 300;    // 指数退避后的冷却上限
};

struct KeyLease {
  std::size_t index = 0;
  std::string key;
};

class KeyRing {
 public:
  explicit KeyRing(std::vector<std::string> keys, KeyRingConfig config = {});

  bool empty() const;
  std::size_t size() const;

  /// 选一个不在冷却、且未超过 RPM/TPM 的 Key。
  /// 全部不可用时返回 nullopt（调用方应快速失败，不要无限重试）。
  std::optional<KeyLease> acquire();

  /// 兼容旧调用：拿一个 Key 字符串；全部不可用时返回空串。
  std::string next();

  /// 成功：清零连续失败与冷却。
  void record_success(std::size_t index);

  /// 失败：对 429/5xx/401/403/网络类错误累计；达到阈值进入冷却。
  /// 返回 true 表示本次调用把 Key 置入/延长了冷却（调用方可打指标）。
  bool record_failure(std::size_t index, int http_status, bool retryable);

  /// 记录本次请求消耗的 token（用于每 Key TPM）。
  void record_usage(std::size_t index, long long prompt_tokens, long long completion_tokens);

  /// 当前可被 acquire 选中的 Key 数量（用于 gauge）。
  std::size_t available() const;

  /// 距离该 Key 冷却结束还有多少毫秒；0 表示不在冷却。
  long long cooldown_remaining_ms(std::size_t index) const;

 private:
  struct KeyState {
    std::string key;
    int consecutive_failures = 0;
    std::chrono::system_clock::time_point cooldown_until{};
    long long rpm_window_minute = -1;
    int rpm_count = 0;
    long long tpm_window_minute = -1;
    long long tpm_tokens = 0;
  };

  bool usable_locked(const KeyState& state, std::chrono::system_clock::time_point now) const;
  void refresh_window_locked(KeyState& state, long long minute) const;

  mutable std::mutex mutex_;
  std::vector<KeyState> keys_;
  KeyRingConfig config_;
  std::size_t cursor_ = 0;
};

}  // namespace llm
