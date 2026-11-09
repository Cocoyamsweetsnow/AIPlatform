// 鉴权编排：注册/登录/刷新、Bearer 解析、API Key、账号安全。
//
// 安全闭环（M6）：
//   - 登录/注册/找回密码按 IP + 邮箱限流；
//   - 邮箱验证（可选）、找回密码（一次性 token，只存哈希）；
//   - refresh token 轮换 + 服务端吊销；
//   - 改密/重置后 token_version + 1，旧 access token 立即失效；
//   - 登录失败走恒定时间假哈希，避免用户枚举。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "auth/email_sender.h"
#include "auth/jwt.h"
#include "auth/password_hasher.h"
#include "auth/principal.h"
#include "auth/rate_limiter.h"
#include "auth/types.h"
#include "auth/user_store.h"

namespace auth {

struct AuthConfig {
  std::string static_token;  // AIP_TOKEN；为空表示不启用静态管理员 Token
  JwtConfig jwt;

  bool email_verification_required = false;
  std::string public_base_url{"http://127.0.0.1:8080"};
  int login_rpm = 20;
  int register_rpm = 10;
  int email_rpm = 5;
  int email_verify_ttl_s = 86400;
  int password_reset_ttl_s = 1800;

  RateLimiter* rate_limiter = nullptr;
  EmailSender* email_sender = nullptr;
};

struct RegisterOutcome {
  TokenPair tokens;       // verification_required=true 时为空
  User user;
  bool verification_required = false;
};

class AuthService {
 public:
  AuthService(UserStore& store, AuthConfig config);

  /// 注册。verification_required=true 时不返回 token，需要先验证邮箱。
  RegisterOutcome register_user(const std::string& email, const std::string& password,
                                const std::string& display_name,
                                const std::string& client_ip = "");

  /// 登录；邮箱/密码错误抛 AuthError(InvalidCredentials)。
  TokenPair login(const std::string& email, const std::string& password,
                  const std::string& client_ip = "");

  /// 用 refresh token 换新 token 对；旧 refresh token 立即失效（轮换）。
  TokenPair refresh(const std::string& refresh_token, const std::string& client_ip = "");

  /// 吊销 refresh token；无效 token 也返回成功（幂等，避免枚举）。
  void logout(const std::string& refresh_token);

  /// 消费邮箱验证 token，标记验证并签发 token 对。
  TokenPair verify_email(const std::string& token, const std::string& client_ip = "");

  /// 发送重置密码邮件；无论邮箱是否存在都返回成功（避免枚举）。
  void request_password_reset(const std::string& email, const std::string& client_ip = "");

  /// 消费重置 token 并更新密码；成功后旧 access/refresh 全部失效。
  void reset_password(const std::string& token, const std::string& new_password,
                      const std::string& client_ip = "");

  /// 已登录用户改密；返回新的 token 对（旧 token 因 token_version 变化失效）。
  TokenPair change_password(const Principal& principal, const std::string& old_password,
                            const std::string& new_password);

  /// 解析 Bearer Token：静态 Token / API Key / JWT。失败返回 nullopt。
  std::optional<Principal> authenticate_bearer(const std::string& token);

  std::optional<User> get_user(std::int64_t user_id);

  /// 只能给已绑定用户签发 API Key（静态 admin user_id=0 不允许）。
  IssuedApiKey create_api_key(const Principal& principal, const std::string& name);

  std::vector<ApiKeyInfo> list_api_keys(const Principal& principal);
  bool revoke_api_key(const Principal& principal, std::int64_t key_id);

 private:
  TokenPair issue_tokens(const User& user, const std::string& refresh_jti = {});
  void enforce_rate_limit(const std::string& bucket, int limit);
  void send_email(const EmailMessage& message);
  std::string make_link(const std::string& path, const std::string& token) const;
  bool token_version_matches(const JwtClaims& claims, const UserRecord& user) const;

  UserStore& store_;
  AuthConfig config_;
  PasswordHasher hasher_;
  JwtService jwt_;
  std::string dummy_hash_;
};

}  // namespace auth
