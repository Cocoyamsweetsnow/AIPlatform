#include "http/server.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include "common/log.h"
#include "util/id.h"

namespace http {
namespace {

using json = nlohmann::json;

std::string json_error(const std::string& type, const std::string& message) {
  return json{{"error", {{"type", type}, {"message", message}}}}.dump();
}

void send_error(httplib::Response& res, int status, const std::string& type,
                const std::string& message) {
  res.status = status;
  res.set_content(json_error(type, message), "application/json; charset=utf-8");
}

bool is_blank(const std::string& s) {
  return s.find_first_not_of(" \t\r\n") == std::string::npos;
}

std::string sse_pack(const std::string& payload, const std::string& event = {}) {
  std::string out;
  if (!event.empty()) out += "event: " + event + "\n";
  out += "data: " + payload + "\n\n";
  return out;
}

json chunk_base(const std::string& id, const std::string& model, long long created) {
  return json{{"id", id},
              {"object", "chat.completion.chunk"},
              {"created", created},
              {"model", model}};
}

void add_cors(httplib::Response& res) {
  res.set_header("Access-Control-Allow-Origin", "*");
  res.set_header("Access-Control-Allow-Headers", "Authorization, Content-Type");
  res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  res.set_header("Access-Control-Expose-Headers", "X-Session-Id");
}

// 一次流式响应的共享状态。用 shared_ptr 是因为 httplib 的 provider 是
// std::function（要求可拷贝），不能直接捕获 unique_ptr。
struct SseState {
  std::shared_ptr<llm::Stream> stream;
  std::string id;
  std::string model;
  long long created = 0;
  bool sent_role = false;
};

}  // namespace

ApiServer::ApiServer(const platform::Config& config, chat::ChatService& service,
                     chat::SessionStore& store)
    : config_(config), service_(service), store_(store) {
  setup_routes();
}

ApiServer::~ApiServer() = default;

bool ApiServer::run() {
  platform::log_info("HTTP 服务启动: http://" + config_.host + ":" +
                     std::to_string(config_.port));
  if (config_.auth_enabled()) {
    platform::log_info("已启用 Bearer Token 鉴权（AIP_TOKEN）");
  } else {
    platform::log_warn("未设置 AIP_TOKEN：接口无鉴权，请勿直接暴露到公网");
  }
  return server_.listen(config_.host, config_.port);
}

void ApiServer::stop() { server_.stop(); }

bool ApiServer::is_running() const { return server_.is_running(); }

void ApiServer::setup_routes() {
  server_.set_payload_max_length(1024 * 1024);
  server_.set_read_timeout(30, 0);
  server_.set_write_timeout(600, 0);

  server_.set_pre_routing_handler(
      [this](const httplib::Request& req, httplib::Response& res) {
        add_cors(res);
        if (req.method == "OPTIONS") {
          res.status = 204;
          return httplib::Server::HandlerResponse::Handled;
        }
        if (!config_.auth_enabled() || req.path == "/healthz") {
          return httplib::Server::HandlerResponse::Unhandled;
        }
        const std::string expected = "Bearer " + config_.api_token;
        if (req.get_header_value("Authorization") == expected) {
          return httplib::Server::HandlerResponse::Unhandled;
        }
        send_error(res, 401, "unauthorized", "缺少或错误的 Authorization: Bearer <token>");
        return httplib::Server::HandlerResponse::Handled;
      });

  server_.set_post_routing_handler([](const httplib::Request& req,
                                      const httplib::Response& res) {
    platform::log_info(req.method + " " + req.path + " -> " + std::to_string(res.status));
  });

  server_.Options(R"(.*)", [](const httplib::Request&, httplib::Response& res) {
    res.status = 204;
  });

  server_.Get("/healthz", [this](const httplib::Request&, httplib::Response& res) {
    res.set_content(json{{"status", "ok"},
                         {"sessions", store_.session_count()},
                         {"model", config_.model},
                         {"provider", config_.provider}}
                        .dump(),
                    "application/json; charset=utf-8");
  });

  server_.Post("/v1/chat/completions",
               [this](const httplib::Request& req, httplib::Response& res) {
                 handle_chat(req, res);
               });

  server_.Get(R"(/v1/sessions/([A-Za-z0-9_\-]+)/messages)",
              [this](const httplib::Request& req, httplib::Response& res) {
                handle_history(req, res);
              });

  server_.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
    if (res.status == 404) {
      send_error(res, 404, "not_found", "接口不存在: " + req.path);
    }
  });

  server_.set_exception_handler(
      [](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        std::string what = "unknown";
        try {
          if (ep) std::rethrow_exception(ep);
        } catch (const std::exception& e) {
          what = e.what();
        }
        platform::log_error("未捕获异常: " + what);
        send_error(res, 500, "internal_error", "服务内部错误");
      });
}

