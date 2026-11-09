// token 预算上下文构建：system + 最近历史 + 本轮 user，按估算 token 裁剪。
//
// 估算不是 tokenizer（见 token_estimate.h），但能防止“消息条数没超、上下文却爆掉”。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "llm/types.h"

namespace chat {

struct ContextBudget {
  int max_context_tokens = 8192;       // 模型上下文总预算
  int reserve_output_tokens = 1024;    // 给 completion 预留
  int safety_tokens = 64;              // 估算误差/格式开销
};

/// 返回可直接发给 Provider 的消息序列（system 可选，最后一条一定是 user）。
/// 无法在预算内放下 system + user 时抛 ContextTooLong。
std::vector<llm::Message> build_context(const std::string& system_prompt,
                                        const std::vector<llm::Message>& history,
                                        const std::string& user_text,
                                        const ContextBudget& budget);

/// OpenAI `messages` 数组是权威上下文时使用：不再读服务端历史。
/// 保留客户端给的 system（没有则用配置的 system prompt），从最新消息往前装，
/// 超预算时丢弃更早的对话；最后一条必须是 user。
std::vector<llm::Message> build_context_from_messages(
    const std::string& system_prompt, const std::vector<llm::Message>& messages,
    const ContextBudget& budget);

}  // namespace chat
