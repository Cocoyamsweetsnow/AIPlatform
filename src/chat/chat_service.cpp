#include "chat/chat_service.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "chat/context_builder.h"
#include "chat/errors.h"
#include "chat/token_estimate.h"
#include "common/log.h"

namespace chat {
namespace {

// 用首条用户消息生成会话标题：按 UTF-8 码点截断，避免切断多字节字符。
std::string make_title(const std::string& text) {
  constexpr std::size_t kMaxCodepoints = 40;
  std::string out;
  std::size_t i = 0;
  std::size_t codepoints = 0;
  bool pending_space = false;

  while (i < text.size() && codepoints < kMaxCodepoints) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    std::size_t len = 1;
    if ((c & 0xE0) == 0xC0) {
      len = 2;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
    }
    if (i + len > text.size()) len = 1;

    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      pending_space = !out.empty();
      i += 1;
      continue;
    }
    if (pending_space) {
      out.push_back(' ');
      pending_space = false;
    }
    out.append(text, i, len);
    i += len;
    ++codepoints;
  }

  if (out.empty()) return "新会话";
  return out;
}

// 装饰器：把上游的增量事件透传给 HTTP 层，同时把完整回复写回会话。
// 「流式输出」和「持久化」在这里解耦，HTTP 层不需要自己攒字符串。
class RecordingStream final : public llm::Stream {
 public:
  RecordingStream(std::shared_ptr<llm::Stream> inner, SessionStore& store, std::string session_id,
                  usage::UsageSink* usage, usage::UsageEvent usage_event,
                  quota::QuotaGuard quota_guard, std::shared_ptr<TurnObserver> observer)
      : inner_(std::move(inner)),
        store_(store),
        session_id_(std::move(session_id)),
        usage_(usage),
        usage_event_(std::move(usage_event)),
        quota_guard_(std::move(quota_guard)),
        observer_(std::move(observer)),
        started_at_(std::chrono::steady_clock::now()) {}

  ~RecordingStream() override { commit(/*success=*/false, "stream destroyed"); }

  llm::Pull next(llm::Event& out, std::chrono::milliseconds timeout) override {
    const llm::Pull pull = inner_->next(out, timeout);
    switch (pull) {
      case llm::Pull::Event:
        if (out.kind == llm::EventKind::Delta) {
          if (!first_token_at_.has_value()) first_token_at_ = std::chrono::steady_clock::now();
          assistant_text_ += out.text;
        } else if (out.kind == llm::EventKind::Done) {
          prompt_tokens_ = out.prompt_tokens;
          completion_tokens_ = out.completion_tokens;
          commit(/*success=*/true, {});
        } else if (out.kind == llm::EventKind::Error) {
          commit(/*success=*/false, out.text);
        }
        break;
      case llm::Pull::End:
        commit(/*success=*/true, {});
        break;
      case llm::Pull::Timeout:
        break;
      case llm::Pull::Cancelled:
        commit(/*success=*/false, "cancelled");
        break;
    }
    return pull;
  }

  void cancel() override {
    inner_->cancel();
    commit(/*success=*/false, "cancelled");
  }

  std::string describe() const override { return inner_->describe(); }

 private:
  void commit(bool success, std::string error) {
    std::lock_guard<std::mutex> lock(commit_mutex_);
    if (committed_) return;
    committed_ = true;

    std::int64_t assistant_message_id = 0;
    if (!assistant_text_.empty()) {
      try {
        assistant_message_id =
            store_.append(session_id_, llm::Message{"assistant", assistant_text_});
      } catch (const std::exception& e) {
        // 流已经发给客户端了，这里不能把异常抛回泵线程；记录后让上层从日志发现。
        platform::log_error("写入 assistant 消息失败（session=" + session_id_ + "）: " + e.what());
        success = false;
        error = e.what();
      }
    }

    if (observer_ != nullptr) {
      try {
        if (success && assistant_message_id > 0) {
          observer_->on_success(session_id_, assistant_message_id);
        } else {
          observer_->on_failure(session_id_, error.empty() ? "empty response" : error);
        }
      } catch (const std::exception& e) {
        platform::log_error("幂等回调失败（session=" + session_id_ + "）: " + e.what());
      }
    }

    if (usage_ == nullptr || recorded_) {
      quota_guard_.release();
      return;
    }
    recorded_ = true;

    usage_event_.prompt_tokens =
        prompt_tokens_ >= 0 ? prompt_tokens_ : usage_event_.prompt_tokens;
    usage_event_.completion_tokens =
        completion_tokens_ >= 0 ? completion_tokens_ : estimate_tokens(assistant_text_);
    const auto elapsed = std::chrono::steady_clock::now() - started_at_;
    usage_event_.latency_ms =
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    usage_event_.ttft_ms =
        first_token_at_.has_value()
            ? static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   *first_token_at_ - started_at_)
                                   .count())
            : -1;
    try {
      usage_->record(usage_event_);
    } catch (const std::exception& e) {
      platform::log_error("写入 usage 失败（session=" + session_id_ + "）: " + e.what());
    }
    // 先落 usage（或确认写入失败），再释放 PG 配额预留；避免预留先删、用量还没写
    // 导致同一时刻的配额检查看到“空窗”。
    quota_guard_.release();
  }

  std::shared_ptr<llm::Stream> inner_;
  SessionStore& store_;
  std::string session_id_;
  std::string assistant_text_;
  usage::UsageSink* usage_ = nullptr;
  usage::UsageEvent usage_event_;
  quota::QuotaGuard quota_guard_;
  std::shared_ptr<TurnObserver> observer_;
  std::chrono::steady_clock::time_point started_at_;
  std::optional<std::chrono::steady_clock::time_point> first_token_at_;
  int prompt_tokens_ = -1;
  int completion_tokens_ = -1;
  std::mutex commit_mutex_;
  bool committed_ = false;
  bool recorded_ = false;
};

}  // namespace

