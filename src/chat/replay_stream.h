// 幂等重放用的最小 Stream：把已保存的完整回复重新编码成 SSE。
#pragma once

#include <chrono>
#include <string>
#include <utility>

#include "llm/types.h"

namespace chat {

class ReplayStream final : public llm::Stream {
 public:
  explicit ReplayStream(std::string text) : text_(std::move(text)) {}

  llm::Pull next(llm::Event& out, std::chrono::milliseconds /*timeout*/) override {
    if (cancelled_) return llm::Pull::Cancelled;
    if (!delta_sent_ && !text_.empty()) {
      delta_sent_ = true;
      out.kind = llm::EventKind::Delta;
      out.text = text_;
      return llm::Pull::Event;
    }
    if (!done_sent_) {
      done_sent_ = true;
      out.kind = llm::EventKind::Done;
      out.finish_reason = "stop";
      return llm::Pull::Event;
    }
    return llm::Pull::End;
  }

  void cancel() override { cancelled_ = true; }

  std::string describe() const override { return "idempotent-replay"; }

 private:
  std::string text_;
  bool delta_sent_ = false;
  bool done_sent_ = false;
  bool cancelled_ = false;
};

}  // namespace chat
