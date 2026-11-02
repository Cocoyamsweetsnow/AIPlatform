#include "llm/openai_provider.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include "llm/event_queue.h"
#include "llm/threaded_stream.h"

namespace llm {
namespace {

using json = nlohmann::json;

std::string trim(const std::string& s) {
  const auto begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  const auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

std::string snippet(const std::string& s, std::size_t limit = 400) {
  if (s.size() <= limit) return s;
  return s.substr(0, limit) + "...(truncated)";
}

std::string build_payload(const ChatRequest& req, const OpenAiOptions& opts) {
  json messages = json::array();
  for (const auto& m : req.messages) {
    messages.push_back({{"role", m.role}, {"content", m.content}});
  }

  json body;
  body["model"] = req.model.empty() ? opts.default_model : req.model;
  body["messages"] = std::move(messages);
  body["stream"] = true;
  body["temperature"] = req.temperature;
  if (req.max_tokens > 0) body["max_tokens"] = req.max_tokens;
  // 让上游在最后一个 chunk 里返回 usage，用于计费；个别兼容实现不支持时可关掉。
  if (req.include_usage) body["stream_options"] = {{"include_usage", true}};
  return body.dump();
}

// 首包可能是 SSE，也可能是普通 JSON（错误、或被代理降级成非流式）。
enum class BodyMode { Unknown, Sse, Json };

class OpenAiStream final : public ThreadedStream {
 public:
  OpenAiStream(std::shared_ptr<EventQueue> queue, OpenAiOptions opts, ChatRequest req)
      : ThreadedStream(std::move(queue)), opts_(std::move(opts)), req_(std::move(req)) {}

  ~OpenAiStream() override { shutdown(); }

  void start() { start_worker(); }

  std::string describe() const override {
    return "openai/" + (req_.model.empty() ? opts_.default_model : req_.model);
  }

 private:
  void run() override {
    const std::string url = opts_.base_url + "/chat/completions";
    const std::string payload = build_payload(req_, opts_);
    const std::string auth = "Authorization: Bearer " + opts_.api_key;

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    headers = curl_slist_append(headers, auth.c_str());

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
      curl_slist_free_all(headers);
      fail("curl_easy_init 失败");
      return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &OpenAiStream::on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(opts_.timeout_s));
    // 上游卡住（不吐字也不断开）时主动放弃，避免线程和连接被永久占用。
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);

    const CURLcode rc = curl_easy_perform(curl);

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    http_status_ = static_cast<int>(status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    finish(rc);
  }

  static size_t on_body(char* data, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* self = static_cast<OpenAiStream*>(userdata);
    if (self->queue_->cancelled()) return 0;  // 返回 0 -> curl 立即中断传输
    const std::size_t n = size * nmemb;
    self->consume(data, n);
    return n;
  }

  void consume(const char* data, std::size_t n) {
    if (raw_tail_.size() < 4096) raw_tail_.append(data, n);

    switch (mode_) {
      case BodyMode::Unknown: {
        pending_.append(data, n);
        const std::string head = trim(pending_.substr(0, 64));
        if (head.empty()) return;  // 还不够判断
        mode_ = head.rfind("data:", 0) == 0 || head.rfind(":", 0) == 0 ||
                        head.rfind("event:", 0) == 0 || head.rfind("id:", 0) == 0
                    ? BodyMode::Sse
                    : BodyMode::Json;
        std::string buffered;
        buffered.swap(pending_);
        consume_decided(buffered);
        return;
      }
      case BodyMode::Sse:
        consume_decided(std::string(data, n));
        return;
      case BodyMode::Json:
        json_body_.append(data, n);
        return;
    }
  }

  void consume_decided(const std::string& chunk) {
    if (mode_ == BodyMode::Json) {
      json_body_ += chunk;
      return;
    }
    pending_ += chunk;
    std::size_t pos = 0;
    while ((pos = pending_.find('\n', pos)) != std::string::npos) {
      handle_line(pending_.substr(0, pos));
      pending_.erase(0, pos + 1);
      pos = 0;
      if (closed_) return;
    }
  }

  void handle_line(const std::string& raw_line) {
    std::string line = raw_line;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) return;

    // 注释行（": ping"）与 event:/id: 行直接忽略。
    if (line.rfind("data:", 0) != 0) return;

    const std::string payload = trim(line.substr(5));
    if (payload.empty()) return;

    if (payload == "[DONE]") {
      emit_done();
      return;
    }

    const json j = json::parse(payload, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return;  // 半包/脏数据，丢掉即可

    if (j.contains("error") && !j["error"].is_null()) {
      std::string msg = "上游返回错误";
      const auto& err = j["error"];
      if (err.is_object() && err.contains("message") && err["message"].is_string()) {
        msg += ": " + err["message"].get<std::string>();
      }
      fail(msg);
      return;
    }

    if (j.contains("usage") && j["usage"].is_object()) {
      const auto& u = j["usage"];
      if (u.contains("prompt_tokens") && u["prompt_tokens"].is_number_integer()) {
        prompt_tokens_ = u["prompt_tokens"].get<int>();
      }
      if (u.contains("completion_tokens") && u["completion_tokens"].is_number_integer()) {
        completion_tokens_ = u["completion_tokens"].get<int>();
      }
    }

    if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) return;

    const auto& choice = j["choices"][0];
    if (choice.contains("delta") && choice["delta"].is_object()) {
      const auto& delta = choice["delta"];
      if (delta.contains("content") && delta["content"].is_string()) {
        const std::string text = delta["content"].get<std::string>();
        if (!text.empty()) {
          got_delta_ = true;
          Event e;
          e.kind = EventKind::Delta;
          e.text = text;
          queue_->push(std::move(e));
        }
      }
    }
    if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
      finish_reason_ = choice["finish_reason"].get<std::string>();
    }
  }

