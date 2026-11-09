// HTTP 层实现之一：cpp-httplib（回退实现，默认不编译）。
//
// 为什么保留：ADR 0001 把 httplib 定为回退项——Drogon 装不上（客户机房、国产化
// OS）或依赖出问题时，用 -DAIPLATFORM_HTTP=httplib 仍能构建出功能一致的二进制。
//
// 已知限制（与 Drogon 实现的差异，见 docs/adr/0001-http-framework.md）：
//   - 每连接一线程：并发流上限 = CPPHTTPLIB_THREAD_POOL_COUNT（CMake 里设为 64）；
//   - 没有流数量准入控制（线程池本身就是上限），超额请求只会排队；
//   - 退出只停止 accept，不等待在途流（drain 只在 Drogon 实现里做）。

#include "http/server.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "auth/principal.h"
#include "chat/errors.h"
#include "chat/replay_stream.h"
#include "chat/token_estimate.h"
#include "common/log.h"
#include "common/metrics.h"
#include "common/structured_log.h"
#include "http/auth_json.h"
#include "http/chat_request.h"
#include "http/public_config.h"
#include "http/request_context.h"
#include "http/session_json.h"
#include "httplib.h"
#include "util/id.h"
#include "web/assets.h"
#ifdef AIP_WITH_AUTH
#include "auth/api_key.h"
#endif
#ifdef AIP_WITH_POSTGRES
#include "storage/db.h"
#endif

namespace http {
namespace {

namespace metrics = platform::metrics;
using json = nlohmann::json;

// 信号处理里只能调用 async-signal-safe 的东西，所以只存一个裸指针。
std::atomic<httplib::Server*> g_signal_target{nullptr};

void on_signal(int) {
  if (auto* server = g_signal_target.load()) server->stop();
}

std::string json_error(const std::string& type, const std::string& message) {
  return json{{"error", {{"type", type}, {"message", message}}}}.dump();
}

void send_error(httplib::Response& res, int status, const std::string& type,
                const std::string& message) {
  res.status = status;
  res.set_content(json_error(type, message), "application/json; charset=utf-8");
}

void send_quota_error(httplib::Response& res, const quota::QuotaDecision& decision) {
  send_error(res, 429, decision.type, decision.message);
  res.set_header("Retry-After", std::to_string(std::max(1, decision.retry_after_s)));
}

int estimate_request_tokens(const std::string& text, int max_tokens) {
  return chat::estimate_tokens(text) + std::max(0, max_tokens) + 8;
}

int effective_max_tokens(const http::ChatRequestInput& input, int fallback) {
  return input.max_tokens > 0 ? input.max_tokens : fallback;
}

chat::ChatService::StartOptions to_start_options(const http::ChatRequestInput& input) {
  chat::ChatService::StartOptions options;
  options.session_id = input.session_id;
  options.user_text = input.user_text;
  options.messages = input.messages;
  options.model_override = input.model;
  options.request_id = input.request_id;
  options.temperature = input.temperature;
  options.max_tokens = input.max_tokens;
  options.top_p = input.top_p;
  options.stop = input.stop;
  options.has_presence_penalty = input.has_presence_penalty;
  options.presence_penalty = input.presence_penalty;
  options.has_frequency_penalty = input.has_frequency_penalty;
  options.frequency_penalty = input.frequency_penalty;
  options.has_seed = input.has_seed;
  options.seed = input.seed;
  options.response_format = input.response_format;
  options.user = input.user;
  options.include_usage = input.include_usage;
  return options;
}

void send_idempotency_busy(httplib::Response& res) {
  send_error(res, 409, "request_in_progress", "相同 request_id 的请求正在处理中，请稍后重试");
  res.set_header("Retry-After", "1");
}

void send_idempotency_conflict(httplib::Response& res) {
  send_error(res, 409, "idempotency_conflict", "该 request_id 已完成，但原始回复已不可用");
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
  res.set_header("Access-Control-Allow-Headers", "Authorization, Content-Type, X-Request-Id");
  res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  res.set_header("Access-Control-Expose-Headers", "X-Session-Id, X-Request-Id");
}

// 内置 Web UI：资源编译进二进制，只按 URL 精确查表，不接触文件系统。
void send_asset(httplib::Response& res, const web::Asset& asset) {
  res.set_content(std::string(asset.content), std::string(asset.content_type));
  res.set_header("Cache-Control", "no-cache");
  res.set_header("X-Content-Type-Options", "nosniff");
  res.set_header("Referrer-Policy", "no-referrer");
  if (asset.content_type.rfind("text/html", 0) == 0) {
    res.set_header("X-Frame-Options", "DENY");
    res.set_header("Content-Security-Policy",
                   "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
                   "img-src 'self' data:; connect-src 'self'; font-src 'self'; "
                   "object-src 'none'; base-uri 'none'; frame-ancestors 'none'");
  }
}

// 不需要 Bearer Token 的公开路径：探针、前端配置、以及内置静态资源。
bool is_public_path(const platform::Config& config, const std::string& path) {
  if (path == "/healthz" || path == "/readyz" || path == "/metrics" ||
      path == "/v1/config") {
    return true;
  }
  if (path == "/v1/auth/register" || path == "/v1/auth/login" ||
      path == "/v1/auth/refresh") {
    return true;
  }
  return config.web_enabled && web::is_public_asset(path);
}

bool token_equals(const std::string& a, const std::string& b) {
#ifdef AIP_WITH_AUTH
  return auth::secure_equals(a, b);
#else
  return a == b;
#endif
}

// 一次流式响应的共享状态。用 shared_ptr 是因为 httplib 的 provider 是
// std::function（要求可拷贝），不能直接捕获 unique_ptr。
struct SseState {
  std::shared_ptr<llm::Stream> stream;
  std::string id;
  std::string model;
  std::string request_id;
  std::string session_id;
  std::int64_t user_id = 0;
  int prompt_tokens_estimate = 0;
  bool include_usage = true;
  long long created = 0;
  bool sent_role = false;
  bool client_cancelled = false;
  bool saw_error = false;
  std::size_t ascii_bytes = 0;
  std::size_t non_ascii_codepoints = 0;
  int prompt_tokens = -1;
  int completion_tokens = -1;
  std::string finish_reason = "stop";
  std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
  std::optional<std::chrono::steady_clock::time_point> first_token_at;
};

// httplib 每个连接一个线程，因此 request 上下文可以放 thread_local；
// pre_routing 写入，post_routing 统一输出结构化访问日志和 HTTP 指标。
struct ThreadRequestContext {
  std::string request_id;
  std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
  std::int64_t user_id = 0;
  std::string auth_type;
  bool active = false;
};

thread_local ThreadRequestContext t_request;

}  // namespace

struct ApiServer::Impl {
  Impl(const platform::Config& config, chat::ChatService& service, chat::SessionStore& store,
       auth::AuthService* auth, quota::QuotaService* quota_service,
       usage::UsageSink* usage_sink, chat::IdempotencyStore* idempotency)
      : config(config),
        service(service),
        store(store),
        auth(auth),
        quota(quota_service),
        usage(usage_sink),
        idempotency(idempotency) {
    setup_routes();
  }

