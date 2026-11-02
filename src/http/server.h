#pragma once

#include <string>

#include "chat/chat_service.h"
#include "chat/session_store.h"
#include "config.h"
#include "httplib.h"

namespace http {

// HTTP 层：解析请求、鉴权、SSE 编码、错误码。业务全在 ChatService。
class ApiServer {
 public:
  ApiServer(const platform::Config& config, chat::ChatService& service,
            chat::SessionStore& store);
  ~ApiServer();

  ApiServer(const ApiServer&) = delete;
  ApiServer& operator=(const ApiServer&) = delete;

  /// 阻塞运行，直到 stop() 被调用。
  bool run();
  void stop();

  bool is_running() const;

 private:
  void setup_routes();
  void handle_chat(const httplib::Request& req, httplib::Response& res);
  void handle_history(const httplib::Request& req, httplib::Response& res);

  const platform::Config& config_;
  chat::ChatService& service_;
  chat::SessionStore& store_;
  httplib::Server server_;
};

}  // namespace http
