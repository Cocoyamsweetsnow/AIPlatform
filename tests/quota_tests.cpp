// 进程内配额单元测试；不需要数据库。
#include <cstdio>

#include "quota/quota.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
      ++g_failures;                                                        \
    }                                                                      \
  } while (false)

}  // namespace

int main() {
  std::printf("running quota tests...\n");

  auth::Principal user;
  user.user_id = 7;
  auth::Principal admin;
  admin.is_admin = true;

  quota::QuotaConfig config;
  config.rpm_limit = 2;
  config.daily_token_limit = 100;
  config.max_concurrent = 1;
  quota::QuotaService service(config);

  auto first = service.try_acquire(user, 10, 0);
  CHECK(first.decision.allowed);

  auto concurrent = service.try_acquire(user, 10, 0);
  CHECK(!concurrent.decision.allowed);
  CHECK(concurrent.decision.type == "rate_limited");
  CHECK(concurrent.decision.retry_after_s >= 1);

  first.guard.release();

  auto second = service.try_acquire(user, 10, 0);
  CHECK(second.decision.allowed);
  second.guard.release();

  auto third = service.try_acquire(user, 10, 0);
  CHECK(!third.decision.allowed);
  CHECK(third.decision.type == "rate_limited");

  auto admin_request = service.try_acquire(admin, 1000000, 1000000);
  CHECK(admin_request.decision.allowed);

  auth::Principal other;
  other.user_id = 8;
  auto daily = service.try_acquire(other, 10, 95);
  CHECK(!daily.decision.allowed);
  CHECK(daily.decision.type == "quota_exceeded");
  CHECK(daily.decision.retry_after_s >= 1);

  quota::QuotaConfig unlimited;
  unlimited.rpm_limit = 0;
  unlimited.daily_token_limit = 0;
  unlimited.max_concurrent = 0;
  quota::QuotaService unlimited_service(unlimited);
  for (int i = 0; i < 100; ++i) {
    auto acquired = unlimited_service.try_acquire(user, 1000000, 1000000);
    CHECK(acquired.decision.allowed);
  }

  if (g_failures == 0) {
    std::printf("quota tests passed\n");
    return 0;
  }
  std::printf("%d quota check(s) failed\n", g_failures);
  return 1;
}