  void setup_routes();
  auth::Principal principal_from(const httplib::Request& req) const;
  void handle_chat(const httplib::Request& req, httplib::Response& res);
  void handle_history(const httplib::Request& req, httplib::Response& res);
  void handle_sessions_list(const httplib::Request& req, httplib::Response& res);
  void handle_session_update(const httplib::Request& req, httplib::Response& res);
  void handle_static(const httplib::Request& req, httplib::Response& res);
  void handle_auth_register(const httplib::Request& req, httplib::Response& res);
  void handle_auth_login(const httplib::Request& req, httplib::Response& res);
  void handle_auth_refresh(const httplib::Request& req, httplib::Response& res);
  void handle_auth_me(const httplib::Request& req, httplib::Response& res);
  void handle_auth_list_api_keys(const httplib::Request& req, httplib::Response& res);
  void handle_auth_create_api_key(const httplib::Request& req, httplib::Response& res);
  void handle_auth_delete_api_key(const httplib::Request& req, httplib::Response& res);

  const platform::Config& config;
  chat::ChatService& service;
  chat::SessionStore& store;
  auth::AuthService* auth = nullptr;
  quota::QuotaService* quota = nullptr;
  usage::UsageSink* usage = nullptr;
  chat::IdempotencyStore* idempotency = nullptr;
  httplib::Server server;
  std::atomic<std::size_t> active_streams{0};
};

void ApiServer::Impl::setup_routes() {
  server.set_payload_max_length(1024 * 1024);
  server.set_read_timeout(30, 0);
  server.set_write_timeout(600, 0);

  server.set_pre_routing_handler(
      [this](const httplib::Request& req, httplib::Response& res) {
        t_request.request_id = ensure_request_id(req.get_header_value("X-Request-Id"));
        t_request.started = std::chrono::steady_clock::now();
        t_request.active = true;
        res.set_header("X-Request-Id", t_request.request_id);
        add_cors(res);

        const auto deny = [&](int status, const std::string& type,
                              const std::string& message, const char* reason) {
          metrics::inc_auth_failure(reason);
          platform::log_event_info(
              "auth_failure",
              json{{"request_id", t_request.request_id}, {"path", req.path},
                   {"reason", reason}});
          send_error(res, status, type, message);
        };

        if (req.method == "OPTIONS") {
          res.status = 204;
          return httplib::Server::HandlerResponse::Handled;
        }
        if (!config.auth_enabled() || is_public_path(config, req.path)) {
          return httplib::Server::HandlerResponse::Unhandled;
        }
        const auto token = bearer_token(req.get_header_value("Authorization"));
        if (token.has_value() && !config.api_token.empty() &&
            token_equals(*token, config.api_token)) {
          return httplib::Server::HandlerResponse::Unhandled;
        }
#ifdef AIP_WITH_AUTH
        if (token.has_value() && auth != nullptr) {
          try {
            if (auth->authenticate_bearer(*token).has_value()) {
              return httplib::Server::HandlerResponse::Unhandled;
            }
          } catch (const std::exception& e) {
            platform::log_error(std::string("鉴权失败（存储）: ") + e.what());
            deny(503, "storage_unavailable", "鉴权服务暂时不可用，请稍后重试",
                 "storage_unavailable");
            return httplib::Server::HandlerResponse::Handled;
          }
        }
#endif
        deny(401, "unauthorized", "缺少或无效的 Authorization: Bearer <token>",
             token.has_value() ? "invalid_token" : "missing_token");
        return httplib::Server::HandlerResponse::Handled;
      });

  server.set_post_routing_handler([](const httplib::Request& req,
                                     const httplib::Response& res) {
    const std::string& trace_id = t_request.request_id;
    if (!trace_id.empty()) {
      // res 在 post_routing 里是 const，header 已在 pre_routing / 各 handler 设置；
      // 这里只保证 CORS 暴露头存在。
    }
    const double duration_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_request.started)
            .count();
    const std::string route = route_label(req.path);
    metrics::inc_http_request(req.method, route, res.status);
    metrics::observe_http_duration(req.method, route, duration_s);
    platform::log_event_info(
        "http_request",
        json{{"request_id", trace_id},
             {"method", req.method},
             {"route", route},
             {"path", req.path},
             {"status", res.status},
             {"duration_ms", static_cast<long long>(duration_s * 1000.0)},
             {"user_id", t_request.user_id},
             {"auth_type", t_request.auth_type}});
    t_request.active = false;
  });

  server.Options(R"(.*)", [](const httplib::Request&, httplib::Response& res) {
    res.status = 204;
  });

  server.Get("/healthz", [this](const httplib::Request&, httplib::Response& res) {
    std::size_t sessions = 0;
    bool storage_ok = true;
    try {
      sessions = store.session_count();
    } catch (const std::exception& e) {
      storage_ok = false;
      platform::log_warn(std::string("健康检查读取会话数失败: ") + e.what());
    }
    res.set_content(json{{"status", storage_ok ? "ok" : "degraded"},
                         {"sessions", sessions},
                         {"active_streams", active_streams.load()},
                         {"max_streams", config.max_streams},
                         {"model", config.model},
                         {"provider", config.provider},
                         {"storage", config.storage},
                         {"storage_ok", storage_ok},
                         {"web", config.web_enabled},
                         {"http", "httplib"}}
                        .dump(),
                    "application/json; charset=utf-8");
  });

