#include "llm/openai_provider.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/log.h"
#include "common/metrics.h"
#include "llm/event_queue.h"
#include "llm/key_ring.h"
#include "llm/threaded_stream.h"

namespace llm {
namespace {

using json = nlohmann::json;
namespace metrics = platform::metrics;

constexpr int kMaxRetryDelayMs = 5000;

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
  if (req.top_p >= 0.0) body["top_p"] = req.top_p;
  if (!req.stop.empty()) body["stop"] = req.stop;
  if (req.has_presence_penalty) body["presence_penalty"] = req.presence_penalty;
  if (req.has_frequency_penalty) body["frequency_penalty"] = req.frequency_penalty;
  if (req.has_seed) body["seed"] = req.seed;
  if (!req.response_format.empty()) {
    body["response_format"] = {{"type", req.response_format}};
  }
  if (!req.user.empty()) body["user"] = req.user;
  // 让上游在最后一个 chunk 里返回 usage，用于计费；个别兼容实现不支持时可关掉。
  if (req.include_usage) body["stream_options"] = {{"include_usage", true}};
  return body.dump();
}

// 只重试「换一个 key / 等一会儿就可能成功」的错误；鉴权、参数错误重试没有意义。
bool retryable_status(int status) {
  return status == 408 || status == 409 || status == 425 || status == 429 ||
         (status >= 500 && status <= 599);
}

bool retryable_curl(CURLcode rc) {
  switch (rc) {
    case CURLE_COULDNT_CONNECT:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_GOT_NOTHING:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_PARTIAL_FILE:
    case CURLE_SSL_CONNECT_ERROR:
      return true;
    // 总超时不重试：默认 AIP_TIMEOUT_S=120，重试会把用户等待放大到 6 分钟。
    default:
      return false;
  }
}

std::string upstream_error_message(const json& j, int status) {
  std::string msg = "上游返回错误";
  if (status > 0) msg += " (HTTP " + std::to_string(status) + ")";
  const auto& err = j["error"];
  if (err.is_object() && err.contains("message") && err["message"].is_string()) {
    msg += ": " + err["message"].get<std::string>();
  } else if (err.is_string()) {
    msg += ": " + err.get<std::string>();
  }
  return msg;
}

// 首包可能是 SSE，也可能是普通 JSON（错误、或被代理降级成非流式）。
enum class BodyMode { Unknown, Sse, Json };

enum class AttemptResult {
  Success,    // 已产生 Done 事件（或流正常结束）
  Retryable,  // 还没吐字，可换 key / 退避后重试
  Fatal,      // 不可重试，或已经吐过字（重试会造成重复内容）
  Cancelled,  // 客户端已断开
};

class OpenAiStream final : public ThreadedStream {
 public:
  OpenAiStream(std::shared_ptr<EventQueue> queue, OpenAiOptions opts, ChatRequest req,
               std::shared_ptr<KeyRing> keys)
      : ThreadedStream(std::move(queue)),
        opts_(std::move(opts)),
        req_(std::move(req)),
        keys_(std::move(keys)) {}

  ~OpenAiStream() override { shutdown(); }

  void start() { start_worker(); }

  std::string describe() const override {
    return "openai/" + (req_.model.empty() ? opts_.default_model : req_.model);
  }

 private:
  void run() override {
    const int max_attempts = 1 + std::max(0, opts_.max_retries);
    std::string last_error;
    int attempts_made = 0;

    for (int attempt = 0; attempt < max_attempts; ++attempt) {
      if (queue_->cancelled()) return;

      reset_attempt();
      attempts_made = attempt + 1;
      const AttemptResult result = attempt_once();
      if (result == AttemptResult::Success || result == AttemptResult::Cancelled) return;

      last_error = attempt_error_.empty() ? "上游请求失败" : attempt_error_;
      if (result == AttemptResult::Fatal || attempt + 1 >= max_attempts) break;

      platform::log_warn("上游请求失败（第 " + std::to_string(attempt + 1) +
                         " 次），准备换 key 重试: " + last_error);
      if (!sleep_before_retry(attempt)) return;  // 等待期间被取消
    }

    if (queue_->cancelled()) return;
    if (attempts_made > 1) {
      last_error += "（已尝试 " + std::to_string(attempts_made) + " 次）";
    }
    fail(last_error);
  }

  std::string display_model() const {
    return req_.model.empty() ? opts_.default_model : req_.model;
  }

  AttemptResult attempt_once() {
    if (keys_ == nullptr) {
      attempt_error_ = "没有可用的 AIP_API_KEY / AIP_API_KEYS";
      metrics::inc_upstream_request(display_model(), "keys_unavailable");
      return AttemptResult::Fatal;
    }
    auto lease = keys_->acquire();
    if (!lease.has_value()) {
      attempt_error_ = "所有上游 Key 均在冷却或限流中，请稍后重试";
      metrics::inc_upstream_request(display_model(), "keys_unavailable");
      metrics::set_upstream_keys_available(0);
      return AttemptResult::Fatal;
    }

    const auto started = std::chrono::steady_clock::now();
    AttemptResult result = attempt_http(lease->index, lease->key);
    // 单 Key 的 401/403 可能是 Key 本身失效；多 Key 时换一个 Key 再试，
    // 单 Key 则保持原来的「鉴权错误不重试」语义。
    if (result == AttemptResult::Fatal && (http_status_ == 401 || http_status_ == 403) &&
        keys_->size() > 1) {
      result = AttemptResult::Retryable;
    }
    const double seconds = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - started)
                               .count();

