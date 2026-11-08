// 多 API Key 轮转：同一个进程内所有请求共享一个游标，按请求/重试次数均匀分摊。
//
// 这里刻意不做过期、熔断和配额——那是「多 key 管理」的下一层（阶段 4）。
// 当前只解决一个最小问题：一个 key 被限流时，重试能换到下一个 key。
#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace llm {

class KeyRing {
 public:
  explicit KeyRing(std::vector<std::string> keys) : keys_(std::move(keys)) {}

  bool empty() const { return keys_.empty(); }
  std::size_t size() const { return keys_.size(); }

  /// 取下一个 key；没有配置时返回空串（调用方应报配置错误，而不是发空 Bearer）。
  std::string next() {
    if (keys_.empty()) return {};
    const std::size_t index =
        cursor_.fetch_add(1, std::memory_order_relaxed) % keys_.size();
    return keys_[index];
  }

 private:
  std::vector<std::string> keys_;
  std::atomic<std::size_t> cursor_{0};
};

}  // namespace llm