  // 前端启动时读取的公开配置（是否要求 Token、模型名、大小上限等）。
  server.Get("/v1/config", [this](const httplib::Request&, httplib::Response& res) {
    res.set_content(public_config_json(config, "httplib"),
                    "application/json; charset=utf-8");
  });

  server.Get("/readyz", [this](const httplib::Request&, httplib::Response& res) {
    bool storage_ok = true;
    try {
      (void)store.session_count();
    } catch (const std::exception& e) {
      storage_ok = false;
      platform::log_warn(std::string("就绪检查读取会话数失败: ") + e.what());
    }
    metrics::set_ready(storage_ok);
    if (!storage_ok) {
      res.status = 503;
      res.set_content(json{{"status", "not_ready"}, {"storage_ok", false}}.dump(),
                      "application/json; charset=utf-8");
      return;
    }
    res.set_content(json{{"status", "ready"}, {"storage_ok", true}}.dump(),
                    "application/json; charset=utf-8");
  });

  server.Get("/metrics", [this](const httplib::Request& req, httplib::Response& res) {
    if (!config.metrics_enabled) {
      return send_error(res, 404, "not_found", "指标端点已禁用");
    }
    if (!config.metrics_token.empty()) {
      const auto token = bearer_token(req.get_header_value("Authorization"));
      if (!token.has_value() || !token_equals(*token, config.metrics_token)) {
        metrics::inc_auth_failure("metrics_token");
        return send_error(res, 401, "unauthorized", "缺少或无效的指标 Token");
      }
    }
    res.set_header("Cache-Control", "no-store");
    res.set_content(metrics::render_prometheus(),
                    "text/plain; version=0.0.4; charset=utf-8");
  });

#ifdef AIP_WITH_AUTH
  server.Post("/v1/auth/register", [this](const httplib::Request& req, httplib::Response& res) {
    handle_auth_register(req, res);
  });
  server.Post("/v1/auth/login", [this](const httplib::Request& req, httplib::Response& res) {
    handle_auth_login(req, res);
  });
  server.Post("/v1/auth/refresh", [this](const httplib::Request& req, httplib::Response& res) {
    handle_auth_refresh(req, res);
  });
  server.Get("/v1/auth/me", [this](const httplib::Request& req, httplib::Response& res) {
    handle_auth_me(req, res);
  });
  server.Get("/v1/auth/api-keys", [this](const httplib::Request& req, httplib::Response& res) {
    handle_auth_list_api_keys(req, res);
  });
  server.Post("/v1/auth/api-keys", [this](const httplib::Request& req, httplib::Response& res) {
    handle_auth_create_api_key(req, res);
  });
  server.Delete(R"(/v1/auth/api-keys/([0-9]+))",
                [this](const httplib::Request& req, httplib::Response& res) {
                  handle_auth_delete_api_key(req, res);
                });
#endif

  // 内置 Web UI 首页与静态资源。
  server.Get("/", [this](const httplib::Request& req, httplib::Response& res) {
    handle_static(req, res);
  });
  server.Get(R"(/(?:index\.html|favicon\.svg|assets/[A-Za-z0-9._\-]+))",
             [this](const httplib::Request& req, httplib::Response& res) {
               handle_static(req, res);
             });

  server.Get("/v1/models", [this](const httplib::Request&, httplib::Response& res) {
    json data = json::array();
    for (const auto& model : config.models) {
      data.push_back(json{{"id", model},
                          {"object", "model"},
                          {"created", 0},
                          {"owned_by", "aiplatform"}});
    }
    res.set_content(json{{"object", "list"}, {"data", data}}.dump(),
                    "application/json; charset=utf-8");
  });

  server.Get(R"(/v1/models/([A-Za-z0-9._:\-]+))",
             [this](const httplib::Request& req, httplib::Response& res) {
               const std::string requested = req.matches[1];
               for (const auto& model : config.models) {
                 if (model == requested) {
                   res.set_content(json{{"id", model},
                                        {"object", "model"},
                                        {"created", 0},
                                        {"owned_by", "aiplatform"}}
                                       .dump(),
                                   "application/json; charset=utf-8");
                   return;
                 }
               }
               send_error(res, 404, "model_not_found", "模型不存在: " + requested);
             });

  server.Post("/v1/chat/completions",
              [this](const httplib::Request& req, httplib::Response& res) {
                handle_chat(req, res);
              });

  server.Get("/v1/sessions", [this](const httplib::Request& req, httplib::Response& res) {
    handle_sessions_list(req, res);
  });
  server.Patch(R"(/v1/sessions/([A-Za-z0-9_\-]+))",
               [this](const httplib::Request& req, httplib::Response& res) {
                 handle_session_update(req, res);
               });
  server.Delete(R"(/v1/sessions/([A-Za-z0-9_\-]+))",
                [this](const httplib::Request& req, httplib::Response& res) {
                  handle_session_update(req, res);
                });

  server.Get(R"(/v1/sessions/([A-Za-z0-9_\-]+)/messages)",
             [this](const httplib::Request& req, httplib::Response& res) {
               handle_history(req, res);
             });

  // 兜底：以后往 web/ 里加新资源，不注册路由也能访问；未知 GET 仍回 JSON 404。
  server.Get(R"(/.*)", [this](const httplib::Request& req, httplib::Response& res) {
    handle_static(req, res);
  });

