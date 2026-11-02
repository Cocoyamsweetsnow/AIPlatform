// 生产者/消费者队列：上游 I/O 线程 push，HTTP 写线程 pull。
// 这是把「阻塞式 curl」和「httplib 的 chunked 回调」解耦的关键。
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

#include "llm/types.h"

namespace llm {

class EventQueue {
 public:
  // 放入一个事件；已关闭/已取消时丢弃。
  void push(Event ev) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (closed_) return;
      queue_.push_back(std::move(ev));
    }
    cv_.notify_one();
  }

  // 生产者结束：队列清空后 pull 返回 End。
  void close() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      closed_ = true;
    }
    cv_.notify_all();
  }

  // 消费者取消：立即丢弃后续事件。
  void cancel() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      cancelled_ = true;
      closed_ = true;
      queue_.clear();
    }
    cv_.notify_all();
  }

  Pull pull(Event& out, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mu_);
    const bool ready = cv_.wait_for(lock, timeout, [this] {
      return !queue_.empty() || closed_ || cancelled_;
    });
    if (cancelled_) return Pull::Cancelled;
    if (!queue_.empty()) {
      out = std::move(queue_.front());
      queue_.pop_front();
      return Pull::Event;
    }
    if (!ready) return Pull::Timeout;
    return Pull::End;
  }

  bool cancelled() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cancelled_;
  }

 private:
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Event> queue_;
  bool closed_ = false;
  bool cancelled_ = false;
};

}  // namespace llm
