#include "config.h"

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace platform {
namespace {

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

  c.provider = env_str("AIP_PROVIDER", c.provider);
  c.base_url = env_str("AIP_BASE_URL", c.base_url);
  c.api_key = env_str("AIP_API_KEY", c.api_key);
  c.model = env_str("AIP_MODEL", c.model);
  c.request_timeout_s = env_int("AIP_TIMEOUT_S", c.request_timeout_s);
  c.max_tokens = env_int("AIP_MAX_TOKENS", c.max_tokens);
  c.temperature = env_double("AIP_TEMPERATURE", c.temperature);
  c.include_usage = env_bool("AIP_INCLUDE_USAGE", c.include_usage);

  c.system_prompt = env_str("AIP_SYSTEM_PROMPT", c.system_prompt);
  c.max_history_messages = env_int("AIP_MAX_HISTORY", c.max_history_messages);
  c.max_sessions = env_int("AIP_MAX_SESSIONS", c.max_sessions);

  // 去掉 base_url 末尾的 '/'，避免拼出双斜杠
  while (!c.base_url.empty() && c.base_url.back() == '/') c.base_url.pop_back();

  if (c.port <= 0 || c.port > 65535) throw std::runtime_error("AIP_PORT 超出范围");
  if (c.max_tokens <= 0) throw std::runtime_error("AIP_MAX_TOKENS 必须为正数");
  if (c.temperature < 0.0 || c.temperature > 2.0) {
    throw std::runtime_error("AIP_TEMPERATURE 应在 [0, 2] 区间");
  }
  if (c.provider != "mock" && c.provider != "openai") {
    throw std::runtime_error("AIP_PROVIDER 只支持 mock 或 openai，当前值: " + c.provider);
  }
  if (c.provider == "openai" && c.api_key.empty()) {
    throw std::runtime_error(
        "AIP_PROVIDER=openai 时必须设置 AIP_API_KEY（也可以先用 AIP_PROVIDER=mock 离线联调）");
  }
  return c;
}

}  // namespace platform
