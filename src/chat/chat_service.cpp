#include "chat/chat_service.h"

#include <mutex>
#include <utility>

namespace chat {
namespace {

// 装饰器：把上游的增量事件透传给 HTTP 层，同时把完整回复写回会话。
// 「流式输出」和「持久化」在这里解耦，HTTP 层不需要自己攒字符串。
class RecordingStream final : public llm::Stream {
 public:
  RecordingStream(std::shared_ptr<llm::Stream> inner, SessionStore& store, std::string session_id)
      : inner_(std::move(inner)), store_(store), session_id_(std::move(session_id)) {}

  llm::Pull next(llm::Event& out, std::chrono::milliseconds timeout) override {
    const llm::Pull pull = inner_->next(out, timeout);
    switch (pull) {
      case llm::Pull::Event:
        if (out.kind == llm::EventKind::Delta) {
          assistant_text_ += out.text;
        } else if (out.kind == llm::EventKind::Done || out.kind == llm::EventKind::Error) {
          commit();
        }
        break;
      case llm::Pull::End:
        commit();
        break;
      case llm::Pull::Timeout:
      case llm::Pull::Cancelled:
        break;
    }
    return pull;
  }

  void cancel() override {
    inner_->cancel();
    commit();
  }

  std::string describe() const override { return inner_->describe(); }

 private:
  void commit() {
    std::lock_guard<std::mutex> lock(commit_mutex_);
    if (committed_) return;
    committed_ = true;
    if (!assistant_text_.empty()) {
      store_.append(session_id_, llm::Message{"assistant", assistant_text_});
    }
  }

  std::shared_ptr<llm::Stream> inner_;
  SessionStore& store_;
  std::string session_id_;
  std::string assistant_text_;
  std::mutex commit_mutex_;
  bool committed_ = false;
};

}  // namespace

ChatService::ChatService(llm::Provider& provider, SessionStore& store, ChatOptions options)
    : provider_(provider), store_(store), options_(std::move(options)) {}

ChatService::Turn ChatService::start(const std::string& session_id, const std::string& user_text,
                                     const std::string& model_override) {
  const std::string sid = session_id.empty() ? store_.create() : session_id;

  llm::ChatRequest request;
  request.model = model_override.empty() ? options_.default_model : model_override;
  request.temperature = options_.temperature;
  request.max_tokens = options_.max_tokens;

  if (!options_.system_prompt.empty()) {
    request.messages.push_back(llm::Message{"system", options_.system_prompt});
  }
  for (const auto& message : store_.history(sid, options_.max_history_messages)) {
    request.messages.push_back(message);
  }
  request.messages.push_back(llm::Message{"user", user_text});

  store_.append(sid, llm::Message{"user", user_text});

  std::shared_ptr<llm::Stream> stream = provider_.chat_stream(request);
  stream = std::make_shared<RecordingStream>(std::move(stream), store_, sid);

  return Turn{sid, std::move(stream)};
}

}  // namespace chat
