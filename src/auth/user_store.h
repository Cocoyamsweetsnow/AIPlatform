// 用户与 API Key 的存储接口。
//
// 生产实现是 PgUserStore；单测可以用内存 fake，不把 PostgreSQL 带进每个测试。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "auth/types.h"

namespace auth {

class UserStore {
 public:
  virtual ~UserStore() = default;

  virtual std::optional<UserRecord> find_by_email(const std::string& email) = 0;
  virtual std::optional<UserRecord> find_by_id(std::int64_t user_id) = 0;

  /// 创建用户；邮箱已存在时返回 nullopt。
  virtual std::optional<UserRecord> create(const std::string& email,
                                           const std::string& password_hash,
                                           const std::string& display_name) = 0;

  /// 按 API Key 哈希找用户，并顺带更新 last_used_at。
  virtual std::optional<UserRecord> find_by_api_key_hash(const std::string& key_hash) = 0;

  virtual std::int64_t create_api_key(std::int64_t user_id, const std::string& name,
                                      const std::string& prefix,
                                      const std::string& key_hash) = 0;

  virtual std::vector<ApiKeyInfo> list_api_keys(std::int64_t user_id) = 0;
  virtual bool revoke_api_key(std::int64_t user_id, std::int64_t key_id) = 0;
};

}  // namespace auth
