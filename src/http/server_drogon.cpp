// HTTP 层实现之二：Drogon（主线，见 docs/adr/0001-http-framework.md）。
//
// 与 POC（poc/drogon_sse.cpp）的关系：泵线程 + 推送流的桥接方式照搬，但补上了
// 生产必需的几件事——鉴权、CORS、错误码、流数量准入、优雅退出 drain、
// 非流式请求的线程池、以及**关闭 Drogon 默认的静态文件服务**（默认根目录是 "./"，
// 不关等于把工作目录里的 .env / 源码暴露成静态站点）。
//
// 线程模型（重要）：
//   - 连接本身不占线程（epoll），这是相对 httplib 的核心收益；
//   - 每路**活跃** SSE 流仍占 2 个线程：泵线程 + 上游读线程（libcurl 是阻塞的）；
//   - 非流式请求由固定大小的 WorkerPool 处理，绝不在 IO 线程里等模型。

#include "http/server.h"

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "common/log.h"
#include "http/public_config.h"
#include "util/id.h"
#include "web/assets.h"

namespace http {
namespace {

using json = nlohmann::json;
using drogon::HttpRequestPtr;
using drogon::HttpResponsePtr;
using ResponseCallback = std::function<void(const HttpResponsePtr&)>;

constexpr std::size_t kMaxBodyBytes = 1024 * 1024;
constexpr std::size_t kMaxMessageBytes = 32 * 1024;
constexpr std::size_t kNonStreamQueueSize = 256;

// Drogon 的静态文件根目录**默认是 "./"**。本服务不提供任何静态文件，
// 指到一个不存在的目录，让静态路由永远落到我们的 JSON 404 上。
// （不要设成空串：StaticFileRouter 会对空路径取 realLocation_[len-1]，是未定义行为。）
constexpr const char* kDisabledDocumentRoot = "/nonexistent-aiplatform-public";

bool is_blank(const std::string& s) {
  return s.find_first_not_of(" \t\r\n") == std::string::npos;
}

std::string json_error(const std::string& type, const std::string& message) {
  return json{{"error", {{"type", type}, {"message", message}}}}.dump();
}

HttpResponsePtr error_response(drogon::HttpStatusCode status, const std::string& type,
                               const std::string& message) {
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(status);
  resp->setContentTypeString("application/json; charset=utf-8");
  resp->setBody(json_error(type, message));
  return resp;
}

HttpResponsePtr json_ok(const json& body) {
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setContentTypeString("application/json; charset=utf-8");
  resp->setBody(body.dump());
  return resp;
}

HttpResponsePtr body_response(std::string body, const std::string& content_type) {
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setContentTypeString(content_type);
  resp->setBody(std::move(body));
  return resp;
}

// 内置 Web UI：资源已经编译进二进制，这里只按 URL 精确查表。
// 不接触文件系统，因此不需要担心目录穿越；文档根目录仍然保持禁用。
HttpResponsePtr asset_response(const web::Asset& asset) {
  auto resp = body_response(std::string(asset.content), std::string(asset.content_type));
  resp->addHeader("Cache-Control", "no-cache");
  resp->addHeader("X-Content-Type-Options", "nosniff");
  resp->addHeader("Referrer-Policy", "no-referrer");
  if (asset.content_type.rfind("text/html", 0) == 0) {
    resp->addHeader("X-Frame-Options", "DENY");
    resp->addHeader("Content-Security-Policy",
                    "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
                    "img-src 'self' data:; connect-src 'self'; font-src 'self'; "
                    "object-src 'none'; base-uri 'none'; frame-ancestors 'none'");
  }
  return resp;
}

// 不需要 Bearer Token 的公开路径：探针、前端配置、以及内置静态资源。
// 对话与历史接口仍然走鉴权。
bool is_public_path(const platform::Config& config, const std::string& path) {
  if (path == "/healthz" || path == "/v1/config") return true;
  return config.web_enabled && web::is_public_asset(path);
}

void add_cors(const HttpResponsePtr& resp) {
  resp->addHeader("Access-Control-Allow-Origin", "*");
  resp->addHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
  resp->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  resp->addHeader("Access-Control-Expose-Headers", "X-Session-Id");
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

long long now_s() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 流数量准入：超过上限直接 503，而不是让上游请求堆积。
class StreamGate {
 public:
  bool try_enter(std::size_t max) {
    std::size_t current = active_.load(std::memory_order_relaxed);
    while (current < max) {
      if (active_.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel,
                                        std::memory_order_relaxed)) {
        return true;
      }
    }
    return false;
  }

  void leave() { active_.fetch_sub(1, std::memory_order_acq_rel); }

  std::size_t active() const { return active_.load(std::memory_order_acquire); }

 private:
  std::atomic<std::size_t> active_{0};
};

// 保证「准入名额」恰好释放一次：
//   - 正常路径：泵线程结束时 release()；
//   - 异常路径：响应还没真正开始发（客户端先断了），Drogon 连 stream 回调都不调，
//     此时 lambda 被销毁 → 析构函数兜底释放。
class StreamSlot {
 public:
  explicit StreamSlot(StreamGate& gate) : gate_(&gate) {}
  ~StreamSlot() { release(); }

