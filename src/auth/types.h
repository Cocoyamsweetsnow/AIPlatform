// 鉴权领域类型：用户、API Key、Token 对。
#pragma once

#include <cstdint>
#include <string>

namespace auth {

struct User {
  std::int64_t id = 0;
  std::string email;
  std::string display_name;
  std::string role = "user";
  bool email_verified = false;
  int token_version = 0;  // 改密/重置后 +1，旧 access token 立即失效

  bool is_admin() const { return role == "admin"; }
};

struct UserRecord : User {
  std::string password_hash;
  bool disabled = false;
};

struct ApiKeyInfo {
  std::int64_t id = 0;
  std::string name;
  std::string prefix;
  long long created_at = 0;    // Unix 秒
  long long last_used_at = 0;  // 0 = 从未使用
};

struct IssuedApiKey {
  ApiKeyInfo info;
  std::string plaintext;  // 只在创建时返回一次
};

struct TokenPair {
  std::string access_token;
  std::string refresh_token;
  long long expires_in = 0;  // access token 有效秒数
  User user;
};

}  // namespace auth
