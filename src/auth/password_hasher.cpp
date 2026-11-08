#include "auth/password_hasher.h"

#include <argon2.h>
#include <openssl/rand.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace auth {
namespace {

constexpr std::uint32_t kTimeCost = 2;
constexpr std::uint32_t kMemoryKib = 19 * 1024;  // 19 MiB
constexpr std::uint32_t kParallelism = 1;
constexpr std::size_t kSaltLen = 16;
constexpr std::size_t kHashLen = 32;

std::string argon_error(int code) {
  const char* message = argon2_error_message(code);
  return message == nullptr ? ("Argon2 错误 " + std::to_string(code)) : std::string(message);
}

}  // namespace

std::string PasswordHasher::hash(const std::string& password) const {
  unsigned char salt[kSaltLen];
  if (RAND_bytes(salt, static_cast<int>(sizeof(salt))) != 1) {
    throw std::runtime_error("生成 Argon2 salt 失败");
  }

  const std::size_t encoded_len =
      argon2_encodedlen(kTimeCost, kMemoryKib, kParallelism, kSaltLen, kHashLen, Argon2_id);
  std::vector<char> encoded(encoded_len + 1, '\0');

  const int rc = argon2id_hash_encoded(
      kTimeCost, kMemoryKib, kParallelism, password.data(), password.size(), salt, sizeof(salt),
      kHashLen, encoded.data(), encoded.size());
  if (rc != ARGON2_OK) throw std::runtime_error("Argon2id 哈希失败: " + argon_error(rc));
  return std::string(encoded.data());
}

bool PasswordHasher::verify(const std::string& encoded_hash, const std::string& password) const {
  if (encoded_hash.empty()) return false;
  const int rc = argon2id_verify(encoded_hash.c_str(), password.data(), password.size());
  return rc == ARGON2_OK;
}

}  // namespace auth