  StreamSlot(const StreamSlot&) = delete;
  StreamSlot& operator=(const StreamSlot&) = delete;

  void release() {
    if (gate_ != nullptr) {
      gate_->leave();
      gate_ = nullptr;
    }
  }

 private:
  StreamGate* gate_;
};

// 一路 SSE 的泵：在自己的线程里「拉」上游事件，往 Drogon 的响应流里「推」。
//
// 生命周期由 PumpRegistry 持有，线程同时持有一份 shared_ptr：
// 线程结束时只置 finished_，真正的 join 由 registry 在下次 add 或退出时做。
class SsePump : public std::enable_shared_from_this<SsePump> {
 public:
  SsePump(std::shared_ptr<llm::Stream> stream, drogon::ResponseStreamPtr out, std::string id,
          std::string model, std::chrono::milliseconds heartbeat,
          std::shared_ptr<StreamSlot> slot)
      : stream_(std::move(stream)),
        out_(std::move(out)),
        id_(std::move(id)),
        model_(std::move(model)),
        heartbeat_(heartbeat),
        slot_(std::move(slot)) {}

  ~SsePump() {
    // 兜底：正常路径下 registry 已经 join 过，这里不会触发。
    if (thread_.joinable()) thread_.detach();
  }

  SsePump(const SsePump&) = delete;
  SsePump& operator=(const SsePump&) = delete;

  void start() {
    auto self = shared_from_this();
    thread_ = std::thread([self] { self->run(); });
  }

  void cancel() { stream_->cancel(); }

  void join() {
    if (thread_.joinable()) thread_.join();
  }

  bool finished() const { return finished_.load(std::memory_order_acquire); }

