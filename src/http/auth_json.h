// 鉴权 HTTP 的共享 JSON / Header 小工具（Drogon 与 httplib 两个后端共用）。
#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

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

/// 从 Cookie 头里取一个值；找不到返回 nullopt。
inline std::optional<std::string> cookie_value(std::string_view header,
                                               std::string_view name) {
  std::size_t start = 0;
  while (start < header.size()) {
    const std::size_t end = header.find(';', start);
    std::string_view part =
        header.substr(start, end == std::string_view::npos ? header.size() - start
                                                           : end - start);
    while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front()))) {
      part.remove_prefix(1);
    }
    while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back()))) {
      part.remove_suffix(1);
    }
    if (part.size() > name.size() && part.compare(0, name.size(), name) == 0 &&
        part[name.size()] == '=') {
      return std::string(part.substr(name.size() + 1));
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return std::nullopt;
}

inline std::string make_cookie(const std::string& name, const std::string& value,
                               long long max_age_s, const std::string& path,
                               bool secure) {
  std::string cookie = name + "=" + value + "; Path=" + path + "; Max-Age=" +
                       std::to_string(max_age_s) + "; HttpOnly; SameSite=Strict";
  if (secure) cookie += "; Secure";
  return cookie;
}

/// 比较 Origin 与 public_base_url 的 scheme://host[:port]；用于 Cookie 模式的 CSRF 防护。
inline bool same_origin(std::string_view origin, std::string_view base_url) {
  const auto normalize = [](std::string_view value) {
    const std::size_t scheme = value.find("://");
    if (scheme == std::string_view::npos) return std::string{};
    const std::size_t start = scheme + 3;
    const std::size_t slash = value.find('/', start);
    std::string out(value.substr(0, slash == std::string_view::npos ? value.size() : slash));
    for (char& c : out) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
  };
  const std::string a = normalize(origin);
  const std::string b = normalize(base_url);
  return !a.empty() && a == b;
}

/// Cookie 模式下不把 token 放进响应体，只返回用户信息。
inline nlohmann::json auth_response_json(const auth::TokenPair& pair, bool cookie_mode) {
  if (!cookie_mode) return token_pair_json(pair);
  return nlohmann::json{{"token_type", "Bearer"},
                        {"cookie_mode", true},
                        {"expires_in", pair.expires_in},
                        {"user", user_json(pair.user)}};
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