  server.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
    // 静态资源处理器会自己写更具体的 404（资源不存在 / Web UI 已禁用），不要覆盖。
    if (res.status == 404 && res.body.empty()) {
      send_error(res, 404, "not_found", "接口不存在: " + req.path);
    }
  });

  server.set_exception_handler(
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

auth::Principal ApiServer::Impl::principal_from(const httplib::Request& req) const {
  auth::Principal principal;
  const auto remember = [&principal] {
    t_request.user_id = principal.user_id;
    t_request.auth_type = principal.auth_type;
    return principal;
  };

  if (!config.auth_enabled()) {
    principal.is_admin = true;
    principal.auth_type = "local";
    return remember();
  }

  const auto token = bearer_token(req.get_header_value("Authorization"));
  if (!token.has_value()) return remember();

  if (!config.api_token.empty() && token_equals(*token, config.api_token)) {
    principal.is_admin = true;
    principal.auth_type = "static";
    return remember();
  }
#ifdef AIP_WITH_AUTH
  if (auth != nullptr) {
    auto authenticated = auth->authenticate_bearer(*token);
    if (authenticated.has_value()) {
      principal = *authenticated;
      return remember();
    }
  }
#endif
  return remember();
}

void ApiServer::Impl::handle_chat(const httplib::Request& req, httplib::Response& res) {
  const std::string trace_id = t_request.request_id;
  const auto chat_started = std::chrono::steady_clock::now();
  json body;
  try {
    body = json::parse(req.body);
  } catch (const std::exception&) {
    return send_error(res, 400, "invalid_json", "请求体不是合法 JSON");
  }

  http::ChatParseError parse_error;
  auto parsed = http::parse_chat_request(body, config, parse_error);
  if (!parsed.has_value()) {
    return send_error(res, parse_error.status, parse_error.type, parse_error.message);
  }
  const http::ChatRequestInput& input = *parsed;
  const std::string model_name = input.model.empty() ? config.model : input.model;
  const auth::Principal principal = principal_from(req);
  const std::string scope = principal.scope();
  const std::string& request_id = input.request_id;
  auto mark_failed = [this, &scope, &request_id](const std::string& type,
                                                 const std::string& message) {
    if (request_id.empty() || idempotency == nullptr) return;
    try {
      idempotency->fail(scope, request_id, type, message);
    } catch (const std::exception& e) {
      platform::log_warn(std::string("写幂等失败状态失败: ") + e.what());
    }
  };

  chat::ChatService::Turn turn;
  bool replay = false;
  try {
    if (!request_id.empty() && idempotency != nullptr) {
      const chat::DedupRecord dedup = idempotency->claim(scope, request_id);
      if (dedup.status == chat::DedupStatus::InProgress) {
        return send_idempotency_busy(res);
      }
      if (dedup.status == chat::DedupStatus::Completed) {
        const auto message = store.message(dedup.assistant_message_id);
        if (!message.has_value()) {
          idempotency->remove(scope, request_id);
          return send_idempotency_conflict(res);
        }
        turn.session_id = dedup.session_id;
        turn.stream = std::make_shared<chat::ReplayStream>(message->content);
        replay = true;
      }
    }

    if (!replay) {
      long long daily_used = 0;
      if (usage != nullptr) daily_used = usage->summary_today(principal.user_id).total_tokens();

      quota::QuotaGuard guard;
      if (quota != nullptr) {
        auto acquired = quota->try_acquire(
            principal,
            estimate_request_tokens(input.user_text,
                                    effective_max_tokens(input, config.max_tokens)),
            daily_used);
        if (!acquired.decision.allowed) {
          metrics::inc_quota_rejection(acquired.decision.type);
          mark_failed("rate_limited", acquired.decision.message);
          return send_quota_error(res, acquired.decision);
        }
        guard = std::move(acquired.guard);
      }

      std::shared_ptr<chat::TurnObserver> observer;
      if (!request_id.empty() && idempotency != nullptr) {
        observer = std::make_shared<chat::IdempotencyObserver>(*idempotency, scope, request_id);
      }
      turn = service.start(principal, to_start_options(input), std::move(guard), observer);
    }
  } catch (const chat::ContextTooLong& e) {
    mark_failed("context_too_long", e.what());
    return send_error(res, 413, "context_too_long", e.what());
  } catch (const chat::Forbidden& e) {
    mark_failed("forbidden", e.what());
    return send_error(res, 404, "not_found", e.what());
  }
#ifdef AIP_WITH_POSTGRES
  catch (const storage::DbError& e) {
    mark_failed("storage_unavailable", e.what());
    platform::log_error(std::string("启动对话失败（存储）: ") + e.what());
    return send_error(res, 503, "storage_unavailable", "会话存储暂时不可用，请稍后重试");
  }
#endif
  catch (const std::exception& e) {
    mark_failed("upstream_error", e.what());
    platform::log_error(std::string("启动对话失败: ") + e.what());
    return send_error(res, 502, "upstream_error", "无法连接模型服务");
  }

  res.set_header("X-Session-Id", turn.session_id);

  if (!input.stream) {
    std::string answer;
    std::string error;
    std::string finish_reason = "stop";
    int prompt_tokens = -1;
    int completion_tokens = -1;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(config.request_timeout_s);
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
             {"idempotent_replay", replay},
             {"choices",
              json::array({{{"index", 0},
                            {"message", {{"role", "assistant"}, {"content", answer}}},
                            {"finish_reason", finish_reason}}})}};
    if (prompt_tokens < 0) prompt_tokens = turn.prompt_tokens_estimate;
    if (completion_tokens < 0) completion_tokens = chat::estimate_tokens(answer);
    out["usage"] = {{"prompt_tokens", prompt_tokens},
                    {"completion_tokens", completion_tokens},
                    {"total_tokens", prompt_tokens + completion_tokens}};

    const auto elapsed = std::chrono::steady_clock::now() - chat_started;
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    metrics::inc_chat_request(/*stream=*/false, "completed");
    metrics::observe_chat_latency(/*stream=*/false,
                                  std::chrono::duration<double>(elapsed).count());
    metrics::inc_tokens(model_name, "prompt", prompt_tokens);
    metrics::inc_tokens(model_name, "completion", completion_tokens);
    platform::log_event_info(
        "chat_completion",
        json{{"request_id", trace_id},
             {"session_id", turn.session_id},
             {"user_id", principal.user_id},
             {"model", model_name},
             {"stream", false},
             {"outcome", "completed"},
             {"finish_reason", finish_reason},
             {"duration_ms", elapsed_ms},
             {"ttft_ms", -1},
             {"prompt_tokens", prompt_tokens},
             {"completion_tokens", completion_tokens}});

    res.set_content(out.dump(), "application/json; charset=utf-8");
    return;
  }

  auto state = std::make_shared<SseState>();
  state->stream = turn.stream;
  state->id = util::random_id("chatcmpl-");
  state->model = model_name;
  state->request_id = trace_id;
  state->session_id = turn.session_id;
  state->user_id = principal.user_id;
  state->prompt_tokens_estimate = turn.prompt_tokens_estimate;
  state->include_usage = input.include_usage;
  state->created = util::now_ms() / 1000;
  state->started = chat_started;

  res.set_header("Cache-Control", "no-cache, no-transform");
  res.set_header("X-Accel-Buffering", "no");  // 关闭 Nginx 缓冲，否则前端会一次性收到全部内容

  active_streams.fetch_add(1);
  metrics::set_active_streams(active_streams.load());
  res.set_chunked_content_provider(
      "text/event-stream",
      [state, this](std::size_t /*offset*/, httplib::DataSink& sink) -> bool {
        llm::Event ev;
        const llm::Pull pull =
            state->stream->next(ev, std::chrono::seconds(config.heartbeat_s));

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
            if (!state->first_token_at.has_value()) {
              state->first_token_at = std::chrono::steady_clock::now();
            }
            for (const unsigned char c : ev.text) {
              if (c < 0x80) {
                ++state->ascii_bytes;
              } else if ((c & 0xC0) != 0x80) {
                ++state->non_ascii_codepoints;
              }
            }
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
            state->finish_reason =
                ev.finish_reason.empty() ? "stop" : ev.finish_reason;
            state->prompt_tokens =
                ev.prompt_tokens >= 0 ? ev.prompt_tokens : state->prompt_tokens_estimate;
            state->completion_tokens =
                ev.completion_tokens >= 0
                    ? ev.completion_tokens
                    : static_cast<int>((state->ascii_bytes + 3) / 4 +
                                       state->non_ascii_codepoints);
            json chunk = chunk_base(state->id, state->model, state->created);
            chunk["choices"] = json::array(
                {{{"index", 0}, {"delta", json::object()},
                  {"finish_reason", state->finish_reason}}});
            if (state->include_usage) {
              chunk["usage"] = {{"prompt_tokens", state->prompt_tokens},
                                {"completion_tokens", state->completion_tokens},
                                {"total_tokens",
                                 state->prompt_tokens + state->completion_tokens}};
            }
            frame += sse_pack(chunk.dump());
            frame += sse_pack("[DONE]");
            terminal = true;
            break;
          }
          case llm::EventKind::Error: {
            state->saw_error = true;
            frame += sse_pack(json{{"error", {{"type", "upstream_error"}, {"message", ev.text}}}}
                                  .dump(),
                              "error");
            frame += sse_pack("[DONE]");
            terminal = true;
            break;
          }
        }

        const bool ok = sink.write(frame.data(), frame.size());
        if (!ok) state->client_cancelled = true;
        if (terminal) sink.done();
        return ok;
      },
      [state, this](bool /*success*/) {
        // 响应结束（含客户端断开）时释放上游连接，并记录最终指标。
        state->stream->cancel();
        active_streams.fetch_sub(1);
        metrics::set_active_streams(active_streams.load());

        if (state->prompt_tokens < 0) state->prompt_tokens = state->prompt_tokens_estimate;
        if (state->completion_tokens < 0) {
          state->completion_tokens = static_cast<int>((state->ascii_bytes + 3) / 4 +
                                                      state->non_ascii_codepoints);
        }
        const auto elapsed = std::chrono::steady_clock::now() - state->started;
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        const int ttft_ms =
            state->first_token_at.has_value()
                ? static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       *state->first_token_at - state->started)
                                       .count())
                : -1;
        const std::string outcome =
            state->client_cancelled ? "cancelled" : (state->saw_error ? "error" : "completed");

        metrics::inc_chat_request(/*stream=*/true, outcome);
        metrics::observe_chat_latency(/*stream=*/true,
                                      std::chrono::duration<double>(elapsed).count());
        if (state->first_token_at.has_value()) {
          metrics::observe_ttft(
              state->model,
              std::chrono::duration<double>(*state->first_token_at - state->started).count());
        }
        metrics::inc_tokens(state->model, "prompt", state->prompt_tokens);
        metrics::inc_tokens(state->model, "completion", state->completion_tokens);
        platform::log_event_info(
            "chat_stream",
            json{{"request_id", state->request_id},
                 {"session_id", state->session_id},
                 {"user_id", state->user_id},
                 {"model", state->model},
                 {"stream", true},
                 {"outcome", outcome},
                 {"finish_reason", state->finish_reason},
                 {"duration_ms", elapsed_ms},
                 {"ttft_ms", ttft_ms},
                 {"prompt_tokens", state->prompt_tokens},
                 {"completion_tokens", state->completion_tokens}});
      });
}

