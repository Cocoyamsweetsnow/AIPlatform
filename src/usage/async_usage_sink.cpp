#include "usage/async_usage_sink.h"

#include <chrono>
#include <exception>
#include <utility>

#include "common/log.h"

namespace usage {

AsyncUsageSink::AsyncUsageSink(UsageSink& inner, std::size_t max_queue)
    : inner_(inner), max_queue_(max_queue == 0 ? 1 : max_queue) {
  worker_ = std::thread([this] { run(); });
}

AsyncUsageSink::~AsyncUsageSink() {
  flush();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void AsyncUsageSink::record(const UsageEvent& event) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stop_ && queue_.size() < max_queue_) {
      queue_.push_back(event);
      cv_.notify_one();
      return;
    }
  }
  // 队列满/已停止：同步兜底，避免丢流水。
  try {
    inner_.record(event);
  } catch (const std::exception& e) {
    platform::log_error(std::string("usage 同步兜底写入失败: ") + e.what());
  }
}

UsageSummary AsyncUsageSink::summary_today(std::int64_t user_id) {
  return inner_.summary_today(user_id);
}

void AsyncUsageSink::flush() {
  std::unique_lock<std::mutex> lock(mutex_);
  idle_cv_.wait(lock, [this] { return queue_.empty() && !writing_; });
}

std::size_t AsyncUsageSink::pending() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

void AsyncUsageSink::run() {
  while (true) {
    UsageEvent event;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (queue_.empty() && stop_) return;
      event = std::move(queue_.front());
      queue_.pop_front();
      writing_ = true;
    }

    write_with_retry(event);

    {
      std::lock_guard<std::mutex> lock(mutex_);
      writing_ = false;
      if (queue_.empty()) idle_cv_.notify_all();
    }
  }
}

void AsyncUsageSink::write_with_retry(const UsageEvent& event) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    try {
      inner_.record(event);
      return;
    } catch (const std::exception& e) {
      if (attempt == 2) {
        platform::log_error(std::string("usage 异步写入失败（已重试 3 次）: ") + e.what());
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100 * (attempt + 1)));
    }
  }
}

}  // namespace usage
