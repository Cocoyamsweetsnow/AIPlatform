// 结构化事件日志：一行一个 JSON，字段稳定，便于 Loki/ELK 检索。
//
// 与 platform::log_info 的区别：事件日志带 event 名和业务字段（request_id、
// user_id、session_id、status、ttft_ms...），基础日志只有 msg。
#pragma once

#include <nlohmann/json.hpp>

#include <string_view>

#include "common/log.h"

namespace platform {

/// 输出一行 JSON 事件；fields 必须是 object（否则忽略）。
/// 固定的 ts/level/event 字段会覆盖 fields 里的同名键。
void log_event(LogLevel level, std::string_view event,
               const nlohmann::json& fields = nlohmann::json::object());

inline void log_event_info(std::string_view event,
                           const nlohmann::json& fields = nlohmann::json::object()) {
  log_event(LogLevel::Info, event, fields);
}

inline void log_event_warn(std::string_view event,
                           const nlohmann::json& fields = nlohmann::json::object()) {
  log_event(LogLevel::Warn, event, fields);
}

inline void log_event_error(std::string_view event,
                            const nlohmann::json& fields = nlohmann::json::object()) {
  log_event(LogLevel::Error, event, fields);
}

}  // namespace platform