void ApiServer::Impl::handle_static(const httplib::Request& req, httplib::Response& res) {
  if (!config.web_enabled) {
    return send_error(res, 404, "not_found", "Web UI 已禁用（AIP_WEB=false）");
  }
  const web::Asset* asset = web::find_asset(req.path);
  if (asset == nullptr) {
    return send_error(res, 404, "not_found", "资源不存在: " + req.path);
  }
  send_asset(res, *asset);
}

#ifdef AIP_WITH_AUTH
namespace {

bool parse_auth_json(const httplib::Request& req, nlohmann::json& body,
                     httplib::Response& res) {
  try {
    body = nlohmann::json::parse(req.body);
  } catch (const std::exception&) {
    send_error(res, 400, "invalid_json", "请求体不是合法 JSON");
    return false;
  }
  if (!body.is_object()) {
    send_error(res, 400, "invalid_json", "请求体必须是 JSON 对象");
    return false;
  }
  return true;
}

void send_auth_error(httplib::Response& res, const auth::AuthError& error) {
  const AuthHttpError info = auth_http_error(error.code());
  send_error(res, info.status, info.type, error.what());
}

}  // namespace

void ApiServer::Impl::handle_auth_register(const httplib::Request& req,
                                           httplib::Response& res) {
  if (auth == nullptr) {
    return send_error(res, 503, "auth_unavailable",
                      "用户鉴权未启用（需要 AIP_STORAGE=postgres 且编译时开启 AUTH）");
  }
  json body;
  if (!parse_auth_json(req, body, res)) return;
  if (!body.contains("email") || !body["email"].is_string() || !body.contains("password") ||
      !body["password"].is_string()) {
    return send_error(res, 400, "invalid_request", "email 和 password 必须是字符串");
  }
  if (body.contains("display_name") && !body["display_name"].is_string()) {
    return send_error(res, 400, "invalid_request", "display_name 必须是字符串");
  }

  try {
    const auth::TokenPair pair = auth->register_user(
        body["email"].get<std::string>(), body["password"].get<std::string>(),
        body.contains("display_name") ? body["display_name"].get<std::string>() : std::string{});
    res.set_content(token_pair_json(pair).dump(), "application/json; charset=utf-8");
  } catch (const auth::AuthError& e) {
    send_auth_error(res, e);
  }
#ifdef AIP_WITH_POSTGRES
  catch (const storage::DbError& e) {
    platform::log_error(std::string("注册失败（存储）: ") + e.what());
    send_error(res, 503, "storage_unavailable", "用户存储暂时不可用，请稍后重试");
  }
#endif
  catch (const std::exception& e) {
    platform::log_error(std::string("注册失败: ") + e.what());
    send_error(res, 500, "internal_error", "注册失败");
  }
}

