#include "auth/api_key.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <stdexcept>
#include <string>

namespace auth {
namespace {

constexpr const char* kPrefix = "aip_";
constexpr std::size_t kRandomBytes = 32;

std::string hex_encode(const unsigned char* data, std::size_t len) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (std::size_t i = 0; i < len; ++i) {
    out.push_back(kHex[data[i] >> 4]);
    out.push_back(kHex[data[i] & 0x0F]);
  }
  return out;
}

}  // namespace

std::string sha256_hex(const std::string& data) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  if (EVP_Digest(data.data(), data.size(), digest, &digest_len, EVP_sha256(), nullptr) != 1) {
    throw std::runtime_error("SHA-256 计算失败");
  }
  return hex_encode(digest, digest_len);
}

GeneratedApiKey generate_api_key() {
  unsigned char bytes[kRandomBytes];
  if (RAND_bytes(bytes, sizeof(bytes)) != 1) throw std::runtime_error("生成 API Key 失败");

  GeneratedApiKey key;
  key.plaintext = std::string(kPrefix) + hex_encode(bytes, sizeof(bytes));
  key.prefix = key.plaintext.substr(0, std::string(kPrefix).size() + 8);
  key.hash = sha256_hex(key.plaintext);
  return key;
}

std::string hash_api_key(const std::string& plaintext) { return sha256_hex(plaintext); }

bool looks_like_api_key(const std::string& token) {
  return token.rfind(kPrefix, 0) == 0 && token.size() > std::string(kPrefix).size() + 16;
}

bool secure_equals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  if (a.empty()) return true;
  return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

}  // namespace auth
