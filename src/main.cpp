#include <csignal>
#include <cstdlib>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>

#include "chat/chat_service.h"
#include "chat/session_store.h"
#include "common/log.h"
#include "common/metrics.h"
#include "config.h"
#include "http/server.h"
#include "llm/mock_provider.h"
#include "llm/openai_provider.h"
#include "quota/quota.h"

#include "auth/auth_service.h"
#if defined(AIP_WITH_AUTH)
#include "auth/email_sender.h"
#endif
#if defined(AIP_WITH_AUTH) && defined(AIP_WITH_POSTGRES)
#include "auth/pg_user_store.h"
#include "auth/rate_limiter.h"
#endif
#ifdef AIP_WITH_POSTGRES
#include "chat/pg_idempotency_store.h"
#include "chat/pg_session_store.h"
#include "storage/db.h"
#include "storage/migrations.h"
#include "usage/pg_usage_sink.h"
#endif

namespace {

// 信号处理由 HTTP 后端自己安装：Drogon 用 setTermSignalHandler（带 drain），
// httplib 回退实现用自己的 handler。main 只负责与框架无关的 SIGPIPE。
platform::LogLevel parse_level(const char* raw) {
  const std::string level = raw == nullptr ? "" : raw;
  if (level == "debug" || level == "DEBUG") return platform::LogLevel::Debug;
  if (level == "warn" || level == "WARN") return platform::LogLevel::Warn;
  if (level == "error" || level == "ERROR") return platform::LogLevel::Error;
  return platform::LogLevel::Info;
}

}  // namespace