void ApiServer::Impl::handle_auth_login(const httplib::Request& req, httplib::Response& res) {
  if (auth == nullptr) {
    return send_error(res, 503, "auth_unavailable",
                      "用户鉴权未启用（需要 AIP_STORAGE=postgres 且编译时开启 AUTH）");
  }
  json body;
  if (!parse_auth_json(req, body, res)) return;
  if (!body.contains("email") || !body["email"].is_string() || !body.contains("password") ||
      !body["password"].is_string()) {
    return send_error(res, 400, "invalid_request", "email 和 password 必须是字符串");
  }

  try {
    const auth::TokenPair pair = auth->login(body["email"].get<std::string>(),
                                             body["password"].get<std::string>());
    res.set_content(token_pair_json(pair).dump(), "application/json; charset=utf-8");
  } catch (const auth::AuthError& e) {
    send_auth_error(res, e);
  }
#ifdef AIP_WITH_POSTGRES
  catch (const storage::DbError& e) {
    platform::log_error(std::string("登录失败（存储）: ") + e.what());
    send_error(res, 503, "storage_unavailable", "用户存储暂时不可用，请稍后重试");
  }
#endif
  catch (const std::exception& e) {
    platform::log_error(std::string("登录失败: ") + e.what());
    send_error(res, 500, "internal_error", "登录失败");
  }
}

void ApiServer::Impl::handle_auth_refresh(const httplib::Request& req, httplib::Response& res) {
  if (auth == nullptr) {
    return send_error(res, 503, "auth_unavailable",
                      "用户鉴权未启用（需要 AIP_STORAGE=postgres 且编译时开启 AUTH）");
  }

  std::string refresh_token;
  const auto header_token = bearer_token(req.get_header_value("Authorization"));
  if (header_token.has_value()) {
    refresh_token = *header_token;
  } else {
    json body;
    if (!parse_auth_json(req, body, res)) return;
    if (!body.contains("refresh_token") || !body["refresh_token"].is_string()) {
      return send_error(res, 400, "invalid_request",
                        "缺少 Authorization: Bearer <refresh_token> 或 refresh_token 字段");
    }
    refresh_token = body["refresh_token"].get<std::string>();
  }

  try {
    const auth::TokenPair pair = auth->refresh(refresh_token);
    res.set_content(token_pair_json(pair).dump(), "application/json; charset=utf-8");
  } catch (const auth::AuthError& e) {
    send_auth_error(res, e);
  }
#ifdef AIP_WITH_POSTGRES
  catch (const storage::DbError& e) {
    platform::log_error(std::string("刷新 token 失败（存储）: ") + e.what());
    send_error(res, 503, "storage_unavailable", "用户存储暂时不可用，请稍后重试");
  }
#endif
  catch (const std::exception& e) {
    platform::log_error(std::string("刷新 token 失败: ") + e.what());
    send_error(res, 500, "internal_error", "刷新 token 失败");
  }
}

void ApiServer::Impl::handle_auth_me(const httplib::Request& req, httplib::Response& res) {
  if (auth == nullptr) {
    return send_error(res, 503, "auth_unavailable",
                      "用户鉴权未启用（需要 AIP_STORAGE=postgres 且编译时开启 AUTH）");
  }
  const auth::Principal principal = principal_from(req);
  if (principal.user_id <= 0) {
    return send_error(res, 401, "unauthorized",
                      "静态管理员 Token 没有用户资料，请使用 JWT/API Key");
  }
  try {
    auto user = auth->get_user(principal.user_id);
    if (!user.has_value()) return send_error(res, 404, "not_found", "用户不存在");
    res.set_content(json{{"user", user_json(*user)}}.dump(),
                    "application/json; charset=utf-8");
  }
#ifdef AIP_WITH_POSTGRES
  catch (const storage::DbError& e) {
    platform::log_error(std::string("读取用户失败（存储）: ") + e.what());
    send_error(res, 503, "storage_unavailable", "用户存储暂时不可用，请稍后重试");
  }
#endif
  catch (const std::exception& e) {
    platform::log_error(std::string("读取用户失败: ") + e.what());
    send_error(res, 500, "internal_error", "读取用户失败");
  }
}