ChatService::ChatService(llm::Provider& provider, SessionStore& store, ChatOptions options,
                         usage::UsageSink* usage)
    : provider_(provider), store_(store), options_(std::move(options)), usage_(usage) {}

ChatService::Turn ChatService::start(const auth::Principal& principal,
                                     const StartOptions& options,
                                     quota::QuotaGuard quota_guard,
                                     std::shared_ptr<TurnObserver> observer) {
  if (!principal.is_admin && principal.user_id == 0) {
    throw Forbidden("需要登录后才能发起对话");
  }

  std::string user_text = options.user_text;
  if (user_text.empty() && !options.messages.empty()) {
    for (auto it = options.messages.rbegin(); it != options.messages.rend(); ++it) {
      if (it->role == "user") {
        user_text = it->content;
        break;
      }
    }
  }
  if (user_text.empty()) {
    throw ContextTooLong("本轮消息不能为空");
  }

  const std::string model =
      options.model_override.empty() ? options_.default_model : options.model_override;
  const std::string title = make_title(user_text);

  std::string sid = options.session_id;
  if (sid.empty()) {
    sid = store_.create(principal.user_id, title, model);
  } else if (!store_.exists(sid)) {
    if (!principal.is_admin) throw Forbidden("会话不存在或无权访问");
    store_.ensure(sid, principal.user_id, title, model);
    if (!store_.owns(principal.user_id, principal.is_admin, sid)) {
      throw Forbidden("会话不存在或无权访问");
    }
  } else if (!store_.owns(principal.user_id, principal.is_admin, sid)) {
    throw Forbidden("无权访问该会话");
  }

  llm::ChatRequest request;
  request.model = model;
  request.temperature =
      options.temperature >= 0.0 ? options.temperature : options_.temperature;
  request.max_tokens =
      options.max_tokens > 0 ? options.max_tokens : options_.max_tokens;
  request.top_p = options.top_p;
  request.stop = options.stop;
  request.has_presence_penalty = options.has_presence_penalty;
  request.presence_penalty = options.presence_penalty;
  request.has_frequency_penalty = options.has_frequency_penalty;
  request.frequency_penalty = options.frequency_penalty;
  request.has_seed = options.has_seed;
  request.seed = options.seed;
  request.response_format = options.response_format;
  request.user = options.user;
  request.include_usage = options.include_usage;

  ContextBudget budget;
  budget.max_context_tokens = options_.max_context_tokens;
  budget.reserve_output_tokens = request.max_tokens;
  if (!options.messages.empty()) {
    // OpenAI messages 数组是权威上下文：客户端每次都带完整历史，
    // 不能再叠加服务端历史，否则会出现重复消息。
    request.messages =
        build_context_from_messages(options_.system_prompt, options.messages, budget);
  } else {
    const std::vector<llm::Message> history =
        store_.history(sid, options_.max_history_messages);
    request.messages = build_context(options_.system_prompt, history, user_text, budget);
  }

  store_.append(sid, llm::Message{"user", user_text});

  int estimated_prompt_tokens = 0;
  for (const auto& message : request.messages) {
    estimated_prompt_tokens += estimate_tokens(message.content) + 4;
  }

  usage::UsageEvent usage_event;
  usage_event.user_id = principal.user_id;
  usage_event.session_id = sid;
  usage_event.request_id = options.request_id;
  usage_event.model = model;
  usage_event.prompt_tokens = estimated_prompt_tokens;

  std::shared_ptr<llm::Stream> stream = provider_.chat_stream(request);
  stream = std::make_shared<RecordingStream>(std::move(stream), store_, sid, usage_,
                                              std::move(usage_event), std::move(quota_guard),
                                              std::move(observer));

  return Turn{sid, std::move(stream), estimated_prompt_tokens};
}

ChatService::Turn ChatService::start(const auth::Principal& principal,
                                     const std::string& session_id,
                                     const std::string& user_text,
                                     const std::string& model_override,
                                     const std::string& request_id,
                                     quota::QuotaGuard quota_guard,
                                     std::shared_ptr<TurnObserver> observer) {
  StartOptions options;
  options.session_id = session_id;
  options.user_text = user_text;
  options.model_override = model_override;
  options.request_id = request_id;
  return start(principal, options, std::move(quota_guard), std::move(observer));
}

}  // namespace chat
