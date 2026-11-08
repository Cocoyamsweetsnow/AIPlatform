// 极简单元测试：不引入第三方框架，保证 `cmake -DAIPLATFORM_BUILD_TESTS=ON` 即可运行。
// 真实项目建议换 GoogleTest（见 docs/ROADMAP.md 的测试策略）。
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "chat/chat_service.h"
#include "chat/session_store.h"
#include "config.h"
#include "http/public_config.h"
#include "llm/event_queue.h"
#include "llm/key_ring.h"
#include "llm/mock_provider.h"
#include "web/assets.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
      ++g_failures;                                                        \
    }                                                                      \
  } while (false)

using namespace std::chrono_literals;

void test_event_queue() {
  llm::EventQueue queue;
  llm::Event first;
  first.kind = llm::EventKind::Delta;
  first.text = "你好";
  queue.push(first);

  llm::Event out;
  CHECK(queue.pull(out, 10ms) == llm::Pull::Event);
  CHECK(out.text == "你好");

  // 队列空且未关闭 -> 超时
  CHECK(queue.pull(out, 10ms) == llm::Pull::Timeout);

  queue.close();
  CHECK(queue.pull(out, 10ms) == llm::Pull::End);
}

void test_queue_cancel_drops_pending() {
  llm::EventQueue queue;
  llm::Event e;
  e.text = "x";
  queue.push(e);
  queue.cancel();
  llm::Event out;
  CHECK(queue.pull(out, 10ms) == llm::Pull::Cancelled);
}

void test_session_store_trim() {
  chat::SessionStore store(/*max_messages=*/3, /*max_sessions=*/2);
  const std::string id = store.create();
  CHECK(store.exists(id));

  for (int i = 0; i < 5; ++i) {
    store.append(id, llm::Message{"user", "m" + std::to_string(i)});
  }
  CHECK(store.message_count(id) == 3);

  const auto recent = store.history(id, 10);
  CHECK(recent.size() == 3);
  CHECK(recent.back().content == "m4");

  // limit 生效
  const auto limited = store.history(id, 2);
  CHECK(limited.size() == 2);
  CHECK(limited.front().content == "m3");
}

void test_session_store_lru_eviction() {
  chat::SessionStore store(10, /*max_sessions=*/2);
  const std::string a = store.create();
  const std::string b = store.create();
  CHECK(store.session_count() == 2);
  (void)a;
  (void)b;
  // 第三个会话会淘汰最旧的
  store.create();
  CHECK(store.session_count() == 2);
}

std::string drain(llm::Stream& stream, int* prompt_tokens = nullptr) {
  std::string text;
  while (true) {
    llm::Event ev;
    const llm::Pull pull = stream.next(ev, 2s);
    if (pull == llm::Pull::End || pull == llm::Pull::Cancelled) break;
    if (pull == llm::Pull::Timeout) continue;
    if (ev.kind == llm::EventKind::Delta) {
      text += ev.text;
    } else if (ev.kind == llm::EventKind::Done) {
      if (prompt_tokens != nullptr) *prompt_tokens = ev.prompt_tokens;
      break;
    } else {
      std::printf("  stream error: %s\n", ev.text.c_str());
      break;
    }
  }
  return text;
}

void test_mock_provider_streams() {
  auto provider = llm::make_mock_provider();
  llm::ChatRequest req;
  req.model = "mock";
  req.messages.push_back(llm::Message{"user", "测试"});

  auto stream = provider->chat_stream(req);
  CHECK(stream != nullptr);
  const std::string text = drain(*stream);
  CHECK(!text.empty());
  CHECK(text.find("测试") != std::string::npos);
}

void test_chat_service_records_history() {
  auto provider = llm::make_mock_provider();
  chat::SessionStore store;
  chat::ChatOptions options;
  options.system_prompt = "system";
  chat::ChatService service(*provider, store, options);

  auto turn = service.start({}, "第一句话");
  CHECK(!turn.session_id.empty());
  const std::string reply = drain(*turn.stream);
  CHECK(!reply.empty());

  // 取消后不应重复落库
  turn.stream->cancel();

  const auto history = store.history(turn.session_id, 10);
  CHECK(history.size() == 2);
  CHECK(history[0].role == "user");
  CHECK(history[0].content == "第一句话");
  CHECK(history[1].role == "assistant");
  CHECK(history[1].content == reply);
}

void test_cancel_is_idempotent() {
  auto provider = llm::make_mock_provider();
  llm::ChatRequest req;
  req.messages.push_back(llm::Message{"user", "取消"});
  auto stream = provider->chat_stream(req);
  stream->cancel();
  stream->cancel();

  llm::Event ev;
  const llm::Pull pull = stream->next(ev, 50ms);
  CHECK(pull == llm::Pull::Cancelled || pull == llm::Pull::End);
}