 private:
  void run() {
    const auto started = std::chrono::steady_clock::now();
    const long long created = now_s();
    bool sent_role = false;
    bool cancelled = false;

    while (true) {
      llm::Event ev;
      const llm::Pull pull = stream_->next(ev, heartbeat_);

      if (pull == llm::Pull::Cancelled) {
        cancelled = true;
        break;
      }
      if (pull == llm::Pull::Timeout) {
        // 心跳：既保活，也是探测「客户端是否已断开」的低成本手段。
        if (!out_->send(": keep-alive\n\n")) {
          cancelled = true;
          stream_->cancel();
          break;
        }
        continue;
      }
      if (pull == llm::Pull::End) {
        out_->send(sse_pack("[DONE]"));
        break;
      }

      std::string frame;
      bool terminal = false;

      switch (ev.kind) {
        case llm::EventKind::Delta: {
          if (!sent_role) {
            sent_role = true;
            json role = chunk_base(id_, model_, created);
            role["choices"] = json::array(
                {{{"index", 0},
                  {"delta", {{"role", "assistant"}, {"content", ""}}},
                  {"finish_reason", nullptr}}});
            frame += sse_pack(role.dump());
          }
          json chunk = chunk_base(id_, model_, created);
          chunk["choices"] = json::array(
              {{{"index", 0}, {"delta", {{"content", ev.text}}}, {"finish_reason", nullptr}}});
          frame += sse_pack(chunk.dump());
          break;
        }
        case llm::EventKind::Done: {
          json chunk = chunk_base(id_, model_, created);
          chunk["choices"] = json::array(
              {{{"index", 0},
                {"delta", json::object()},
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
          frame += sse_pack(
              json{{"error", {{"type", "upstream_error"}, {"message", ev.text}}}}.dump(),
              "error");
          frame += sse_pack("[DONE]");
          terminal = true;
          break;
        }
      }

      if (!frame.empty() && !out_->send(frame)) {
        // 客户端已经断开：立刻取消上游，别继续烧 token。
        cancelled = true;
        stream_->cancel();
        break;
      }
      if (terminal) break;
    }

    out_->close();
    if (slot_) slot_->release();
    finished_.store(true, std::memory_order_release);

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    if (cancelled) {
      // 取消要能看见：它直接对应「有没有白烧 token」。
      platform::log_info("SSE 取消（客户端断开）: " + id_ + " " +
                         std::to_string(elapsed_ms) + "ms");
    } else {
      platform::log_debug("SSE 完成: " + id_ + " " + std::to_string(elapsed_ms) + "ms");
    }
  }

  std::shared_ptr<llm::Stream> stream_;
  drogon::ResponseStreamPtr out_;
  const std::string id_;
  const std::string model_;
  const std::chrono::milliseconds heartbeat_;
  std::shared_ptr<StreamSlot> slot_;
  std::thread thread_;
  std::atomic<bool> finished_{false};
};

// 只在「新增流」和「退出」时顺带回收已结束的泵，避免单独开清理线程。
class PumpRegistry {
 public:
  void add(std::shared_ptr<SsePump> pump) {
    std::vector<std::shared_ptr<SsePump>> finished;
    {
      std::lock_guard<std::mutex> lock(mu_);
      for (auto it = alive_.begin(); it != alive_.end();) {
        if ((*it)->finished()) {
          finished.push_back(std::move(*it));
          it = alive_.erase(it);
        } else {
          ++it;
        }
      }
      alive_.push_back(std::move(pump));
    }
    // join 放在锁外：join 可能等几毫秒，不要挡住其它请求注册新流。
    for (auto& pump : finished) pump->join();
  }

  void drain() {
    std::vector<std::shared_ptr<SsePump>> alive;
    {
      std::lock_guard<std::mutex> lock(mu_);
      alive.swap(alive_);
    }
    for (auto& pump : alive) pump->cancel();
    for (auto& pump : alive) pump->join();
  }

 private:
  std::mutex mu_;
  std::vector<std::shared_ptr<SsePump>> alive_;
};

// 非流式请求的固定线程池：绝不在 IO 线程里等模型。
class WorkerPool {
 public:
  WorkerPool(std::size_t threads, std::size_t max_queue)
      : max_queue_(max_queue) {
    threads_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
      threads_.emplace_back([this] { run(); });
    }
  }

  ~WorkerPool() { stop(); }

  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  /// 队列满返回 false，调用方应立即回 503，不要堆积。
  bool submit(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (stop_ || tasks_.size() >= max_queue_) return false;
      tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
    return true;
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (stop_) return;
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& thread : threads_) {
      if (thread.joinable()) thread.join();
    }
    threads_.clear();
  }

 private:
  void run() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
        if (stop_) return;
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      task();
    }
  }

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  const std::size_t max_queue_;
  bool stop_ = false;
  std::vector<std::thread> threads_;
};

std::size_t default_worker_count() {
  auto hw = std::thread::hardware_concurrency();
  if (hw == 0) hw = 4;
  return std::clamp<std::size_t>(hw, 4, 16);
}

}  // namespace

struct ApiServer::Impl {
  Impl(const platform::Config& config, chat::ChatService& service, chat::SessionStore& store)
      : config(config),
        service(service),
        store(store),
        heartbeat(std::chrono::seconds(std::max(1, config.heartbeat_s))),
        max_streams(static_cast<std::size_t>(std::max(1, config.max_streams))),
        workers(default_worker_count(), kNonStreamQueueSize) {}

  void setup();
  bool run();
  void begin_shutdown();

  std::size_t io_threads() const;
  HttpResponsePtr health_response();
  HttpResponsePtr make_sse_response(std::shared_ptr<llm::Stream> stream, std::string id,
                                    std::string model, std::shared_ptr<StreamSlot> slot);
  void pre_routing(const HttpRequestPtr& req, drogon::AdviceCallback&& callback,
                   drogon::AdviceChainCallback&& chain);
  void handle_chat(const HttpRequestPtr& req, ResponseCallback&& callback);
  void handle_non_stream(const std::string& session_id, const std::string& user_text,
                         const std::string& model, const std::string& model_name,
                         const std::shared_ptr<ResponseCallback>& callback);
  void handle_history(const HttpRequestPtr& req, ResponseCallback&& callback);
  void handle_static(const HttpRequestPtr& req, ResponseCallback&& callback);
  void post(const std::shared_ptr<ResponseCallback>& callback, HttpResponsePtr resp);

