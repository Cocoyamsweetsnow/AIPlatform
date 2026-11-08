// 鉴权层可预期错误：HTTP 层据此映射 400/401/403/409。
#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace auth {

enum class AuthErrorCode {
  InvalidRequest,     // 400
  EmailExists,        // 409
  InvalidCredentials, // 401
  Unauthorized,       // 401
  Disabled,           // 403
};

class AuthError : public std::runtime_error {
 public:
  AuthError(AuthErrorCode code, std::string message)
      : std::runtime_error(std::move(message)), code_(code) {}

  AuthErrorCode code() const noexcept { return code_; }

 private:
  AuthErrorCode code_;
};

}  // namespace auth
