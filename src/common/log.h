// 极简线程安全日志。
// 骨架阶段故意不引入 spdlog：先让项目能编译、能跑；
// 到 M6（可观测性）再换成 spdlog + OpenTelemetry，接口保持兼容即可。
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

inline LogLevel& global_log_level() {
  static LogLevel level = LogLevel::Info;
  return level;
}

inline void set_log_level(LogLevel l) { global_log_level() = l; }

namespace detail {

inline const char* level_name(LogLevel l) {
  switch (l) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
  }
  return "?";
}

inline std::mutex& log_mutex() {
  static std::mutex m;
  return m;
}

inline void write_log(LogLevel level, std::string_view msg) {
  if (static_cast<int>(level) < static_cast<int>(global_log_level())) return;

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

  std::lock_guard<std::mutex> lock(log_mutex());
  std::fprintf(stdout, "%s %-5s %.*s\n", ts, level_name(level),
               static_cast<int>(msg.size()), msg.data());
  std::fflush(stdout);
}

}  // namespace detail

inline void log_debug(std::string_view m) { detail::write_log(LogLevel::Debug, m); }
inline void log_info(std::string_view m) { detail::write_log(LogLevel::Info, m); }
inline void log_warn(std::string_view m) { detail::write_log(LogLevel::Warn, m); }
inline void log_error(std::string_view m) { detail::write_log(LogLevel::Error, m); }

}  // namespace platform