  const platform::Config& config;
  chat::ChatService& service;
  chat::SessionStore& store;
  const std::chrono::milliseconds heartbeat;
  const std::size_t max_streams;

  StreamGate gate;
  PumpRegistry pumps;
  WorkerPool workers;
  std::atomic<bool> shutting_down{false};
};

std::size_t ApiServer::Impl::io_threads() const {
  if (config.io_threads > 0) return static_cast<std::size_t>(config.io_threads);
  auto hw = std::thread::hardware_concurrency();
  if (hw == 0) hw = 4;
  // IO 线程只做 epoll 与 send，不需要跟核数一一对应；上限 8 防止几十核机器上浪费。
  return std::clamp<std::size_t>(hw, 2, 8);
}

void ApiServer::Impl::setup() {
  drogon::app().addListener(config.host, config.port);
  drogon::app().setThreadNum(io_threads());
  drogon::app().setClientMaxBodySize(kMaxBodyBytes);
  // SSE 是长连接：默认 60s 的 idle 超时会把「上游思考中」的连接掐掉。
  drogon::app().setIdleConnectionTimeout(600);
  drogon::app().setDocumentRoot(kDisabledDocumentRoot);
  drogon::app().setFileTypes({});
  drogon::app().setServerHeaderField("AIPlatform");
  drogon::app().setLogLevel(trantor::Logger::kWarn);

  drogon::app().registerPreRoutingAdvice(
      [this](const HttpRequestPtr& req, drogon::AdviceCallback&& callback,
             drogon::AdviceChainCallback&& chain) {
        pre_routing(req, std::move(callback), std::move(chain));
      });

  // preSending 对「所有」响应生效（含 404 与框架自身生成的错误），CORS 和访问日志放这里最省心。
  drogon::app().registerPreSendingAdvice(
      [](const HttpRequestPtr& req, const HttpResponsePtr& resp) {
        add_cors(resp);
        platform::log_info(std::string(req->methodString()) + " " + req->path() + " -> " +
                           std::to_string(static_cast<int>(resp->statusCode())));
      });

  drogon::app().registerHandler(
      "/healthz",
      [this](const HttpRequestPtr&, ResponseCallback&& callback) {
        callback(health_response());
      },
      {drogon::Get});

  // 前端启动时读取的公开配置（是否要求 Token、模型名、大小上限等）。
  drogon::app().registerHandler(
      "/v1/config",
      [this](const HttpRequestPtr&, ResponseCallback&& callback) {
        callback(body_response(public_config_json(config, "drogon"),
                               "application/json; charset=utf-8"));
      },
      {drogon::Get});

  // 内置 Web UI：首页与静态资源。资源是编译期查表，不存在文件系统访问。
  drogon::app().registerHandler(
      "/",
      [this](const HttpRequestPtr& req, ResponseCallback&& callback) {
        handle_static(req, std::move(callback));
      },
      {drogon::Get});

  drogon::app().registerHandlerViaRegex(
      R"(/(?:index\.html|favicon\.svg|assets/[A-Za-z0-9._\-]+))",
      [this](const HttpRequestPtr& req, ResponseCallback&& callback) {
        handle_static(req, std::move(callback));
      },
      {drogon::Get});

  drogon::app().registerHandler(
      "/v1/chat/completions",
      [this](const HttpRequestPtr& req, ResponseCallback&& callback) {
        handle_chat(req, std::move(callback));
      },
      {drogon::Post});

  // 注意：函数式 registerHandler 不支持路径参数（{1} 是 query 参数占位符，
  // 只有 controller 的 METHOD_ADD 才支持路径参数），所以这里用正则路由，
  // session_id 在 handle_history 里自己从 path 解析。
  drogon::app().registerHandlerViaRegex(
      R"(/v1/sessions/([A-Za-z0-9_\-]+)/messages)",
      [this](const HttpRequestPtr& req, ResponseCallback&& callback) {
        handle_history(req, std::move(callback));
      },
      {drogon::Get});

  drogon::app().setDefaultHandler(
      [this](const HttpRequestPtr& req, ResponseCallback&& callback) {
        // 兜底：以后往 web/ 里加新资源，不注册路由也能访问。
        if (config.web_enabled && req->method() == drogon::Get &&
            web::find_asset(req->path()) != nullptr) {
          handle_static(req, std::move(callback));
          return;
        }
        callback(error_response(drogon::k404NotFound, "not_found", "接口不存在: " + req->path()));
      });
}

