#include "auth/auth_service.h"

#include <openssl/rand.h>

#include <cctype>
#include <ctime>
#include <stdexcept>
#include <string>
#include <utility>

#include "auth/api_key.h"
#include "auth/errors.h"
#include "common/log.h"

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

std::string random_hex(std::size_t bytes) {
  if (bytes == 0 || bytes > 64) throw std::runtime_error("随机字节数超出范围");
  std::vector<unsigned char> buffer(bytes);
  if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1) {
    throw std::runtime_error("生成安全随机数失败");
  }
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes * 2);
  for (const unsigned char byte : buffer) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0F]);
  }
  return out;
}

long long now_s() {
  return static_cast<long long>(std::time(nullptr));
}

std::string one_time_hash(const std::string& token) { return sha256_hex(token); }

bool email_needs_verification(const AuthConfig& config, const User& user) {
  return config.email_verification_required && !user.email_verified;
}

}  // namespace

AuthService::AuthService(UserStore& store, AuthConfig config)
    : store_(store), config_(std::move(config)), jwt_(config_.jwt) {
  // 登录用户不存在时也跑一次 Argon2，避免通过响应时间枚举邮箱。
  dummy_hash_ = hasher_.hash("aiplatform-dummy-password-not-used");
}

RegisterOutcome AuthService::register_user(const std::string& email,
                                                       const std::string& password,
                                                       const std::string& display_name,
                                                       const std::string& client_ip) {
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

  if (!client_ip.empty()) {
    enforce_rate_limit("auth:register:ip:" + client_ip, config_.register_rpm);
  }
  enforce_rate_limit("auth:register:email:" + sha256_hex(normalized), config_.email_rpm);

  auto created = store_.create(normalized, hasher_.hash(password), display);
  if (!created.has_value()) {
    throw AuthError(AuthErrorCode::EmailExists, "该邮箱已注册");
  }

  RegisterOutcome outcome;
  outcome.user = static_cast<User>(*created);

  if (config_.email_verification_required) {
    const std::string token = random_hex(32);
    const long long expires = now_s() + config_.email_verify_ttl_s;
    if (!store_.create_one_time_token(created->id, "email_verify", one_time_hash(token),
                                      expires)) {
      throw std::runtime_error("写入邮箱验证 token 失败");
    }
    EmailMessage message;
    message.to = created->email;
    message.subject = "验证你的 AIPlatform 邮箱";
    message.body = "请在 " + std::to_string(config_.email_verify_ttl_s / 3600) +
                   " 小时内打开以下链接完成验证：\n" +
                   make_link("/account.html?mode=verify", token) + "\n";
    send_email(message);
    outcome.verification_required = true;
    return outcome;
  }

  if (!store_.mark_email_verified(created->id)) {
    // 配置未强制验证时，即使标记失败也不阻断登录；记录后继续。
    platform::log_error("注册后标记邮箱已验证失败（user_id=" +
                        std::to_string(created->id) + "）");
  }
  created->email_verified = true;
  outcome.user = static_cast<User>(*created);
  outcome.tokens = issue_tokens(*created);
  return outcome;
}

TokenPair AuthService::login(const std::string& email, const std::string& password,
                             const std::string& client_ip) {
  const std::string normalized = normalize_email(email);
  if (!valid_email(normalized)) {
    throw AuthError(AuthErrorCode::InvalidCredentials, "邮箱或密码错误");
  }

  if (!client_ip.empty()) {
    enforce_rate_limit("auth:login:ip:" + client_ip, config_.login_rpm);
  }
  enforce_rate_limit("auth:login:email:" + sha256_hex(normalized), config_.email_rpm);

  auto record = store_.find_by_email(normalized);
  if (!record.has_value()) {
    // 恒定时间：用户不存在也做一次 Argon2 校验。
    (void)hasher_.verify(dummy_hash_, password);
    throw AuthError(AuthErrorCode::InvalidCredentials, "邮箱或密码错误");
  }
  if (record->disabled) {
    throw AuthError(AuthErrorCode::Disabled, "账号已被禁用");
  }
  if (email_needs_verification(config_, *record)) {
    throw AuthError(AuthErrorCode::EmailNotVerified, "请先验证邮箱后再登录");
  }
  if (!hasher_.verify(record->password_hash, password)) {
    throw AuthError(AuthErrorCode::InvalidCredentials, "邮箱或密码错误");
  }
  return issue_tokens(*record);
}