void ApiServer::Impl::handle_auth_list_api_keys(const httplib::Request& req,
                                                httplib::Response& res) {
  if (auth == nullptr) {
    return send_error(res, 503, "auth_unavailable",
                      "用户鉴权未启用（需要 AIP_STORAGE=postgres 且编译时开启 AUTH）");
  }
  const auth::Principal principal = principal_from(req);
  if (principal.user_id <= 0) {
    return send_error(res, 401, "unauthorized", "该接口需要用户身份（JWT/API Key）");
  }
  try {
    json list = json::array();
    for (const auto& key : auth->list_api_keys(principal)) list.push_back(api_key_json(key));
    res.set_content(json{{"api_keys", list}}.dump(), "application/json; charset=utf-8");
  } catch (const auth::AuthError& e) {
    send_auth_error(res, e);
  }
#ifdef AIP_WITH_POSTGRES
  catch (const storage::DbError& e) {
    platform::log_error(std::string("读取 API Key 失败（存储）: ") + e.what());
    send_error(res, 503, "storage_unavailable", "用户存储暂时不可用，请稍后重试");
  }
#endif
  catch (const std::exception& e) {
    platform::log_error(std::string("读取 API Key 失败: ") + e.what());
    send_error(res, 500, "internal_error", "读取 API Key 失败");
  }
}

void ApiServer::Impl::handle_auth_create_api_key(const httplib::Request& req,
                                                 httplib::Response& res) {
  if (auth == nullptr) {
    return send_error(res, 503, "auth_unavailable",
                      "用户鉴权未启用（需要 AIP_STORAGE=postgres 且编译时开启 AUTH）");
  }
  const auth::Principal principal = principal_from(req);
  if (principal.user_id <= 0) {
    return send_error(res, 401, "unauthorized", "该接口需要用户身份（JWT/API Key）");
  }

  std::string name;
  if (!req.body.empty()) {
    json body;
    if (!parse_auth_json(req, body, res)) return;
    if (body.contains("name") && !body["name"].is_string()) {
      return send_error(res, 400, "invalid_request", "name 必须是字符串");
    }
    if (body.contains("name")) name = body["name"].get<std::string>();
  }

  try {
    const auth::IssuedApiKey issued = auth->create_api_key(principal, name);
    json out = api_key_json(issued.info);
    out["key"] = issued.plaintext;
    res.status = 201;
    res.set_content(out.dump(), "application/json; charset=utf-8");
  } catch (const auth::AuthError& e) {
    send_auth_error(res, e);
  }
#ifdef AIP_WITH_POSTGRES
  catch (const storage::DbError& e) {
    platform::log_error(std::string("创建 API Key 失败（存储）: ") + e.what());
    send_error(res, 503, "storage_unavailable", "用户存储暂时不可用，请稍后重试");
  }
#endif
  catch (const std::exception& e) {
    platform::log_error(std::string("创建 API Key 失败: ") + e.what());
    send_error(res, 500, "internal_error", "创建 API Key 失败");
  }
}

void ApiServer::Impl::handle_auth_delete_api_key(const httplib::Request& req,
                                                 httplib::Response& res) {
  if (auth == nullptr) {
    return send_error(res, 503, "auth_unavailable",
                      "用户鉴权未启用（需要 AIP_STORAGE=postgres 且编译时开启 AUTH）");
  }
  const auth::Principal principal = principal_from(req);
  if (principal.user_id <= 0) {
    return send_error(res, 401, "unauthorized", "该接口需要用户身份（JWT/API Key）");
  }

  std::int64_t key_id = 0;
  try {
    key_id = std::stoll(req.matches[1]);
  } catch (const std::exception&) {
    key_id = 0;
  }
  if (key_id <= 0) return send_error(res, 400, "invalid_request", "API Key ID 不合法");

  try {
    if (!auth->revoke_api_key(principal, key_id)) {
      return send_error(res, 404, "not_found", "API Key 不存在");
    }
    res.set_content(json{{"revoked", true}, {"id", key_id}}.dump(),
                    "application/json; charset=utf-8");
  } catch (const auth::AuthError& e) {
    send_auth_error(res, e);
  }
#ifdef AIP_WITH_POSTGRES
  catch (const storage::DbError& e) {
    platform::log_error(std::string("撤销 API Key 失败（存储）: ") + e.what());
    send_error(res, 503, "storage_unavailable", "用户存储暂时不可用，请稍后重试");
  }
#endif
  catch (const std::exception& e) {
    platform::log_error(std::string("撤销 API Key 失败: ") + e.what());
    send_error(res, 500, "internal_error", "撤销 API Key 失败");
  }
}
#endif

void ApiServer::Impl::handle_sessions_list(const httplib::Request& req,
                                            httplib::Response& res) {
  const auth::Principal principal = principal_from(req);
  if (!principal.is_admin && principal.user_id <= 0) {
    return send_error(res, 401, "unauthorized", "该接口需要用户身份（JWT/API Key）");
  }

  std::size_t limit = 20;
  std::size_t offset = 0;
  const std::string limit_text = req.get_param_value("limit");
  const std::string offset_text = req.get_param_value("offset");
  try {
    if (!limit_text.empty()) {
      const unsigned long parsed = std::stoul(limit_text);
      if (parsed < 1 || parsed > 100) throw std::out_of_range("limit");
      limit = static_cast<std::size_t>(parsed);
    }
    if (!offset_text.empty()) {
      const unsigned long parsed = std::stoul(offset_text);
      if (parsed > 100000) throw std::out_of_range("offset");
      offset = static_cast<std::size_t>(parsed);
    }
  } catch (const std::exception&) {
    return send_error(res, 400, "invalid_request", "limit 应为 1~100，offset 应为 0~100000");
  }

  try {
    auto sessions = store.list(principal.user_id, principal.is_admin, limit + 1, offset);
    const bool has_more = sessions.size() > limit;
    if (has_more) sessions.resize(limit);
    json list = json::array();
    for (const auto& session : sessions) list.push_back(session_json(session));
    res.set_content(json{{"sessions", list}, {"has_more", has_more}}.dump(),
                    "application/json; charset=utf-8");
  }
#ifdef AIP_WITH_POSTGRES
  catch (const storage::DbError& e) {
    platform::log_error(std::string("读取会话列表失败（存储）: ") + e.what());
    send_error(res, 503, "storage_unavailable", "会话存储暂时不可用，请稍后重试");
  }
#endif
  catch (const std::exception& e) {
    platform::log_error(std::string("读取会话列表失败: ") + e.what());
    send_error(res, 500, "internal_error", "读取会话列表失败");
  }
}