void ApiServer::Impl::pre_routing(const HttpRequestPtr& req, drogon::AdviceCallback&& callback,
                                  drogon::AdviceChainCallback&& chain) {
  if (req->method() == drogon::Options) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k204NoContent);
    callback(resp);
    return;
  }

  if (shutting_down.load(std::memory_order_acquire) && req->path() != "/healthz") {
    callback(error_response(drogon::k503ServiceUnavailable, "shutting_down",
                            "服务正在退出，请稍后重试"));
    return;
  }

  if (!config.auth_enabled() || is_public_path(config, req->path())) {
    chain();
    return;
  }

  // Drogon 会把 header 名统一转小写，这里用小写的键查。
  const std::string expected = "Bearer " + config.api_token;
  if (req->getHeader("authorization") == expected) {
    chain();
    return;
  }

  callback(error_response(drogon::k401Unauthorized, "unauthorized",
                          "缺少或错误的 Authorization: Bearer <token>"));
}

HttpResponsePtr ApiServer::Impl::health_response() {
  json body{{"status", "ok"},
            {"sessions", store.session_count()},
            {"active_streams", gate.active()},
            {"max_streams", max_streams},
            {"model", config.model},
            {"provider", config.provider},
            {"web", config.web_enabled},
            {"http", "drogon"}};
  if (shutting_down.load(std::memory_order_acquire)) {
    // 保持 200：避免把 liveness 探针也打挂导致被强杀；对接负载均衡时看 draining 字段摘流量。
    body["status"] = "draining";
    body["draining"] = true;
  }
  return json_ok(body);
}

HttpResponsePtr ApiServer::Impl::make_sse_response(std::shared_ptr<llm::Stream> stream,
                                                   std::string id, std::string model,
                                                   std::shared_ptr<StreamSlot> slot) {
  // disableKickoffTimeout=true：真实模型首字可能要十几秒，不能让 trantor 的 kickoff
  // 超时把还没出字的流掐掉（我们有 15s 心跳会主动探测连接）。
  auto resp = drogon::HttpResponse::newAsyncStreamResponse(
      [this, stream = std::move(stream), id = std::move(id), model = std::move(model),
       slot = std::move(slot)](drogon::ResponseStreamPtr out) mutable {
        auto pump = std::make_shared<SsePump>(std::move(stream), std::move(out), std::move(id),
                                              std::move(model), heartbeat, std::move(slot));
        pumps.add(pump);
        pump->start();
      },
      /*disableKickoffTimeout=*/true);

  resp->setContentTypeString("text/event-stream");
  resp->addHeader("Cache-Control", "no-cache, no-transform");
  resp->addHeader("X-Accel-Buffering", "no");
  return resp;
}

