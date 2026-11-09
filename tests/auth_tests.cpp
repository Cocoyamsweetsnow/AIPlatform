// 鉴权原语与 AuthService 的单元测试；不需要 PostgreSQL。
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "auth/api_key.h"
#include "auth/auth_service.h"
#include "auth/email_sender.h"
#include "auth/errors.h"
#include "auth/jwt.h"
#include "auth/password_hasher.h"
#include "auth/rate_limiter.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
      ++g_failures;                                                        \
    }                                                                      \
  } while (false)

std::string lower(std::string value) {
  for (char& c : value) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return value;
}

class FakeUserStore final : public auth::UserStore {
 public:
  std::optional<auth::UserRecord> find_by_email(const std::string& email) override {
    for (const auto& user : users_) {
      if (lower(user.email) == lower(email)) return user;
    }
    return std::nullopt;
  }

  std::optional<auth::UserRecord> find_by_id(std::int64_t user_id) override {
    for (const auto& user : users_) {
      if (user.id == user_id) return user;
    }
    return std::nullopt;
  }

  std::optional<auth::UserRecord> create(const std::string& email,
                                         const std::string& password_hash,
                                         const std::string& display_name) override {
    if (find_by_email(email).has_value()) return std::nullopt;
    auth::UserRecord user;
    user.id = next_user_++;
    user.email = email;
    user.password_hash = password_hash;
    user.display_name = display_name;
    user.role = "user";
    users_.push_back(user);
    return user;
  }

  std::optional<auth::UserRecord> find_by_api_key_hash(const std::string& key_hash) override {
    for (auto& key : keys_) {
      if (key.hash == key_hash && !key.revoked) {
        key.last_used_at = 1;
        return find_by_id(key.user_id);
      }
    }
    return std::nullopt;
  }

  std::int64_t create_api_key(std::int64_t user_id, const std::string& name,
                              const std::string& prefix,
                              const std::string& key_hash) override {
    Key key;
    key.id = next_key_++;
    key.user_id = user_id;
    key.name = name;
    key.prefix = prefix;
    key.hash = key_hash;
    key.created_at = 1;
    keys_.push_back(std::move(key));
    return keys_.back().id;
  }

  std::vector<auth::ApiKeyInfo> list_api_keys(std::int64_t user_id) override {
    std::vector<auth::ApiKeyInfo> out;
    for (const auto& key : keys_) {
      if (key.user_id != user_id || key.revoked) continue;
      auth::ApiKeyInfo info;
      info.id = key.id;
      info.name = key.name;
      info.prefix = key.prefix;
      info.created_at = key.created_at;
      info.last_used_at = key.last_used_at;
      out.push_back(std::move(info));
    }
    return out;
  }

  bool revoke_api_key(std::int64_t user_id, std::int64_t key_id) override {
    for (auto& key : keys_) {
      if (key.id == key_id && key.user_id == user_id && !key.revoked) {
        key.revoked = true;
        return true;
      }
    }
    return false;
  }

  bool mark_email_verified(std::int64_t user_id) override {
    for (auto& user : users_) {
      if (user.id == user_id) {
        user.email_verified = true;
        return true;
      }
    }
    return false;
  }

  std::optional<int> update_password(std::int64_t user_id,
                                     const std::string& password_hash) override {
    for (auto& user : users_) {
      if (user.id == user_id) {
        user.password_hash = password_hash;
        ++user.token_version;
        for (auto& token : refresh_tokens_) {
          if (token.user_id == user_id) token.revoked = true;
        }
        return user.token_version;
      }
    }
    return std::nullopt;
  }

  bool create_refresh_token(std::int64_t user_id, const std::string& jti,
                            long long expires_at) override {
    RefreshToken token;
    token.user_id = user_id;
    token.jti = jti;
    token.expires_at = expires_at;
    refresh_tokens_.push_back(std::move(token));
    return true;
  }

  bool rotate_refresh_token(std::int64_t user_id, const std::string& old_jti,
                            const std::string& new_jti, long long expires_at) override {
    for (auto& token : refresh_tokens_) {
      if (token.user_id == user_id && token.jti == old_jti && !token.revoked &&
          token.expires_at > 1) {
        token.revoked = true;
        RefreshToken next;
        next.user_id = user_id;
        next.jti = new_jti;
        next.expires_at = expires_at;
        refresh_tokens_.push_back(std::move(next));
        return true;
      }
    }
    return false;
  }

  bool revoke_refresh_token(std::int64_t user_id, const std::string& jti) override {
    for (auto& token : refresh_tokens_) {
      if (token.user_id == user_id && token.jti == jti && !token.revoked) {
        token.revoked = true;
        return true;
      }
    }
    return false;
  }

