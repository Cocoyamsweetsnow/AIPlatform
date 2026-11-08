// 幂等存储 / 流完成回调 / 重放流单元测试；不需要 PostgreSQL。
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

#include "chat/chat_service.h"
#include "chat/idempotency.h"
#include "chat/replay_stream.h"
#include "chat/session_store.h"
#include "llm/mock_provider.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
      ++g_failures;                                                        \
    }                                                                      \
  } while (false)

void test_memory_store() {
  chat::MemoryIdempotencyStore store;
  const std::string scope = "u:1";
  const std::string request_id = "req-1";

  const auto first = store.claim(scope, request_id);
  CHECK(first.status == chat::DedupStatus::Claimed);

  const auto busy = store.claim(scope, request_id);
  CHECK(busy.status == chat::DedupStatus::InProgress);

  store.complete(scope, request_id, "s_1", 42);
  const auto completed = store.claim(scope, request_id);
  CHECK(completed.status == chat::DedupStatus::Completed);
  CHECK(completed.session_id == "s_1");
  CHECK(completed.assistant_message_id == 42);

  store.fail("u:2", "req-2", "upstream_error", "boom");
  const auto failed = store.claim("u:2", "req-2");
  CHECK(failed.status == chat::DedupStatus::Claimed);  // failed 可重试

  store.remove(scope, request_id);
  CHECK(store.claim(scope, request_id).status == chat::DedupStatus::Claimed);
}

void test_replay_stream() {
  chat::ReplayStream stream("hello");
  llm::Event event;
  CHECK(stream.next(event, std::chrono::milliseconds(1)) == llm::Pull::Event);
  CHECK(event.kind == llm::EventKind::Delta);
  CHECK(event.text == "hello");
  CHECK(stream.next(event, std::chrono::milliseconds(1)) == llm::Pull::Event);
  CHECK(event.kind == llm::EventKind::Done);
  CHECK(stream.next(event, std::chrono::milliseconds(1)) == llm::Pull::End);

  chat::ReplayStream cancelled("x");
  cancelled.cancel();
  CHECK(cancelled.next(event, std::chrono::milliseconds(1)) == llm::Pull::Cancelled);
}

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

void test_observer_marks_completed() {
  auto provider = llm::make_mock_provider();
  chat::MemorySessionStore sessions;
  chat::MemoryIdempotencyStore dedup;
  chat::ChatOptions options;
  options.default_model = "mock-model";
  chat::ChatService service(*provider, sessions, options);

  auth::Principal principal;
  principal.user_id = 1;

  const std::string scope = principal.scope();
  const std::string request_id = "req-observer";
  CHECK(dedup.claim(scope, request_id).status == chat::DedupStatus::Claimed);

  auto observer = std::make_shared<chat::IdempotencyObserver>(dedup, scope, request_id);
  auto turn = service.start(principal, {}, "幂等测试", {}, request_id, quota::QuotaGuard{},
                            observer);
  const std::string reply = drain(*turn.stream);
  CHECK(!reply.empty());

  const auto record = dedup.claim(scope, request_id);
  CHECK(record.status == chat::DedupStatus::Completed);
  CHECK(record.session_id == turn.session_id);
  CHECK(record.assistant_message_id > 0);
  if (record.assistant_message_id > 0) {
    const auto message = sessions.message(record.assistant_message_id);
    CHECK(message.has_value());
    if (message.has_value()) CHECK(message->content == reply);
  }
}

void test_observer_marks_failed_on_cancel() {
  auto provider = llm::make_mock_provider();
  chat::MemorySessionStore sessions;
  chat::MemoryIdempotencyStore dedup;
  chat::ChatService service(*provider, sessions, chat::ChatOptions{});

  auth::Principal principal;
  principal.user_id = 2;
  const std::string scope = principal.scope();
  const std::string request_id = "req-cancel";
  CHECK(dedup.claim(scope, request_id).status == chat::DedupStatus::Claimed);

  auto observer = std::make_shared<chat::IdempotencyObserver>(dedup, scope, request_id);
  auto turn = service.start(principal, {}, "取消", {}, request_id, quota::QuotaGuard{}, observer);
  llm::Event first;
  turn.stream->next(first, std::chrono::seconds(2));
  turn.stream->cancel();

  CHECK(dedup.claim(scope, request_id).status == chat::DedupStatus::Claimed);
}

}  // namespace

int main() {
  std::printf("running idempotency tests...\n");
  test_memory_store();
  test_replay_stream();
  test_observer_marks_completed();
  test_observer_marks_failed_on_cancel();

  if (g_failures == 0) {
    std::printf("idempotency tests passed\n");
    return 0;
  }
  std::printf("%d idempotency check(s) failed\n", g_failures);
  return 1;
}
