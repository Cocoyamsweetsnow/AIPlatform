// 鉴权 HTTP 的共享 JSON / Header 小工具（Drogon 与 httplib 两个后端共用）。
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

#include "auth/errors.h"
#include "auth/types.h"

namespace http {

inline std::optional<std::string> bearer_token(const std::string& authorization) {
  static const std::string kPrefix = "Bearer ";
  if (authorization.rfind(kPrefix, 0) != 0) return std::nullopt;
  std::string token = authorization.substr(kPrefix.size());
  if (token.empty()) return std::nullopt;
  return token;
}

inline nlohmann::json user_json(const auth::User& user) {
  return nlohmann::json{{"id", user.id},
                        {"email", user.email},
                        {"display_name", user.display_name},
                        {"role", user.role},
                        {"email_verified", user.email_verified}};
}

inline nlohmann::json token_pair_json(const auth::TokenPair& pair) {
  return nlohmann::json{{"token_type", "Bearer"},
                        {"access_token", pair.access_token},
                        {"refresh_token", pair.refresh_token},
                        {"expires_in", pair.expires_in},
                        {"user", user_json(pair.user)}};
}

inline nlohmann::json register_outcome_json(const auth::RegisterOutcome& outcome) {
  if (outcome.verification_required) {
    return nlohmann::json{{"verification_required", true},
                          {"email", outcome.user.email},
                          {"message", "验证邮件已发送，请先验证邮箱后再登录"}};
  }
  return token_pair_json(outcome.tokens);
}

inline nlohmann::json api_key_json(const auth::ApiKeyInfo& info) {
  return nlohmann::json{{"id", info.id},
                        {"name", info.name},
                        {"prefix", info.prefix},
                        {"created_at", info.created_at},
                        {"last_used_at", info.last_used_at}};
}

struct AuthHttpError {
  int status = 400;
  const char* type = "invalid_request";
};

inline AuthHttpError auth_http_error(auth::AuthErrorCode code) {
  switch (code) {
    case auth::AuthErrorCode::EmailExists:
      return {409, "email_exists"};
    case auth::AuthErrorCode::InvalidToken:
      return {400, "invalid_token"};
    case auth::AuthErrorCode::EmailNotVerified:
      return {403, "email_not_verified"};
    case auth::AuthErrorCode::RateLimited:
      return {429, "rate_limited"};
    case auth::AuthErrorCode::EmailUnavailable:
      return {503, "email_unavailable"};
    case auth::AuthErrorCode::InvalidCredentials:
      return {401, "invalid_credentials"};
    case auth::AuthErrorCode::Unauthorized:
      return {401, "unauthorized"};
    case auth::AuthErrorCode::Disabled:
      return {403, "account_disabled"};
    case auth::AuthErrorCode::InvalidRequest:
    default:
      return {400, "invalid_request"};
  }
}

}  // namespace http
