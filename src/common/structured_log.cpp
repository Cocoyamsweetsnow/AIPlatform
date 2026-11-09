#include "common/structured_log.h"

#include <utility>

namespace platform {

void log_event(LogLevel level, std::string_view event, const nlohmann::json& fields) {
  nlohmann::json line = fields.is_object() ? fields : nlohmann::json::object();
  line["ts"] = detail::timestamp_iso8601();
  line["level"] = detail::level_name(level);
  line["event"] = std::string(event);
  detail::write_line(level, line.dump());
}

}  // namespace platform
