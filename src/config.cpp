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

  c.provider = env_str("AIP_PROVIDER", c.provider);
  c.base_url = env_str("AIP_BASE_URL", c.base_url);
  c.api_key = env_str("AIP_API_KEY", c.api_key);
  c.api_keys = split_keys(env_str("AIP_API_KEYS", ""));
  if (c.api_keys.empty() && !c.api_key.empty()) c.api_keys.push_back(c.api_key);
  c.model = env_str("AIP_MODEL", c.model);
  c.request_timeout_s = env_int("AIP_TIMEOUT_S", c.request_timeout_s);
  c.max_tokens = env_int("AIP_MAX_TOKENS", c.max_tokens);
  c.temperature = env_double("AIP_TEMPERATURE", c.temperature);
  c.include_usage = env_bool("AIP_INCLUDE_USAGE", c.include_usage);
  c.max_retries = env_int("AIP_MAX_RETRIES", c.max_retries);
  c.retry_base_ms = env_int("AIP_RETRY_BASE_MS", c.retry_base_ms);

  c.system_prompt = env_str("AIP_SYSTEM_PROMPT", c.system_prompt);
  c.max_history_messages = env_int("AIP_MAX_HISTORY", c.max_history_messages);
  c.max_sessions = env_int("AIP_MAX_SESSIONS", c.max_sessions);

  c.io_threads = env_int("AIP_IO_THREADS", c.io_threads);
  c.max_streams = env_int("AIP_MAX_STREAMS", c.max_streams);
  c.heartbeat_s = env_int("AIP_HEARTBEAT_S", c.heartbeat_s);
  c.shutdown_drain_s = env_int("AIP_SHUTDOWN_DRAIN_S", c.shutdown_drain_s);

  // 去掉 base_url 末尾的 '/'，避免拼出双斜杠
  while (!c.base_url.empty() && c.base_url.back() == '/') c.base_url.pop_back();

  if (c.port <= 0 || c.port > 65535) throw std::runtime_error("AIP_PORT 超出范围");
  if (c.max_tokens <= 0) throw std::runtime_error("AIP_MAX_TOKENS 必须为正数");
  if (c.temperature < 0.0 || c.temperature > 2.0) {
    throw std::runtime_error("AIP_TEMPERATURE 应在 [0, 2] 区间");
  }
  if (c.max_retries < 0 || c.max_retries > 5) {
    throw std::runtime_error("AIP_MAX_RETRIES 应在 [0, 5]（0 表示不重试）");
  }
  if (c.retry_base_ms < 0 || c.retry_base_ms > 10000) {
    throw std::runtime_error("AIP_RETRY_BASE_MS 应在 [0, 10000]");
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
