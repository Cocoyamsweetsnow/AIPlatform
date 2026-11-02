// Drogon SSE 可行性验证（POC，不是生产代码）。
//
// 验证三件事：
//   1. llm::Stream 的「阻塞拉取」能不能桥接到 Drogon 的「推送式异步流」；
//   2. 客户端断开后，能否及时 cancel 上游（省 token）；
//   3. N 路并发流下的线程数 / 内存，是否明显优于 cpp-httplib 的「每连接一线程」。
//
// 刻意复用 src/llm/mock_provider.cpp：它内部就是 ThreadedStream + EventQueue，
// 和真实 OpenAI provider 走同一条链路，所以这里验的不是一个假 sleep 循环。
//
// 编译（需先安装 Drogon，见 docs/POC-DROGON.md）：
//   cmake -S . -B build-poc -DAIPLATFORM_BUILD_POC=ON
//   cmake --build build-poc -j4
// 运行（默认 8081，不占用 httplib 版本用的 8080）：
//   ./build-poc/poc_drogon_sse 8081
//
// 三个端点：
//   GET  /healthz                  健康检查 + 活跃流计数
//   GET  /sse?text=...             流式（最容易用 curl -N 验证）
//   POST /v1/chat/completions      与生产同形；stream=false 时走「工作线程 + 回调」

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "llm/mock_provider.h"

namespace {

using json = nlohmann::json;

// 拉取超时 = 心跳间隔。超时没有事件就发一行 SSE 注释，顺便探测连接是否还活着。
constexpr std::chrono::milliseconds kPullTimeout{5000};

std::atomic<long long> g_seq{0};
std::atomic<long long> g_started{0};
std::atomic<long long> g_finished{0};
std::atomic<long long> g_cancelled{0};

bool verbose() {
  static const bool v = std::getenv("POC_VERBOSE") != nullptr;
  return v;
}

long long now_s() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
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

drogon::HttpResponsePtr json_response(int status, const std::string& type,
                                      const std::string& message) {
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
  resp->setContentTypeString("application/json; charset=utf-8");
  resp->setBody(json{{"error", {{"type", type}, {"message", message}}}}.dump());
  return resp;
}

// 只用来保活 /idle 的连接：证明「连接本身不占线程」——这是 Drogon 相对
// cpp-httplib 的真实收益。注意 /sse 那条路径仍要 2 个线程/流（泵 + 上游读线程），
// 因为 libcurl 是阻塞的；要消掉它得把上游改异步（本 POC 不做）。
class IdleRegistry {
 public:
  static IdleRegistry& instance() {
    static IdleRegistry registry;
    return registry;
  }

  void add(std::shared_ptr<drogon::ResponseStreamPtr> stream) {
    std::lock_guard<std::mutex> lock(mu_);
    streams_.push_back(std::move(stream));
  }

  std::size_t size() {
    std::lock_guard<std::mutex> lock(mu_);
    return streams_.size();
  }

  void close_all() {
    std::vector<std::shared_ptr<drogon::ResponseStreamPtr>> copy;
    {
      std::lock_guard<std::mutex> lock(mu_);
      copy.swap(streams_);
    }
    for (auto& s : copy) (*s)->close();
  }

 private:
  std::mutex mu_;
  std::vector<std::shared_ptr<drogon::ResponseStreamPtr>> streams_;
};

// 一路 SSE 的泵：在自己的线程里「拉」上游事件，往 Drogon 的响应流里「推」。
//
// 生命周期：由 PumpRegistry 持有，线程同时持有一份 shared_ptr。
// 线程结束时只置 finished_ 标志，真正的 join 由 registry 在下次 add 或 shutdown 时做，
// 保证析构永远发生在 join 之后（否则 std::thread 析构会 terminate）。
class SsePump : public std::enable_shared_from_this<SsePump> {
 public:
  SsePump(std::shared_ptr<llm::Stream> stream, drogon::ResponseStreamPtr out,
          std::string id, std::string model)
      : stream_(std::move(stream)),
        out_(std::move(out)),
        id_(std::move(id)),
        model_(std::move(model)) {}

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
    const long long created = now_s();
    bool sent_role = false;
    bool clean = true;

