// 会话 JSON 序列化（两个 HTTP 后端共用）。
#pragma once

#include <nlohmann/json.hpp>

#include "chat/session_store.h"

namespace http {

inline nlohmann::json session_json(const chat::SessionInfo& session) {
  return nlohmann::json{{"id", session.id},
                        {"title", session.title},
                        {"model", session.model},
                        {"created_at", session.created_at},
                        {"updated_at", session.updated_at},
                        {"message_count", session.message_count}};
}

}  // namespace http