int main() {
  using namespace platform;

  try {
    const Config config = Config::from_env();
    set_log_level(parse_level(std::getenv("AIP_LOG_LEVEL")));
    set_log_format(config.log_format == "json" ? LogFormat::Json : LogFormat::Text);
#if defined(AIP_HTTP_BACKEND_DROGON)
    constexpr const char* kHttpBackend = "drogon";
#else
    constexpr const char* kHttpBackend = "httplib";
#endif
    metrics::set_build_info(AIP_VERSION, kHttpBackend, config.provider);

    std::unique_ptr<llm::Provider> provider;
    if (config.provider == "openai") {
      llm::OpenAiOptions options;
      options.base_url = config.base_url;
      options.api_keys = config.api_keys;
      options.default_model = config.model;
      options.timeout_s = config.request_timeout_s;
      options.include_usage = config.include_usage;
      options.max_retries = config.max_retries;
      options.retry_base_ms = config.retry_base_ms;
      options.key_rpm_limit = config.key_rpm_limit;
      options.key_tpm_limit = config.key_tpm_limit;
      options.key_failure_threshold = config.key_failure_threshold;
      options.key_cooldown_s = config.key_cooldown_s;
      options.key_cooldown_max_s = config.key_cooldown_max_s;
      provider = llm::make_openai_provider(std::move(options));
      log_info("model gateway: " + config.base_url + " (model=" + config.model +
               ", keys=" + std::to_string(config.api_keys.size()) +
               ", retries=" + std::to_string(config.max_retries) + ")");
    } else {
      provider = llm::make_mock_provider();
      log_info("model gateway: mock（离线模式，不消耗 token）");
    }

    std::unique_ptr<chat::SessionStore> store;
    auth::AuthService* auth_ptr = nullptr;
    usage::UsageSink* usage_ptr = nullptr;
    std::unique_ptr<chat::IdempotencyStore> idempotency;
#ifdef AIP_WITH_POSTGRES
    std::unique_ptr<storage::DbPool> db;
    std::unique_ptr<usage::PgUsageSink> usage_sink;
#ifdef AIP_WITH_AUTH
    std::unique_ptr<auth::PgUserStore> user_store;
    std::unique_ptr<auth::AuthService> auth_service;
    std::unique_ptr<auth::RateLimiter> rate_limiter;
    std::unique_ptr<auth::EmailSender> email_sender;
#endif
#endif
    if (config.postgres_enabled()) {
#ifdef AIP_WITH_POSTGRES
      storage::DbConfig db_config;
      db_config.dsn = config.db_dsn;
      db_config.pool_size = config.db_pool_size;
      db_config.connect_timeout_s = config.db_connect_timeout_s;
      db = std::make_unique<storage::DbPool>(db_config);
      storage::run_migrations(*db);
      store = std::make_unique<chat::PgSessionStore>(*db);
      idempotency = std::make_unique<chat::PgIdempotencyStore>(*db,
                                                               config.idempotency_ttl_s);
      usage_sink = std::make_unique<usage::PgUsageSink>(*db);
      usage_ptr = usage_sink.get();
      log_info("storage: postgres（会话/消息持久化，进程重启不丢）");
#ifdef AIP_WITH_AUTH
      user_store = std::make_unique<auth::PgUserStore>(*db);
      rate_limiter = std::make_unique<auth::PgRateLimiter>(*db);
      if (config.smtp_url.empty()) {
        email_sender = auth::make_log_email_sender();
        log_warn("auth: 未配置 AIP_SMTP_URL，验证/重置邮件只写日志（仅适合本地/测试）");
      } else {
        email_sender = auth::make_smtp_email_sender(config.smtp_url, config.smtp_from,
                                                    config.smtp_starttls);
        log_info("auth: SMTP 邮件发送已启用（from=" + config.smtp_from + "）");
      }
      auth::AuthConfig auth_config;
      auth_config.static_token = config.api_token;
      auth_config.jwt.secret = config.jwt_secret;
      auth_config.jwt.issuer = config.jwt_issuer;
      auth_config.jwt.access_ttl_s = config.jwt_ttl_s;
      auth_config.jwt.refresh_ttl_s = config.jwt_refresh_ttl_s;
      auth_config.email_verification_required = config.email_verification_required;
      auth_config.public_base_url = config.public_base_url;
      auth_config.login_rpm = config.auth_login_rpm;
      auth_config.register_rpm = config.auth_register_rpm;
      auth_config.email_rpm = config.auth_email_rpm;
      auth_config.email_verify_ttl_s = config.email_verify_ttl_s;
      auth_config.password_reset_ttl_s = config.password_reset_ttl_s;
      auth_config.rate_limiter = rate_limiter.get();
      auth_config.email_sender = email_sender.get();
      auth_service = std::make_unique<auth::AuthService>(*user_store, std::move(auth_config));
      auth_ptr = auth_service.get();
      if (config.auth_enabled()) {
        log_info("auth: 用户鉴权已启用（静态 Token / JWT / API Key）");
      } else {
        log_warn("auth: 用户接口已启用，但未配置 AIP_TOKEN/AIP_JWT_SECRET/AIP_AUTH_REQUIRED，"
                 "对话接口仍匿名开放");
      }
#endif
#else
      throw std::runtime_error(
          "当前构建未启用 PostgreSQL（-DAIPLATFORM_WITH_POSTGRES=OFF）");
#endif
    } else {
      store = std::make_unique<chat::MemorySessionStore>(
          /*max_messages_per_session=*/200,
          static_cast<std::size_t>(config.max_sessions));
      idempotency = std::make_unique<chat::MemoryIdempotencyStore>();
      log_info("storage: memory（重启即丢，仅适合演示/测试）");
    }

    chat::ChatOptions chat_options;
    chat_options.system_prompt = config.system_prompt;
    chat_options.max_history_messages = static_cast<std::size_t>(config.max_history_messages);
    chat_options.max_tokens = config.max_tokens;
    chat_options.max_context_tokens = config.max_context_tokens;
    chat_options.temperature = config.temperature;
    chat_options.default_model = config.model;

    chat::ChatService service(*provider, *store, std::move(chat_options), usage_ptr);

    quota::QuotaConfig quota_config;
    quota_config.rpm_limit = config.rpm_limit;
    quota_config.daily_token_limit = config.daily_token_limit;
    quota_config.max_concurrent = config.max_concurrent_per_user;
    quota::QuotaService quota_service(quota_config);

    http::ApiServer server(config, service, *store, auth_ptr, &quota_service, usage_ptr,
                      idempotency.get());
    std::signal(SIGPIPE, SIG_IGN);  // 客户端断开时不要被 SIGPIPE 干掉

    log_info("try: curl -N http://127.0.0.1:" + std::to_string(config.port) +
             "/v1/chat/completions -H 'Content-Type: application/json' "
             "-d '{\"message\":\"你好\"}'");

    const bool ok = server.run();
    if (!ok) {
      log_error("服务启动失败：端口可能已被占用");
      return 1;
    }
    log_info("服务已退出");
    return 0;
  } catch (const std::exception& e) {
    log_error(std::string("启动失败: ") + e.what());
    return 1;
  }
}