    while (true) {
      llm::Event ev;
      const llm::Pull pull = stream_->next(ev, kPullTimeout);

      if (pull == llm::Pull::Cancelled) {
        clean = false;
        break;
      }
      if (pull == llm::Pull::Timeout) {
        // 心跳：既保活，也是探测「客户端是否已断开」的低成本手段。
        if (!out_->send(": keep-alive\n\n")) {
          stream_->cancel();
          clean = false;
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
        stream_->cancel();
        clean = false;
        break;
      }
      if (terminal) break;
    }

    out_->close();
    if (clean) {
      ++g_finished;
    } else {
      ++g_cancelled;
      std::fprintf(stderr, "[poc] client gone, upstream cancelled: %s\n", id_.c_str());
    }
    if (verbose()) {
      std::fprintf(stderr, "[poc] stream %s -> %s\n", id_.c_str(),
                   clean ? "done" : "cancelled");
    }
    finished_.store(true, std::memory_order_release);
  }

  std::shared_ptr<llm::Stream> stream_;
  drogon::ResponseStreamPtr out_;
  const std::string id_;
  const std::string model_;
  std::thread thread_;
  std::atomic<bool> finished_{false};
};

// 只在「新增流」和「退出」时顺带回收已结束的泵，避免单独开一个清理线程。
class PumpRegistry {
 public:
  static PumpRegistry& instance() {
    static PumpRegistry registry;
    return registry;
  }

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
    for (auto& p : finished) p->join();
  }

  void shutdown() {
    std::vector<std::shared_ptr<SsePump>> alive;
    {
      std::lock_guard<std::mutex> lock(mu_);
      alive.swap(alive_);
    }
    for (auto& p : alive) p->cancel();
    for (auto& p : alive) p->join();
    std::fprintf(stderr,
                 "[poc] shutdown: started=%lld finished=%lld cancelled=%lld\n",
                 g_started.load(), g_finished.load(), g_cancelled.load());
  }

 private:
  std::mutex mu_;
  std::vector<std::shared_ptr<SsePump>> alive_;
};

drogon::HttpResponsePtr make_sse_response(std::shared_ptr<llm::Stream> stream,
                                          std::string id, std::string model) {
  // disableKickoffTimeout=true：流可能很久才出第一个字节（真实模型的首字延迟），
  // 不能让 trantor 的 kickoff 超时把长流掐掉。
  auto resp = drogon::HttpResponse::newAsyncStreamResponse(
      [stream = std::move(stream), id = std::move(id),
       model = std::move(model)](drogon::ResponseStreamPtr out) mutable {
        ++g_started;
        auto pump = std::make_shared<SsePump>(std::move(stream), std::move(out),
                                              std::move(id), std::move(model));
        PumpRegistry::instance().add(pump);
        pump->start();
      },
      /*disableKickoffTimeout=*/true);

  resp->setContentTypeString("text/event-stream");
  resp->addHeader("Cache-Control", "no-cache, no-transform");
  resp->addHeader("X-Accel-Buffering", "no");
  return resp;
}

llm::ChatRequest make_chat_request(const std::string& text) {
  llm::ChatRequest chat;
  chat.model = "mock";
  chat.messages.push_back({"user", text});
  return chat;
}

std::shared_ptr<llm::Stream> open_stream(llm::Provider& provider,
                                         const std::string& text) {
  return std::shared_ptr<llm::Stream>(provider.chat_stream(make_chat_request(text)));
}

std::string next_id() { return "chatcmpl-poc-" + std::to_string(++g_seq); }

void handle_sse(llm::Provider& provider, const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  std::string text = req->getParameter("text");
  if (text.empty()) text = "你好，介绍一下你自己";
  callback(make_sse_response(open_stream(provider, text), next_id(), "mock"));
}

// 只发一个注释帧，然后一直挂着：发起 n 个这样的连接，观察服务端线程数是否增长。
// 注意这些连接不会自己结束，压测脚本用 --hold 主动断开（POC 不回收已断开的流）。
void handle_idle(std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  auto resp = drogon::HttpResponse::newAsyncStreamResponse(
      [](drogon::ResponseStreamPtr out) {
        auto holder = std::make_shared<drogon::ResponseStreamPtr>(std::move(out));
        (*holder)->send(": idle\n\n");
        IdleRegistry::instance().add(std::move(holder));
      },
      /*disableKickoffTimeout=*/true);
  resp->setContentTypeString("text/event-stream");
  resp->addHeader("Cache-Control", "no-cache");
  callback(resp);
}

