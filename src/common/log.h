// 极简线程安全日志。
//
// 输出两种格式，由 AIP_LOG_FORMAT 选择：
//   - text（默认）：`2026-01-01T00:00:00.000Z INFO  message`
//   - json        ：`{"ts":"...","level":"info","msg":"message"}`
//
// JSON 事件日志（带 request_id 等字段）由 common/structured_log.h 输出，
// 无论基础格式是什么都是单行 JSON，便于 Loki/ELK 采集。
#pragma once

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <string_view>

namespace platform {

enum class LogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };
enum class LogFormat { Text, Json };

inline LogLevel& global_log_level() {
  static LogLevel level = LogLevel::Info;
  return level;
}

inline LogFormat& global_log_format() {
  static LogFormat format = LogFormat::Text;
  return format;
}

inline void set_log_level(LogLevel l) { global_log_level() = l; }
inline void set_log_format(LogFormat f) { global_log_format() = f; }

namespace detail {

inline const char* level_name(LogLevel l) {
  switch (l) {
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
  }
  return "?";
}

inline std::mutex& log_mutex() {
  static std::mutex m;
  return m;
}

inline std::string timestamp_iso8601() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t secs = std::chrono::system_clock::to_time_t(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) % 1000;

  std::tm tm_buf{};
#if defined(_WIN32)
  gmtime_s(&tm_buf, &secs);
#else
  gmtime_r(&secs, &tm_buf);
#endif

  char base[32];
  std::strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm_buf);
  char ts[48];
  std::snprintf(ts, sizeof(ts), "%s.%03dZ", base, static_cast<int>(ms.count()));
  return ts;
}

inline std::string json_escape(std::string_view input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (const unsigned char c : input) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[7];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  return out;
}

/// 写一行已经格式化好的内容（JSON 事件日志用）。
inline void write_line(LogLevel level, std::string_view line) {
  if (static_cast<int>(level) < static_cast<int>(global_log_level())) return;
  std::lock_guard<std::mutex> lock(log_mutex());
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

inline void write_log(LogLevel level, std::string_view msg) {
  if (static_cast<int>(level) < static_cast<int>(global_log_level())) return;

  if (global_log_format() == LogFormat::Json) {
    std::string line;
    line.reserve(msg.size() + 80);
    line += "{\"ts\":\"";
    line += timestamp_iso8601();
    line += "\",\"level\":\"";
    line += level_name(level);
    line += "\",\"msg\":\"";
    line += json_escape(msg);
    line += "\"}";
    write_line(level, line);
    return;
  }

  const std::string ts = timestamp_iso8601();
  std::string line;
  line.reserve(ts.size() + msg.size() + 16);
  line += ts;
  line += ' ';
  // 固定宽度，保持 text 模式可读。
  const char* name = level_name(level);
  for (std::size_t i = std::strlen(name); i < 5; ++i) line += ' ';
  line += name;
  line += ' ';
  line += msg;
  write_line(level, line);
}

}  // namespace detail

inline void log_debug(std::string_view m) { detail::write_log(LogLevel::Debug, m); }
inline void log_info(std::string_view m) { detail::write_log(LogLevel::Info, m); }
inline void log_warn(std::string_view m) { detail::write_log(LogLevel::Warn, m); }
inline void log_error(std::string_view m) { detail::write_log(LogLevel::Error, m); }

}  // namespace platform
