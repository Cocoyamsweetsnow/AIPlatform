// 业务层：拼上下文、调 Provider、把回复落库（骨架里是内存）。
// HTTP 层只看到「一个 Turn（会话 ID + 流）」，不关心模型细节。
#pragma once

#include <memory>
#include <string>

#include "chat/session_store.h"
#include "llm/types.h"

namespace chat {

struct ChatOptions {
  std::string system_prompt;
  std::size_t max_history_messages = 20;
  int max_tokens = 1024;
  double temperature = 0.7;
  std::string default_model;
};

class ChatService {
 public:
  ChatService(llm::Provider& provider, SessionStore& store, ChatOptions options);

  struct Turn {
    std::string session_id;
    std::shared_ptr<llm::Stream> stream;
  };

  /// 开始一轮对话；session_id 为空时新建会话。
  Turn start(const std::string& session_id, const std::string& user_text,
             const std::string& model_override = {});

  const ChatOptions& options() const { return options_; }

 private:
  llm::Provider& provider_;
  SessionStore& store_;
  ChatOptions options_;
};

}  // namespace chat
