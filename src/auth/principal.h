// 一次请求的身份。M1 只有静态 Token（admin）；M2 起 JWT / API Key 会设置 user_id。
#pragma once

#include <cstdint>
#include <string>

namespace auth {

struct Principal {
  std::int64_t user_id = 0;   // 0 = 未绑定用户（静态 Token / 本地开发）
  bool is_admin = false;      // 只有静态管理员 Token 或 role=admin 的用户为 true
  std::string auth_type = "none";  // none | static | jwt | api_key

  /// 幂等 / 限流的作用域：未绑定用户时用 token 级作用域，避免和真实用户冲突。
  std::string scope() const {
    if (user_id != 0) return "u:" + std::to_string(user_id);
    return "t:" + auth_type;
  }
};

}  // namespace auth
