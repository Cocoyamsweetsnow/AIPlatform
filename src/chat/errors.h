// 业务层可预期的错误：HTTP 层据此映射 403/404，而不是一律 500/502。
#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace chat {

class SessionNotFound : public std::runtime_error {
 public:
  explicit SessionNotFound(std::string session_id)
      : std::runtime_error("会话不存在: " + session_id) {}
};

class Forbidden : public std::runtime_error {
 public:
  explicit Forbidden(std::string message = "无权访问该会话")
      : std::runtime_error(std::move(message)) {}
};

class ContextTooLong : public std::runtime_error {
 public:
  explicit ContextTooLong(std::string message = "上下文超过模型预算")
      : std::runtime_error(std::move(message)) {}
};

}  // namespace chat