    const char* outcome = "fatal";
    if (result == AttemptResult::Success) {
      outcome = "success";
    } else if (result == AttemptResult::Retryable) {
      outcome = "retryable";
    } else if (result == AttemptResult::Cancelled) {
      outcome = "cancelled";
    }
    metrics::inc_upstream_request(display_model(), outcome);
    metrics::observe_upstream_duration(display_model(), seconds);

    if (result == AttemptResult::Success) {
      keys_->record_success(lease->index);
      keys_->record_usage(lease->index, prompt_tokens_, completion_tokens_);
    } else if (result == AttemptResult::Retryable || result == AttemptResult::Fatal) {
      if (keys_->record_failure(lease->index, http_status_, result == AttemptResult::Retryable)) {
        metrics::inc_key_cooldown();
        platform::log_warn("上游 Key #" + std::to_string(lease->index) + " 进入冷却（HTTP " +
                           std::to_string(http_status_) + "）");
      }
    }
    metrics::set_upstream_keys_available(keys_->available());
    return result;
  }

  AttemptResult attempt_http(std::size_t /*key_index*/, const std::string& key) {
    const std::string url = opts_.base_url + "/chat/completions";
    const std::string payload = build_payload(req_, opts_);
    const std::string auth = "Authorization: Bearer " + key;

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    headers = curl_slist_append(headers, auth.c_str());

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
      curl_slist_free_all(headers);
      attempt_error_ = "curl_easy_init 失败";
      return AttemptResult::Fatal;
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

    if (queue_->cancelled()) return AttemptResult::Cancelled;
    if (closed_) return AttemptResult::Success;  // [DONE] 已经产生

    if (rc != CURLE_OK) {
      attempt_error_ = std::string("上游请求失败: ") + curl_easy_strerror(rc);
      if (!got_delta_ && retryable_curl(rc)) return AttemptResult::Retryable;
      return AttemptResult::Fatal;
    }

    // 非流式响应（或被代理降级成整段 JSON）：整体解析一次。
    if (!aborted_ && (mode_ == BodyMode::Json || !got_delta_)) {
      const json j = json::parse(json_body_.empty() ? raw_tail_ : json_body_, nullptr,
                                 /*allow_exceptions=*/false);
      if (!j.is_discarded() && j.is_object()) {
        if (j.contains("error") && !j["error"].is_null()) {
          set_abort(upstream_error_message(j, http_status_));
        } else if (j.contains("choices") && j["choices"].is_array() &&
                   !j["choices"].empty()) {
          const auto& choice = j["choices"][0];
          if (choice.contains("message") && choice["message"].is_object() &&
              choice["message"].contains("content") &&
              choice["message"]["content"].is_string()) {
            push_delta(choice["message"]["content"].get<std::string>());
            if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
              finish_reason_ = choice["finish_reason"].get<std::string>();
            }
            emit_done();
            return AttemptResult::Success;
          }
        }
      }

      if (aborted_) {
        return (!got_delta_ && retryable_status(http_status_)) ? AttemptResult::Retryable
                                                               : AttemptResult::Fatal;
      }
      attempt_error_ = "无法解析上游响应 (HTTP " + std::to_string(http_status_) +
                       "): " + snippet(raw_tail_);
      return retryable_status(http_status_) ? AttemptResult::Retryable : AttemptResult::Fatal;
    }

    if (aborted_) {
      return (!got_delta_ && retryable_status(http_status_)) ? AttemptResult::Retryable
                                                             : AttemptResult::Fatal;
    }

    // 流式但没收到 [DONE]（连接被上游提前关闭）：按正常结束处理，保证前端能收尾。
    if (got_delta_) {
      emit_done();
      return AttemptResult::Success;
    }

