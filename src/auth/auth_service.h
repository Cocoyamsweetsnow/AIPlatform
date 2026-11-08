// 鉴权编排：注册/登录/刷新、Bearer 解析、API Key 管理。
//
// 静态 AIP_TOKEN 仍然可用（视为管理员）；JWT/API Key 走 users/api_keys 表。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "auth/jwt.h"
#include "auth/password_hasher.h"
#include "auth/principal.h"
#include "auth/types.h"
#include "auth/user_store.h"

namespace auth {

struct AuthConfig {
  std::string static_token;  // AIP_TOKEN；为空表示不启用静态管理员 Token
  JwtConfig jwt;
};

class AuthService {
 public:
  AuthService(UserStore& store, AuthConfig config);

  /// 注册并直接签发 token；邮箱已存在抛 AuthError(EmailExists)。
  TokenPair register_user(const std::string& email, const std::string& password,
                          const std::string& display_name);

  /// 登录；邮箱/密码错误抛 AuthError(InvalidCredentials)。
  TokenPair login(const std::string& email, const std::string& password);

  /// 用 refresh token 换新 token 对。
  TokenPair refresh(const std::string& refresh_token);

  /// 解析 Bearer Token：静态 Token / API Key / JWT。失败返回 nullopt。
  std::optional<Principal> authenticate_bearer(const std::string& token);

  std::optional<User> get_user(std::int64_t user_id);

  /// 只能给已绑定用户签发 API Key（静态 admin user_id=0 不允许）。
  IssuedApiKey create_api_key(const Principal& principal, const std::string& name);

  std::vector<ApiKeyInfo> list_api_keys(const Principal& principal);
  bool revoke_api_key(const Principal& principal, std::int64_t key_id);

 private:
  TokenPair issue_tokens(const User& user);

  UserStore& store_;
  AuthConfig config_;
  PasswordHasher hasher_;
  JwtService jwt_;
};

}  // namespace auth