  void emit_done() {
    if (closed_) return;
    closed_ = true;
    Event e;
    e.kind = EventKind::Done;
    e.finish_reason = finish_reason_.empty() ? "stop" : finish_reason_;
    e.prompt_tokens = prompt_tokens_;
    e.completion_tokens = completion_tokens_;
    queue_->push(std::move(e));
    queue_->close();
  }

  void fail(const std::string& message) {
    if (closed_) return;
    closed_ = true;
    Event e;
    e.kind = EventKind::Error;
    e.text = message;
    queue_->push(std::move(e));
    queue_->close();
  }

  void finish(CURLcode rc) {
    if (queue_->cancelled()) return;  // 客户端已断开，不再产生事件

    if (rc != CURLE_OK) {
      fail(std::string("上游请求失败: ") + curl_easy_strerror(rc));
      return;
    }

    // 非流式响应（或错误 JSON）：整体解析一次。
    if (mode_ == BodyMode::Json || (!got_delta_ && !closed_)) {
      const json j = json::parse(json_body_.empty() ? raw_tail_ : json_body_, nullptr,
                                 /*allow_exceptions=*/false);
      if (!j.is_discarded() && j.is_object()) {
        if (j.contains("error") && !j["error"].is_null()) {
          std::string msg = "上游返回错误 (HTTP " + std::to_string(http_status_) + ")";
          const auto& err = j["error"];
          if (err.is_object() && err.contains("message") && err["message"].is_string()) {
            msg += ": " + err["message"].get<std::string>();
          } else if (err.is_string()) {
            msg += ": " + err.get<std::string>();
          }
          fail(msg);
          return;
        }
        if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
          const auto& msg_obj = j["choices"][0];
          if (msg_obj.contains("message") && msg_obj["message"].is_object() &&
              msg_obj["message"].contains("content") &&
              msg_obj["message"]["content"].is_string()) {
            Event delta;
            delta.kind = EventKind::Delta;
            delta.text = msg_obj["message"]["content"].get<std::string>();
            if (!delta.text.empty()) {
              got_delta_ = true;
              queue_->push(std::move(delta));
            }
            if (msg_obj.contains("finish_reason") && msg_obj["finish_reason"].is_string()) {
              finish_reason_ = msg_obj["finish_reason"].get<std::string>();
            }
            emit_done();
            return;
          }
        }
      }
      fail("无法解析上游响应 (HTTP " + std::to_string(http_status_) +
           "): " + snippet(raw_tail_));
      return;
    }

    // 流式但没收到 [DONE]（连接被上游提前关闭）：按正常结束处理，保证前端能收尾。
    emit_done();
  }

  OpenAiOptions opts_;
  ChatRequest req_;

  BodyMode mode_ = BodyMode::Unknown;
  std::string pending_;
  std::string json_body_;
  std::string raw_tail_;

  int http_status_ = 0;
  bool got_delta_ = false;
  bool closed_ = false;
  std::string finish_reason_;
  int prompt_tokens_ = -1;
  int completion_tokens_ = -1;
};

class OpenAiProvider final : public Provider {
 public:
  explicit OpenAiProvider(OpenAiOptions opts) : opts_(std::move(opts)) {}

  std::string name() const override { return "openai-compatible"; }

  std::unique_ptr<Stream> chat_stream(const ChatRequest& req) override {
    auto queue = std::make_shared<EventQueue>();
    auto stream = std::make_unique<OpenAiStream>(queue, opts_, req);
    stream->start();
    return stream;
  }

 private:
  OpenAiOptions opts_;
};

}  // namespace

std::unique_ptr<Provider> make_openai_provider(OpenAiOptions options) {
  return std::make_unique<OpenAiProvider>(std::move(options));
}

}  // namespace llm