TokenPair AuthService::refresh(const std::string& refresh_token,
                               const std::string& client_ip) {
  if (!client_ip.empty()) {
    enforce_rate_limit("auth:refresh:ip:" + client_ip, config_.login_rpm);
  }

  const auto claims = jwt_.verify(refresh_token, /*expect_refresh=*/true);
  if (!claims.has_value() || claims->jti.empty()) {
    throw AuthError(AuthErrorCode::Unauthorized, "refresh token 无效或已过期");
  }
  auto record = store_.find_by_id(claims->user_id);
  if (!record.has_value() || record->disabled) {
    throw AuthError(AuthErrorCode::Unauthorized, "用户不存在或已被禁用");
  }
  if (email_needs_verification(config_, *record)) {
    throw AuthError(AuthErrorCode::EmailNotVerified, "请先验证邮箱");
  }
  if (!token_version_matches(*claims, *record)) {
    throw AuthError(AuthErrorCode::Unauthorized, "refresh token 已失效，请重新登录");
  }

  const std::string new_jti = random_hex(16);
  const long long expires = now_s() + config_.jwt.refresh_ttl_s;
  if (!store_.rotate_refresh_token(record->id, claims->jti, new_jti, expires)) {
    throw AuthError(AuthErrorCode::Unauthorized, "refresh token 已被使用或已吊销");
  }
  return issue_tokens(*record, new_jti);
}

void AuthService::logout(const std::string& refresh_token) {
  const auto claims = jwt_.verify(refresh_token, /*expect_refresh=*/true);
  if (!claims.has_value() || claims->jti.empty()) return;
  try {
    store_.revoke_refresh_token(claims->user_id, claims->jti);
  } catch (const std::exception& e) {
    platform::log_warn(std::string("吊销 refresh token 失败: ") + e.what());
  }
}

TokenPair AuthService::verify_email(const std::string& token,
                                    const std::string& client_ip) {
  if (!client_ip.empty()) {
    enforce_rate_limit("auth:verify:ip:" + client_ip, config_.register_rpm);
  }
  if (token.empty()) {
    throw AuthError(AuthErrorCode::InvalidToken, "验证 token 不能为空");
  }
  const auto user_id = store_.consume_one_time_token("email_verify", one_time_hash(token));
  if (!user_id.has_value()) {
    throw AuthError(AuthErrorCode::InvalidToken, "验证 token 无效或已过期");
  }
  if (!store_.mark_email_verified(*user_id)) {
    throw AuthError(AuthErrorCode::InvalidToken, "用户不存在");
  }
  auto record = store_.find_by_id(*user_id);
  if (!record.has_value()) {
    throw AuthError(AuthErrorCode::InvalidToken, "用户不存在");
  }
  return issue_tokens(*record);
}

void AuthService::request_password_reset(const std::string& email,
                                         const std::string& client_ip) {
  if (config_.email_sender == nullptr) {
    throw AuthError(AuthErrorCode::EmailUnavailable, "邮件服务未配置，无法找回密码");
  }

  const std::string normalized = normalize_email(email);
  if (!valid_email(normalized)) return;  // 不泄露邮箱是否存在

  if (!client_ip.empty()) {
    enforce_rate_limit("auth:forgot:ip:" + client_ip, config_.login_rpm);
  }
  enforce_rate_limit("auth:forgot:email:" + sha256_hex(normalized), config_.email_rpm);

  auto record = store_.find_by_email(normalized);
  if (!record.has_value() || record->disabled) return;  // 静默成功，避免枚举

  const std::string token = random_hex(32);
  const long long expires = now_s() + config_.password_reset_ttl_s;
  if (!store_.create_one_time_token(record->id, "password_reset", one_time_hash(token),
                                    expires)) {
    throw std::runtime_error("写入重置密码 token 失败");
  }

  EmailMessage message;
  message.to = record->email;
  message.subject = "重置你的 AIPlatform 密码";
  message.body = "请在 " + std::to_string(config_.password_reset_ttl_s / 60) +
                 " 分钟内打开以下链接重置密码：\n" +
                 make_link("/account.html?mode=reset", token) +
                 "\n\n如果不是你本人操作，请忽略这封邮件。\n";
  send_email(message);
}

