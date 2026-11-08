// 最小 HS256 JWT 实现：够用、无额外依赖（OpenSSL HMAC + nlohmann JSON）。
//
// 只支持 HS256；不支持 JWK / RS256 / 密钥轮转。生产多副本必须共享同一个
// AIP_JWT_SECRET，否则各副本签发的 token 互不认可。
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "auth/types.h"

namespace auth {

struct JwtConfig {
  std::string secret;                       // AIP_JWT_SECRET；空则启动时随机生成（重启失效）
  std::string issuer{"aiplatform"};         // AIP_JWT_ISSUER
  int access_ttl_s = 900;                   // 15 分钟
  int refresh_ttl_s = 7 * 24 * 3600;        // 7 天
};

struct JwtClaims {
  std::int64_t user_id = 0;
  std::string email;
  std::string role;
  bool refresh = false;
  long long iat = 0;
  long long exp = 0;

  bool is_admin() const { return role == "admin"; }
};

class JwtService {
 public:
  explicit JwtService(JwtConfig config);

  std::string sign(const User& user, bool refresh) const;

  /// 校验签名、iss、exp、type；失败返回 nullopt。
  std::optional<JwtClaims> verify(const std::string& token, bool expect_refresh) const;

  const JwtConfig& config() const { return config_; }

 private:
  JwtConfig config_;
};

}  // namespace auth
