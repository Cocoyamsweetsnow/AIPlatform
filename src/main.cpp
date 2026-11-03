#include <csignal>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>

#include "chat/chat_service.h"
#include "chat/session_store.h"
#include "common/log.h"
#include "config.h"
#include "http/server.h"
#include "llm/mock_provider.h"
#include "llm/openai_provider.h"

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

    std::unique_ptr<llm::Provider> provider;
    if (config.provider == "openai") {
      llm::OpenAiOptions options;
      options.base_url = config.base_url;
      options.api_key = config.api_key;
      options.default_model = config.model;
      options.timeout_s = config.request_timeout_s;
      options.include_usage = config.include_usage;
      provider = llm::make_openai_provider(std::move(options));
      log_info("model gateway: " + config.base_url + " (model=" + config.model + ")");
    } else {
      provider = llm::make_mock_provider();
      log_info("model gateway: mock（离线模式，不消耗 token）");
    }

    chat::SessionStore store(/*max_messages_per_session=*/200, /*max_sessions=*/
                             static_cast<std::size_t>(config.max_sessions));

    chat::ChatOptions chat_options;
    chat_options.system_prompt = config.system_prompt;
    chat_options.max_history_messages = static_cast<std::size_t>(config.max_history_messages);
    chat_options.max_tokens = config.max_tokens;
    chat_options.temperature = config.temperature;
    chat_options.default_model = config.model;

    chat::ChatService service(*provider, store, std::move(chat_options));

    http::ApiServer server(config, service, store);
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
