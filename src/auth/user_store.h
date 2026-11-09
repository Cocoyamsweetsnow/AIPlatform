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

  // ---- 账号安全（M6）----
  virtual bool mark_email_verified(std::int64_t user_id) = 0;

  /// 更新密码并把 token_version + 1、吊销全部 refresh token。
  /// 返回新的 token_version；用户不存在返回 nullopt。
  virtual std::optional<int> update_password(std::int64_t user_id,
                                             const std::string& password_hash) = 0;

  virtual bool create_refresh_token(std::int64_t user_id, const std::string& jti,
                                    long long expires_at) = 0;

  /// 原子轮换：旧 jti 必须未吊销且未过期；成功后写入新 jti。
  virtual bool rotate_refresh_token(std::int64_t user_id, const std::string& old_jti,
                                    const std::string& new_jti, long long expires_at) = 0;

  virtual bool revoke_refresh_token(std::int64_t user_id, const std::string& jti) = 0;
  virtual void revoke_all_refresh_tokens(std::int64_t user_id) = 0;

  virtual bool create_one_time_token(std::int64_t user_id, const std::string& purpose,
                                     const std::string& token_hash,
                                     long long expires_at) = 0;

  /// 原子消费：未使用且未过期才返回 user_id。
  virtual std::optional<std::int64_t> consume_one_time_token(
      const std::string& purpose, const std::string& token_hash) = 0;
};

}  // namespace auth
