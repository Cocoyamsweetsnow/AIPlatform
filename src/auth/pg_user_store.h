// PostgreSQL 用户/API Key 仓储。
#pragma once

#include "auth/user_store.h"
#include "storage/db.h"

namespace auth {

class PgUserStore final : public UserStore {
 public:
  explicit PgUserStore(storage::DbPool& db) : db_(db) {}

  std::optional<UserRecord> find_by_email(const std::string& email) override;
  std::optional<UserRecord> find_by_id(std::int64_t user_id) override;
  std::optional<UserRecord> create(const std::string& email,
                                   const std::string& password_hash,
                                   const std::string& display_name) override;
  std::optional<UserRecord> find_by_api_key_hash(const std::string& key_hash) override;
  std::int64_t create_api_key(std::int64_t user_id, const std::string& name,
                              const std::string& prefix,
                              const std::string& key_hash) override;
  std::vector<ApiKeyInfo> list_api_keys(std::int64_t user_id) override;
  bool revoke_api_key(std::int64_t user_id, std::int64_t key_id) override;

  bool mark_email_verified(std::int64_t user_id) override;
  std::optional<int> update_password(std::int64_t user_id,
                                     const std::string& password_hash) override;
  bool create_refresh_token(std::int64_t user_id, const std::string& jti,
                            long long expires_at) override;
  bool rotate_refresh_token(std::int64_t user_id, const std::string& old_jti,
                            const std::string& new_jti, long long expires_at) override;
  bool revoke_refresh_token(std::int64_t user_id, const std::string& jti) override;
  void revoke_all_refresh_tokens(std::int64_t user_id) override;
  bool create_one_time_token(std::int64_t user_id, const std::string& purpose,
                             const std::string& token_hash, long long expires_at) override;
  std::optional<std::int64_t> consume_one_time_token(
      const std::string& purpose, const std::string& token_hash) override;

 private:
  storage::DbPool& db_;
};

}  // namespace auth
