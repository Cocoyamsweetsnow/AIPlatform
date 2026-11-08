// token 预算上下文单测；不需要数据库。
#include <cstdio>
#include <string>
#include <vector>

#include "chat/context_builder.h"
#include "chat/errors.h"
#include "chat/token_estimate.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
      ++g_failures;                                                        \
    }                                                                      \
  } while (false)

int tokens(const std::string& text) { return chat::estimate_tokens(text) + 4; }

}  // namespace

int main() {
  std::printf("running context tests...\n");

  chat::ContextBudget budget;
  budget.max_context_tokens = 80;
  budget.reserve_output_tokens = 16;
  budget.safety_tokens = 4;  // allowed = 60

  std::vector<llm::Message> history;
  for (int i = 0; i < 12; ++i) {
    history.push_back(llm::Message{"user", "历史消息 " + std::to_string(i) + " ........"});
    history.push_back(llm::Message{"assistant", "回答 " + std::to_string(i) + " ........"});
  }

  const auto context =
      chat::build_context("你是助手", history, "最后一轮问题", budget);
  CHECK(!context.empty());
  CHECK(context.back().role == "user");
  CHECK(context.back().content == "最后一轮问题");
  CHECK(context.front().role == "system");

  int used = 0;
  for (const auto& message : context) used += tokens(message.content);
  CHECK(used <= 60);

  // 历史被裁剪后不能以 assistant 开头。
  if (context.size() > 1 && context[0].role == "system") {
    CHECK(context[1].role == "user");
  } else {
    CHECK(context[0].role == "user");
  }

  // 单独一条 user 就超预算 -> ContextTooLong。
  bool too_long = false;
  try {
    chat::build_context("", {}, std::string(4000, 'x'), budget);
  } catch (const chat::ContextTooLong&) {
    too_long = true;
  }
  CHECK(too_long);

  // 预算为负（max_context 小于 reserve+safety）也拒绝。
  bool bad_budget = false;
  try {
    chat::ContextBudget tiny;
    tiny.max_context_tokens = 32;
    tiny.reserve_output_tokens = 16;
    tiny.safety_tokens = 64;
    chat::build_context("", {}, "hi", tiny);
  } catch (const chat::ContextTooLong&) {
    bad_budget = true;
  }
  CHECK(bad_budget);

  if (g_failures == 0) {
    std::printf("context tests passed\n");
    return 0;
  }
  std::printf("%d context check(s) failed\n", g_failures);
  return 1;
}
