// 鉴权层可预期错误：HTTP 层据此映射 400/401/403/409。
#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace auth {

enum class AuthErrorCode {
  InvalidRequest,     // 400
  InvalidToken,       // 400（验证/重置 token 无效或过期）
  EmailExists,        // 409
  InvalidCredentials, // 401
  Unauthorized,       // 401
  Disabled,           // 403
  EmailNotVerified,   // 403
  RateLimited,        // 429
  EmailUnavailable,   // 503（未配置邮件发送）
};

class AuthError : public std::runtime_error {
 public:
  AuthError(AuthErrorCode code, std::string message, int retry_after_s = 0)
      : std::runtime_error(std::move(message)),
        code_(code),
        retry_after_s_(retry_after_s) {}

  AuthErrorCode code() const noexcept { return code_; }
  int retry_after_s() const noexcept { return retry_after_s_; }

 private:
  AuthErrorCode code_;
  int retry_after_s_ = 0;
};

}  // namespace auth
