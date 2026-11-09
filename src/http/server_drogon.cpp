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
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "auth/principal.h"
#include "chat/errors.h"
#include "chat/replay_stream.h"
#include "chat/token_estimate.h"
#include "common/log.h"
#include "common/metrics.h"
#include "common/structured_log.h"
#include "http/auth_json.h"
#include "http/public_config.h"
#include "http/request_context.h"
#include "http/session_json.h"
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

void add_cors(const HttpResponsePtr& resp) {
  resp->addHeader("Access-Control-Allow-Origin", "*");
  resp->addHeader("Access-Control-Allow-Headers", "Authorization, Content-Type, X-Request-Id");
  resp->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  resp->addHeader("Access-Control-Expose-Headers", "X-Session-Id, X-Request-Id");
}

HttpResponsePtr quota_error(const quota::QuotaDecision& decision) {
  auto resp = error_response(drogon::k429TooManyRequests, decision.type, decision.message);
  resp->addHeader("Retry-After", std::to_string(std::max(1, decision.retry_after_s)));
  return resp;
}

int estimate_request_tokens(const std::string& text, int max_tokens) {
  return chat::estimate_tokens(text) + std::max(0, max_tokens) + 8;
}

HttpResponsePtr idempotency_busy_response() {
  auto resp = error_response(drogon::k409Conflict, "request_in_progress",
                             "相同 request_id 的请求正在处理中，请稍后重试");
  resp->addHeader("Retry-After", "1");
  return resp;
}

