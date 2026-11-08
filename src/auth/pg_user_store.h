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

 private:
  storage::DbPool& db_;
};

}  // namespace auth
