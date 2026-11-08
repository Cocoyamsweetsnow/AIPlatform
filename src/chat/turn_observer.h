// 一轮对话结束时的回调：用于把 request_id 的状态从 in_progress 改为 completed/failed。
#pragma once

#include <cstdint>
#include <string>

namespace chat {

class TurnObserver {
 public:
  virtual ~TurnObserver() = default;

  virtual void on_success(const std::string& session_id,
                          std::int64_t assistant_message_id) = 0;
  virtual void on_failure(const std::string& session_id, const std::string& error) = 0;
};

}  // namespace chat