void ApiServer::Impl::handle_chat(const HttpRequestPtr& req, ResponseCallback&& callback) {
  json body;
  try {
    body = json::parse(std::string(req->body()));
  } catch (const std::exception&) {
    callback(error_response(drogon::k400BadRequest, "invalid_json", "请求体不是合法 JSON"));
    return;
  }
  if (!body.is_object()) {
    callback(error_response(drogon::k400BadRequest, "invalid_json", "请求体必须是 JSON 对象"));
    return;
  }

  std::string user_text;
  if (body.contains("message") && body["message"].is_string()) {
    user_text = body["message"].get<std::string>();
  } else if (body.contains("messages") && body["messages"].is_array()) {
    // 兼容 OpenAI 原生格式：取最后一条 user 作为本轮输入。
    for (const auto& message : body["messages"]) {
      if (message.is_object() && message.value("role", "") == "user" &&
          message.contains("content") && message["content"].is_string()) {
        user_text = message["content"].get<std::string>();
      }
    }
  }
  if (is_blank(user_text)) {
    callback(error_response(drogon::k400BadRequest, "invalid_request",
                            "缺少 message 字段（或 messages 中最后一条 user 消息为空）"));
    return;
  }
  if (user_text.size() > kMaxMessageBytes) {
    callback(error_response(drogon::k413RequestEntityTooLarge, "payload_too_large",
                            "单条消息超过 32KB 限制"));
    return;
  }

  // 逐字段校验类型：body.value() 在字段类型不对时会抛 type_error（→ 500），
  // 客户端传错类型应该得到 400 而不是内部错误。
  std::string session_id;
  if (body.contains("session_id")) {
    if (!body["session_id"].is_string()) {
      callback(error_response(drogon::k400BadRequest, "invalid_request",
                              "session_id 必须是字符串"));
      return;
    }
    session_id = body["session_id"].get<std::string>();
  }
  std::string model;
  if (body.contains("model")) {
    if (!body["model"].is_string()) {
      callback(error_response(drogon::k400BadRequest, "invalid_request", "model 必须是字符串"));
      return;
    }
    model = body["model"].get<std::string>();
  }
  bool stream = true;
  if (body.contains("stream")) {
    if (!body["stream"].is_boolean()) {
      callback(error_response(drogon::k400BadRequest, "invalid_request",
                              "stream 必须是布尔值"));
      return;
    }
    stream = body["stream"].get<bool>();
  }

  const std::string model_name = model.empty() ? config.model : model;

  if (!stream) {
    auto shared_callback = std::make_shared<ResponseCallback>(std::move(callback));
    const bool queued =
        workers.submit([this, session_id, user_text, model, model_name, shared_callback] {
          handle_non_stream(session_id, user_text, model, model_name, shared_callback);
        });
    if (!queued) {
      (*shared_callback)(error_response(drogon::k503ServiceUnavailable, "server_busy",
                                        "非流式请求排队已满，请稍后重试"));
    }
    return;
  }

  if (!gate.try_enter(max_streams)) {
    callback(error_response(drogon::k503ServiceUnavailable, "too_many_streams",
                            "并发流已达上限，请稍后重试（当前上限 " +
                                std::to_string(max_streams) + "）"));
    return;
  }
  auto slot = std::make_shared<StreamSlot>(gate);

  chat::ChatService::Turn turn;
  try {
    turn = service.start(session_id, user_text, model);
  } catch (const std::exception& e) {
    platform::log_error(std::string("启动对话失败: ") + e.what());
    // slot 随作用域释放，名额不会泄漏。
    callback(error_response(drogon::k502BadGateway, "upstream_error", "无法连接模型服务"));
    return;
  }

  auto resp = make_sse_response(std::move(turn.stream), util::random_id("chatcmpl-"), model_name,
                                std::move(slot));
  resp->addHeader("X-Session-Id", turn.session_id);
  callback(resp);
}