    attempt_error_ = "上游返回空响应 (HTTP " + std::to_string(http_status_) + ")";
    return retryable_status(http_status_) ? AttemptResult::Retryable : AttemptResult::Fatal;
  }

  bool sleep_before_retry(int retry_index) {
    int delay_ms = std::max(0, opts_.retry_base_ms);
    for (int i = 0; i < retry_index && delay_ms < kMaxRetryDelayMs; ++i) {
      delay_ms = std::min(delay_ms * 2, kMaxRetryDelayMs);
    }
    // 抖动：避免多副本在同一时刻一起重试；用时钟低位做种子，不引入随机库。
    const auto seed = std::chrono::steady_clock::now().time_since_epoch();
    delay_ms += static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(seed).count() % 100);
    delay_ms = std::min(delay_ms, kMaxRetryDelayMs);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (queue_->cancelled()) return false;
      const auto left = deadline - std::chrono::steady_clock::now();
      const auto left_ms = std::chrono::duration_cast<std::chrono::milliseconds>(left);
      if (left_ms <= std::chrono::milliseconds(0)) break;
      std::this_thread::sleep_for(std::min(left_ms, std::chrono::milliseconds(50)));
    }
    return !queue_->cancelled();
  }

  void reset_attempt() {
    mode_ = BodyMode::Unknown;
    pending_.clear();
    json_body_.clear();
    raw_tail_.clear();
    http_status_ = 0;
    got_delta_ = false;
    closed_ = false;
    aborted_ = false;
    finish_reason_.clear();
    prompt_tokens_ = -1;
    completion_tokens_ = -1;
    attempt_error_.clear();
  }

  static size_t on_body(char* data, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* self = static_cast<OpenAiStream*>(userdata);
    if (self->queue_->cancelled()) return 0;  // 返回 0 -> curl 立即中断传输
    const std::size_t n = size * nmemb;
    self->consume(data, n);
    return n;
  }

  void consume(const char* data, std::size_t n) {
    if (aborted_ || closed_) return;
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
    if (aborted_ || closed_) return;
    if (mode_ == BodyMode::Json) {
      json_body_ += chunk;
      return;
    }
    pending_ += chunk;
    while (!aborted_ && !closed_) {
      const std::size_t pos = pending_.find('\n');
      if (pos == std::string::npos) break;
      const std::string line = pending_.substr(0, pos);
      pending_.erase(0, pos + 1);
      handle_line(line);
    }
  }

  void handle_line(const std::string& raw_line) {
    if (aborted_ || closed_) return;
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
      set_abort(upstream_error_message(j, http_status_));
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
        push_delta(delta["content"].get<std::string>());
      }
    }
    if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
      finish_reason_ = choice["finish_reason"].get<std::string>();
    }
  }

  void push_delta(const std::string& text) {
    if (text.empty() || closed_ || aborted_) return;
    got_delta_ = true;
    Event e;
    e.kind = EventKind::Delta;
    e.text = text;
    queue_->push(std::move(e));
  }

  void emit_done() {
    if (closed_ || aborted_) return;
    closed_ = true;
    Event e;
    e.kind = EventKind::Done;
    e.finish_reason = finish_reason_.empty() ? "stop" : finish_reason_;
    e.prompt_tokens = prompt_tokens_;
    e.completion_tokens = completion_tokens_;
    queue_->push(std::move(e));
    queue_->close();
  }

  void set_abort(std::string message) {
    if (aborted_ || closed_) return;
    aborted_ = true;
    attempt_error_ = std::move(message);
  }

  void fail(std::string message) {
    if (closed_ || queue_->cancelled()) return;
    closed_ = true;
    Event e;
    e.kind = EventKind::Error;
    e.text = std::move(message);
    queue_->push(std::move(e));
    queue_->close();
  }

  OpenAiOptions opts_;
  ChatRequest req_;
  std::shared_ptr<KeyRing> keys_;

  BodyMode mode_ = BodyMode::Unknown;
  std::string pending_;
  std::string json_body_;
  std::string raw_tail_;

  int http_status_ = 0;
  bool got_delta_ = false;
  bool closed_ = false;
  bool aborted_ = false;
  std::string attempt_error_;
  std::string finish_reason_;
  int prompt_tokens_ = -1;
  int completion_tokens_ = -1;
};

class OpenAiProvider final : public Provider {
 public:
  explicit OpenAiProvider(OpenAiOptions opts)
      : opts_(std::move(opts)), keys_(std::make_shared<KeyRing>(collect_keys(opts_), key_config(opts_))) {
    metrics::set_upstream_keys_available(keys_->available());
  }

  std::string name() const override { return "openai-compatible"; }

  std::unique_ptr<Stream> chat_stream(const ChatRequest& req) override {
    auto queue = std::make_shared<EventQueue>();
    auto stream = std::make_unique<OpenAiStream>(queue, opts_, req, keys_);
    stream->start();
    return stream;
  }

 private:
  static std::vector<std::string> collect_keys(const OpenAiOptions& opts) {
    std::vector<std::string> keys = opts.api_keys;
    if (keys.empty() && !opts.api_key.empty()) keys.push_back(opts.api_key);
    return keys;
  }

  static KeyRingConfig key_config(const OpenAiOptions& opts) {
    KeyRingConfig config;
    config.rpm_limit = opts.key_rpm_limit;
    config.tpm_limit = opts.key_tpm_limit;
    config.failure_threshold = opts.key_failure_threshold;
    config.cooldown_s = opts.key_cooldown_s;
    config.cooldown_max_s = opts.key_cooldown_max_s;
    return config;
  }

  OpenAiOptions opts_;
  std::shared_ptr<KeyRing> keys_;
};

}  // namespace

std::unique_ptr<Provider> make_openai_provider(OpenAiOptions options) {
  return std::make_unique<OpenAiProvider>(std::move(options));
}

}  // namespace llm
