#include "auth/auth_service.h"

#include <cctype>
#include <ctime>
#include <stdexcept>
#include <string>
#include <utility>

#include "auth/api_key.h"
#include "auth/errors.h"

namespace auth {
namespace {

std::string trim(const std::string& value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::string normalize_email(const std::string& raw) {
  std::string email = trim(raw);
  for (char& c : email) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return email;
}

bool valid_email(const std::string& email) {
  if (email.size() < 3 || email.size() > 254) return false;
  if (email.find_first_of(" \t\r\n") != std::string::npos) return false;
  const std::size_t at = email.find('@');
  if (at == std::string::npos || at == 0 || at + 1 >= email.size()) return false;
  return email.find('@', at + 1) == std::string::npos;
}

bool valid_password(const std::string& password) {
  return password.size() >= 8 && password.size() <= 128;
}

std::string default_display_name(const std::string& email) {
  const std::size_t at = email.find('@');
  return at == std::string::npos ? email : email.substr(0, at);
}

void require_user_id(const Principal& principal) {
  if (principal.user_id <= 0) {
    throw AuthError(AuthErrorCode::Unauthorized,
                    "该操作需要用户身份（JWT 或 API Key），静态管理员 Token 不支持");
  }
}

}  // namespace

AuthService::AuthService(UserStore& store, AuthConfig config)
    : store_(store), config_(std::move(config)), jwt_(config_.jwt) {}

TokenPair AuthService::register_user(const std::string& email, const std::string& password,
                                     const std::string& display_name) {
  const std::string normalized = normalize_email(email);
  if (!valid_email(normalized)) {
    throw AuthError(AuthErrorCode::InvalidRequest, "邮箱格式不合法");
  }
  if (!valid_password(password)) {
    throw AuthError(AuthErrorCode::InvalidRequest, "密码长度必须在 8~128 个字节之间");
  }

  std::string display = trim(display_name);
  if (display.empty()) display = default_display_name(normalized);
  if (display.size() > 80) {
    throw AuthError(AuthErrorCode::InvalidRequest, "display_name 不能超过 80 个字节");
  }

  auto created = store_.create(normalized, hasher_.hash(password), display);
  if (!created.has_value()) {
    throw AuthError(AuthErrorCode::EmailExists, "该邮箱已注册");
  }
  return issue_tokens(*created);
}

TokenPair AuthService::login(const std::string& email, const std::string& password) {
  const std::string normalized = normalize_email(email);
  if (!valid_email(normalized)) {
    throw AuthError(AuthErrorCode::InvalidCredentials, "邮箱或密码错误");
  }

  auto record = store_.find_by_email(normalized);
  if (!record.has_value()) {
    throw AuthError(AuthErrorCode::InvalidCredentials, "邮箱或密码错误");
  }
  if (record->disabled) {
    throw AuthError(AuthErrorCode::Disabled, "账号已被禁用");
  }
  if (!hasher_.verify(record->password_hash, password)) {
    throw AuthError(AuthErrorCode::InvalidCredentials, "邮箱或密码错误");
  }
  return issue_tokens(*record);
}

TokenPair AuthService::refresh(const std::string& refresh_token) {
  const auto claims = jwt_.verify(refresh_token, /*expect_refresh=*/true);
  if (!claims.has_value()) {
    throw AuthError(AuthErrorCode::Unauthorized, "refresh token 无效或已过期");
  }
  auto record = store_.find_by_id(claims->user_id);
  if (!record.has_value() || record->disabled) {
    throw AuthError(AuthErrorCode::Unauthorized, "用户不存在或已被禁用");
  }
  return issue_tokens(*record);
}

std::optional<Principal> AuthService::authenticate_bearer(const std::string& token) {
  if (token.empty()) return std::nullopt;

  if (!config_.static_token.empty() && secure_equals(token, config_.static_token)) {
    Principal principal;
    principal.is_admin = true;
    principal.auth_type = "static";
    return principal;
  }

  if (looks_like_api_key(token)) {
    auto record = store_.find_by_api_key_hash(hash_api_key(token));
    if (!record.has_value() || record->disabled) return std::nullopt;
    Principal principal;
    principal.user_id = record->id;
    principal.is_admin = record->is_admin();
    principal.auth_type = "api_key";
    return principal;
  }

  const auto claims = jwt_.verify(token, /*expect_refresh=*/false);
  if (!claims.has_value()) return std::nullopt;

  auto record = store_.find_by_id(claims->user_id);
  if (!record.has_value() || record->disabled) return std::nullopt;

  Principal principal;
  principal.user_id = record->id;
  principal.is_admin = record->is_admin();
  principal.auth_type = "jwt";
  return principal;
}

std::optional<User> AuthService::get_user(std::int64_t user_id) {
  auto record = store_.find_by_id(user_id);
  if (!record.has_value()) return std::nullopt;
  return static_cast<User>(*record);
}

IssuedApiKey AuthService::create_api_key(const Principal& principal, const std::string& name) {
  require_user_id(principal);

  std::string key_name = trim(name);
  if (key_name.empty()) key_name = "默认 Key";
  if (key_name.size() > 80) {
    throw AuthError(AuthErrorCode::InvalidRequest, "API Key 名称不能超过 80 个字节");
  }

  GeneratedApiKey generated = generate_api_key();
  const std::int64_t id =
      store_.create_api_key(principal.user_id, key_name, generated.prefix, generated.hash);

  IssuedApiKey issued;
  issued.info.id = id;
  issued.info.name = key_name;
  issued.info.prefix = generated.prefix;
  issued.info.created_at = static_cast<long long>(std::time(nullptr));
  issued.plaintext = generated.plaintext;
  return issued;
}

std::vector<ApiKeyInfo> AuthService::list_api_keys(const Principal& principal) {
  require_user_id(principal);
  return store_.list_api_keys(principal.user_id);
}

bool AuthService::revoke_api_key(const Principal& principal, std::int64_t key_id) {
  require_user_id(principal);
  return store_.revoke_api_key(principal.user_id, key_id);
}

TokenPair AuthService::issue_tokens(const User& user) {
  TokenPair pair;
  pair.access_token = jwt_.sign(user, /*refresh=*/false);
  pair.refresh_token = jwt_.sign(user, /*refresh=*/true);
  pair.expires_in = config_.jwt.access_ttl_s;
  pair.user = user;
  return pair;
}

}  // namespace auth
