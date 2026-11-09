#include "common/metrics.h"

#include <algorithm>
#include <iomanip>
#include <initializer_list>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace platform::metrics {
namespace {

struct HistogramValue {
  std::vector<std::uint64_t> buckets;
  double sum = 0.0;
  std::uint64_t count = 0;
};

struct CounterFamily {
  std::string help;
  std::map<std::string, double> values;
};

struct GaugeFamily {
  std::string help;
  std::map<std::string, double> values;
};

struct HistogramFamily {
  std::string help;
  std::vector<double> bounds;
  std::map<std::string, HistogramValue> values;

  explicit HistogramFamily(std::string help_text, std::vector<double> bucket_bounds)
      : help(std::move(help_text)), bounds(std::move(bucket_bounds)) {}
};

struct Registry {
  CounterFamily http_requests{
      "HTTP requests by method, route and status code.", {}};
  HistogramFamily http_duration{
      "HTTP request duration in seconds.", {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5,
                                            1.0, 2.0, 5.0, 10.0, 30.0, 60.0}};
  CounterFamily chat_requests{
      "Chat requests by stream mode and terminal outcome.", {}};
  HistogramFamily chat_latency{
      "Chat request latency in seconds (stream: full stream duration).",
      {0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 120.0, 300.0}};
  HistogramFamily chat_ttft{
      "Time to first generated token in seconds.",
      {0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 3.0, 5.0, 8.0, 13.0, 21.0, 30.0}};
  CounterFamily chat_tokens{"Token counts reported by the upstream or estimated.", {}};
  CounterFamily quota_rejections{"Quota/rate-limit rejections by error type.", {}};
  CounterFamily upstream_requests{"Upstream model attempts by outcome.", {}};
  HistogramFamily upstream_duration{
      "Upstream attempt duration in seconds.",
      {0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 120.0}};
  CounterFamily key_cooldowns{"Upstream API key cooldown transitions.", {}};
  GaugeFamily upstream_keys_available{"Upstream API keys currently selectable.", {}};
  CounterFamily auth_failures{"Authentication/authorization failures by reason.", {}};
  GaugeFamily active_streams{"Currently active SSE streams.", {}};
  GaugeFamily ready{"1 when /readyz would return 200, else 0.", {}};
  GaugeFamily build_info{"Build and runtime information.", {}};
};

Registry& registry() {
  static Registry r;
  return r;
}

std::mutex& registry_mutex() {
  static std::mutex m;
  return m;
}

std::string escape_label(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

std::string make_key(
    std::initializer_list<std::pair<std::string_view, std::string_view>> labels) {
  std::string key;
  bool first = true;
  for (const auto& [name, value] : labels) {
    if (!first) key += ',';
    first = false;
    key += std::string(name) + "=\"" + escape_label(value) + "\"";
  }
  return key;
}

std::string status_text(int status) { return std::to_string(status); }

template <typename Family>
void increment(Family& family, const std::string& key, double value = 1.0) {
  family.values[key] += value;
}

void observe(HistogramFamily& family, const std::string& key, double value) {
  HistogramValue& entry = family.values[key];
  if (entry.buckets.empty()) entry.buckets.assign(family.bounds.size(), 0);
  for (std::size_t i = 0; i < family.bounds.size(); ++i) {
    if (value <= family.bounds[i]) ++entry.buckets[i];
  }
  entry.sum += value;
  ++entry.count;
}

std::string format_number(double value) {
  std::ostringstream out;
  out << std::setprecision(12) << value;
  return out.str();
}

std::string with_le(const std::string& key, double bound) {
  const std::string le = "le=\"" + format_number(bound) + "\"";
  if (key.empty()) return "{" + le + "}";
  return "{" + key + "," + le + "}";
}

std::string with_inf(const std::string& key) {
  const std::string le = "le=\"+Inf\"";
  if (key.empty()) return "{" + le + "}";
  return "{" + key + "," + le + "}";
}

std::string braces(const std::string& key) {
  if (key.empty()) return {};
  return "{" + key + "}";
}

void render_counter(std::ostringstream& out, const std::string& name,
                    const CounterFamily& family) {
  out << "# HELP " << name << " " << family.help << "\n";
  out << "# TYPE " << name << " counter\n";
  for (const auto& [key, value] : family.values) {
    out << name << braces(key) << " " << format_number(value) << "\n";
  }
}

void render_gauge(std::ostringstream& out, const std::string& name,
                  const GaugeFamily& family) {
  out << "# HELP " << name << " " << family.help << "\n";
  out << "# TYPE " << name << " gauge\n";
  for (const auto& [key, value] : family.values) {
    out << name << braces(key) << " " << format_number(value) << "\n";
  }
}

void render_histogram(std::ostringstream& out, const std::string& name,
                      const HistogramFamily& family) {
  out << "# HELP " << name << " " << family.help << "\n";
  out << "# TYPE " << name << " histogram\n";
  for (const auto& [key, entry] : family.values) {
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < family.bounds.size(); ++i) {
      cumulative = i < entry.buckets.size() ? entry.buckets[i] : cumulative;
      out << name << "_bucket" << with_le(key, family.bounds[i]) << " " << cumulative
          << "\n";
    }
    out << name << "_bucket" << with_inf(key) << " " << entry.count << "\n";
    out << name << "_sum" << braces(key) << " " << format_number(entry.sum) << "\n";
    out << name << "_count" << braces(key) << " " << entry.count << "\n";
  }
}

}  // namespace