void handle_chat(llm::Provider& provider, const drogon::HttpRequestPtr& req,
                 std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
  json body;
  try {
    body = json::parse(std::string(req->body()));
  } catch (const std::exception&) {
    callback(json_response(400, "invalid_json", "请求体不是合法 JSON"));
    return;
  }
  if (!body.is_object() || !body.contains("message") || !body["message"].is_string()) {
    callback(json_response(400, "invalid_request", "缺少 message 字段"));
    return;
  }

  const std::string text = body["message"].get<std::string>();
  // 不用 body.value("stream", true)：字段类型不对时它会抛异常，这里直接判类型。
  const bool stream =
      body.contains("stream") && body["stream"].is_boolean() ? body["stream"].get<bool>() : true;

  if (stream) {
    auto resp = make_sse_response(open_stream(provider, text), next_id(), "mock");
    resp->addHeader("X-Session-Id", "poc");
    callback(resp);
    return;
  }

  // 非流式：绝不能在 IO 线程里阻塞等模型。
  // 丢到工作线程拉完，再回到 event loop 里回调（这就是迁移时生产代码要用的形态）。
  auto stream_ptr = open_stream(provider, text);
  auto cb = std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(
      std::move(callback));

  std::thread([stream_ptr, cb] {
    std::string answer;
    std::string error;
    llm::Event ev;
    while (true) {
      const llm::Pull pull = stream_ptr->next(ev, std::chrono::milliseconds(5000));
      if (pull == llm::Pull::Timeout) continue;
      if (pull == llm::Pull::Cancelled || pull == llm::Pull::End) break;
      if (ev.kind == llm::EventKind::Delta) {
        answer += ev.text;
      } else if (ev.kind == llm::EventKind::Done) {
        break;
      } else {
        error = ev.text;
        break;
      }
    }

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeString("application/json; charset=utf-8");
    if (error.empty()) {
      resp->setStatusCode(drogon::k200OK);
      resp->setBody(json{{"object", "chat.completion"},
                         {"answer", answer},
                         {"poc", "non-streaming"}}
                        .dump());
    } else {
      resp->setStatusCode(drogon::k502BadGateway);
      resp->setBody(json{{"error", {{"type", "upstream_error"}, {"message", error}}}}.dump());
    }
    // detached 线程 → 只能通过 app 的 event loop 把响应交回去。
    if (auto* loop = drogon::app().getLoop()) {
      loop->queueInLoop([cb, resp] { (*cb)(resp); });
    }
  }).detach();
}

}  // namespace

int main(int argc, char** argv) {
  const uint16_t port =
      argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : static_cast<uint16_t>(8081);

  auto provider = llm::make_mock_provider();
  llm::Provider& provider_ref = *provider;

  drogon::app().addListener("0.0.0.0", port);
  drogon::app().setThreadNum(4);  // 4 核机器：I/O 线程数不用多，SSE 是 epoll 场景
  drogon::app().setClientMaxBodySize(1024 * 1024);
  trantor::Logger::setLogLevel(trantor::Logger::kWarn);

  drogon::app().registerHandler(
      "/healthz",
      [](const drogon::HttpRequestPtr&,
         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeString("application/json; charset=utf-8");
        resp->setBody(json{{"status", "ok"},
                           {"provider", "mock"},
                           {"started", g_started.load()},
                           {"finished", g_finished.load()},
                           {"cancelled", g_cancelled.load()},
                           {"idle", IdleRegistry::instance().size()}}
                          .dump());
        callback(resp);
      },
      {drogon::Get});

  drogon::app().registerHandler(
      "/idle",
      [](const drogon::HttpRequestPtr&,
         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        handle_idle(std::move(callback));
      },
      {drogon::Get});

  drogon::app().registerHandler(
      "/sse",
      [&provider_ref](const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        handle_sse(provider_ref, req, std::move(callback));
      },
      {drogon::Get});

  drogon::app().registerHandler(
      "/v1/chat/completions",
      [&provider_ref](const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        handle_chat(provider_ref, req, std::move(callback));
      },
      {drogon::Post});

  std::fprintf(stderr,
               "[poc] listening on :%u  (GET /sse?text=..., POST /v1/chat/completions)\n",
               static_cast<unsigned>(port));

  drogon::app().run();
  PumpRegistry::instance().shutdown();
  IdleRegistry::instance().close_all();
  return 0;
}