void AuthService::reset_password(const std::string& token, const std::string& new_password,
                                 const std::string& client_ip) {
  if (!valid_password(new_password)) {
    throw AuthError(AuthErrorCode::InvalidRequest, "密码长度必须在 8~128 个字节之间");
  }
  if (!client_ip.empty()) {
    enforce_rate_limit("auth:reset:ip:" + client_ip, config_.login_rpm);
  }
  if (token.empty()) {
    throw AuthError(AuthErrorCode::InvalidToken, "重置 token 不能为空");
  }
  const auto user_id =
      store_.consume_one_time_token("password_reset", one_time_hash(token));
  if (!user_id.has_value()) {
    throw AuthError(AuthErrorCode::InvalidToken, "重置 token 无效或已过期");
  }
  if (!store_.update_password(*user_id, hasher_.hash(new_password)).has_value()) {
    throw AuthError(AuthErrorCode::InvalidToken, "用户不存在");
  }
}

TokenPair AuthService::change_password(const Principal& principal,
                                       const std::string& old_password,
                                       const std::string& new_password) {
  require_user_id(principal);
  if (!valid_password(new_password)) {
    throw AuthError(AuthErrorCode::InvalidRequest, "密码长度必须在 8~128 个字节之间");
  }

  auto record = store_.find_by_id(principal.user_id);
  if (!record.has_value()) {
    throw AuthError(AuthErrorCode::Unauthorized, "用户不存在");
  }
  if (!hasher_.verify(record->password_hash, old_password)) {
    throw AuthError(AuthErrorCode::InvalidCredentials, "原密码错误");
  }
  if (!store_.update_password(record->id, hasher_.hash(new_password)).has_value()) {
    throw AuthError(AuthErrorCode::Unauthorized, "更新密码失败");
  }

  auto updated = store_.find_by_id(record->id);
  if (!updated.has_value()) {
    throw AuthError(AuthErrorCode::Unauthorized, "用户不存在");
  }
  return issue_tokens(*updated);
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
    if (email_needs_verification(config_, *record)) return std::nullopt;
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
  if (email_needs_verification(config_, *record)) return std::nullopt;
  if (!token_version_matches(*claims, *record)) return std::nullopt;

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
  issued.info.created_at = now_s();
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

TokenPair AuthService::issue_tokens(const User& user, const std::string& refresh_jti) {
  const std::string jti = refresh_jti.empty() ? random_hex(16) : refresh_jti;
  if (refresh_jti.empty()) {
    const long long expires = now_s() + config_.jwt.refresh_ttl_s;
    if (!store_.create_refresh_token(user.id, jti, expires)) {
      throw AuthError(AuthErrorCode::Unauthorized, "无法签发登录凭证");
    }
  }

  TokenPair pair;
  pair.access_token = jwt_.sign(user, /*refresh=*/false, random_hex(16));
  pair.refresh_token = jwt_.sign(user, /*refresh=*/true, jti);
  pair.expires_in = config_.jwt.access_ttl_s;
  pair.user = user;
  return pair;
}

void AuthService::enforce_rate_limit(const std::string& bucket, int limit) {
  if (config_.rate_limiter == nullptr || limit <= 0) return;
  const RateLimitDecision decision = config_.rate_limiter->check(bucket, limit, 60);
  if (!decision.allowed) {
    throw AuthError(AuthErrorCode::RateLimited, "请求过于频繁，请稍后重试",
                    decision.retry_after_s);
  }
}

void AuthService::send_email(const EmailMessage& message) {
  if (config_.email_sender == nullptr) return;
  std::string error;
  if (!config_.email_sender->send(message, error)) {
    platform::log_error("发送邮件失败（to=" + message.to + "）: " + error);
  }
}

std::string AuthService::make_link(const std::string& path, const std::string& token) const {
  std::string base = config_.public_base_url.empty() ? "http://127.0.0.1:8080"
                                                     : config_.public_base_url;
  while (!base.empty() && base.back() == '/') base.pop_back();
  const char separator = path.find('?') == std::string::npos ? '?' : '&';
  return base + path + separator + "token=" + token;
}

bool AuthService::token_version_matches(const JwtClaims& claims,
                                        const UserRecord& user) const {
  return claims.token_version == user.token_version;
}

}  // namespace auth
