// 把「后台生产者线程 + 阻塞队列」这套模板抽出来，避免每个 Provider 重写一遍。
//
// 生命周期约定（很重要）：
//   派生类的析构函数必须调用 shutdown()，因为 C++ 的析构顺序是
//   「派生析构体 -> 成员 -> 基类」，若等到基类析构才 join，
//   生产线程可能还在调用已被销毁的派生类成员（表现为 "pure virtual method called"）。
//   shutdown() 可重复调用，基类析构时也会兜底再调一次。
#pragma once

#include <chrono>
#include <memory>
#include <thread>

#include "llm/event_queue.h"
#include "llm/types.h"

namespace llm {

class ThreadedStream : public Stream {
 public:
  explicit ThreadedStream(std::shared_ptr<EventQueue> queue) : queue_(std::move(queue)) {}

  ~ThreadedStream() override { shutdown(); }

  Pull next(Event& out, std::chrono::milliseconds timeout) override {
    return queue_->pull(out, timeout);
  }

  void cancel() override { queue_->cancel(); }

 protected:
  /// 启动生产线程（派生类构造完成后调用）。
  void start_worker() { worker_ = std::thread([this] { run(); }); }

  /// 取消并等待生产线程结束；派生类析构函数里调用。
  void shutdown() {
    queue_->cancel();
    if (worker_.joinable()) worker_.join();
  }

  virtual void run() = 0;

  std::shared_ptr<EventQueue> queue_;

 private:
  std::thread worker_;
};

}  // namespace llm
