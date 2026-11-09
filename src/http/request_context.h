// HTTP 层的 request_id 与指标路由归一化工具（Drogon / httplib 共用）。
#pragma once

#include <cctype>
#include <string>
#include <string_view>

#include "util/id.h"

namespace http {

/// 客户端可以传 X-Request-Id，但只接受安全字符，避免日志注入/超长。
inline bool valid_request_id(std::string_view id) {
  if (id.empty() || id.size() > 128) return false;
  for (const unsigned char c : id) {
    if (std::isalnum(c) != 0) continue;
    if (c == '-' || c == '_' || c == '.') continue;
    return false;
  }
  return true;
}

inline std::string ensure_request_id(const std::string& candidate) {
  return valid_request_id(candidate) ? candidate : util::random_id("req_");
}

/// 把任意路径归一到低基数标签，避免 /v1/sessions/<随机 ID> 把 Prometheus 打爆。
inline std::string route_label(std::string_view path) {
  if (path == "/healthz") return "/healthz";
  if (path == "/readyz") return "/readyz";
  if (path == "/metrics") return "/metrics";
  if (path == "/v1/config") return "/v1/config";
  if (path == "/v1/models") return "/v1/models";
  if (path.rfind("/v1/models/", 0) == 0) return "/v1/models/{id}";
  if (path == "/v1/chat/completions") return "/v1/chat/completions";
  if (path == "/v1/sessions") return "/v1/sessions";
  if (path.rfind("/v1/sessions/", 0) == 0) {
    if (path.size() > 9 && path.substr(path.size() - 9) == "/messages") {
      return "/v1/sessions/{id}/messages";
    }
    return "/v1/sessions/{id}";
  }
  if (path == "/v1/auth/register") return "/v1/auth/register";
  if (path == "/v1/auth/login") return "/v1/auth/login";
  if (path == "/v1/auth/refresh") return "/v1/auth/refresh";
  if (path == "/v1/auth/logout") return "/v1/auth/logout";
  if (path == "/v1/auth/me") return "/v1/auth/me";
  if (path == "/v1/auth/verify-email") return "/v1/auth/verify-email";
  if (path == "/v1/auth/password/forgot") return "/v1/auth/password/forgot";
  if (path == "/v1/auth/password/reset") return "/v1/auth/password/reset";
  if (path == "/v1/auth/password/change") return "/v1/auth/password/change";
  if (path == "/v1/auth/api-keys") return "/v1/auth/api-keys";
  if (path.rfind("/v1/auth/api-keys/", 0) == 0) return "/v1/auth/api-keys/{id}";
  if (path == "/") return "/";
  if (path == "/index.html") return "/index.html";
  if (path == "/favicon.svg") return "/favicon.svg";
  if (path.rfind("/assets/", 0) == 0) return "/assets/*";
  return "other";
}

}  // namespace http
