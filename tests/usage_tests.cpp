// 用量记录与 token 估算的单元测试；不需要 PostgreSQL。
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "chat/chat_service.h"
#include "chat/session_store.h"
#include "chat/token_estimate.h"
#include "llm/mock_provider.h"
#include "usage/usage.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
      ++g_failures;                                                        \
    }                                                                      \
  } while (false)

class FakeUsageSink final : public usage::UsageSink {
 public:
  void record(const usage::UsageEvent& event) override { events.push_back(event); }

  usage::UsageSummary summary_today(std::int64_t) override {
    usage::UsageSummary summary;
    summary.requests = static_cast<long long>(events.size());
    for (const auto& event : events) {
      summary.prompt_tokens += event.prompt_tokens;
      summary.completion_tokens += event.completion_tokens;
    }
    return summary;
  }

  std::vector<usage::UsageEvent> events;
};

std::string drain(llm::Stream& stream) {
  std::string text;
  while (true) {
    llm::Event event;
    const llm::Pull pull = stream.next(event, std::chrono::seconds(2));
    if (pull == llm::Pull::End || pull == llm::Pull::Cancelled) break;
    if (pull == llm::Pull::Timeout) continue;
    if (event.kind == llm::EventKind::Delta) text += event.text;
    if (event.kind == llm::EventKind::Done || event.kind == llm::EventKind::Error) break;
  }
  return text;
}

void test_estimate_tokens() {
  CHECK(chat::estimate_tokens("") == 0);
  CHECK(chat::estimate_tokens("hello") >= 1);
  CHECK(chat::estimate_tokens("你好") >= 2);
  CHECK(chat::estimate_tokens("a") <= chat::estimate_tokens("hello world"));
}

void test_usage_recorded() {
  auto provider = llm::make_mock_provider();
  chat::MemorySessionStore store;
  FakeUsageSink sink;
  chat::ChatOptions options;
  options.default_model = "mock-model";
  chat::ChatService service(*provider, store, options, &sink);

  auth::Principal principal;
  principal.user_id = 7;
  auto turn = service.start(principal, {}, "统计一下 token", {}, "req-usage-1");
  const std::string reply = drain(*turn.stream);
  CHECK(!reply.empty());
  CHECK(sink.events.size() == 1);
  if (!sink.events.empty()) {
    const auto& event = sink.events.front();
    CHECK(event.user_id == 7);
    CHECK(event.session_id == turn.session_id);
    CHECK(event.request_id == "req-usage-1");
    CHECK(event.model == "mock-model");
    CHECK(event.prompt_tokens > 0);
    CHECK(event.completion_tokens > 0);
    CHECK(event.latency_ms >= 0);
    CHECK(event.ttft_ms >= 0);
  }

  // 取消也要落一份已经产生的用量
  auto cancelled = service.start(principal, {}, "取消测试", {}, "req-usage-2");
  llm::Event first;
  cancelled.stream->next(first, std::chrono::seconds(2));
  cancelled.stream->cancel();
  CHECK(sink.events.size() == 2);
}

}  // namespace

int main() {
  std::printf("running usage tests...\n");
  test_estimate_tokens();
  test_usage_recorded();

  if (g_failures == 0) {
    std::printf("usage tests passed\n");
    return 0;
  }
  std::printf("%d usage check(s) failed\n", g_failures);
  return 1;
}