void inc_http_request(std::string_view method, std::string_view route, int status) {
  std::lock_guard<std::mutex> lock(registry_mutex());
  increment(registry().http_requests,
            make_key({{"method", method}, {"route", route}, {"status", status_text(status)}}));
}

void observe_http_duration(std::string_view method, std::string_view route, double seconds) {
  std::lock_guard<std::mutex> lock(registry_mutex());
  observe(registry().http_duration,
          make_key({{"method", method}, {"route", route}}), seconds);
}

void inc_chat_request(bool stream, std::string_view outcome) {
  std::lock_guard<std::mutex> lock(registry_mutex());
  increment(registry().chat_requests,
            make_key({{"stream", stream ? "true" : "false"}, {"outcome", outcome}}));
}

void observe_chat_latency(bool stream, double seconds) {
  std::lock_guard<std::mutex> lock(registry_mutex());
  observe(registry().chat_latency, make_key({{"stream", stream ? "true" : "false"}}),
          seconds);
}

void observe_ttft(std::string_view model, double seconds) {
  std::lock_guard<std::mutex> lock(registry_mutex());
  observe(registry().chat_ttft, make_key({{"model", model}}), seconds);
}

void inc_tokens(std::string_view model, std::string_view type, long long count) {
  if (count < 0) return;
  std::lock_guard<std::mutex> lock(registry_mutex());
  increment(registry().chat_tokens, make_key({{"model", model}, {"type", type}}),
            static_cast<double>(count));
}

void inc_quota_rejection(std::string_view type) {
  std::lock_guard<std::mutex> lock(registry_mutex());
  increment(registry().quota_rejections, make_key({{"type", type}}));
}

void inc_upstream_request(std::string_view model, std::string_view outcome) {
  std::lock_guard<std::mutex> lock(registry_mutex());
  increment(registry().upstream_requests,
            make_key({{"model", model}, {"outcome", outcome}}));
}

void observe_upstream_duration(std::string_view model, double seconds) {
  std::lock_guard<std::mutex> lock(registry_mutex());
  observe(registry().upstream_duration, make_key({{"model", model}}), seconds);
}

void inc_key_cooldown() {
  std::lock_guard<std::mutex> lock(registry_mutex());
  increment(registry().key_cooldowns, {});
}

void set_upstream_keys_available(std::size_t value) {
  std::lock_guard<std::mutex> lock(registry_mutex());
  registry().upstream_keys_available.values[""] = static_cast<double>(value);
}

void inc_auth_failure(std::string_view reason) {
  std::lock_guard<std::mutex> lock(registry_mutex());
  increment(registry().auth_failures, make_key({{"reason", reason}}));
}

void set_active_streams(std::size_t value) {
  std::lock_guard<std::mutex> lock(registry_mutex());
  registry().active_streams.values[""] = static_cast<double>(value);
}

void set_ready(bool value) {
  std::lock_guard<std::mutex> lock(registry_mutex());
  registry().ready.values[""] = value ? 1.0 : 0.0;
}

void set_build_info(std::string_view version, std::string_view http_backend,
                    std::string_view provider) {
  std::lock_guard<std::mutex> lock(registry_mutex());
  registry().build_info.values[make_key(
      {{"version", version}, {"http", http_backend}, {"provider", provider}})] = 1.0;
}

std::string render_prometheus() {
  std::lock_guard<std::mutex> lock(registry_mutex());
  const Registry& r = registry();
  std::ostringstream out;
  render_counter(out, "aip_http_requests_total", r.http_requests);
  render_histogram(out, "aip_http_request_duration_seconds", r.http_duration);
  render_counter(out, "aip_chat_requests_total", r.chat_requests);
  render_histogram(out, "aip_chat_latency_seconds", r.chat_latency);
  render_histogram(out, "aip_chat_ttft_seconds", r.chat_ttft);
  render_counter(out, "aip_chat_tokens_total", r.chat_tokens);
  render_counter(out, "aip_quota_rejections_total", r.quota_rejections);
  render_counter(out, "aip_upstream_requests_total", r.upstream_requests);
  render_histogram(out, "aip_upstream_request_duration_seconds", r.upstream_duration);
  render_counter(out, "aip_upstream_key_cooldowns_total", r.key_cooldowns);
  render_gauge(out, "aip_upstream_keys_available", r.upstream_keys_available);
  render_counter(out, "aip_auth_failures_total", r.auth_failures);
  render_gauge(out, "aip_active_streams", r.active_streams);
  render_gauge(out, "aip_ready", r.ready);
  render_gauge(out, "aip_build_info", r.build_info);
  return out.str();
}

void reset_for_tests() {
  std::lock_guard<std::mutex> lock(registry_mutex());
  registry() = Registry{};
}

}  // namespace platform::metrics
