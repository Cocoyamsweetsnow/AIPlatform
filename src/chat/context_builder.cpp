#include "chat/context_builder.h"

#include <algorithm>
#include <string>
#include <vector>

#include "chat/errors.h"
#include "chat/session_store.h"
#include "chat/token_estimate.h"

namespace chat {
namespace {

constexpr int kMessageOverheadTokens = 4;

int message_tokens(const llm::Message& message) {
  return estimate_tokens(message.content) + kMessageOverheadTokens;
}

}  // namespace

std::vector<llm::Message> build_context(const std::string& system_prompt,
                                        const std::vector<llm::Message>& history,
                                        const std::string& user_text,
                                        const ContextBudget& budget) {
  const int allowed = budget.max_context_tokens - budget.reserve_output_tokens -
                      budget.safety_tokens;
  if (allowed <= 0) {
    throw ContextTooLong("AIP_MAX_CONTEXT_TOKENS 必须大于预留输出与安全余量");
  }

  int remaining = allowed;
  std::vector<llm::Message> system_messages;
  if (!system_prompt.empty()) {
    llm::Message system{"system", system_prompt};
    const int tokens = message_tokens(system);
    if (tokens > remaining) {
      throw ContextTooLong("system prompt 已超过上下文预算");
    }
    remaining -= tokens;
    system_messages.push_back(std::move(system));
  }

  const llm::Message user{"user", user_text};
  const int user_tokens = message_tokens(user);
  if (user_tokens > remaining) {
    throw ContextTooLong("本轮消息超过可用的上下文预算（" + std::to_string(remaining) +
                         " tokens）");
  }
  remaining -= user_tokens;

  // 从最新历史往前装，装不下就停；保持消息连续，避免上下文出现跳跃。
  std::vector<llm::Message> selected;
  for (auto it = history.rbegin(); it != history.rend(); ++it) {
    const int tokens = message_tokens(*it);
    if (tokens > remaining) break;
    selected.push_back(*it);
    remaining -= tokens;
  }
  std::reverse(selected.begin(), selected.end());
  trim_context(selected);

  std::vector<llm::Message> out;
  out.reserve(system_messages.size() + selected.size() + 1);
  for (auto& message : system_messages) out.push_back(std::move(message));
  for (auto& message : selected) out.push_back(std::move(message));
  out.push_back(user);
  return out;
}

}  // namespace chat