void ApiServer::Impl::handle_session_update(const httplib::Request& req,
                                             httplib::Response& res) {
  const auth::Principal principal = principal_from(req);
  if (!principal.is_admin && principal.user_id <= 0) {
    return send_error(res, 401, "unauthorized", "该接口需要用户身份（JWT/API Key）");
  }
  const std::string session_id = req.matches[1];

  if (req.method == "PATCH") {
    json body;
    try {
      body = json::parse(req.body);
    } catch (const std::exception&) {
      return send_error(res, 400, "invalid_json", "请求体不是合法 JSON");
    }
    if (!body.is_object() || !body.contains("title") || !body["title"].is_string()) {
      return send_error(res, 400, "invalid_request", "title 必须是字符串");
    }
    const std::string title = body["title"].get<std::string>();
    if (title.empty() || title.size() > 200 ||
        title.find_first_not_of(" \t\r\n") == std::string::npos) {
      return send_error(res, 400, "invalid_request", "title 长度应在 1~200，且不能全为空白");
    }

    try {
      if (!store.rename(principal.user_id, principal.is_admin, session_id, title)) {
        return send_error(res, 404, "not_found", "会话不存在或无权访问");
      }
      const auto updated = store.get(session_id);
      if (!updated.has_value()) return send_error(res, 404, "not_found", "会话不存在");
      res.set_content(json{{"session", session_json(*updated)}}.dump(),
                      "application/json; charset=utf-8");
    }
#ifdef AIP_WITH_POSTGRES
    catch (const storage::DbError& e) {
      platform::log_error(std::string("重命名会话失败（存储）: ") + e.what());
      send_error(res, 503, "storage_unavailable", "会话存储暂时不可用，请稍后重试");
    }
#endif
    catch (const std::exception& e) {
      platform::log_error(std::string("重命名会话失败: ") + e.what());
      send_error(res, 500, "internal_error", "重命名失败");
    }
    return;
  }

  if (req.method == "DELETE") {
    try {
      if (!store.remove(principal.user_id, principal.is_admin, session_id)) {
        return send_error(res, 404, "not_found", "会话不存在或无权访问");
      }
      res.set_content(json{{"deleted", true}, {"id", session_id}}.dump(),
                      "application/json; charset=utf-8");
    }
#ifdef AIP_WITH_POSTGRES
    catch (const storage::DbError& e) {
      platform::log_error(std::string("删除会话失败（存储）: ") + e.what());
      send_error(res, 503, "storage_unavailable", "会话存储暂时不可用，请稍后重试");
    }
#endif
    catch (const std::exception& e) {
      platform::log_error(std::string("删除会话失败: ") + e.what());
      send_error(res, 500, "internal_error", "删除失败");
    }
    return;
  }

  send_error(res, 405, "method_not_allowed", "只支持 PATCH / DELETE");
}

void ApiServer::Impl::handle_history(const httplib::Request& req, httplib::Response& res) {
  const std::string session_id = req.matches[1];
  std::size_t limit = 100;
  const std::string limit_text = req.get_param_value("limit");
  if (!limit_text.empty()) {
    try {
      const unsigned long parsed = std::stoul(limit_text);
      if (parsed < 1 || parsed > 100) throw std::out_of_range("limit");
      limit = static_cast<std::size_t>(parsed);
    } catch (const std::exception&) {
      return send_error(res, 400, "invalid_request", "limit 必须是 1~100 的整数");
    }
  }

  const auth::Principal principal = principal_from(req);
  try {
    if (!store.owns(principal.user_id, principal.is_admin, session_id)) {
      return send_error(res, 404, "not_found", "会话不存在或无权访问");
    }
    const auto messages = store.history(session_id, limit);

    json list = json::array();
    for (const auto& m : messages) {
      list.push_back({{"role", m.role}, {"content", m.content}});
    }
    res.set_content(json{{"session_id", session_id}, {"messages", list}}.dump(),
                    "application/json; charset=utf-8");
  }
#ifdef AIP_WITH_POSTGRES
  catch (const storage::DbError& e) {
    platform::log_error(std::string("读取会话历史失败（存储）: ") + e.what());
    send_error(res, 503, "storage_unavailable", "会话存储暂时不可用，请稍后重试");
  }
#endif
  catch (const std::exception& e) {
    platform::log_error(std::string("读取会话历史失败: ") + e.what());
    send_error(res, 500, "internal_error", "读取会话失败");
  }
}

ApiServer::ApiServer(const platform::Config& config, chat::ChatService& service,
                     chat::SessionStore& store, auth::AuthService* auth,
                     quota::QuotaService* quota, usage::UsageSink* usage,
                     chat::IdempotencyStore* idempotency)
    : impl_(std::make_unique<Impl>(config, service, store, auth, quota, usage, idempotency)) {}

ApiServer::~ApiServer() = default;

bool ApiServer::run() {
  platform::log_info("HTTP 服务启动（httplib 回退实现）: http://" + impl_->config.host + ":" +
                     std::to_string(impl_->config.port));
  if (impl_->config.auth_enabled()) {
    platform::log_info("鉴权已启用（静态 Token / JWT / API Key）");
  } else {
    platform::log_warn("未启用鉴权（AIP_TOKEN/AIP_JWT_SECRET/AIP_AUTH_REQUIRED 均为空），请勿暴露到公网");
  }

  g_signal_target.store(&impl_->server);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  const bool ok = impl_->server.listen(impl_->config.host, impl_->config.port);
  g_signal_target.store(nullptr);
  return ok;
}

void ApiServer::stop() { impl_->server.stop(); }

bool ApiServer::is_running() const { return impl_->server.is_running(); }

std::size_t ApiServer::active_streams() const { return impl_->active_streams.load(); }

}  // namespace http