HttpResponsePtr idempotency_conflict_response() {
  return error_response(drogon::k409Conflict, "idempotency_conflict",
                        "该 request_id 已完成，但原始回复已不可用");
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
        metrics::set_active_streams(active_.load(std::memory_order_acquire));
        return true;
      }
    }
    return false;
  }

  void leave() {
    active_.fetch_sub(1, std::memory_order_acq_rel);
    metrics::set_active_streams(active_.load(std::memory_order_acquire));
  }

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
          std::shared_ptr<StreamSlot> slot, std::string request_id, std::int64_t user_id,
          std::string session_id, int prompt_tokens_estimate, bool include_usage)
      : stream_(std::move(stream)),
        out_(std::move(out)),
        id_(std::move(id)),
        model_(std::move(model)),
        heartbeat_(heartbeat),
        slot_(std::move(slot)),
        request_id_(std::move(request_id)),
        user_id_(user_id),
        session_id_(std::move(session_id)),
        prompt_tokens_estimate_(prompt_tokens_estimate),
        include_usage_(include_usage) {}

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
  void add_text(const std::string& text) {
    for (const unsigned char c : text) {
      if (c < 0x80) {
        ++ascii_bytes_;
      } else if ((c & 0xC0) != 0x80) {
        ++non_ascii_codepoints_;
      }
    }
  }

  int estimated_completion_tokens() const {
    const int ascii_tokens = static_cast<int>((ascii_bytes_ + 3) / 4);
    return ascii_tokens + static_cast<int>(non_ascii_codepoints_);
  }

  void run() {
    const auto started = std::chrono::steady_clock::now();
    const long long created = now_s();
    bool sent_role = false;
    bool cancelled = false;
    bool saw_error = false;

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
          if (!first_token_at_.has_value()) first_token_at_ = std::chrono::steady_clock::now();
          add_text(ev.text);
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
          finish_reason_ = ev.finish_reason.empty() ? "stop" : ev.finish_reason;
          prompt_tokens_ =
              ev.prompt_tokens >= 0 ? ev.prompt_tokens : prompt_tokens_estimate_;
          completion_tokens_ = ev.completion_tokens >= 0 ? ev.completion_tokens
                                                         : estimated_completion_tokens();
          json chunk = chunk_base(id_, model_, created);
          chunk["choices"] = json::array(
              {{{"index", 0},
                {"delta", json::object()},
                {"finish_reason", finish_reason_}}});
          if (include_usage_) {
            chunk["usage"] = {{"prompt_tokens", prompt_tokens_},
                              {"completion_tokens", completion_tokens_},
                              {"total_tokens", prompt_tokens_ + completion_tokens_}};
          }
          frame += sse_pack(chunk.dump());
          frame += sse_pack("[DONE]");
          terminal = true;
          break;
        }
        case llm::EventKind::Error: {
          saw_error = true;
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

    if (prompt_tokens_ < 0) prompt_tokens_ = prompt_tokens_estimate_;
    if (completion_tokens_ < 0) completion_tokens_ = estimated_completion_tokens();

    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    const int ttft_ms =
        first_token_at_.has_value()
            ? static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   *first_token_at_ - started)
                                   .count())
            : -1;
    const std::string outcome = cancelled ? "cancelled" : (saw_error ? "error" : "completed");

    metrics::inc_chat_request(/*stream=*/true, outcome);
    metrics::observe_chat_latency(/*stream=*/true,
                                  std::chrono::duration<double>(elapsed).count());
    if (first_token_at_.has_value()) {
      metrics::observe_ttft(
          model_, std::chrono::duration<double>(*first_token_at_ - started).count());
    }
    metrics::inc_tokens(model_, "prompt", prompt_tokens_);
    metrics::inc_tokens(model_, "completion", completion_tokens_);

    platform::log_event_info(
        "chat_stream",
        json{{"request_id", request_id_},
             {"session_id", session_id_},
             {"user_id", user_id_},
             {"model", model_},
             {"stream", true},
             {"outcome", outcome},
             {"finish_reason", finish_reason_},
             {"duration_ms", elapsed_ms},
             {"ttft_ms", ttft_ms},
             {"prompt_tokens", prompt_tokens_},
             {"completion_tokens", completion_tokens_}});
  }

  std::shared_ptr<llm::Stream> stream_;
  drogon::ResponseStreamPtr out_;
  const std::string id_;
  const std::string model_;
  const std::chrono::milliseconds heartbeat_;
  std::shared_ptr<StreamSlot> slot_;
  const std::string request_id_;
  const std::int64_t user_id_;
  const std::string session_id_;
  const int prompt_tokens_estimate_;
  const bool include_usage_;
  std::size_t ascii_bytes_ = 0;
  std::size_t non_ascii_codepoints_ = 0;
  int prompt_tokens_ = -1;
  int completion_tokens_ = -1;
  std::string finish_reason_ = "stop";
  std::optional<std::chrono::steady_clock::time_point> first_token_at_;
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
  Impl(const platform::Config& config, chat::ChatService& service, chat::SessionStore& store,
       auth::AuthService* auth, quota::QuotaService* quota_service,
       usage::UsageSink* usage_sink, chat::IdempotencyStore* idempotency)
      : config(config),
        service(service),
        store(store),
        auth_(auth),
        quota_(quota_service),
        usage_(usage_sink),
        idempotency_(idempotency),
        heartbeat(std::chrono::seconds(std::max(1, config.heartbeat_s))),
        max_streams(static_cast<std::size_t>(std::max(1, config.max_streams))),
        workers(default_worker_count(), kNonStreamQueueSize),
        db_workers(4, kNonStreamQueueSize * 2) {}

  void setup();
  bool run();
  void begin_shutdown();

  std::size_t io_threads() const;
  HttpResponsePtr health_response();
  HttpResponsePtr ready_response();
  HttpResponsePtr metrics_response(const HttpRequestPtr& req);
  HttpResponsePtr make_sse_response(std::shared_ptr<llm::Stream> stream, std::string id,
                                    std::string model, std::shared_ptr<StreamSlot> slot,
                                    std::string request_id, std::int64_t user_id,
                                    std::string session_id, int prompt_tokens_estimate,
                                    bool include_usage);
  void pre_routing(const HttpRequestPtr& req, drogon::AdviceCallback&& callback,
                   drogon::AdviceChainCallback&& chain);
  auth::Principal principal_from(const HttpRequestPtr& req) const;
  void handle_chat(const HttpRequestPtr& req, ResponseCallback&& callback);
  void handle_non_stream(const auth::Principal& principal, const std::string& session_id,
                         const std::string& user_text, const std::string& model,
                         const std::string& model_name, const std::string& request_id,
                         const std::string& trace_id,
                         const std::shared_ptr<ResponseCallback>& callback);
  void handle_history(const HttpRequestPtr& req, ResponseCallback&& callback);
  void handle_sessions_list(const HttpRequestPtr& req, ResponseCallback&& callback);
  void handle_session_update(const HttpRequestPtr& req, ResponseCallback&& callback);
  void handle_static(const HttpRequestPtr& req, ResponseCallback&& callback);
  void handle_auth_register(const HttpRequestPtr& req, ResponseCallback&& callback);
  void handle_auth_login(const HttpRequestPtr& req, ResponseCallback&& callback);
  void handle_auth_refresh(const HttpRequestPtr& req, ResponseCallback&& callback);
  void handle_auth_me(const HttpRequestPtr& req, ResponseCallback&& callback);
  void handle_auth_list_api_keys(const HttpRequestPtr& req, ResponseCallback&& callback);
  void handle_auth_create_api_key(const HttpRequestPtr& req, ResponseCallback&& callback);
  void handle_auth_delete_api_key(const HttpRequestPtr& req, ResponseCallback&& callback);
  void post_auth_error(const std::shared_ptr<ResponseCallback>& callback,
                       const auth::AuthError& error);
  void post(const std::shared_ptr<ResponseCallback>& callback, HttpResponsePtr resp);

  const platform::Config& config;
  chat::ChatService& service;
  chat::SessionStore& store;
  auth::AuthService* auth_ = nullptr;
  quota::QuotaService* quota_ = nullptr;
  usage::UsageSink* usage_ = nullptr;
  chat::IdempotencyStore* idempotency_ = nullptr;
  const std::chrono::milliseconds heartbeat;
  const std::size_t max_streams;

  StreamGate gate;
  PumpRegistry pumps;
  WorkerPool workers;
  WorkerPool db_workers;
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

        std::string trace_id;
        std::int64_t user_id = 0;
        std::string auth_type;
        double duration_s = 0.0;
        if (const auto& attrs = req->attributes(); attrs) {
          if (attrs->find("request_id")) trace_id = attrs->get<std::string>("request_id");
          if (attrs->find("request_started")) {
            const auto started =
                attrs->get<std::chrono::steady_clock::time_point>("request_started");
            duration_s =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
                    .count();
          }
          if (attrs->find("principal")) {
            const auto& principal = attrs->get<auth::Principal>("principal");
            user_id = principal.user_id;
            auth_type = principal.auth_type;
          }
        }
        if (trace_id.empty()) trace_id = util::random_id("req_");
        resp->addHeader("X-Request-Id", trace_id);

        const int status = static_cast<int>(resp->statusCode());
        const std::string route = route_label(req->path());
        metrics::inc_http_request(req->methodString(), route, status);
        metrics::observe_http_duration(req->methodString(), route, duration_s);
        platform::log_event_info(
            "http_request",
            json{{"request_id", trace_id},
                 {"method", req->methodString()},
                 {"route", route},
                 {"path", req->path()},
                 {"status", status},
                 {"duration_ms", static_cast<long long>(duration_s * 1000.0)},
                 {"user_id", user_id},
                 {"auth_type", auth_type}});
      });

  drogon::app().registerHandler(
      "/healthz",
      [this](const HttpRequestPtr&, ResponseCallback&& callback) {
        callback(health_response());
      },
      {drogon::Get});

  drogon::app().registerHandler(
      "/readyz",
      [this](const HttpRequestPtr&, ResponseCallback&& callback) {
        callback(ready_response());
      },
      {drogon::Get});

  drogon::app().registerHandler(
      "/metrics",
      [this](const HttpRequestPtr& req, ResponseCallback&& callback) {
        callback(metrics_response(req));
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

#ifdef AIP_WITH_AUTH
  drogon::app().registerHandler(
      "/v1/auth/register",
      [this](const HttpRequestPtr& req, ResponseCallback&& callback) {
        handle_auth_register(req, std::move(callback));
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/v1/auth/login",
      [this](const HttpRequestPtr& req, ResponseCallback&& callback) {
        handle_auth_login(req, std::move(callback));
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/v1/auth/refresh",
      [this](const HttpRequestPtr& req, ResponseCallback&& callback) {
        handle_auth_refresh(req, std::move(callback));
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/v1/auth/me",
      [this](const HttpRequestPtr& req, ResponseCallback&& callback) {
        handle_auth_me(req, std::move(callback));
      },
      {drogon::Get});

  drogon::app().registerHandler(
      "/v1/auth/api-keys",
      [this](const HttpRequestPtr& req, ResponseCallback&& callback) {
        handle_auth_list_api_keys(req, std::move(callback));
      },
      {drogon::Get});

  drogon::app().registerHandler(
      "/v1/auth/api-keys",
      [this](const HttpRequestPtr& req, ResponseCallback&& callback) {
        handle_auth_create_api_key(req, std::move(callback));
      },
      {drogon::Post});

  drogon::app().registerHandlerViaRegex(
      R"(/v1/auth/api-keys/([0-9]+))",
      [this](const HttpRequestPtr& req, ResponseCallback&& callback) {
        handle_auth_delete_api_key(req, std::move(callback));
      },
      {drogon::Delete});

#endif

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

  drogon::app().registerHandler(
      "/v1/sessions",
      [this](const HttpRequestPtr& req, ResponseCallback&& callback) {
        handle_sessions_list(req, std::move(callback));
      },
      {drogon::Get});

  drogon::app().registerHandlerViaRegex(
      R"(/v1/sessions/([A-Za-z0-9_\-]+))",
      [this](const HttpRequestPtr& req, ResponseCallback&& callback) {
        handle_session_update(req, std::move(callback));
      },
      {drogon::Patch, drogon::Delete});

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
  const std::string trace_id = ensure_request_id(req->getHeader("x-request-id"));
  req->attributes()->insert("request_id", trace_id);
  req->attributes()->insert("request_started", std::chrono::steady_clock::now());

  if (req->method() == drogon::Options) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k204NoContent);
    callback(resp);
    return;
  }

  if (shutting_down.load(std::memory_order_acquire) && req->path() != "/healthz" &&
      req->path() != "/readyz") {
    metrics::set_ready(false);
    callback(error_response(drogon::k503ServiceUnavailable, "shutting_down",
                            "服务正在退出，请稍后重试"));
    return;
  }

  if (!config.auth_enabled() || is_public_path(config, req->path())) {
    chain();
    return;
  }

  const auto deny = [&](drogon::HttpStatusCode status, const std::string& type,
                        const std::string& message, const char* reason) {
    metrics::inc_auth_failure(reason);
    platform::log_event_info("auth_failure", json{{"request_id", trace_id},
                                                  {"path", req->path()},
                                                  {"reason", reason}});
    callback(error_response(status, type, message));
  };

  const auto token = bearer_token(req->getHeader("authorization"));
  if (!token.has_value()) {
    deny(drogon::k401Unauthorized, "unauthorized", "缺少 Authorization: Bearer <token>",
         "missing_token");
    return;
  }

  // 静态管理员 Token 不需要查库，直接放行。
  if (!config.api_token.empty() && token_equals(*token, config.api_token)) {
    auth::Principal principal;
    principal.is_admin = true;
    principal.auth_type = "static";
    req->attributes()->insert("principal", principal);
    chain();
    return;
  }

#ifdef AIP_WITH_AUTH
  if (auth_ == nullptr) {
    deny(drogon::k401Unauthorized, "unauthorized",
         "Token 无效（当前构建未启用用户鉴权）", "auth_unavailable");
    return;
  }

  auto chain_ptr = std::make_shared<drogon::AdviceChainCallback>(std::move(chain));
  auto callback_ptr = std::make_shared<drogon::AdviceCallback>(std::move(callback));
  const HttpRequestPtr req_ptr = req;
  const std::string token_value = *token;

  // JWT/API Key 需要查 users/api_keys；放到 db_workers，避免阻塞 IO 线程。
  const bool queued = db_workers.submit(
      [this, token_value, req_ptr, trace_id, chain_ptr, callback_ptr] {
        std::optional<auth::Principal> principal;
        std::optional<HttpResponsePtr> auth_error;
        try {
          principal = auth_->authenticate_bearer(token_value);
        }
        catch (const std::exception& e) {
          platform::log_error(std::string("鉴权失败（存储）: ") + e.what());
          auth_error = error_response(drogon::k503ServiceUnavailable, "storage_unavailable",
                                      "鉴权服务暂时不可用，请稍后重试");
        }

        auto* loop = drogon::app().getLoop();
        if (loop == nullptr) return;
        loop->queueInLoop([req_ptr, principal, auth_error, trace_id, chain_ptr, callback_ptr] {
          if (auth_error.has_value()) {
            (*callback_ptr)(*auth_error);
            return;
          }
          if (!principal.has_value()) {
            metrics::inc_auth_failure("invalid_token");
            platform::log_event_info(
                "auth_failure",
                json{{"request_id", trace_id},
                     {"path", req_ptr->path()},
                     {"reason", "invalid_token"}});
            (*callback_ptr)(error_response(drogon::k401Unauthorized, "unauthorized",
                                           "Token 无效或已过期"));
            return;
          }
          req_ptr->attributes()->insert("principal", *principal);
          (*chain_ptr)();
        });
      });

  if (!queued) {
    (*callback_ptr)(error_response(drogon::k503ServiceUnavailable, "server_busy",
                                   "鉴权排队已满，请稍后重试"));
  }
#else
  callback(error_response(drogon::k401Unauthorized, "unauthorized",
                          "Token 无效（当前构建未启用用户鉴权）"));
#endif
}

auth::Principal ApiServer::Impl::principal_from(const HttpRequestPtr& req) const {
  const auto& attributes = req->attributes();
  if (attributes && attributes->find("principal")) {
    return attributes->get<auth::Principal>("principal");
  }

  // 未启用鉴权时，本地请求视为管理员；启用鉴权但没写入 principal 时，
  // 返回未绑定用户的普通身份，ChatService 会拒绝它。
  auth::Principal principal;
  principal.is_admin = !config.auth_enabled();
  principal.auth_type = config.auth_enabled() ? "none" : "local";
  return principal;
}

HttpResponsePtr ApiServer::Impl::health_response() {
  std::size_t sessions = 0;
  bool storage_ok = true;
  try {
    sessions = store.session_count();
  } catch (const std::exception& e) {
    storage_ok = false;
    platform::log_warn(std::string("健康检查读取会话数失败: ") + e.what());
  }

  json body{{"status", storage_ok ? "ok" : "degraded"},
            {"sessions", sessions},
            {"active_streams", gate.active()},
            {"max_streams", max_streams},
            {"model", config.model},
            {"provider", config.provider},
            {"storage", config.storage},
            {"storage_ok", storage_ok},
            {"web", config.web_enabled},
            {"http", "drogon"}};
  if (shutting_down.load(std::memory_order_acquire)) {
    // 保持 200：避免把 liveness 探针也打挂导致被强杀；对接负载均衡时看 draining 字段摘流量。
    body["status"] = "draining";
    body["draining"] = true;
  }
  return json_ok(body);
}

HttpResponsePtr ApiServer::Impl::ready_response() {
  bool storage_ok = true;
  try {
    (void)store.session_count();
  } catch (const std::exception& e) {
    storage_ok = false;
    platform::log_warn(std::string("就绪检查读取会话数失败: ") + e.what());
  }
  const bool ready = storage_ok && !shutting_down.load(std::memory_order_acquire);
  metrics::set_ready(ready);
  if (!ready) {
    return error_response(drogon::k503ServiceUnavailable, "not_ready",
                          storage_ok ? "服务正在退出" : "存储暂时不可用");
  }
  return json_ok(json{{"status", "ready"}, {"storage_ok", true}});
}

HttpResponsePtr ApiServer::Impl::metrics_response(const HttpRequestPtr& req) {
  if (!config.metrics_enabled) {
    return error_response(drogon::k404NotFound, "not_found", "指标端点已禁用");
  }
  if (!config.metrics_token.empty()) {
    const auto token = bearer_token(req->getHeader("authorization"));
    if (!token.has_value() || !token_equals(*token, config.metrics_token)) {
      metrics::inc_auth_failure("metrics_token");
      return error_response(drogon::k401Unauthorized, "unauthorized",
                            "缺少或无效的指标 Token");
    }
  }
  auto resp = body_response(metrics::render_prometheus(),
                            "text/plain; version=0.0.4; charset=utf-8");
  resp->addHeader("Cache-Control", "no-store");
  return resp;
}

HttpResponsePtr ApiServer::Impl::make_sse_response(
    std::shared_ptr<llm::Stream> stream, std::string id, std::string model,
    std::shared_ptr<StreamSlot> slot, std::string request_id, std::int64_t user_id,
    std::string session_id, int prompt_tokens_estimate, bool include_usage) {
  // disableKickoffTimeout=true：真实模型首字可能要十几秒，不能让 trantor 的 kickoff
  // 超时把还没出字的流掐掉（我们有 15s 心跳会主动探测连接）。
  auto resp = drogon::HttpResponse::newAsyncStreamResponse(
      [this, stream = std::move(stream), id = std::move(id), model = std::move(model),
       slot = std::move(slot), request_id = std::move(request_id), user_id,
       session_id = std::move(session_id), prompt_tokens_estimate,
       include_usage](drogon::ResponseStreamPtr out) mutable {
        auto pump = std::make_shared<SsePump>(
            std::move(stream), std::move(out), std::move(id), std::move(model), heartbeat,
            std::move(slot), std::move(request_id), user_id, std::move(session_id),
            prompt_tokens_estimate, include_usage);
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

  std::string request_id;
  if (body.contains("request_id")) {
    if (!body["request_id"].is_string()) {
      callback(error_response(drogon::k400BadRequest, "invalid_request",
                              "request_id 必须是字符串"));
      return;
    }
    request_id = body["request_id"].get<std::string>();
    if (request_id.empty() || request_id.size() > 128) {
      callback(error_response(drogon::k400BadRequest, "invalid_request",
                              "request_id 长度应在 1~128"));
      return;
    }
  }

  const std::string model_name = model.empty() ? config.model : model;
  const auth::Principal principal = principal_from(req);
  std::string trace_id;
  if (const auto& attrs = req->attributes(); attrs && attrs->find("request_id")) {
    trace_id = attrs->get<std::string>("request_id");
  }

  if (!stream) {
    auto shared_callback = std::make_shared<ResponseCallback>(std::move(callback));
    const bool queued = workers.submit(
        [this, principal, session_id, user_text, model, model_name, request_id, trace_id,
         shared_callback] {
          handle_non_stream(principal, session_id, user_text, model, model_name, request_id,
                            trace_id, shared_callback);
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
  auto shared_callback = std::make_shared<ResponseCallback>(std::move(callback));

  // 会话读写放到独立线程池：Drogon 的 IO 线程只做 epoll/send，不在这里等 PostgreSQL。
  const bool queued = db_workers.submit(
      [this, principal, session_id, user_text, model, model_name, request_id, trace_id, slot,
       shared_callback] {
        chat::ChatService::Turn turn;
        const std::string scope = principal.scope();
        auto mark_failed = [this, &scope, &request_id](const std::string& type,
                                                       const std::string& message) {
          if (request_id.empty() || idempotency_ == nullptr) return;
          try {
            idempotency_->fail(scope, request_id, type, message);
          } catch (const std::exception& e) {
            platform::log_warn(std::string("写幂等失败状态失败: ") + e.what());
          }
        };

        try {
          if (!request_id.empty() && idempotency_ != nullptr) {
            const chat::DedupRecord dedup = idempotency_->claim(scope, request_id);
            if (dedup.status == chat::DedupStatus::InProgress) {
              post(shared_callback, idempotency_busy_response());
              return;
            }
            if (dedup.status == chat::DedupStatus::Completed) {
              const auto message = store.message(dedup.assistant_message_id);
              if (!message.has_value()) {
                idempotency_->remove(scope, request_id);
                post(shared_callback, idempotency_conflict_response());
                return;
              }
              auto replay = std::make_shared<chat::ReplayStream>(message->content);
              auto* replay_loop = drogon::app().getLoop();
              if (replay_loop == nullptr) return;
              replay_loop->queueInLoop(
                  [this, replay, dedup, model_name, trace_id, principal, slot,
                   shared_callback]() mutable {
                    auto resp = make_sse_response(
                        replay, util::random_id("chatcmpl-"), model_name, std::move(slot),
                        trace_id, principal.user_id, dedup.session_id, 0,
                        /*include_usage=*/false);
                    resp->addHeader("X-Session-Id", dedup.session_id);
                    (*shared_callback)(resp);
                  });
              return;
            }
          }

          long long daily_used = 0;
          if (usage_ != nullptr) {
            daily_used = usage_->summary_today(principal.user_id).total_tokens();
          }
          quota::QuotaGuard guard;
          if (quota_ != nullptr) {
            auto acquired = quota_->try_acquire(
                principal, estimate_request_tokens(user_text, config.max_tokens), daily_used);
            if (!acquired.decision.allowed) {
              mark_failed("rate_limited", acquired.decision.message);
              post(shared_callback, quota_error(acquired.decision));
              return;
            }
            guard = std::move(acquired.guard);
          }

          std::shared_ptr<chat::TurnObserver> observer;
          if (!request_id.empty() && idempotency_ != nullptr) {
            observer = std::make_shared<chat::IdempotencyObserver>(*idempotency_, scope,
                                                                  request_id);
          }
          turn = service.start(principal, session_id, user_text, model, request_id,
                               std::move(guard), observer);
        } catch (const chat::ContextTooLong& e) {
          mark_failed("context_too_long", e.what());
          post(shared_callback,
               error_response(drogon::k413RequestEntityTooLarge, "context_too_long", e.what()));
          return;
        } catch (const chat::Forbidden& e) {
          mark_failed("forbidden", e.what());
          post(shared_callback,
               error_response(drogon::k404NotFound, "not_found", e.what()));
          return;
        }
#ifdef AIP_WITH_POSTGRES
        catch (const storage::DbError& e) {
          mark_failed("storage_unavailable", e.what());
          platform::log_error(std::string("启动对话失败（存储）: ") + e.what());
          post(shared_callback,
               error_response(drogon::k503ServiceUnavailable, "storage_unavailable",
                              "会话存储暂时不可用，请稍后重试"));
          return;
        }
#endif
        catch (const std::exception& e) {
          mark_failed("upstream_error", e.what());
          platform::log_error(std::string("启动对话失败: ") + e.what());
          post(shared_callback,
               error_response(drogon::k502BadGateway, "upstream_error", "无法连接模型服务"));
          return;
        }

        auto* loop = drogon::app().getLoop();
        if (loop == nullptr) return;  // 退出中：turn/slot 析构会取消上游并释放名额
        loop->queueInLoop(
            [this, turn = std::move(turn), model_name, trace_id, principal, slot,
             user_text, shared_callback]() mutable {
              auto resp = make_sse_response(
                  std::move(turn.stream), util::random_id("chatcmpl-"), model_name,
                  std::move(slot), trace_id, principal.user_id, turn.session_id,
                  estimate_request_tokens(user_text, config.max_tokens),
                  /*include_usage=*/true);
              resp->addHeader("X-Session-Id", turn.session_id);
              (*shared_callback)(resp);
            });
      });

  if (!queued) {
    slot->release();
    (*shared_callback)(error_response(drogon::k503ServiceUnavailable, "server_busy",
                                      "请求排队已满，请稍后重试"));
  }
}

void ApiServer::Impl::handle_non_stream(const auth::Principal& principal,
                                        const std::string& session_id,
                                        const std::string& user_text, const std::string& model,
                                        const std::string& model_name,
                                        const std::string& request_id,
                                        const std::string& trace_id,
                                        const std::shared_ptr<ResponseCallback>& callback) {
  const auto started = std::chrono::steady_clock::now();
  chat::ChatService::Turn turn;
  const std::string scope = principal.scope();
  auto mark_failed = [this, &scope, &request_id](const std::string& type,
                                                 const std::string& message) {
    if (request_id.empty() || idempotency_ == nullptr) return;
    try {
      idempotency_->fail(scope, request_id, type, message);
    } catch (const std::exception& e) {
      platform::log_warn(std::string("写幂等失败状态失败: ") + e.what());
    }
  };

  try {
    if (!request_id.empty() && idempotency_ != nullptr) {
      const chat::DedupRecord dedup = idempotency_->claim(scope, request_id);
      if (dedup.status == chat::DedupStatus::InProgress) {
        post(callback, idempotency_busy_response());
        return;
      }
      if (dedup.status == chat::DedupStatus::Completed) {
        const auto message = store.message(dedup.assistant_message_id);
        if (!message.has_value()) {
          idempotency_->remove(scope, request_id);
          post(callback, idempotency_conflict_response());
          return;
        }
        json out{{"id", util::random_id("chatcmpl-")},
                 {"object", "chat.completion"},
                 {"created", now_s()},
                 {"model", model_name},
                 {"session_id", dedup.session_id},
                 {"idempotent_replay", true},
                 {"choices",
                  json::array({{{"index", 0},
                                {"message", {{"role", "assistant"}, {"content", message->content}}},
                                {"finish_reason", "stop"}}})}};
        post(callback, json_ok(out));
        return;
      }
    }

    long long daily_used = 0;
    if (usage_ != nullptr) {
      daily_used = usage_->summary_today(principal.user_id).total_tokens();
    }
    quota::QuotaGuard guard;
    if (quota_ != nullptr) {
      auto acquired = quota_->try_acquire(
          principal, estimate_request_tokens(user_text, config.max_tokens), daily_used);
      if (!acquired.decision.allowed) {
        mark_failed("rate_limited", acquired.decision.message);
        post(callback, quota_error(acquired.decision));
        return;
      }
      guard = std::move(acquired.guard);
    }

    std::shared_ptr<chat::TurnObserver> observer;
    if (!request_id.empty() && idempotency_ != nullptr) {
      observer = std::make_shared<chat::IdempotencyObserver>(*idempotency_, scope, request_id);
    }
    turn = service.start(principal, session_id, user_text, model, request_id,
                         std::move(guard), observer);
  } catch (const chat::ContextTooLong& e) {
    mark_failed("context_too_long", e.what());
    post(callback, error_response(drogon::k413RequestEntityTooLarge, "context_too_long", e.what()));
    return;
  } catch (const chat::Forbidden& e) {
    mark_failed("forbidden", e.what());
    post(callback, error_response(drogon::k404NotFound, "not_found", e.what()));
    return;
  }
#ifdef AIP_WITH_POSTGRES
  catch (const storage::DbError& e) {
    mark_failed("storage_unavailable", e.what());
    platform::log_error(std::string("启动对话失败（存储）: ") + e.what());
    post(callback, error_response(drogon::k503ServiceUnavailable, "storage_unavailable",
                                  "会话存储暂时不可用，请稍后重试"));
    return;
  }
#endif
  catch (const std::exception& e) {
    mark_failed("upstream_error", e.what());
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
           {"idempotent_replay", false},
           {"choices",
            json::array({{{"index", 0},
                          {"message", {{"role", "assistant"}, {"content", answer}}},
                          {"finish_reason", finish_reason}}})}};
  if (prompt_tokens >= 0) {
    out["usage"] = {{"prompt_tokens", prompt_tokens},
                    {"completion_tokens", completion_tokens},
                    {"total_tokens", prompt_tokens + completion_tokens}};
  }

  if (prompt_tokens < 0) {
    prompt_tokens = chat::estimate_tokens(user_text) + 8;
  }
  if (completion_tokens < 0) {
    completion_tokens = chat::estimate_tokens(answer);
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
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

  std::size_t limit = 100;
  const std::string limit_text = req->getParameter("limit");
  if (!limit_text.empty()) {
    try {
      const unsigned long parsed = std::stoul(limit_text);
      if (parsed < 1 || parsed > 100) throw std::out_of_range("limit");
      limit = static_cast<std::size_t>(parsed);
    } catch (const std::exception&) {
      callback(error_response(drogon::k400BadRequest, "invalid_request",
                              "limit 必须是 1~100 的整数"));
      return;
    }
  }

  const auth::Principal principal = principal_from(req);
  auto shared_callback = std::make_shared<ResponseCallback>(std::move(callback));
  const bool queued = db_workers.submit([this, session_id, principal, limit, shared_callback] {
    try {
      if (!store.owns(principal.user_id, principal.is_admin, session_id)) {
        post(shared_callback,
             error_response(drogon::k404NotFound, "not_found", "会话不存在或无权访问"));
        return;
      }
      const auto messages = store.history(session_id, limit);
      json list = json::array();
      for (const auto& message : messages) {
        list.push_back({{"role", message.role}, {"content", message.content}});
      }
      post(shared_callback, json_ok({{"session_id", session_id}, {"messages", list}}));
    }
#ifdef AIP_WITH_POSTGRES
    catch (const storage::DbError& e) {
      platform::log_error(std::string("读取会话历史失败（存储）: ") + e.what());
      post(shared_callback,
           error_response(drogon::k503ServiceUnavailable, "storage_unavailable",
                          "会话存储暂时不可用，请稍后重试"));
    }
#endif
    catch (const std::exception& e) {
      platform::log_error(std::string("读取会话历史失败: ") + e.what());
      post(shared_callback,
           error_response(drogon::k500InternalServerError, "internal_error", "读取会话失败"));
    }
  });
  if (!queued) {
    (*shared_callback)(error_response(drogon::k503ServiceUnavailable, "server_busy",
                                      "请求排队已满，请稍后重试"));
  }
}

void ApiServer::Impl::handle_sessions_list(const HttpRequestPtr& req,
                                            ResponseCallback&& callback) {
  const auth::Principal principal = principal_from(req);
  if (!principal.is_admin && principal.user_id <= 0) {
    callback(error_response(drogon::k401Unauthorized, "unauthorized",
                            "该接口需要用户身份（JWT/API Key）"));
    return;
  }

  std::size_t limit = 20;
  std::size_t offset = 0;
  const std::string limit_text = req->getParameter("limit");
  const std::string offset_text = req->getParameter("offset");
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
    callback(error_response(drogon::k400BadRequest, "invalid_request",
                            "limit 应为 1~100，offset 应为 0~100000"));
    return;
  }

  auto shared_callback = std::make_shared<ResponseCallback>(std::move(callback));
  const bool queued = db_workers.submit([this, principal, limit, offset, shared_callback] {
    try {
      auto sessions =
          store.list(principal.user_id, principal.is_admin, limit + 1, offset);
      const bool has_more = sessions.size() > limit;
      if (has_more) sessions.resize(limit);
      json list = json::array();
      for (const auto& session : sessions) list.push_back(session_json(session));
      post(shared_callback, json_ok(json{{"sessions", list}, {"has_more", has_more}}));
    }
#ifdef AIP_WITH_POSTGRES
    catch (const storage::DbError& e) {
      platform::log_error(std::string("读取会话列表失败（存储）: ") + e.what());
      post(shared_callback,
           error_response(drogon::k503ServiceUnavailable, "storage_unavailable",
                          "会话存储暂时不可用，请稍后重试"));
    }
#endif
    catch (const std::exception& e) {
      platform::log_error(std::string("读取会话列表失败: ") + e.what());
      post(shared_callback,
           error_response(drogon::k500InternalServerError, "internal_error", "读取会话列表失败"));
    }
  });
  if (!queued) {
    (*shared_callback)(error_response(drogon::k503ServiceUnavailable, "server_busy",
                                      "请求排队已满，请稍后重试"));
  }
}

void ApiServer::Impl::handle_session_update(const HttpRequestPtr& req,
                                             ResponseCallback&& callback) {
  const auth::Principal principal = principal_from(req);
  if (!principal.is_admin && principal.user_id <= 0) {
    callback(error_response(drogon::k401Unauthorized, "unauthorized",
                            "该接口需要用户身份（JWT/API Key）"));
    return;
  }

  static constexpr std::string_view kPrefix = "/v1/sessions/";
  const std::string& path = req->path();
  if (path.size() <= kPrefix.size() || path.compare(0, kPrefix.size(), kPrefix) != 0 ||
      path.find('/', kPrefix.size()) != std::string::npos) {
    callback(error_response(drogon::k404NotFound, "not_found", "会话不存在"));
    return;
  }
  const std::string session_id = path.substr(kPrefix.size());

  if (req->method() == drogon::Patch) {
    json body;
    try {
      body = json::parse(std::string(req->body()));
    } catch (const std::exception&) {
      callback(error_response(drogon::k400BadRequest, "invalid_json", "请求体不是合法 JSON"));
      return;
    }
    if (!body.is_object() || !body.contains("title") || !body["title"].is_string()) {
      callback(error_response(drogon::k400BadRequest, "invalid_request",
                              "title 必须是字符串"));
      return;
    }
    const std::string title = body["title"].get<std::string>();
    if (title.empty() || title.size() > 200 ||
        title.find_first_not_of(" \t\r\n") == std::string::npos) {
      callback(error_response(drogon::k400BadRequest, "invalid_request",
                              "title 长度应在 1~200，且不能全为空白"));
      return;
    }

    auto shared_callback = std::make_shared<ResponseCallback>(std::move(callback));
    const bool queued =
        db_workers.submit([this, principal, session_id, title, shared_callback] {
          try {
            if (!store.rename(principal.user_id, principal.is_admin, session_id, title)) {
              post(shared_callback,
                   error_response(drogon::k404NotFound, "not_found", "会话不存在或无权访问"));
              return;
            }
            const auto updated = store.get(session_id);
            if (!updated.has_value()) {
              post(shared_callback,
                   error_response(drogon::k404NotFound, "not_found", "会话不存在"));
              return;
            }
            post(shared_callback, json_ok(json{{"session", session_json(*updated)}}));
          }
#ifdef AIP_WITH_POSTGRES
          catch (const storage::DbError& e) {
            platform::log_error(std::string("重命名会话失败（存储）: ") + e.what());
            post(shared_callback,
                 error_response(drogon::k503ServiceUnavailable, "storage_unavailable",
                                "会话存储暂时不可用，请稍后重试"));
          }
#endif
          catch (const std::exception& e) {
            platform::log_error(std::string("重命名会话失败: ") + e.what());
            post(shared_callback,
                 error_response(drogon::k500InternalServerError, "internal_error", "重命名失败"));
          }
        });
    if (!queued) {
      (*shared_callback)(error_response(drogon::k503ServiceUnavailable, "server_busy",
                                        "请求排队已满，请稍后重试"));
    }
    return;
  }

  if (req->method() == drogon::Delete) {
    auto shared_callback = std::make_shared<ResponseCallback>(std::move(callback));
    const bool queued = db_workers.submit([this, principal, session_id, shared_callback] {
      try {
        if (!store.remove(principal.user_id, principal.is_admin, session_id)) {
          post(shared_callback,
               error_response(drogon::k404NotFound, "not_found", "会话不存在或无权访问"));
          return;
        }
        post(shared_callback, json_ok(json{{"deleted", true}, {"id", session_id}}));
      }
#ifdef AIP_WITH_POSTGRES
      catch (const storage::DbError& e) {
        platform::log_error(std::string("删除会话失败（存储）: ") + e.what());
        post(shared_callback,
             error_response(drogon::k503ServiceUnavailable, "storage_unavailable",
                            "会话存储暂时不可用，请稍后重试"));
      }
#endif
      catch (const std::exception& e) {
        platform::log_error(std::string("删除会话失败: ") + e.what());
        post(shared_callback,
             error_response(drogon::k500InternalServerError, "internal_error", "删除失败"));
      }
    });
    if (!queued) {
      (*shared_callback)(error_response(drogon::k503ServiceUnavailable, "server_busy",
                                        "请求排队已满，请稍后重试"));
    }
    return;
  }

  callback(error_response(drogon::k405MethodNotAllowed, "method_not_allowed",
                          "只支持 PATCH / DELETE"));
}

#ifdef AIP_WITH_AUTH
void ApiServer::Impl::post_auth_error(const std::shared_ptr<ResponseCallback>& callback,
                                      const auth::AuthError& error) {
  const AuthHttpError info = auth_http_error(error.code());
  post(callback, error_response(static_cast<drogon::HttpStatusCode>(info.status), info.type,
                                error.what()));
}

void ApiServer::Impl::handle_auth_register(const HttpRequestPtr& req,
                                           ResponseCallback&& callback) {
  if (auth_ == nullptr) {
    callback(error_response(drogon::k503ServiceUnavailable, "auth_unavailable",
                            "用户鉴权未启用（需要 AIP_STORAGE=postgres 且编译时开启 AUTH）"));
    return;
  }

  json body;
  try {
    body = json::parse(std::string(req->body()));
  } catch (const std::exception&) {
    callback(error_response(drogon::k400BadRequest, "invalid_json", "请求体不是合法 JSON"));
    return;
  }
  if (!body.is_object() || !body.contains("email") || !body["email"].is_string() ||
      !body.contains("password") || !body["password"].is_string()) {
    callback(error_response(drogon::k400BadRequest, "invalid_request",
                            "email 和 password 必须是字符串"));
    return;
  }
  if (body.contains("display_name") && !body["display_name"].is_string()) {
    callback(error_response(drogon::k400BadRequest, "invalid_request",
                            "display_name 必须是字符串"));
    return;
  }

  const std::string email = body["email"].get<std::string>();
  const std::string password = body["password"].get<std::string>();
  const std::string display_name =
      body.contains("display_name") ? body["display_name"].get<std::string>() : std::string{};
  auto shared_callback = std::make_shared<ResponseCallback>(std::move(callback));

  const bool queued = db_workers.submit([this, email, password, display_name, shared_callback] {
    try {
      const auth::TokenPair pair = auth_->register_user(email, password, display_name);
      post(shared_callback, json_ok(token_pair_json(pair)));
    } catch (const auth::AuthError& e) {
      post_auth_error(shared_callback, e);
    }
#ifdef AIP_WITH_POSTGRES
    catch (const storage::DbError& e) {
      platform::log_error(std::string("注册失败（存储）: ") + e.what());
      post(shared_callback, error_response(drogon::k503ServiceUnavailable, "storage_unavailable",
                                           "用户存储暂时不可用，请稍后重试"));
    }
#endif
    catch (const std::exception& e) {
      platform::log_error(std::string("注册失败: ") + e.what());
      post(shared_callback,
           error_response(drogon::k500InternalServerError, "internal_error", "注册失败"));
    }
  });

  if (!queued) {
    (*shared_callback)(error_response(drogon::k503ServiceUnavailable, "server_busy",
                                      "注册排队已满，请稍后重试"));
  }
}

void ApiServer::Impl::handle_auth_login(const HttpRequestPtr& req, ResponseCallback&& callback) {
  if (auth_ == nullptr) {
    callback(error_response(drogon::k503ServiceUnavailable, "auth_unavailable",
                            "用户鉴权未启用（需要 AIP_STORAGE=postgres 且编译时开启 AUTH）"));
    return;
  }

  json body;
  try {
    body = json::parse(std::string(req->body()));
  } catch (const std::exception&) {
    callback(error_response(drogon::k400BadRequest, "invalid_json", "请求体不是合法 JSON"));
    return;
  }
  if (!body.is_object() || !body.contains("email") || !body["email"].is_string() ||
      !body.contains("password") || !body["password"].is_string()) {
    callback(error_response(drogon::k400BadRequest, "invalid_request",
                            "email 和 password 必须是字符串"));
    return;
  }

  const std::string email = body["email"].get<std::string>();
  const std::string password = body["password"].get<std::string>();
  auto shared_callback = std::make_shared<ResponseCallback>(std::move(callback));

  const bool queued = db_workers.submit([this, email, password, shared_callback] {
    try {
      const auth::TokenPair pair = auth_->login(email, password);
      post(shared_callback, json_ok(token_pair_json(pair)));
    } catch (const auth::AuthError& e) {
      post_auth_error(shared_callback, e);
    }
#ifdef AIP_WITH_POSTGRES
    catch (const storage::DbError& e) {
      platform::log_error(std::string("登录失败（存储）: ") + e.what());
      post(shared_callback, error_response(drogon::k503ServiceUnavailable, "storage_unavailable",
                                           "用户存储暂时不可用，请稍后重试"));
    }
#endif
    catch (const std::exception& e) {
      platform::log_error(std::string("登录失败: ") + e.what());
      post(shared_callback,
           error_response(drogon::k500InternalServerError, "internal_error", "登录失败"));
    }
  });

  if (!queued) {
    (*shared_callback)(error_response(drogon::k503ServiceUnavailable, "server_busy",
                                      "登录排队已满，请稍后重试"));
  }
}

void ApiServer::Impl::handle_auth_refresh(const HttpRequestPtr& req,
                                          ResponseCallback&& callback) {
  if (auth_ == nullptr) {
    callback(error_response(drogon::k503ServiceUnavailable, "auth_unavailable",
                            "用户鉴权未启用（需要 AIP_STORAGE=postgres 且编译时开启 AUTH）"));
    return;
  }

  std::string refresh_token;
  const auto header_token = bearer_token(req->getHeader("authorization"));
  if (header_token.has_value()) {
    refresh_token = *header_token;
  } else {
    json body;
    try {
      body = json::parse(std::string(req->body()));
    } catch (const std::exception&) {
      callback(error_response(drogon::k400BadRequest, "invalid_json", "请求体不是合法 JSON"));
      return;
    }
    if (!body.is_object() || !body.contains("refresh_token") ||
        !body["refresh_token"].is_string()) {
      callback(error_response(drogon::k400BadRequest, "invalid_request",
                              "缺少 Authorization: Bearer <refresh_token> 或 refresh_token 字段"));
      return;
    }
    refresh_token = body["refresh_token"].get<std::string>();
  }

  auto shared_callback = std::make_shared<ResponseCallback>(std::move(callback));
  const bool queued = db_workers.submit([this, refresh_token, shared_callback] {
    try {
      const auth::TokenPair pair = auth_->refresh(refresh_token);
      post(shared_callback, json_ok(token_pair_json(pair)));
    } catch (const auth::AuthError& e) {
      post_auth_error(shared_callback, e);
    }
#ifdef AIP_WITH_POSTGRES
    catch (const storage::DbError& e) {
      platform::log_error(std::string("刷新 token 失败（存储）: ") + e.what());
      post(shared_callback, error_response(drogon::k503ServiceUnavailable, "storage_unavailable",
                                           "用户存储暂时不可用，请稍后重试"));
    }
#endif
    catch (const std::exception& e) {
      platform::log_error(std::string("刷新 token 失败: ") + e.what());
      post(shared_callback,
           error_response(drogon::k500InternalServerError, "internal_error", "刷新 token 失败"));
    }
  });

  if (!queued) {
    (*shared_callback)(error_response(drogon::k503ServiceUnavailable, "server_busy",
                                      "刷新排队已满，请稍后重试"));
  }
}

void ApiServer::Impl::handle_auth_me(const HttpRequestPtr& req, ResponseCallback&& callback) {
  if (auth_ == nullptr) {
    callback(error_response(drogon::k503ServiceUnavailable, "auth_unavailable",
                            "用户鉴权未启用（需要 AIP_STORAGE=postgres 且编译时开启 AUTH）"));
    return;
  }
  const auth::Principal principal = principal_from(req);
  if (principal.user_id <= 0) {
    callback(error_response(drogon::k401Unauthorized, "unauthorized",
                            "静态管理员 Token 没有用户资料，请使用 JWT/API Key"));
    return;
  }

  auto shared_callback = std::make_shared<ResponseCallback>(std::move(callback));
  const bool queued = db_workers.submit([this, principal, shared_callback] {
    try {
      auto user = auth_->get_user(principal.user_id);
      if (!user.has_value()) {
        post(shared_callback,
             error_response(drogon::k404NotFound, "not_found", "用户不存在"));
        return;
      }
      post(shared_callback, json_ok(json{{"user", user_json(*user)}}));
    }
#ifdef AIP_WITH_POSTGRES
    catch (const storage::DbError& e) {
      platform::log_error(std::string("读取用户失败（存储）: ") + e.what());
      post(shared_callback, error_response(drogon::k503ServiceUnavailable, "storage_unavailable",
                                           "用户存储暂时不可用，请稍后重试"));
    }
#endif
    catch (const std::exception& e) {
      platform::log_error(std::string("读取用户失败: ") + e.what());
      post(shared_callback,
           error_response(drogon::k500InternalServerError, "internal_error", "读取用户失败"));
    }
  });
  if (!queued) {
    (*shared_callback)(error_response(drogon::k503ServiceUnavailable, "server_busy",
                                      "请求排队已满，请稍后重试"));
  }
}

void ApiServer::Impl::handle_auth_list_api_keys(const HttpRequestPtr& req,
                                                ResponseCallback&& callback) {
  if (auth_ == nullptr) {
    callback(error_response(drogon::k503ServiceUnavailable, "auth_unavailable",
                            "用户鉴权未启用（需要 AIP_STORAGE=postgres 且编译时开启 AUTH）"));
    return;
  }
  const auth::Principal principal = principal_from(req);
  if (principal.user_id <= 0) {
    callback(error_response(drogon::k401Unauthorized, "unauthorized",
                            "该接口需要用户身份（JWT/API Key）"));
    return;
  }

  auto shared_callback = std::make_shared<ResponseCallback>(std::move(callback));
  const bool queued = db_workers.submit([this, principal, shared_callback] {
    try {
      json list = json::array();
      for (const auto& key : auth_->list_api_keys(principal)) list.push_back(api_key_json(key));
      post(shared_callback, json_ok({{"api_keys", list}}));
    } catch (const auth::AuthError& e) {
      post_auth_error(shared_callback, e);
    }
#ifdef AIP_WITH_POSTGRES
    catch (const storage::DbError& e) {
      platform::log_error(std::string("读取 API Key 失败（存储）: ") + e.what());
      post(shared_callback, error_response(drogon::k503ServiceUnavailable, "storage_unavailable",
                                           "用户存储暂时不可用，请稍后重试"));
    }
#endif
    catch (const std::exception& e) {
      platform::log_error(std::string("读取 API Key 失败: ") + e.what());
      post(shared_callback,
           error_response(drogon::k500InternalServerError, "internal_error", "读取 API Key 失败"));
    }
  });
  if (!queued) {
    (*shared_callback)(error_response(drogon::k503ServiceUnavailable, "server_busy",
                                      "请求排队已满，请稍后重试"));
  }
}

void ApiServer::Impl::handle_auth_create_api_key(const HttpRequestPtr& req,
                                                 ResponseCallback&& callback) {
  if (auth_ == nullptr) {
    callback(error_response(drogon::k503ServiceUnavailable, "auth_unavailable",
                            "用户鉴权未启用（需要 AIP_STORAGE=postgres 且编译时开启 AUTH）"));
    return;
  }
  const auth::Principal principal = principal_from(req);
  if (principal.user_id <= 0) {
    callback(error_response(drogon::k401Unauthorized, "unauthorized",
                            "该接口需要用户身份（JWT/API Key）"));
    return;
  }

  std::string name;
  if (!req->body().empty()) {
    json body;
    try {
      body = json::parse(std::string(req->body()));
    } catch (const std::exception&) {
      callback(error_response(drogon::k400BadRequest, "invalid_json", "请求体不是合法 JSON"));
      return;
    }
    if (!body.is_object() || (body.contains("name") && !body["name"].is_string())) {
      callback(error_response(drogon::k400BadRequest, "invalid_request", "name 必须是字符串"));
      return;
    }
    if (body.contains("name")) name = body["name"].get<std::string>();
  }

  auto shared_callback = std::make_shared<ResponseCallback>(std::move(callback));
  const bool queued = db_workers.submit([this, principal, name, shared_callback] {
    try {
      const auth::IssuedApiKey issued = auth_->create_api_key(principal, name);
      json out = api_key_json(issued.info);
      out["key"] = issued.plaintext;
      auto resp = json_ok(out);
      resp->setStatusCode(drogon::k201Created);
      post(shared_callback, std::move(resp));
    } catch (const auth::AuthError& e) {
      post_auth_error(shared_callback, e);
    }
#ifdef AIP_WITH_POSTGRES
    catch (const storage::DbError& e) {
      platform::log_error(std::string("创建 API Key 失败（存储）: ") + e.what());
      post(shared_callback, error_response(drogon::k503ServiceUnavailable, "storage_unavailable",
                                           "用户存储暂时不可用，请稍后重试"));
    }
#endif
    catch (const std::exception& e) {
      platform::log_error(std::string("创建 API Key 失败: ") + e.what());
      post(shared_callback,
           error_response(drogon::k500InternalServerError, "internal_error", "创建 API Key 失败"));
    }
  });
  if (!queued) {
    (*shared_callback)(error_response(drogon::k503ServiceUnavailable, "server_busy",
                                      "请求排队已满，请稍后重试"));
  }
}

void ApiServer::Impl::handle_auth_delete_api_key(const HttpRequestPtr& req,
                                                 ResponseCallback&& callback) {
  if (auth_ == nullptr) {
    callback(error_response(drogon::k503ServiceUnavailable, "auth_unavailable",
                            "用户鉴权未启用（需要 AIP_STORAGE=postgres 且编译时开启 AUTH）"));
    return;
  }
  const auth::Principal principal = principal_from(req);
  if (principal.user_id <= 0) {
    callback(error_response(drogon::k401Unauthorized, "unauthorized",
                            "该接口需要用户身份（JWT/API Key）"));
    return;
  }

  static constexpr std::string_view kPrefix = "/v1/auth/api-keys/";
  const std::string& path = req->path();
  if (path.size() <= kPrefix.size() || path.compare(0, kPrefix.size(), kPrefix) != 0) {
    callback(error_response(drogon::k400BadRequest, "invalid_request", "API Key ID 不合法"));
    return;
  }
  std::int64_t key_id = 0;
  try {
    key_id = std::stoll(path.substr(kPrefix.size()));
  } catch (const std::exception&) {
    key_id = 0;
  }
  if (key_id <= 0) {
    callback(error_response(drogon::k400BadRequest, "invalid_request", "API Key ID 不合法"));
    return;
  }

  auto shared_callback = std::make_shared<ResponseCallback>(std::move(callback));
  const bool queued = db_workers.submit([this, principal, key_id, shared_callback] {
    try {
      if (!auth_->revoke_api_key(principal, key_id)) {
        post(shared_callback,
             error_response(drogon::k404NotFound, "not_found", "API Key 不存在"));
        return;
      }
      post(shared_callback, json_ok({{"revoked", true}, {"id", key_id}}));
    } catch (const auth::AuthError& e) {
      post_auth_error(shared_callback, e);
    }
#ifdef AIP_WITH_POSTGRES
    catch (const storage::DbError& e) {
      platform::log_error(std::string("撤销 API Key 失败（存储）: ") + e.what());
      post(shared_callback, error_response(drogon::k503ServiceUnavailable, "storage_unavailable",
                                           "用户存储暂时不可用，请稍后重试"));
    }
#endif
    catch (const std::exception& e) {
      platform::log_error(std::string("撤销 API Key 失败: ") + e.what());
      post(shared_callback,
           error_response(drogon::k500InternalServerError, "internal_error", "撤销 API Key 失败"));
    }
  });
  if (!queued) {
    (*shared_callback)(error_response(drogon::k503ServiceUnavailable, "server_busy",
                                      "请求排队已满，请稍后重试"));
  }
}
#endif

void ApiServer::Impl::post(const std::shared_ptr<ResponseCallback>& callback,
                           HttpResponsePtr resp) {
  // 工作线程不能直接调 callback：必须回到 event loop 里交回响应。
  auto* loop = drogon::app().getLoop();
  if (loop == nullptr) return;  // 已经开始退出，丢掉即可。
  loop->queueInLoop([callback, resp = std::move(resp)] { (*callback)(resp); });
}

bool ApiServer::Impl::run() {
  setup();
  metrics::set_ready(true);

  drogon::app().setTermSignalHandler([this] { begin_shutdown(); });
  drogon::app().setIntSignalHandler([this] { begin_shutdown(); });

  // Drogon 1.9.x 的 app().run() 返回 void：进程退出码由 main 决定，
  // 端口占用这类失败会在 run() 里直接打日志并退出。
  drogon::app().run();

  // 走到这里 event loop 已经停了：在途泵的 send 会失败并 cancel，然后 join 掉。
  pumps.drain();
  workers.stop();
  db_workers.stop();
  return true;
}

void ApiServer::Impl::begin_shutdown() {
  if (shutting_down.exchange(true, std::memory_order_acq_rel)) {
    platform::log_warn("再次收到退出信号：立即退出");
    drogon::app().quit();
    return;
  }

  metrics::set_ready(false);
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
                     chat::SessionStore& store, auth::AuthService* auth,
                     quota::QuotaService* quota, usage::UsageSink* usage,
                     chat::IdempotencyStore* idempotency)
    : impl_(std::make_unique<Impl>(config, service, store, auth, quota, usage, idempotency)) {}

ApiServer::~ApiServer() = default;

bool ApiServer::run() {
  platform::log_info("HTTP 服务启动（Drogon）: http://" + impl_->config.host + ":" +
                     std::to_string(impl_->config.port) + "  io_threads=" +
                     std::to_string(impl_->io_threads()) + "  max_streams=" +
                     std::to_string(impl_->max_streams));
  if (impl_->config.auth_enabled()) {
    platform::log_info("鉴权已启用（静态 Token / JWT / API Key）");
  } else {
    platform::log_warn("未启用鉴权（AIP_TOKEN/AIP_JWT_SECRET/AIP_AUTH_REQUIRED 均为空），请勿暴露到公网");
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
