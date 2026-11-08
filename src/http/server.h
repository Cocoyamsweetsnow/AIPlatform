// HTTP 层对外接口：与具体框架解耦（PIMPL）。
//
// 用哪个框架由 CMake 的 AIPLATFORM_HTTP 决定（默认 drogon，回退 httplib）：
//   - src/http/server_drogon.cpp   ← 主线，见 docs/adr/0001-http-framework.md
//   - src/http/server_httplib.cpp  ← 回退实现，API 契约完全一致
// 业务层（llm/ chat/）不感知 HTTP 框架，两个实现都用同一套 ChatService。
#pragma once

#include <cstddef>
#include <memory>

#include "auth/auth_service.h"
#include "chat/chat_service.h"
#include "chat/idempotency.h"
#include "chat/session_store.h"
#include "config.h"
#include "quota/quota.h"
#include "usage/usage.h"

namespace http {

class ApiServer {
 public:
  ApiServer(const platform::Config& config, chat::ChatService& service,
            chat::SessionStore& store, auth::AuthService* auth = nullptr,
            quota::QuotaService* quota = nullptr, usage::UsageSink* usage = nullptr,
            chat::IdempotencyStore* idempotency = nullptr);
  ~ApiServer();

  ApiServer(const ApiServer&) = delete;
  ApiServer& operator=(const ApiServer&) = delete;

  /// 阻塞运行，直到收到 SIGINT/SIGTERM 或 stop()。
  /// 收到信号后先停止接收新的对话请求，再等待在途流收尾（最长 shutdown_drain_s）。
  bool run();
  void stop();

  bool is_running() const;

  /// 当前在途的 SSE 流数量（/healthz 用来判断是否打满）。
  std::size_t active_streams() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace http
