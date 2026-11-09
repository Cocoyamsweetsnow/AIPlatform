#include "config.h"

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace platform {
namespace {

std::string trim(const std::string& s) {
  const auto begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  const auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

// 逗号分隔的多 key，忽略空项；顺序就是轮转顺序。
std::vector<std::string> split_keys(const std::string& raw) {
  std::vector<std::string> keys;
  std::size_t start = 0;
  while (start <= raw.size()) {
    const std::size_t comma = raw.find(',', start);
    const std::string item =
        trim(raw.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
    if (!item.empty()) keys.push_back(item);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return keys;
}

std::string env_str(const char* key, const std::string& fallback) {
  const char* v = std::getenv(key);
  if (v == nullptr || *v == '\0') return fallback;
  return std::string(v);
}

int env_int(const char* key, int fallback) {
  const char* v = std::getenv(key);
  if (v == nullptr || *v == '\0') return fallback;
  try {
    std::size_t pos = 0;
    const int value = std::stoi(v, &pos);
    if (pos != std::string(v).size()) throw std::invalid_argument("trailing chars");
    return value;
  } catch (const std::exception&) {
    throw std::runtime_error(std::string("环境变量 ") + key + " 不是合法整数: " + v);
  }
}

double env_double(const char* key, double fallback) {
  const char* v = std::getenv(key);
  if (v == nullptr || *v == '\0') return fallback;
  try {
    return std::stod(v);
  } catch (const std::exception&) {
    throw std::runtime_error(std::string("环境变量 ") + key + " 不是合法浮点数: " + v);
  }
}

bool env_bool(const char* key, bool fallback) {
  const std::string v = env_str(key, "");
  if (v.empty()) return fallback;
  if (v == "1" || v == "true" || v == "TRUE" || v == "yes") return true;
  if (v == "0" || v == "false" || v == "FALSE" || v == "no") return false;
  throw std::runtime_error(std::string("环境变量 ") + key + " 不是合法布尔值: " + v);
}

}  // namespace

Config Config::from_env() {
  Config c;
  c.host = env_str("AIP_HOST", c.host);
  c.port = env_int("AIP_PORT", c.port);
  c.api_token = env_str("AIP_TOKEN", c.api_token);
  c.web_enabled = env_bool("AIP_WEB", c.web_enabled);
  c.log_format = env_str("AIP_LOG_FORMAT", c.log_format);
  c.metrics_enabled = env_bool("AIP_METRICS_ENABLED", c.metrics_enabled);
  c.metrics_token = env_str("AIP_METRICS_TOKEN", c.metrics_token);
  c.trust_proxy = env_bool("AIP_TRUST_PROXY", c.trust_proxy);

  c.provider = env_str("AIP_PROVIDER", c.provider);
  c.base_url = env_str("AIP_BASE_URL", c.base_url);
  c.api_key = env_str("AIP_API_KEY", c.api_key);
  c.api_keys = split_keys(env_str("AIP_API_KEYS", ""));
  if (c.api_keys.empty() && !c.api_key.empty()) c.api_keys.push_back(c.api_key);
  c.model = env_str("AIP_MODEL", c.model);
  c.models = split_keys(env_str("AIP_MODELS", ""));
  if (c.models.empty()) c.models.push_back(c.model);
  c.request_timeout_s = env_int("AIP_TIMEOUT_S", c.request_timeout_s);
  c.max_tokens = env_int("AIP_MAX_TOKENS", c.max_tokens);
  c.temperature = env_double("AIP_TEMPERATURE", c.temperature);
  c.include_usage = env_bool("AIP_INCLUDE_USAGE", c.include_usage);
  c.max_retries = env_int("AIP_MAX_RETRIES", c.max_retries);
  c.retry_base_ms = env_int("AIP_RETRY_BASE_MS", c.retry_base_ms);
  c.key_rpm_limit = env_int("AIP_KEY_RPM_LIMIT", c.key_rpm_limit);
  c.key_tpm_limit = env_int("AIP_KEY_TPM_LIMIT", c.key_tpm_limit);
  c.key_failure_threshold =
      env_int("AIP_KEY_FAILURE_THRESHOLD", c.key_failure_threshold);
  c.key_cooldown_s = env_int("AIP_KEY_COOLDOWN_S", c.key_cooldown_s);
  c.key_cooldown_max_s =
      env_int("AIP_KEY_COOLDOWN_MAX_S", c.key_cooldown_max_s);

  c.system_prompt = env_str("AIP_SYSTEM_PROMPT", c.system_prompt);
  c.max_history_messages = env_int("AIP_MAX_HISTORY", c.max_history_messages);
  c.max_context_tokens = env_int("AIP_MAX_CONTEXT_TOKENS", c.max_context_tokens);
  c.max_sessions = env_int("AIP_MAX_SESSIONS", c.max_sessions);

  c.storage = env_str("AIP_STORAGE", c.storage);
  c.db_dsn = env_str("AIP_DB_DSN", c.db_dsn);
  c.db_pool_size = env_int("AIP_DB_POOL_SIZE", c.db_pool_size);
  c.db_connect_timeout_s = env_int("AIP_DB_CONNECT_TIMEOUT_S", c.db_connect_timeout_s);

  c.auth_required = env_bool("AIP_AUTH_REQUIRED", c.auth_required);
  c.jwt_secret = env_str("AIP_JWT_SECRET", c.jwt_secret);
  c.jwt_issuer = env_str("AIP_JWT_ISSUER", c.jwt_issuer);
  c.jwt_ttl_s = env_int("AIP_JWT_TTL_S", c.jwt_ttl_s);
  c.jwt_refresh_ttl_s = env_int("AIP_JWT_REFRESH_TTL_S", c.jwt_refresh_ttl_s);

  c.email_verification_required =
      env_bool("AIP_EMAIL_VERIFICATION_REQUIRED", c.email_verification_required);
  c.public_base_url = env_str("AIP_PUBLIC_BASE_URL", c.public_base_url);
  c.smtp_url = env_str("AIP_SMTP_URL", c.smtp_url);
  c.smtp_from = env_str("AIP_SMTP_FROM", c.smtp_from);
  c.smtp_starttls = env_bool("AIP_SMTP_STARTTLS", c.smtp_starttls);
  c.auth_login_rpm = env_int("AIP_AUTH_LOGIN_RPM", c.auth_login_rpm);
  c.auth_register_rpm = env_int("AIP_AUTH_REGISTER_RPM", c.auth_register_rpm);
  c.auth_email_rpm = env_int("AIP_AUTH_EMAIL_RPM", c.auth_email_rpm);
  c.email_verify_ttl_s = env_int("AIP_EMAIL_VERIFY_TTL_S", c.email_verify_ttl_s);
  c.password_reset_ttl_s =
      env_int("AIP_PASSWORD_RESET_TTL_S", c.password_reset_ttl_s);
  c.rpm_limit = env_int("AIP_RPM_LIMIT", c.rpm_limit);
  c.daily_token_limit = env_int("AIP_DAILY_TOKEN_LIMIT", c.daily_token_limit);
  c.max_concurrent_per_user =
      env_int("AIP_MAX_CONCURRENT_PER_USER", c.max_concurrent_per_user);
  c.idempotency_ttl_s = env_int("AIP_IDEMPOTENCY_TTL_S", c.idempotency_ttl_s);

  c.io_threads = env_int("AIP_IO_THREADS", c.io_threads);
  c.max_streams = env_int("AIP_MAX_STREAMS", c.max_streams);
  c.heartbeat_s = env_int("AIP_HEARTBEAT_S", c.heartbeat_s);
  c.shutdown_drain_s = env_int("AIP_SHUTDOWN_DRAIN_S", c.shutdown_drain_s);

  // 去掉 base_url 末尾的 '/'，避免拼出双斜杠
  while (!c.base_url.empty() && c.base_url.back() == '/') c.base_url.pop_back();

  if (c.port <= 0 || c.port > 65535) throw std::runtime_error("AIP_PORT 超出范围");
  if (c.models.size() > 64) throw std::runtime_error("AIP_MODELS 最多 64 个模型");
  for (const auto& model : c.models) {
    if (model.size() > 128) throw std::runtime_error("AIP_MODELS 中单个模型名不能超过 128 字节");
  }
  if (c.log_format != "text" && c.log_format != "json") {
    throw std::runtime_error("AIP_LOG_FORMAT 只支持 text 或 json，当前值: " + c.log_format);
  }
  if (c.max_tokens <= 0) throw std::runtime_error("AIP_MAX_TOKENS 必须为正数");
  if (c.max_context_tokens < 1024 || c.max_context_tokens > 1000000) {
    throw std::runtime_error("AIP_MAX_CONTEXT_TOKENS 应在 [1024, 1000000]");
  }
  if (c.max_tokens >= c.max_context_tokens) {
    throw std::runtime_error("AIP_MAX_TOKENS 必须小于 AIP_MAX_CONTEXT_TOKENS");
  }
  if (c.temperature < 0.0 || c.temperature > 2.0) {
    throw std::runtime_error("AIP_TEMPERATURE 应在 [0, 2] 区间");
  }
  if (c.max_retries < 0 || c.max_retries > 5) {
    throw std::runtime_error("AIP_MAX_RETRIES 应在 [0, 5]（0 表示不重试）");
  }
  if (c.retry_base_ms < 0 || c.retry_base_ms > 10000) {
    throw std::runtime_error("AIP_RETRY_BASE_MS 应在 [0, 10000]");
  }
  if (c.key_rpm_limit < 0 || c.key_rpm_limit > 1000000) {
    throw std::runtime_error("AIP_KEY_RPM_LIMIT 应在 [0, 1000000]");
  }
  if (c.key_tpm_limit < 0 || c.key_tpm_limit > 1000000000) {
    throw std::runtime_error("AIP_KEY_TPM_LIMIT 应在 [0, 1000000000]");
  }
  if (c.key_failure_threshold < 1 || c.key_failure_threshold > 100) {
    throw std::runtime_error("AIP_KEY_FAILURE_THRESHOLD 应在 [1, 100]");
  }
  if (c.key_cooldown_s < 1 || c.key_cooldown_s > 3600) {
    throw std::runtime_error("AIP_KEY_COOLDOWN_S 应在 [1, 3600]");
  }
  if (c.key_cooldown_max_s < c.key_cooldown_s || c.key_cooldown_max_s > 86400) {
    throw std::runtime_error("AIP_KEY_COOLDOWN_MAX_S 应在 [AIP_KEY_COOLDOWN_S, 86400]");
  }
  if (c.io_threads < 0 || c.io_threads > 64) {
    throw std::runtime_error("AIP_IO_THREADS 应在 [0, 64]（0 表示按核数自动）");
  }
  if (c.max_streams <= 0) throw std::runtime_error("AIP_MAX_STREAMS 必须为正数");
  // 心跳太短会浪费带宽，太长会让「客户端断开」迟迟发现不了（最坏 = 一个心跳周期）。
  if (c.heartbeat_s < 1 || c.heartbeat_s > 300) {
    throw std::runtime_error("AIP_HEARTBEAT_S 应在 [1, 300] 秒");
  }
  if (c.shutdown_drain_s < 0 || c.shutdown_drain_s > 600) {
    throw std::runtime_error("AIP_SHUTDOWN_DRAIN_S 应在 [0, 600] 秒");
  }
  if (c.storage != "memory" && c.storage != "postgres") {
    throw std::runtime_error("AIP_STORAGE 只支持 memory 或 postgres，当前值: " + c.storage);
  }
  if (c.storage == "postgres") {
#if !defined(AIP_WITH_POSTGRES)
    throw std::runtime_error(
        "当前构建未启用 PostgreSQL 支持（-DAIPLATFORM_WITH_POSTGRES=OFF），"
        "请用 AIP_STORAGE=memory 或重新编译");
#else
    if (c.db_dsn.empty()) {
      throw std::runtime_error(
          "AIP_STORAGE=postgres 时必须设置 AIP_DB_DSN，"
          "例如 postgresql://aiplatform:aiplatform@127.0.0.1:55432/aiplatform");
    }
#endif
  }
  if (c.db_pool_size < 1 || c.db_pool_size > 64) {
    throw std::runtime_error("AIP_DB_POOL_SIZE 应在 [1, 64]");
  }
  if (c.db_connect_timeout_s < 1 || c.db_connect_timeout_s > 60) {
    throw std::runtime_error("AIP_DB_CONNECT_TIMEOUT_S 应在 [1, 60] 秒");
  }
  if (c.jwt_issuer.empty() || c.jwt_issuer.size() > 64) {
    throw std::runtime_error("AIP_JWT_ISSUER 长度应在 1~64");
  }
  if (c.jwt_ttl_s < 60 || c.jwt_ttl_s > 86400) {
    throw std::runtime_error("AIP_JWT_TTL_S 应在 [60, 86400] 秒");
  }
  if (c.jwt_refresh_ttl_s < 3600 || c.jwt_refresh_ttl_s > 2592000) {
    throw std::runtime_error("AIP_JWT_REFRESH_TTL_S 应在 [3600, 2592000] 秒");
  }
  if (!c.jwt_secret.empty() && c.jwt_secret.size() < 32) {
    throw std::runtime_error("AIP_JWT_SECRET 至少 32 个字节（建议 openssl rand -hex 32）");
  }
  if (c.auth_login_rpm < 0 || c.auth_login_rpm > 100000) {
    throw std::runtime_error("AIP_AUTH_LOGIN_RPM 应在 [0, 100000]");
  }
  if (c.auth_register_rpm < 0 || c.auth_register_rpm > 100000) {
    throw std::runtime_error("AIP_AUTH_REGISTER_RPM 应在 [0, 100000]");
  }
  if (c.auth_email_rpm < 0 || c.auth_email_rpm > 100000) {
    throw std::runtime_error("AIP_AUTH_EMAIL_RPM 应在 [0, 100000]");
  }
  if (c.email_verify_ttl_s < 60 || c.email_verify_ttl_s > 7 * 24 * 3600) {
    throw std::runtime_error("AIP_EMAIL_VERIFY_TTL_S 应在 [60, 604800]");
  }
  if (c.password_reset_ttl_s < 60 || c.password_reset_ttl_s > 24 * 3600) {
    throw std::runtime_error("AIP_PASSWORD_RESET_TTL_S 应在 [60, 86400]");
  }
  if (c.smtp_from.find_first_of("\r\n") != std::string::npos) {
    throw std::runtime_error("AIP_SMTP_FROM 不能包含换行");
  }
  if (!c.public_base_url.empty() && c.public_base_url.back() == '/') {
    c.public_base_url.pop_back();
  }
  if (c.rpm_limit < 0 || c.rpm_limit > 1000000) {
    throw std::runtime_error("AIP_RPM_LIMIT 应在 [0, 1000000]（0 表示不限）");
  }
  if (c.daily_token_limit < 0 || c.daily_token_limit > 1000000000) {
    throw std::runtime_error("AIP_DAILY_TOKEN_LIMIT 应在 [0, 1000000000]（0 表示不限）");
  }
  if (c.max_concurrent_per_user < 0 || c.max_concurrent_per_user > 1000) {
    throw std::runtime_error("AIP_MAX_CONCURRENT_PER_USER 应在 [0, 1000]（0 表示不限）");
  }
  if (c.idempotency_ttl_s < 30 || c.idempotency_ttl_s > 3600) {
    throw std::runtime_error("AIP_IDEMPOTENCY_TTL_S 应在 [30, 3600] 秒");
  }
  if (c.auth_required && c.api_token.empty() && c.jwt_secret.empty()) {
    throw std::runtime_error(
        "AIP_AUTH_REQUIRED=1 时必须配置 AIP_TOKEN 或 AIP_JWT_SECRET");
  }
  if (!c.jwt_secret.empty() && c.storage != "postgres") {
    throw std::runtime_error("JWT/用户体系需要 AIP_STORAGE=postgres（用户存在 PostgreSQL）");
  }
#if !defined(AIP_WITH_AUTH)
  if (!c.jwt_secret.empty() || c.auth_required) {
    throw std::runtime_error(
        "当前构建未启用用户鉴权（-DAIPLATFORM_WITH_AUTH=OFF），"
        "请改用 AIP_TOKEN 或重新编译");
  }
#endif
  if (c.provider != "mock" && c.provider != "openai") {
    throw std::runtime_error("AIP_PROVIDER 只支持 mock 或 openai，当前值: " + c.provider);
  }
  if (c.provider == "openai" && c.api_keys.empty()) {
    throw std::runtime_error(
        "AIP_PROVIDER=openai 时必须设置 AIP_API_KEY 或 AIP_API_KEYS"
        "（也可以先用 AIP_PROVIDER=mock 离线联调）");
  }
  if (c.provider == "openai" && c.base_url.empty()) {
    throw std::runtime_error("AIP_PROVIDER=openai 时 AIP_BASE_URL 不能为空");
  }
  return c;
}

}  // namespace platform