  void revoke_all_refresh_tokens(std::int64_t user_id) override {
    for (auto& token : refresh_tokens_) {
      if (token.user_id == user_id) token.revoked = true;
    }
  }

  bool create_one_time_token(std::int64_t user_id, const std::string& purpose,
                             const std::string& token_hash,
                             long long expires_at) override {
    OneTimeToken token;
    token.user_id = user_id;
    token.purpose = purpose;
    token.token_hash = token_hash;
    token.expires_at = expires_at;
    one_time_tokens_.push_back(std::move(token));
    return true;
  }

  std::optional<std::int64_t> consume_one_time_token(
      const std::string& purpose, const std::string& token_hash) override {
    for (auto& token : one_time_tokens_) {
      if (token.purpose == purpose && token.token_hash == token_hash && !token.used &&
          token.expires_at > 1) {
        token.used = true;
        return token.user_id;
      }
    }
    return std::nullopt;
  }

 private:
  struct RefreshToken {
    std::int64_t user_id = 0;
    std::string jti;
    long long expires_at = 0;
    bool revoked = false;
  };

  struct OneTimeToken {
    std::int64_t user_id = 0;
    std::string purpose;
    std::string token_hash;
    long long expires_at = 0;
    bool used = false;
  };

  struct Key {
    std::int64_t id = 0;
    std::int64_t user_id = 0;
    std::string name;
    std::string prefix;
    std::string hash;
    long long created_at = 0;
    long long last_used_at = 0;
    bool revoked = false;
  };

  std::vector<auth::UserRecord> users_;
  std::vector<Key> keys_;
  std::vector<RefreshToken> refresh_tokens_;
  std::vector<OneTimeToken> one_time_tokens_;
  std::int64_t next_user_ = 1;
  std::int64_t next_key_ = 1;
};

class FakeEmailSender final : public auth::EmailSender {
 public:
  bool send(const auth::EmailMessage& message, std::string& /*error*/) override {
    messages.push_back(message);
    return true;
  }
  std::vector<auth::EmailMessage> messages;
};

class FakeRateLimiter final : public auth::RateLimiter {
 public:
  auth::RateLimitDecision check(const std::string& /*bucket*/, int /*limit*/,
                                int /*window_s*/) override {
    return decision;
  }
  auth::RateLimitDecision decision;
};

void test_password_hasher() {
  auth::PasswordHasher hasher;
  const std::string encoded = hasher.hash("correct horse battery staple");
  CHECK(encoded.rfind("$argon2id$", 0) == 0);
  CHECK(hasher.verify(encoded, "correct horse battery staple"));
  CHECK(!hasher.verify(encoded, "wrong password"));
  CHECK(!hasher.verify("", "anything"));
}

void test_jwt() {
  auth::JwtConfig config;
  config.secret = "0123456789abcdef0123456789abcdef";
  config.issuer = "test-issuer";
  auth::JwtService jwt(config);

  auth::User user;
  user.id = 42;
  user.email = "alice@example.com";
  user.display_name = "Alice";
  user.role = "admin";

  const std::string access = jwt.sign(user, /*refresh=*/false);
  const std::string refresh = jwt.sign(user, /*refresh=*/true);
  CHECK(access.find('.') != std::string::npos);

  const auto access_claims = jwt.verify(access, /*expect_refresh=*/false);
  CHECK(access_claims.has_value());
  if (access_claims.has_value()) {
    CHECK(access_claims->user_id == 42);
    CHECK(access_claims->email == "alice@example.com");
    CHECK(access_claims->is_admin());
    CHECK(!access_claims->refresh);
  }

  CHECK(jwt.verify(refresh, /*expect_refresh=*/true).has_value());
  CHECK(!jwt.verify(refresh, /*expect_refresh=*/false).has_value());
  CHECK(!jwt.verify(access, /*expect_refresh=*/true).has_value());

  std::string tampered = access;
  const std::size_t dot = tampered.find('.');
  CHECK(dot != std::string::npos);
  if (dot != std::string::npos && dot + 1 < tampered.size()) {
    tampered[dot + 1] = tampered[dot + 1] == 'a' ? 'b' : 'a';
  }
  CHECK(!jwt.verify(tampered, false).has_value());
  CHECK(!jwt.verify("not-a-jwt", false).has_value());

  auth::JwtConfig expired = config;
  expired.access_ttl_s = -1;
  auth::JwtService expired_jwt(expired);
  CHECK(!expired_jwt.verify(expired_jwt.sign(user, false), false).has_value());
}

