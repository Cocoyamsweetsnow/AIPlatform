// 鉴权原语与 AuthService 的单元测试；不需要 PostgreSQL。
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "auth/api_key.h"
#include "auth/auth_service.h"
#include "auth/errors.h"
#include "auth/jwt.h"
#include "auth/password_hasher.h"

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

 private:
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
  std::int64_t next_user_ = 1;
  std::int64_t next_key_ = 1;
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

void test_auth_service() {
  FakeUserStore store;
  auth::AuthConfig config;
  config.static_token = "admin-static-token";
  config.jwt.secret = "0123456789abcdef0123456789abcdef";
  config.jwt.access_ttl_s = 900;
  config.jwt.refresh_ttl_s = 3600;
  auth::AuthService service(store, config);

  const auth::TokenPair alice =
      service.register_user("Alice@Example.com", "password123", "Alice");
  CHECK(alice.user.id > 0);
  CHECK(alice.user.email == "alice@example.com");
  CHECK(alice.user.display_name == "Alice");
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

  const auth::TokenPair refreshed = service.refresh(alice.refresh_token);
  CHECK(refreshed.user.id == alice.user.id);
  CHECK(service.authenticate_bearer(refreshed.access_token).has_value());

  bool access_as_refresh = false;
  try {
    service.refresh(alice.access_token);
  } catch (const auth::AuthError& e) {
    access_as_refresh = e.code() == auth::AuthErrorCode::Unauthorized;
  }
  CHECK(access_as_refresh);

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
