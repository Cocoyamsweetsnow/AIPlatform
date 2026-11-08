// API Key：生成、哈希、常量时间比较。
//
// 明文格式：aip_ + 64 位十六进制（32 字节随机）。
// 数据库只存 SHA-256 哈希与前缀，明文只在创建时返回一次。
#pragma once

#include <string>

namespace auth {

struct GeneratedApiKey {
  std::string plaintext;
  std::string prefix;  // 便于 UI 展示，例如 aip_1a2b3c4d
  std::string hash;    // SHA-256 hex
};

GeneratedApiKey generate_api_key();

std::string hash_api_key(const std::string& plaintext);

bool looks_like_api_key(const std::string& token);

/// 常量时间比较（长度不同直接 false；长度本身不算秘密）。
bool secure_equals(const std::string& a, const std::string& b);

std::string sha256_hex(const std::string& data);

}  // namespace auth
