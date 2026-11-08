// 用量流水：记录每次完成的请求，供计费/配额/报表使用。
#pragma once

#include <cstdint>
#include <string>

namespace usage {

struct UsageEvent {
  std::int64_t user_id = 0;       // 0 = 匿名/静态管理员；入库为 NULL
  std::string session_id;
  std::string request_id;         // M4 幂等键；当前为空
  std::string model;
  int prompt_tokens = -1;         // -1 = 上游没返回，用估算值
  int completion_tokens = -1;
  int latency_ms = 0;
  int ttft_ms = -1;               // 首 token 延迟；-1 = 没拿到 token
};

struct UsageSummary {
  long long requests = 0;
  long long prompt_tokens = 0;
  long long completion_tokens = 0;

  long long total_tokens() const { return prompt_tokens + completion_tokens; }
};

class UsageSink {
 public:
  virtual ~UsageSink() = default;

  /// 写一条流水；调用方负责捕获存储异常，不能影响已经发给客户端的流。
  virtual void record(const UsageEvent& event) = 0;

  /// 当天（数据库时区 date_trunc('day', now())）的用量。
  virtual UsageSummary summary_today(std::int64_t user_id) = 0;
};

}  // namespace usage
