#include "auth/jwt.h"

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <ctime>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "common/log.h"

namespace auth {
namespace {

using json = nlohmann::json;

std::string base64url_encode(const std::string& input) {
  if (input.empty()) return {};
  std::vector<unsigned char> buffer(4 * ((input.size() + 2) / 3) + 1, 0);
  const int len = EVP_EncodeBlock(buffer.data(),
                                  reinterpret_cast<const unsigned char*>(input.data()),
                                  static_cast<int>(input.size()));
  if (len <= 0) throw std::runtime_error("base64 编码失败");
  std::string out(reinterpret_cast<char*>(buffer.data()), static_cast<std::size_t>(len));
  for (char& c : out) {
    if (c == '+') c = '-';
    else if (c == '/') c = '_';
  }
  while (!out.empty() && out.back() == '=') out.pop_back();
  return out;
}

std::optional<std::string> base64url_decode(std::string input) {
  for (char& c : input) {
    if (c == '-') c = '+';
    else if (c == '_') c = '/';
  }
  if (input.size() % 4 == 1) return std::nullopt;
  const std::size_t padding = (4 - (input.size() % 4)) % 4;
  input.append(padding, '=');

  std::vector<unsigned char> buffer((input.size() / 4) * 3);
  const int len = EVP_DecodeBlock(buffer.data(),
                                  reinterpret_cast<const unsigned char*>(input.data()),
                                  static_cast<int>(input.size()));
  if (len < 0) return std::nullopt;
  const int actual = len - static_cast<int>(padding);
  if (actual < 0) return std::nullopt;
  return std::string(reinterpret_cast<char*>(buffer.data()), static_cast<std::size_t>(actual));
}

std::vector<unsigned char> hmac_sha256(const std::string& secret, const std::string& data) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  if (HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
           reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest,
           &digest_len) == nullptr) {
    throw std::runtime_error("JWT HMAC 计算失败");
  }
  return std::vector<unsigned char>(digest, digest + digest_len);
}

std::string random_secret_hex() {
  unsigned char bytes[32];
  if (RAND_bytes(bytes, sizeof(bytes)) != 1) throw std::runtime_error("生成 JWT secret 失败");
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(sizeof(bytes) * 2);
  for (unsigned char byte : bytes) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0F]);
  }
  return out;
}

}  // namespace

JwtService::JwtService(JwtConfig config) : config_(std::move(config)) {
  if (config_.secret.empty()) {
    config_.secret = random_secret_hex();
    platform::log_warn(
        "AIP_JWT_SECRET 未设置：本次进程生成了临时 secret，重启后所有 JWT 失效；"
        "生产环境请显式配置并保证多副本一致");
  }
}

std::string JwtService::sign(const User& user, bool refresh,
                            const std::string& jti) const {
  const std::time_t now = std::time(nullptr);
  const int ttl = refresh ? config_.refresh_ttl_s : config_.access_ttl_s;

  const json header{{"alg", "HS256"}, {"typ", "JWT"}};
  json payload{{"iss", config_.issuer},
               {"sub", std::to_string(user.id)},
               {"email", user.email},
               {"role", user.role},
               {"type", refresh ? "refresh" : "access"},
               {"ver", user.token_version},
               {"iat", static_cast<long long>(now)},
               {"exp", static_cast<long long>(now) + ttl}};
  if (!jti.empty()) payload["jti"] = jti;

  const std::string signing_input =
      base64url_encode(header.dump()) + "." + base64url_encode(payload.dump());
  const std::vector<unsigned char> signature = hmac_sha256(config_.secret, signing_input);
  const std::string signature_part =
      base64url_encode(std::string(reinterpret_cast<const char*>(signature.data()),
                                   signature.size()));
  return signing_input + "." + signature_part;
}

std::optional<JwtClaims> JwtService::verify(const std::string& token,
                                            bool expect_refresh) const {
  const std::size_t first = token.find('.');
  if (first == std::string::npos) return std::nullopt;
  const std::size_t second = token.find('.', first + 1);
  if (second == std::string::npos || token.find('.', second + 1) != std::string::npos) {
    return std::nullopt;
  }

  const std::string header_part = token.substr(0, first);
  const std::string payload_part = token.substr(first + 1, second - first - 1);
  const std::string signature_part = token.substr(second + 1);

  const auto header_text = base64url_decode(header_part);
  const auto payload_text = base64url_decode(payload_part);
  const auto signature = base64url_decode(signature_part);
  if (!header_text || !payload_text || !signature) return std::nullopt;

  const json header = json::parse(*header_text, nullptr, false);
  if (header.is_discarded() || !header.is_object() || header.value("alg", "") != "HS256") {
    return std::nullopt;
  }

  const std::string signing_input = token.substr(0, second);
  const std::vector<unsigned char> expected = hmac_sha256(config_.secret, signing_input);
  if (signature->size() != expected.size() ||
      CRYPTO_memcmp(signature->data(), expected.data(), expected.size()) != 0) {
    return std::nullopt;
  }

  const json payload = json::parse(*payload_text, nullptr, false);
  if (payload.is_discarded() || !payload.is_object()) return std::nullopt;
  if (payload.value("iss", "") != config_.issuer) return std::nullopt;

  const std::string type = payload.value("type", "access");
  if (expect_refresh != (type == "refresh")) return std::nullopt;

  const long long exp = payload.value("exp", 0LL);
  if (exp <= static_cast<long long>(std::time(nullptr))) return std::nullopt;

  const std::string subject = payload.value("sub", "");
  if (subject.empty()) return std::nullopt;
  std::int64_t user_id = 0;
  try {
    std::size_t pos = 0;
    user_id = std::stoll(subject, &pos);
    if (pos != subject.size() || user_id <= 0) return std::nullopt;
  } catch (const std::exception&) {
    return std::nullopt;
  }

  JwtClaims claims;
  claims.user_id = user_id;
  claims.email = payload.value("email", "");
  claims.role = payload.value("role", "user");
  claims.refresh = type == "refresh";
  claims.jti = payload.value("jti", "");
  claims.token_version = payload.value("ver", 0);
  claims.iat = payload.value("iat", 0LL);
  claims.exp = exp;
  return claims;
}

}  // namespace auth
