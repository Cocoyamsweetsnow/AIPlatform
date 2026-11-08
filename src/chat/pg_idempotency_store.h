// PostgreSQL 幂等记录实现（request_dedup 表）。
#pragma once

#include <algorithm>

#include "chat/idempotency.h"
#include "storage/db.h"

namespace chat {

class PgIdempotencyStore final : public IdempotencyStore {
 public:
  explicit PgIdempotencyStore(storage::DbPool& db, int stale_after_s = 180)
      : db_(db), stale_after_s_(std::max(1, stale_after_s)) {}

  DedupRecord claim(const std::string& scope, const std::string& request_id) override;
  void complete(const std::string& scope, const std::string& request_id,
                const std::string& session_id,
                std::int64_t assistant_message_id) override;
  void fail(const std::string& scope, const std::string& request_id,
            const std::string& error_type, const std::string& error_message) override;
  void remove(const std::string& scope, const std::string& request_id) override;

 private:
  storage::DbPool& db_;
  int stale_after_s_ = 180;
};

}  // namespace chat
