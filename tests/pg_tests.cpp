// PostgreSQL 集成测试：默认跳过；设置 AIP_TEST_PG_DSN 后执行。
//
// 由 scripts/test_pg.sh 启动临时 PostgreSQL 容器并注入 DSN；CI 里用 service。
#include <cstdio>
#include <cstdlib>
#include <string>

#include "chat/pg_idempotency_store.h"
#include "chat/pg_session_store.h"
#include "quota/pg_quota.h"
#include "storage/db.h"
#include "storage/migrations.h"
#include "usage/pg_usage_sink.h"
#ifdef AIP_WITH_AUTH
#include <ctime>

#include "auth/api_key.h"
#include "auth/auth_service.h"
#include "auth/errors.h"
#include "auth/pg_user_store.h"
#include "auth/rate_limiter.h"
#endif

namespace {

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
      ++g_failures;                                                        \
    }                                                                      \
  } while (false)

std::int64_t make_user(storage::DbPool& pool, const std::string& email) {
  const auto result =
      pool.exec("INSERT INTO users (email, password_hash) VALUES ($1, $2) RETURNING id",
                {email, "test-hash"});
  return result.int64(0, 0);
}

}  // namespace

int main() {
  const char* dsn = std::getenv("AIP_TEST_PG_DSN");
  if (dsn == nullptr || *dsn == '\0') {
    std::printf("pg_tests skipped: AIP_TEST_PG_DSN is not set\n");
    return 0;
  }

  try {
    storage::DbConfig config;
    config.dsn = dsn;
    config.pool_size = 4;
    config.connect_timeout_s = 5;
    storage::DbPool pool(config);

    // 迁移必须可重复执行（多实例启动 / 重试）。
    storage::run_migrations(pool);
    storage::run_migrations(pool);

    pool.exec(
        "TRUNCATE users, api_keys, sessions, messages, usage_records, request_dedup,"
        " refresh_tokens, one_time_tokens, rate_limits, quota_reservations,"
        " quota_stream_leases "
        "RESTART IDENTITY CASCADE");

    const std::int64_t alice = make_user(pool, "alice@example.com");
    const std::int64_t bob = make_user(pool, "bob@example.com");
    CHECK(alice > 0);
    CHECK(bob > 0);
    CHECK(alice != bob);

#ifdef AIP_WITH_AUTH
    auth::PgUserStore user_store(pool);
    auth::AuthConfig auth_config;
    auth_config.jwt.secret = "0123456789abcdef0123456789abcdef";
    auth_config.jwt.access_ttl_s = 900;
    auth_config.public_base_url = "https://chat.example.com";
    auth::AuthService auth_service(user_store, auth_config);

    const auth::RegisterOutcome carol_registered =
        auth_service.register_user("carol@example.com", "password123", "Carol");
    const auth::TokenPair& carol = carol_registered.tokens;
    CHECK(carol.user.id > 0);
    CHECK(carol.user.email == "carol@example.com");
    CHECK(carol.user.email_verified);

    const auto found = user_store.find_by_email("CAROL@example.com");
    CHECK(found.has_value());
    if (found.has_value()) CHECK(found->display_name == "Carol");

    const auto carol_principal = auth_service.authenticate_bearer(carol.access_token);
    CHECK(carol_principal.has_value());
    CHECK(carol_principal->user_id == carol.user.id);

    const auth::TokenPair login = auth_service.login("carol@example.com", "password123");
    CHECK(login.user.id == carol.user.id);

    // refresh 轮换：旧 token 立即失效。
    const auth::TokenPair rotated = auth_service.refresh(login.refresh_token);
    CHECK(rotated.user.id == carol.user.id);
    bool old_refresh_reused = false;
    try {
      auth_service.refresh(login.refresh_token);
    } catch (const auth::AuthError& e) {
      old_refresh_reused = e.code() == auth::AuthErrorCode::Unauthorized;
    }
    CHECK(old_refresh_reused);
    CHECK(auth_service.authenticate_bearer(rotated.access_token).has_value());

    // 改密后旧 access token 失效，新 token 生效。
    const auth::TokenPair changed =
        auth_service.change_password(*carol_principal, "password123", "newpassword123");
    CHECK(!auth_service.authenticate_bearer(carol.access_token).has_value());
    CHECK(auth_service.authenticate_bearer(changed.access_token).has_value());
    CHECK(auth_service.login("carol@example.com", "newpassword123").user.id == carol.user.id);

    // 一次性 token 只能消费一次。
    CHECK(user_store.create_one_time_token(carol.user.id, "password_reset",
                                           auth::sha256_hex("one-time-test"),
                                           std::time(nullptr) + 600));
    const auto consumed =
        user_store.consume_one_time_token("password_reset", auth::sha256_hex("one-time-test"));
    CHECK(consumed.has_value() && *consumed == carol.user.id);
    CHECK(!user_store
               .consume_one_time_token("password_reset", auth::sha256_hex("one-time-test"))
               .has_value());

    // 固定窗口限流在多副本/多连接下共享同一张表。
    auth::PgRateLimiter limiter(pool);
    CHECK(limiter.check("test:bucket", 2, 60).allowed);
    CHECK(limiter.check("test:bucket", 2, 60).allowed);
    const auth::RateLimitDecision denied = limiter.check("test:bucket", 2, 60);
    CHECK(!denied.allowed);
    CHECK(denied.retry_after_s >= 1);

    bool duplicate = false;
    try {
      auth_service.register_user("carol@example.com", "password123", "Carol 2");
    } catch (const auth::AuthError& e) {
      duplicate = e.code() == auth::AuthErrorCode::EmailExists;
    }
    CHECK(duplicate);

    const auth::IssuedApiKey issued = auth_service.create_api_key(*carol_principal, "pg-test");
    CHECK(issued.info.id > 0);
    CHECK(issued.plaintext.rfind("aip_", 0) == 0);
    CHECK(auth_service.list_api_keys(*carol_principal).size() == 1);

    const auto via_key = auth_service.authenticate_bearer(issued.plaintext);
    CHECK(via_key.has_value());
    if (via_key.has_value()) CHECK(via_key->auth_type == "api_key");

    CHECK(auth_service.revoke_api_key(*carol_principal, issued.info.id));
    CHECK(!auth_service.authenticate_bearer(issued.plaintext).has_value());
    CHECK(!user_store.find_by_api_key_hash(auth::hash_api_key(issued.plaintext)).has_value());
#endif

    chat::PgSessionStore store(pool);
    const std::string sid = store.create(alice, "初始标题", "mock-model");
    CHECK(!sid.empty());
    CHECK(store.exists(sid));
    CHECK(store.owns(alice, /*is_admin=*/false, sid));
    CHECK(!store.owns(bob, /*is_admin=*/false, sid));
    CHECK(store.owns(bob, /*is_admin=*/true, sid));

    const auto first_id = store.append(sid, llm::Message{"user", "你好"});
    const auto second_id = store.append(sid, llm::Message{"assistant", "你好，我是 mock"});
    CHECK(first_id > 0);
    CHECK(second_id > first_id);

    const auto history = store.history(sid, 10);
    CHECK(history.size() == 2);
    CHECK(history[0].role == "user");
    CHECK(history[0].content == "你好");
    CHECK(history[1].role == "assistant");

    const auto info = store.get(sid);
    CHECK(info.has_value());
    if (info.has_value()) {
      CHECK(info->user_id == alice);
      CHECK(info->title == "初始标题");
      CHECK(info->message_count == 2);
    }

    const auto alice_sessions = store.list(alice, false, 10, 0);
    CHECK(alice_sessions.size() == 1);
    CHECK(store.list(bob, false, 10, 0).empty());
    CHECK(store.list(bob, true, 10, 0).size() == 1);

    CHECK(store.rename(bob, false, sid, "越权") == false);
    CHECK(store.rename(alice, false, sid, "新标题") == true);
    CHECK(store.get(sid)->title == "新标题");

    // 换一个 store 实例仍能读到，证明不是进程内缓存。
    chat::PgSessionStore reopened(pool);
    CHECK(reopened.history(sid, 10).size() == 2);

    // 客户端自带 ID：ensure 只创建一次。
    const std::string client_sid = "s_client_supplied_test";
    CHECK(store.ensure(client_sid, alice, "客户端 ID", "mock-model"));
    CHECK(!store.ensure(client_sid, alice, "客户端 ID", "mock-model"));
    CHECK(store.exists(client_sid));

    CHECK(store.remove(bob, false, sid) == false);
    CHECK(store.remove(alice, false, sid) == true);
    CHECK(!store.exists(sid));
    CHECK(store.history(sid, 10).empty());

    // 软删除后列表不再出现。
    const auto after_delete = store.list(alice, false, 10, 0);
    for (const auto& session : after_delete) CHECK(session.id != sid);

    usage::PgUsageSink usage_sink(pool);
    usage::UsageEvent usage_event;
    usage_event.user_id = alice;
    usage_event.session_id = client_sid;
    usage_event.request_id = "req-pg-test";
    usage_event.model = "mock-model";
    usage_event.prompt_tokens = 11;
    usage_event.completion_tokens = 22;
    usage_event.latency_ms = 123;
    usage_event.ttft_ms = 45;
    usage_sink.record(usage_event);

    const usage::UsageSummary summary = usage_sink.summary_today(alice);
    CHECK(summary.requests == 1);
    CHECK(summary.prompt_tokens == 11);
    CHECK(summary.completion_tokens == 22);
    CHECK(summary.total_tokens() == 33);

    chat::PgIdempotencyStore dedup(pool);
    const std::string scope = "u:" + std::to_string(alice);
    const std::string request_id = "req-idem-pg";
    CHECK(dedup.claim(scope, request_id).status == chat::DedupStatus::Claimed);
    CHECK(dedup.claim(scope, request_id).status == chat::DedupStatus::InProgress);
    dedup.complete(scope, request_id, client_sid, 99);
    const auto completed = dedup.claim(scope, request_id);
    CHECK(completed.status == chat::DedupStatus::Completed);
    CHECK(completed.session_id == client_sid);
    CHECK(completed.assistant_message_id == 99);

    dedup.fail("u:other", "req-failed", "upstream_error", "boom");
    CHECK(dedup.claim("u:other", "req-failed").status == chat::DedupStatus::Claimed);
    dedup.remove(scope, request_id);
    CHECK(dedup.claim(scope, request_id).status == chat::DedupStatus::Claimed);

    pool.exec(
        "INSERT INTO request_dedup (scope, request_id, status, updated_at)"
        " VALUES ('u:stale', 'req-stale', 'in_progress', now() - interval '10 minutes')");
    CHECK(dedup.claim("u:stale", "req-stale").status == chat::DedupStatus::Claimed);
    dedup.remove("u:stale", "req-stale");

    // 多副本配额：每日 token 原子预留 + 并发 lease。
    auth::Principal alice_principal;
    alice_principal.user_id = alice;
    quota::QuotaConfig daily_config;
    daily_config.rpm_limit = 0;
    daily_config.daily_token_limit = 100;
    daily_config.max_concurrent = 0;
    quota::PgQuotaService daily_quota(pool, daily_config, 300);
    CHECK(!daily_quota.requires_daily_usage());

    auto daily_first = daily_quota.try_acquire(alice_principal, 60, 0);
    CHECK(daily_first.decision.allowed);
    // 60 + 50 > 100，第二笔必须被原子预留拦住。
    auto over_daily = daily_quota.try_acquire(alice_principal, 50, 0);
    CHECK(!over_daily.decision.allowed);
    CHECK(over_daily.decision.type == "quota_exceeded");
    daily_first.guard.release();

    auto daily_second = daily_quota.try_acquire(alice_principal, 50, 0);
    CHECK(daily_second.decision.allowed);
    daily_second.guard.release();

    const auto reservations = pool.exec(
        "SELECT count(*) FROM quota_reservations WHERE user_id = $1",
        {std::to_string(alice)});
    const auto leases = pool.exec(
        "SELECT count(*) FROM quota_stream_leases WHERE user_id = $1",
        {std::to_string(alice)});
    CHECK(reservations.int64(0, 0) == 0);
    CHECK(leases.int64(0, 0) == 0);

    // 并发 lease：第二个在途请求被拒，释放后可再次进入。
    quota::QuotaConfig concurrent_config;
    concurrent_config.rpm_limit = 0;
    concurrent_config.daily_token_limit = 0;
    concurrent_config.max_concurrent = 1;
    quota::PgQuotaService concurrent_quota(pool, concurrent_config, 300);
    auto concurrent_first = concurrent_quota.try_acquire(alice_principal, 10, 0);
    CHECK(concurrent_first.decision.allowed);
    auto concurrent_second = concurrent_quota.try_acquire(alice_principal, 10, 0);
    CHECK(!concurrent_second.decision.allowed);
    CHECK(concurrent_second.decision.type == "rate_limited");
    concurrent_first.guard.release();
    auto concurrent_third = concurrent_quota.try_acquire(alice_principal, 10, 0);
    CHECK(concurrent_third.decision.allowed);
    concurrent_third.guard.release();

    // RPM 固定窗口：两个请求后第三个 429。
    auth::Principal bob_principal;
    bob_principal.user_id = bob;
    quota::QuotaConfig rpm_config;
    rpm_config.rpm_limit = 2;
    rpm_config.daily_token_limit = 0;
    rpm_config.max_concurrent = 0;
    quota::PgQuotaService rpm_quota(pool, rpm_config, 300);
    CHECK(rpm_quota.try_acquire(bob_principal, 10, 0).decision.allowed);
    CHECK(rpm_quota.try_acquire(bob_principal, 10, 0).decision.allowed);
    auto rpm_denied = rpm_quota.try_acquire(bob_principal, 10, 0);
    CHECK(!rpm_denied.decision.allowed);
    CHECK(rpm_denied.decision.type == "rate_limited");
    CHECK(rpm_denied.decision.retry_after_s >= 1);

    pool.exec(
        "TRUNCATE users, api_keys, sessions, messages, usage_records, request_dedup,"
        " refresh_tokens, one_time_tokens, rate_limits, quota_reservations,"
        " quota_stream_leases "
        "RESTART IDENTITY CASCADE");
  } catch (const std::exception& e) {
    std::printf("pg_tests failed with exception: %s\n", e.what());
    return 1;
  }

  if (g_failures == 0) {
    std::printf("pg_tests: all checks passed\n");
    return 0;
  }
  std::printf("pg_tests: %d check(s) failed\n", g_failures);
  return 1;
}
