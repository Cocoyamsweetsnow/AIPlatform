// 异步 usage 写入：SSE 收尾线程不直接等 PostgreSQL。
//
// 语义：
//   - record() 入队后立即返回；队列满时退化为同步写（宁可慢，不丢流水）；
//   - 后台线程失败重试 3 次，仍失败只记日志（不阻塞已发给客户端的流）；
//   - 析构前 flush，保证进程正常退出时队列里的流水都尝试写过。
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>

#include "usage/usage.h"

namespace usage {

class AsyncUsageSink final : public UsageSink {
 public:
  explicit AsyncUsageSink(UsageSink& inner, std::size_t max_queue = 4096);
  ~AsyncUsageSink() override;

  void record(const UsageEvent& event) override;
  UsageSummary summary_today(std::int64_t user_id) override;

  /// 等待队列清空（测试/优雅退出用）。
  void flush();

  std::size_t pending() const;

 private:
  void run();
  void write_with_retry(const UsageEvent& event);

  UsageSink& inner_;
  const std::size_t max_queue_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable idle_cv_;
  std::deque<UsageEvent> queue_;
  bool stop_ = false;
  bool writing_ = false;
  std::thread worker_;
};

}  // namespace usage
