// 业务层：拼上下文、调 Provider、把回复落库。
// HTTP 层只看到「一个 Turn（会话 ID + 流）」，不关心模型与存储细节。
#pragma once

#include <memory>
#include <string>

#include "auth/principal.h"
#include "chat/session_store.h"
#include "chat/turn_observer.h"
#include "llm/types.h"
#include "quota/quota.h"
#include "usage/usage.h"

namespace chat {

struct ChatOptions {
  std::string system_prompt;
  std::size_t max_history_messages = 20;
  int max_tokens = 1024;
  int max_context_tokens = 8192;
  double temperature = 0.7;
  std::string default_model;
};

class ChatService {
 public:
  ChatService(llm::Provider& provider, SessionStore& store, ChatOptions options,
              usage::UsageSink* usage = nullptr);

  struct Turn {
    std::string session_id;
    std::shared_ptr<llm::Stream> stream;
  };

  /// 开始一轮对话；session_id 为空时新建会话。
  /// 会话存在但不属于 principal 时抛 chat::Forbidden；
  /// 会话不存在且不是 admin 时也抛 Forbidden（避免枚举他人会话 ID）。
  Turn start(const auth::Principal& principal, const std::string& session_id,
             const std::string& user_text, const std::string& model_override = {},
             const std::string& request_id = {}, quota::QuotaGuard quota_guard = {},
             std::shared_ptr<TurnObserver> observer = nullptr);

  const ChatOptions& options() const { return options_; }

 private:
  llm::Provider& provider_;
  SessionStore& store_;
  ChatOptions options_;
  usage::UsageSink* usage_ = nullptr;
};

}  // namespace chat