void test_web_assets() {
  // "/" 是 "/index.html" 的别名：直接访问根路径必须能拿到页面。
  const web::Asset* index = web::find_asset("/");
  CHECK(index != nullptr);
  if (index != nullptr) {
    CHECK(index->path == "/index.html");
    CHECK(index->content_type.find("text/html") != std::string::npos);
    CHECK(index->content.find("AIPlatform") != std::string::npos);
    CHECK(index->content.find("assets/app.js") != std::string::npos);
  }

  const web::Asset* css = web::find_asset("/assets/app.css");
  CHECK(css != nullptr);
  if (css != nullptr) {
    CHECK(css->content_type.find("text/css") != std::string::npos);
    CHECK(!css->content.empty());
  }

  const web::Asset* js = web::find_asset("/assets/app.js");
  CHECK(js != nullptr);
  if (js != nullptr) {
    CHECK(js->content_type.find("javascript") != std::string::npos);
    CHECK(js->content.find("/v1/chat/completions") != std::string::npos);
  }

  CHECK(web::asset_count() >= 4);
  CHECK(web::is_public_asset("/"));
  CHECK(web::is_public_asset("/favicon.svg"));
  // 只做精确查表：不能把任意路径映射到文件系统。
  CHECK(web::find_asset("/../etc/passwd") == nullptr);
  CHECK(web::find_asset("/index.html/../../etc/passwd") == nullptr);
  CHECK(!web::is_public_asset("/v1/chat/completions"));
}

void test_key_ring_round_robin() {
  llm::KeyRing ring({"key-a", "key-b", "key-c"});
  CHECK(ring.size() == 3);
  CHECK(ring.next() == "key-a");
  CHECK(ring.next() == "key-b");
  CHECK(ring.next() == "key-c");
  CHECK(ring.next() == "key-a");  // 回到起点，保证多请求均匀分摊

  llm::KeyRing empty({});
  CHECK(empty.empty());
  CHECK(empty.size() == 0);
  CHECK(empty.next().empty());
}

void test_config_parses_api_keys() {
  setenv("AIP_PROVIDER", "openai", 1);
  setenv("AIP_API_KEY", "single-key", 1);
  setenv("AIP_API_KEYS", " key-a , key-b ,, key-c ", 1);
  setenv("AIP_MAX_RETRIES", "1", 1);
  setenv("AIP_RETRY_BASE_MS", "50", 1);

  const platform::Config config = platform::Config::from_env();
  CHECK(config.api_keys.size() == 3);
  CHECK(config.api_keys[0] == "key-a");
  CHECK(config.api_keys[1] == "key-b");
  CHECK(config.api_keys[2] == "key-c");
  CHECK(config.api_key == "single-key");  // 兼容字段仍然保留
  CHECK(config.max_retries == 1);
  CHECK(config.retry_base_ms == 50);

  unsetenv("AIP_PROVIDER");
  unsetenv("AIP_API_KEY");
  unsetenv("AIP_API_KEYS");
  unsetenv("AIP_MAX_RETRIES");
  unsetenv("AIP_RETRY_BASE_MS");
}

void test_public_config_has_no_secrets() {
  platform::Config config;
  config.api_token = "tok";
  config.api_key = "sk-should-not-leak";
  config.base_url = "https://internal.example/v1";
  config.model = "demo-model";

  const std::string body = http::public_config_json(config, "httplib");
  const auto parsed = nlohmann::json::parse(body);
  CHECK(parsed["auth_required"] == true);
  CHECK(parsed["model"] == "demo-model");
  CHECK(parsed["http"] == "httplib");
  CHECK(parsed["web"] == true);
  // 公开接口绝不能带上密钥、上游地址或 system prompt。
  CHECK(body.find("sk-should-not-leak") == std::string::npos);
  CHECK(body.find("internal.example") == std::string::npos);
  CHECK(body.find("api_key") == std::string::npos);
}

}  // namespace

int main() {
  std::printf("running unit tests...\n");
  test_event_queue();
  test_queue_cancel_drops_pending();
  test_session_store_trim();
  test_session_store_lru_eviction();
  test_mock_provider_streams();
  test_chat_service_records_history();
  test_cancel_is_idempotent();
  test_web_assets();
  test_key_ring_round_robin();
  test_config_parses_api_keys();
  test_public_config_has_no_secrets();

  if (g_failures == 0) {
    std::printf("all tests passed\n");
    return 0;
  }
  std::printf("%d check(s) failed\n", g_failures);
  return 1;
}
