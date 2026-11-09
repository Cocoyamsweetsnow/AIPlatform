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
  // 账号体系需要同时满足：编译时带 AUTH、运行时用 PostgreSQL、且显式配置了
  // JWT secret（否则进程重启后所有 token 会因随机 secret 失效，UI 不应引导用户注册）。
#if defined(AIP_WITH_AUTH) && defined(AIP_WITH_POSTGRES)
  const bool user_accounts =
      config.postgres_enabled() && config.auth_enabled() && !config.jwt_secret.empty();
#else
  const bool user_accounts = false;
#endif

  nlohmann::json body{
      {"app", "AIPlatform"},
      {"version", AIP_VERSION},
      {"api", "openai-compatible"},
      {"http", http_backend},
      {"provider", config.provider},
      {"model", config.model},
      {"models", config.models},
      {"storage", config.storage},
      {"auth_required", config.auth_enabled()},
      {"auth",
       {{"user_accounts", user_accounts},
        {"password_login", user_accounts},
        {"registration", user_accounts},
        {"static_token", !config.api_token.empty()},
        {"email_verification_required", config.email_verification_required},
        {"cookie_mode", config.auth_cookie_mode},
        {"cookie_secure", config.auth_cookie_secure},
        {"password_reset", true},
        {"refresh_rotation", true},
        {"access_ttl_s", config.jwt_ttl_s},
        {"refresh_ttl_s", config.jwt_refresh_ttl_s}}},
      {"web", config.web_enabled},
      {"max_message_bytes", 32 * 1024},
      {"max_streams", config.max_streams},
  };
  return body.dump();
}

}  // namespace http