void ApiServer::Impl::handle_non_stream(const std::string& session_id,
                                        const std::string& user_text, const std::string& model,
                                        const std::string& model_name,
                                        const std::shared_ptr<ResponseCallback>& callback) {
  chat::ChatService::Turn turn;
  try {
    turn = service.start(session_id, user_text, model);
  } catch (const std::exception& e) {
    platform::log_error(std::string("启动对话失败: ") + e.what());
    post(callback, error_response(drogon::k502BadGateway, "upstream_error", "无法连接模型服务"));
    return;
  }

  std::string answer;
  std::string error;
  std::string finish_reason = "stop";
  int prompt_tokens = -1;
  int completion_tokens = -1;

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(config.request_timeout_s);
  while (std::chrono::steady_clock::now() < deadline) {
    llm::Event ev;
    const llm::Pull pull = turn.stream->next(ev, heartbeat);
    if (pull == llm::Pull::Timeout) continue;
    if (pull == llm::Pull::Cancelled || pull == llm::Pull::End) break;
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
    post(callback, error_response(drogon::k502BadGateway, "upstream_error", error));
    return;
  }
  if (answer.empty()) {
    post(callback, error_response(drogon::k504GatewayTimeout, "timeout",
                                  "模型未在超时时间内返回内容"));
    return;
  }

  json out{{"id", util::random_id("chatcmpl-")},
           {"object", "chat.completion"},
           {"created", now_s()},
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

  auto resp = json_ok(out);
  resp->addHeader("X-Session-Id", turn.session_id);
  post(callback, std::move(resp));
}

void ApiServer::Impl::handle_static(const HttpRequestPtr& req, ResponseCallback&& callback) {
  if (!config.web_enabled) {
    callback(error_response(drogon::k404NotFound, "not_found", "Web UI 已禁用（AIP_WEB=false）"));
    return;
  }
  const web::Asset* asset = web::find_asset(req->path());
  if (asset == nullptr) {
    callback(error_response(drogon::k404NotFound, "not_found", "资源不存在: " + req->path()));
    return;
  }
  callback(asset_response(*asset));
}

void ApiServer::Impl::handle_history(const HttpRequestPtr& req, ResponseCallback&& callback) {
  // 路径形如 /v1/sessions/<id>/messages，正则路由不提供捕获组，这里自己切。
  static constexpr std::string_view kPrefix = "/v1/sessions/";
  static constexpr std::string_view kSuffix = "/messages";
  const std::string& path = req->path();
  std::string session_id;
  if (path.size() > kPrefix.size() + kSuffix.size() && path.compare(0, kPrefix.size(), kPrefix) == 0 &&
      path.compare(path.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
    session_id = path.substr(kPrefix.size(), path.size() - kPrefix.size() - kSuffix.size());
  }

  const auto messages = store.history(session_id, 100);

  json list = json::array();
  for (const auto& message : messages) {
    list.push_back({{"role", message.role}, {"content", message.content}});
  }
  callback(json_ok({{"session_id", session_id}, {"messages", list}}));
}

void ApiServer::Impl::post(const std::shared_ptr<ResponseCallback>& callback,
                           HttpResponsePtr resp) {
  // 工作线程不能直接调 callback：必须回到 event loop 里交回响应。
  auto* loop = drogon::app().getLoop();
  if (loop == nullptr) return;  // 已经开始退出，丢掉即可。
  loop->queueInLoop([callback, resp = std::move(resp)] { (*callback)(resp); });
}

bool ApiServer::Impl::run() {
  setup();

  drogon::app().setTermSignalHandler([this] { begin_shutdown(); });
  drogon::app().setIntSignalHandler([this] { begin_shutdown(); });

  // Drogon 1.9.x 的 app().run() 返回 void：进程退出码由 main 决定，
  // 端口占用这类失败会在 run() 里直接打日志并退出。
  drogon::app().run();

  // 走到这里 event loop 已经停了：在途泵的 send 会失败并 cancel，然后 join 掉。
  pumps.drain();
  workers.stop();
  return true;
}

void ApiServer::Impl::begin_shutdown() {
  if (shutting_down.exchange(true, std::memory_order_acq_rel)) {
    platform::log_warn("再次收到退出信号：立即退出");
    drogon::app().quit();
    return;
  }

  platform::log_info("收到退出信号：停止接收新对话，等待在途流收尾（最长 " +
                     std::to_string(config.shutdown_drain_s) + "s）");

  std::thread([this] {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(std::max(0, config.shutdown_drain_s));
    while (gate.active() > 0 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (gate.active() > 0) {
      platform::log_warn("退出超时：仍有 " + std::to_string(gate.active()) +
                         " 路流未结束，强制关闭");
    } else {
      platform::log_info("在途流已全部结束，退出");
    }
    drogon::app().quit();
  }).detach();
}

ApiServer::ApiServer(const platform::Config& config, chat::ChatService& service,
                     chat::SessionStore& store)
    : impl_(std::make_unique<Impl>(config, service, store)) {}

ApiServer::~ApiServer() = default;

bool ApiServer::run() {
  platform::log_info("HTTP 服务启动（Drogon）: http://" + impl_->config.host + ":" +
                     std::to_string(impl_->config.port) + "  io_threads=" +
                     std::to_string(impl_->io_threads()) + "  max_streams=" +
                     std::to_string(impl_->max_streams));
  if (impl_->config.auth_enabled()) {
    platform::log_info("已启用 Bearer Token 鉴权（AIP_TOKEN）");
  } else {
    platform::log_warn("未设置 AIP_TOKEN：接口无鉴权，请勿直接暴露到公网");
  }
  return impl_->run();
}

void ApiServer::stop() {
  // 未启动或已退出时不要碰 Drogon 的 event loop。
  if (drogon::app().isRunning()) impl_->begin_shutdown();
}

bool ApiServer::is_running() const { return drogon::app().isRunning(); }

std::size_t ApiServer::active_streams() const { return impl_->gate.active(); }

}  // namespace http
