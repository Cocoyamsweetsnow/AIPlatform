// Argon2id 密码哈希。
//
// 参数按 OWASP 推荐的低内存档：t=2、m=19 MiB、p=1、hash=32B、salt=16B。
// 单次哈希约几十毫秒，登录/注册必须放在工作线程里，不能占 HTTP IO 线程。
#pragma once

#include <string>

namespace auth {

class PasswordHasher {
 public:
  std::string hash(const std::string& password) const;
  bool verify(const std::string& encoded, const std::string& password) const;
};

}  // namespace auth
