// PostgreSQL 用量流水实现。
#pragma once

#include "storage/db.h"
#include "usage/usage.h"

namespace usage {

class PgUsageSink final : public UsageSink {
 public:
  explicit PgUsageSink(storage::DbPool& db) : db_(db) {}

  void record(const UsageEvent& event) override;
  UsageSummary summary_today(std::int64_t user_id) override;

 private:
  storage::DbPool& db_;
};

}  // namespace usage
