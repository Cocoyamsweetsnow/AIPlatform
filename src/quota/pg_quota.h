// 多副本配额：PostgreSQL 原子预留。
//
// 关键点：
//   - RPM 用 rate_limits 固定窗口，UPSERT 一条 SQL 原子计数；
//   - 每日 token 用「advisory lock + 查预留/用量 + 插入预留」事务，避免先查后写超卖；
//   - 并发流用带过期时间的 lease，进程崩溃后 TTL 自动回收；
//   - 流结束时 QuotaGuard 删除预留/lease；usage_records 由 RecordingStream 落库。
#pragma once

#include "quota/quota.h"
#include "storage/db.h"

namespace quota {

class PgQuotaService final : public QuotaController {
 public:
  PgQuotaService(storage::DbPool& db, QuotaConfig config, int lease_ttl_s);

  QuotaAcquired try_acquire(const auth::Principal& principal, int estimated_tokens,
                            long long daily_used_tokens) override;
  bool requires_daily_usage() const override { return false; }

 private:
  QuotaDecision check_rpm(const std::string& user_id);

  storage::DbPool& db_;
  QuotaConfig config_;
  int lease_ttl_s_ = 900;
};

}  // namespace quota
