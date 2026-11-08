// PostgreSQL 会话存储：生产模式。
//
// 只依赖 storage::DbPool；所有 SQL 参数化，避免拼接用户输入。
#pragma once

#include "chat/session_store.h"
#include "storage/db.h"

namespace chat {

class PgSessionStore final : public SessionStore {
 public:
  explicit PgSessionStore(storage::DbPool& db) : db_(db) {}

  std::string create(std::int64_t user_id, const std::string& title,
                     const std::string& model) override;
  bool ensure(const std::string& session_id, std::int64_t user_id, const std::string& title,
              const std::string& model) override;
  bool exists(const std::string& session_id) const override;
  bool owns(std::int64_t user_id, bool is_admin,
            const std::string& session_id) const override;
  std::int64_t append(const std::string& session_id, const llm::Message& message) override;
  std::vector<llm::Message> history(const std::string& session_id,
                                    std::size_t limit) const override;
  std::size_t message_count(const std::string& session_id) const override;
  std::size_t session_count() const override;
  std::optional<llm::Message> message(std::int64_t message_id) const override;
  std::optional<SessionInfo> get(const std::string& session_id) const override;
  std::vector<SessionInfo> list(std::int64_t user_id, bool is_admin, std::size_t limit,
                                std::size_t offset) const override;
  bool rename(std::int64_t user_id, bool is_admin, const std::string& session_id,
              const std::string& title) override;
  bool remove(std::int64_t user_id, bool is_admin,
              const std::string& session_id) override;

 private:
  storage::DbPool& db_;
};

}  // namespace chat
