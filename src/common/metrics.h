// 进程内指标注册表：以 Prometheus 文本格式暴露 /metrics。
//
// 设计取舍：
//   - 不引入 prometheus-cpp / OpenTelemetry，避免多一个重依赖；
//   - 指标名、标签由本文件显式定义，防止调用方随手打出高基数标签；
//   - 所有写操作走一把全局锁。当前只在「请求结束 / 流结束 / 上游尝试」
//     时写一次，QPS 上万时锁竞争仍可忽略；如果将来要按 token 打点再换无锁实现。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace platform::metrics {

// HTTP 层
void inc_http_request(std::string_view method, std::string_view route, int status);
void observe_http_duration(std::string_view method, std::string_view route, double seconds);

// 对话层
void inc_chat_request(bool stream, std::string_view outcome);
void observe_chat_latency(bool stream, double seconds);
void observe_ttft(std::string_view model, double seconds);
void inc_tokens(std::string_view model, std::string_view type, long long count);
void inc_quota_rejection(std::string_view type);

// 上游
void inc_upstream_request(std::string_view model, std::string_view outcome);
void observe_upstream_duration(std::string_view model, double seconds);
void inc_key_cooldown();
void set_upstream_keys_available(std::size_t value);

// 鉴权
void inc_auth_failure(std::string_view reason);

// 运行时状态
void set_active_streams(std::size_t value);
void set_ready(bool value);
void set_build_info(std::string_view version, std::string_view http_backend,
                    std::string_view provider);

/// 渲染 Prometheus text exposition（version 0.0.4）。
std::string render_prometheus();

/// 仅测试使用：清空所有指标。
void reset_for_tests();

}  // namespace platform::metrics
