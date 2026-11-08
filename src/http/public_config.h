// 公开的客户端配置：Web UI 用它决定要不要显示 Token 输入框、消息大小上限等。
//
// 只暴露「非敏感」字段；api_key / base_url / system_prompt 一律不返回。
// 该接口不需要鉴权（页面本身也必须先加载出来），与实际对话接口的鉴权无关。
#pragma once

#include <nlohmann/json.hpp>

#include <string>

#include "config.h"

namespace http {

inline std::string public_config_json(const platform::Config& config, const char* http_backend) {
  nlohmann::json body{
      {"app", "AIPlatform"},
      {"version", AIP_VERSION},
      {"api", "openai-compatible"},
      {"http", http_backend},
      {"provider", config.provider},
      {"model", config.model},
      {"storage", config.storage},
      {"auth_required", config.auth_enabled()},
      {"web", config.web_enabled},
      {"max_message_bytes", 32 * 1024},
      {"max_streams", config.max_streams},
  };
  return body.dump();
}

}  // namespace http