void ApiServer::handle_chat(const httplib::Request& req, httplib::Response& res) {
  json body;
  try {
    body = json::parse(req.body);
  } catch (const std::exception&) {
    return send_error(res, 400, "invalid_json", "请求体不是合法 JSON");
  }
  if (!body.is_object()) {
    return send_error(res, 400, "invalid_json", "请求体必须是 JSON 对象");
  }

  std::string user_text;
  if (body.contains("message") && body["message"].is_string()) {
    user_text = body["message"].get<std::string>();
  } else if (body.contains("messages") && body["messages"].is_array()) {
    // 兼容 OpenAI 原生格式：取最后一条 user 作为本轮输入。
    for (const auto& m : body["messages"]) {
      if (m.is_object() && m.value("role", "") == "user" && m.contains("content") &&
          m["content"].is_string()) {
        user_text = m["content"].get<std::string>();
      }
    }
  }
  if (is_blank(user_text)) {
    return send_error(res, 400, "invalid_request",
                      "缺少 message 字段（或 messages 中最后一条 user 消息为空）");
  }
  if (user_text.size() > 32 * 1024) {
    return send_error(res, 413, "payload_too_large", "单条消息超过 32KB 限制");
  }

  const std::string session_id = body.value("session_id", "");
  const std::string model = body.value("model", "");
  const bool stream = body.value("stream", true);

  chat::ChatService::Turn turn;
  try {
    turn = service_.start(session_id, user_text, model);
  } catch (const std::exception& e) {
    platform::log_error(std::string("启动对话失败: ") + e.what());
    return send_error(res, 502, "upstream_error", "无法连接模型服务");
  }

  const std::string model_name = model.empty() ? config_.model : model;
  res.set_header("X-Session-Id", turn.session_id);

  if (!stream) {
    std::string answer;
    std::string error;
    std::string finish_reason = "stop";
    int prompt_tokens = -1;
    int completion_tokens = -1;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(config_.request_timeout_s);
    while (std::chrono::steady_clock::now() < deadline) {
      llm::Event ev;
      const llm::Pull pull = turn.stream->next(ev, std::chrono::seconds(5));
      if (pull == llm::Pull::Timeout) continue;
      if (pull == llm::Pull::Cancelled) break;
      if (pull == llm::Pull::End) break;
      if (ev.kind == llm::EventKind::Delta) {
        answer += ev.text;
      } else if (ev.kind == llm::EventKind::Done) {
        finish_reason = ev.finish_reason.empty() ? "stop" : ev.finish_reason;
        prompt_tokens = ev.prompt_tokens;
        completion_tokens = ev.completion_tokens;
        break;
      } else {
        error = ev.text;
        break;
      }
    }

    if (!error.empty()) {
      return send_error(res, 502, "upstream_error", error);
    }
    if (answer.empty()) {
      return send_error(res, 504, "timeout", "模型未在超时时间内返回内容");
    }

    json out{{"id", util::random_id("chatcmpl-")},
             {"object", "chat.completion"},
             {"created", util::now_ms() / 1000},
             {"model", model_name},
             {"session_id", turn.session_id},
             {"choices",
              json::array({{{"index", 0},
                            {"message", {{"role", "assistant"}, {"content", answer}}},
                            {"finish_reason", finish_reason}}})}};
    if (prompt_tokens >= 0) {
      out["usage"] = {{"prompt_tokens", prompt_tokens},
                      {"completion_tokens", completion_tokens},
                      {"total_tokens", prompt_tokens + completion_tokens}};
    }
    res.set_content(out.dump(), "application/json; charset=utf-8");
    return;
  }

  auto state = std::make_shared<SseState>();
  state->stream = turn.stream;
  state->id = util::random_id("chatcmpl-");
  state->model = model_name;
  state->created = util::now_ms() / 1000;

  res.set_header("Cache-Control", "no-cache, no-transform");
  res.set_header("X-Accel-Buffering", "no");  // 关闭 Nginx 缓冲，否则前端会一次性收到全部内容

  res.set_chunked_content_provider(
      "text/event-stream",
      [state](std::size_t /*offset*/, httplib::DataSink& sink) -> bool {
        llm::Event ev;
        const llm::Pull pull = state->stream->next(ev, std::chrono::seconds(15));

        if (pull == llm::Pull::Timeout) {
          const std::string ping = ": keep-alive\n\n";
          return sink.write(ping.data(), ping.size());
        }
        if (pull == llm::Pull::Cancelled) {
          sink.done();
          return true;
        }
        if (pull == llm::Pull::End) {
          const std::string tail = sse_pack("[DONE]");
          const bool ok = sink.write(tail.data(), tail.size());
          sink.done();
          return ok;
        }

        std::string frame;
        bool terminal = false;

        switch (ev.kind) {
          case llm::EventKind::Delta: {
            if (!state->sent_role) {
              state->sent_role = true;
              json role_chunk = chunk_base(state->id, state->model, state->created);
              role_chunk["choices"] = json::array(
                  {{{"index", 0}, {"delta", {{"role", "assistant"}, {"content", ""}}},
                    {"finish_reason", nullptr}}});
              frame += sse_pack(role_chunk.dump());
            }
            json chunk = chunk_base(state->id, state->model, state->created);
            chunk["choices"] = json::array(
                {{{"index", 0}, {"delta", {{"content", ev.text}}}, {"finish_reason", nullptr}}});
            frame += sse_pack(chunk.dump());
            break;
          }
          case llm::EventKind::Done: {
            json chunk = chunk_base(state->id, state->model, state->created);
            chunk["choices"] = json::array(
                {{{"index", 0}, {"delta", json::object()},
                  {"finish_reason", ev.finish_reason.empty() ? "stop" : ev.finish_reason}}});
            if (ev.prompt_tokens >= 0) {
              chunk["usage"] = {{"prompt_tokens", ev.prompt_tokens},
                                {"completion_tokens", ev.completion_tokens},
                                {"total_tokens", ev.prompt_tokens + ev.completion_tokens}};
            }
            frame += sse_pack(chunk.dump());
            frame += sse_pack("[DONE]");
            terminal = true;
            break;
          }
          case llm::EventKind::Error: {
            frame += sse_pack(json{{"error", {{"type", "upstream_error"}, {"message", ev.text}}}}
                                  .dump(),
                              "error");
            frame += sse_pack("[DONE]");
            terminal = true;
            break;
          }
        }

        const bool ok = sink.write(frame.data(), frame.size());
        if (terminal) sink.done();
        return ok;
      },
      [state](bool /*success*/) {
        // 响应结束（含客户端断开）时释放上游连接。
        state->stream->cancel();
      });
}

void ApiServer::handle_history(const httplib::Request& req, httplib::Response& res) {
  const std::string session_id = req.matches[1];
  const auto messages = store_.history(session_id, 100);

  json list = json::array();
  for (const auto& m : messages) {
    list.push_back({{"role", m.role}, {"content", m.content}});
  }
  res.set_content(json{{"session_id", session_id}, {"messages", list}}.dump(),
                  "application/json; charset=utf-8");
}

}  // namespace http