void test_api_key() {
  const auth::GeneratedApiKey key = auth::generate_api_key();
  CHECK(key.plaintext.rfind("aip_", 0) == 0);
  CHECK(key.plaintext.size() > 60);
  CHECK(key.prefix.rfind("aip_", 0) == 0);
  CHECK(key.hash.size() == 64);
  CHECK(auth::hash_api_key(key.plaintext) == key.hash);
  CHECK(auth::looks_like_api_key(key.plaintext));
  CHECK(!auth::looks_like_api_key("sk-xxx"));
  CHECK(auth::secure_equals("abc", "abc"));
  CHECK(!auth::secure_equals("abc", "abd"));
  CHECK(!auth::secure_equals("abc", "abcd"));
}

std::string extract_token(const std::string& body) {
  const std::string marker = "token=";
  const std::size_t pos = body.find(marker);
  if (pos == std::string::npos) return {};
  const std::size_t begin = pos + marker.size();
  const std::size_t end = body.find_first_of(" \r\n\t", begin);
  return body.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

void test_auth_service() {
  FakeUserStore store;
  FakeEmailSender emails;
  FakeRateLimiter limiter;
  auth::AuthConfig config;
  config.static_token = "admin-static-token";
  config.jwt.secret = "0123456789abcdef0123456789abcdef";
  config.jwt.access_ttl_s = 900;
  config.jwt.refresh_ttl_s = 3600;
  config.email_sender = &emails;
  config.rate_limiter = &limiter;
  config.public_base_url = "https://chat.example.com";
  auth::AuthService service(store, config);

  const auth::RegisterOutcome registered =
      service.register_user("Alice@Example.com", "password123", "Alice");
  const auth::TokenPair& alice = registered.tokens;
  CHECK(!registered.verification_required);
  CHECK(alice.user.id > 0);
  CHECK(alice.user.email == "alice@example.com");
  CHECK(alice.user.display_name == "Alice");
  CHECK(alice.user.email_verified);
  CHECK(!alice.access_token.empty());
  CHECK(!alice.refresh_token.empty());
  CHECK(alice.expires_in == 900);

  const auto principal = service.authenticate_bearer(alice.access_token);
  CHECK(principal.has_value());
  if (principal.has_value()) {
    CHECK(principal->user_id == alice.user.id);
    CHECK(!principal->is_admin);
    CHECK(principal->auth_type == "jwt");
  }

  const auto admin = service.authenticate_bearer("admin-static-token");
  CHECK(admin.has_value());
  CHECK(admin->is_admin);
  CHECK(admin->user_id == 0);

  bool duplicate = false;
  try {
    service.register_user("alice@example.com", "password123", "Alice 2");
  } catch (const auth::AuthError& e) {
    duplicate = e.code() == auth::AuthErrorCode::EmailExists;
  }
  CHECK(duplicate);

  const auth::TokenPair login = service.login("ALICE@example.com", "password123");
  CHECK(login.user.id == alice.user.id);

  bool bad_password = false;
  try {
    service.login("alice@example.com", "wrong-password");
  } catch (const auth::AuthError& e) {
    bad_password = e.code() == auth::AuthErrorCode::InvalidCredentials;
  }
  CHECK(bad_password);

  // 不存在的邮箱也走恒定时间假哈希，并返回同样的错误。
  bool unknown_email = false;
  try {
    service.login("nobody@example.com", "password123");
  } catch (const auth::AuthError& e) {
    unknown_email = e.code() == auth::AuthErrorCode::InvalidCredentials;
  }
  CHECK(unknown_email);

  const auth::TokenPair refreshed = service.refresh(login.refresh_token);
  CHECK(refreshed.user.id == alice.user.id);
  CHECK(service.authenticate_bearer(refreshed.access_token).has_value());

  bool old_refresh_reused = false;
  try {
    service.refresh(login.refresh_token);
  } catch (const auth::AuthError& e) {
    old_refresh_reused = e.code() == auth::AuthErrorCode::Unauthorized;
  }
  CHECK(old_refresh_reused);

  bool access_as_refresh = false;
  try {
    service.refresh(refreshed.access_token);
  } catch (const auth::AuthError& e) {
    access_as_refresh = e.code() == auth::AuthErrorCode::Unauthorized;
  }
  CHECK(access_as_refresh);

  service.logout(refreshed.refresh_token);
  bool logged_out = false;
  try {
    service.refresh(refreshed.refresh_token);
  } catch (const auth::AuthError& e) {
    logged_out = e.code() == auth::AuthErrorCode::Unauthorized;
  }
  CHECK(logged_out);

  // 改密后旧 access token 立即失效（token_version +1），新 token 可用。
  const auth::TokenPair changed =
      service.change_password(*principal, "password123", "newpassword123");
  CHECK(!service.authenticate_bearer(alice.access_token).has_value());
  CHECK(service.authenticate_bearer(changed.access_token).has_value());

  bool old_password_rejected = false;
  try {
    service.login("alice@example.com", "password123");
  } catch (const auth::AuthError& e) {
    old_password_rejected = e.code() == auth::AuthErrorCode::InvalidCredentials;
  }
  CHECK(old_password_rejected);
  CHECK(service.login("alice@example.com", "newpassword123").user.id == alice.user.id);

  // 找回密码：邮件里有一次性 token，重置后旧 token 失效。
  emails.messages.clear();
  service.request_password_reset("alice@example.com");
  CHECK(emails.messages.size() == 1);
  CHECK(emails.messages[0].body.find(
            "https://chat.example.com/account.html?mode=reset&token=") !=
        std::string::npos);
  const std::string reset_token = extract_token(emails.messages[0].body);
  CHECK(!reset_token.empty());
  service.reset_password(reset_token, "resetpassword123");
  CHECK(service.login("alice@example.com", "resetpassword123").user.id == alice.user.id);
  bool reset_reused = false;
  try {
    service.reset_password(reset_token, "anotherpassword");
  } catch (const auth::AuthError& e) {
    reset_reused = e.code() == auth::AuthErrorCode::InvalidToken;
  }
  CHECK(reset_reused);

  // 登录限流：AuthService 把 Retry-After 带出来，HTTP 层负责写 header。
  limiter.decision.allowed = false;
  limiter.decision.retry_after_s = 42;
  bool rate_limited = false;
  try {
    service.login("alice@example.com", "resetpassword123");
  } catch (const auth::AuthError& e) {
    rate_limited = e.code() == auth::AuthErrorCode::RateLimited && e.retry_after_s() == 42;
  }
  CHECK(rate_limited);
  limiter.decision = auth::RateLimitDecision{};

  const auth::IssuedApiKey key = service.create_api_key(*principal, "ci");
  CHECK(key.info.id > 0);
  CHECK(key.plaintext.rfind("aip_", 0) == 0);
  CHECK(key.info.prefix == key.plaintext.substr(0, 12));

  const auto via_key = service.authenticate_bearer(key.plaintext);
  CHECK(via_key.has_value());
  if (via_key.has_value()) {
    CHECK(via_key->user_id == alice.user.id);
    CHECK(via_key->auth_type == "api_key");
  }

  CHECK(service.list_api_keys(*principal).size() == 1);
  CHECK(service.revoke_api_key(*principal, key.info.id));
  CHECK(!service.authenticate_bearer(key.plaintext).has_value());

  bool static_admin_cannot_create_key = false;
  try {
    service.create_api_key(*admin, "nope");
  } catch (const auth::AuthError& e) {
    static_admin_cannot_create_key = e.code() == auth::AuthErrorCode::Unauthorized;
  }
  CHECK(static_admin_cannot_create_key);

  // 邮箱验证流程：未验证不能登录；验证后拿到 token。
  FakeUserStore verify_store;
  FakeEmailSender verify_emails;
  auth::AuthConfig verify_config = config;
  verify_config.email_verification_required = true;
  verify_config.rate_limiter = nullptr;
  verify_config.email_sender = &verify_emails;
  auth::AuthService verify_service(verify_store, verify_config);

  const auth::RegisterOutcome pending =
      verify_service.register_user("bob@example.com", "password123", "Bob");
  CHECK(pending.verification_required);
  CHECK(pending.tokens.access_token.empty());
  CHECK(verify_emails.messages.size() == 1);
  const std::string verify_token = extract_token(verify_emails.messages[0].body);
  CHECK(!verify_token.empty());

  bool unverified_login = false;
  try {
    verify_service.login("bob@example.com", "password123");
  } catch (const auth::AuthError& e) {
    unverified_login = e.code() == auth::AuthErrorCode::EmailNotVerified;
  }
  CHECK(unverified_login);

  const auth::TokenPair verified = verify_service.verify_email(verify_token);
  CHECK(!verified.access_token.empty());
  CHECK(verify_service.authenticate_bearer(verified.access_token).has_value());
  CHECK(verify_service.login("bob@example.com", "password123").user.id ==
        verified.user.id);

  bool verify_reused = false;
  try {
    verify_service.verify_email(verify_token);
  } catch (const auth::AuthError& e) {
    verify_reused = e.code() == auth::AuthErrorCode::InvalidToken;
  }
  CHECK(verify_reused);
}

}  // namespace

int main() {
  std::printf("running auth tests...\n");
  test_password_hasher();
  test_jwt();
  test_api_key();
  test_auth_service();

  if (g_failures == 0) {
    std::printf("auth tests passed\n");
    return 0;
  }
  std::printf("%d auth check(s) failed\n", g_failures);
  return 1;
}
