// 极简单元测试：不引入第三方框架，保证 `cmake -DAIPLATFORM_BUILD_TESTS=ON` 即可运行。
// 真实项目建议换 GoogleTest（见 docs/ROADMAP.md 的测试策略）。
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "auth/principal.h"
#include "chat/chat_service.h"
#include "chat/context_builder.h"
#include "chat/session_store.h"
#include "common/metrics.h"
#include "config.h"
#include "http/chat_request.h"
#include "http/public_config.h"
#include "http/request_context.h"
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
  chat::MemorySessionStore store(/*max_messages=*/3, /*max_sessions=*/2);
  const std::string id = store.create(/*user_id=*/0, "测试", "mock");
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
  chat::MemorySessionStore store(10, /*max_sessions=*/2);
  const std::string a = store.create(0, "a", "mock");
  const std::string b = store.create(0, "b", "mock");
  CHECK(store.session_count() == 2);
  (void)a;
  (void)b;
  // 第三个会话会淘汰最旧的
  store.create(0, "c", "mock");
  CHECK(store.session_count() == 2);
}

void test_session_metadata_and_ownership() {
  chat::MemorySessionStore store;
  const std::string a = store.create(/*user_id=*/7, "标题", "model-a");
  CHECK(store.owns(7, false, a));
  CHECK(!store.owns(8, false, a));
  CHECK(store.owns(8, true, a));  // admin 可以访问所有会话

  const auto info = store.get(a);
  CHECK(info.has_value());
  if (info.has_value()) {
    CHECK(info->user_id == 7);
    CHECK(info->title == "标题");
    CHECK(info->model == "model-a");
    CHECK(info->message_count == 0);
  }

  CHECK(store.rename(8, false, a, "越权") == false);
  CHECK(store.rename(7, false, a, "新标题") == true);
  CHECK(store.get(a)->title == "新标题");

  const auto own = store.list(7, false, 10, 0);
  CHECK(own.size() == 1);
  const auto other = store.list(8, false, 10, 0);
  CHECK(other.empty());
  CHECK(store.list(8, true, 10, 0).size() == 1);

  CHECK(store.remove(8, false, a) == false);
  CHECK(store.remove(7, false, a) == true);
  CHECK(!store.exists(a));
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
  chat::MemorySessionStore store;
  chat::ChatOptions options;
  options.system_prompt = "system";
  chat::ChatService service(*provider, store, options);

  auth::Principal principal;
  principal.user_id = 0;
  principal.is_admin = true;
  auto turn = service.start(principal, {}, "第一句话");
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
  auto first = ring.acquire();
  auto second = ring.acquire();
  auto third = ring.acquire();
  auto fourth = ring.acquire();
  CHECK(first.has_value() && first->key == "key-a");
  CHECK(second.has_value() && second->key == "key-b");
  CHECK(third.has_value() && third->key == "key-c");
  CHECK(fourth.has_value() && fourth->key == "key-a");  // 回到起点，保证多请求均匀分摊
  CHECK(ring.available() == 3);

  llm::KeyRing empty({});
  CHECK(empty.empty());
  CHECK(empty.size() == 0);
  CHECK(empty.next().empty());
  CHECK(!empty.acquire().has_value());

  llm::KeyRingConfig limiter;
  limiter.rpm_limit = 2;
  limiter.failure_threshold = 100;
  llm::KeyRing rpm({"only"}, limiter);
  CHECK(rpm.acquire().has_value());
  CHECK(rpm.acquire().has_value());
  CHECK(!rpm.acquire().has_value());  // 第 3 次被每 Key RPM 拦住

  llm::KeyRingConfig breaker;
  breaker.failure_threshold = 2;
  breaker.cooldown_s = 60;
  breaker.cooldown_max_s = 60;
  llm::KeyRing cooled({"a", "b"}, breaker);
  auto a = cooled.acquire();
  CHECK(a.has_value() && a->index == 0);
  CHECK(!cooled.record_failure(a->index, 500, /*retryable=*/true));  // 1 次，未到阈值
  auto b = cooled.acquire();
  CHECK(b.has_value() && b->index == 1);
  CHECK(!cooled.record_failure(b->index, 500, /*retryable=*/true));  // 每个 Key 独立计数：1 次
  CHECK(cooled.record_failure(b->index, 500, /*retryable=*/true));  // 第 2 次 -> 冷却
  CHECK(cooled.available() == 1);
  CHECK(cooled.cooldown_remaining_ms(b->index) > 0);
  auto next = cooled.acquire();
  CHECK(next.has_value() && next->index == 0);  // 跳过冷却中的 b

  llm::KeyRingConfig rate;
  rate.failure_threshold = 1;
  rate.cooldown_s = 1;
  rate.cooldown_max_s = 1;
  llm::KeyRing ratelimited({"x"}, rate);
  auto x = ratelimited.acquire();
  CHECK(x.has_value());
  CHECK(ratelimited.record_failure(x->index, 429, /*retryable=*/true));  // 阈值 1 -> 冷却
  CHECK(!ratelimited.acquire().has_value());

  llm::KeyRingConfig multi_rate;
  multi_rate.failure_threshold = 3;
  multi_rate.cooldown_s = 1;
  multi_rate.cooldown_max_s = 1;
  llm::KeyRing multi({"x", "y"}, multi_rate);
  auto mx = multi.acquire();
  CHECK(mx.has_value());
  CHECK(multi.record_failure(mx->index, 429, /*retryable=*/true));  // 多 Key 429 立即冷却
  CHECK(multi.acquire().has_value());  // 另一个 Key 仍可用
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
  CHECK(parsed["storage"] == "memory");
  CHECK(parsed["web"] == true);
  // memory 模式即使带静态 token，也没有用户账号体系；页面应回退到 Token 输入。
  CHECK(parsed["auth"]["user_accounts"] == false);
  CHECK(parsed["auth"]["static_token"] == true);
  // 公开接口绝不能带上密钥、上游地址或 system prompt。
  CHECK(body.find("sk-should-not-leak") == std::string::npos);
  CHECK(body.find("internal.example") == std::string::npos);
  CHECK(body.find("api_key") == std::string::npos);
}

void test_public_config_advertises_user_accounts() {
#if defined(AIP_WITH_AUTH) && defined(AIP_WITH_POSTGRES)
  platform::Config config;
  config.storage = "postgres";
  config.db_dsn = "postgresql://unused";
  config.jwt_secret = "0123456789abcdef0123456789abcdef";
  config.auth_required = true;

  const auto parsed = nlohmann::json::parse(http::public_config_json(config, "drogon"));
  CHECK(parsed["auth"]["user_accounts"] == true);
  CHECK(parsed["auth"]["password_login"] == true);
  CHECK(parsed["auth"]["registration"] == true);
  CHECK(parsed["auth"]["static_token"] == false);
  CHECK(parsed["auth"]["access_ttl_s"] == 900);
  CHECK(parsed["auth"]["refresh_ttl_s"] == 7 * 24 * 3600);
#else
  std::printf("  skip: user auth not compiled in\n");
#endif
}


void test_metrics_and_request_id() {
  platform::metrics::reset_for_tests();
  platform::metrics::inc_http_request("GET", "/healthz", 200);
  platform::metrics::inc_http_request("GET", "/healthz", 200);
  platform::metrics::observe_ttft("demo", 0.2);
  platform::metrics::set_active_streams(3);

  const std::string text = platform::metrics::render_prometheus();
  CHECK(text.find(
            "aip_http_requests_total{method=\"GET\",route=\"/healthz\",status=\"200\"} 2") !=
        std::string::npos);
  CHECK(text.find("aip_chat_ttft_seconds_bucket{model=\"demo\",le=\"+Inf\"} 1") !=
        std::string::npos);
  CHECK(text.find("aip_active_streams 3") != std::string::npos);
  CHECK(text.find("aip_http_request_duration_seconds_count{method=\"GET\",route=\"/healthz\"} 0") ==
        std::string::npos);

  CHECK(http::valid_request_id("abc-123_X.Y"));
  CHECK(!http::valid_request_id("bad id"));
  CHECK(!http::valid_request_id(""));
  CHECK(http::ensure_request_id("client-id") == "client-id");
  CHECK(http::ensure_request_id("bad id").rfind("req_", 0) == 0);
  CHECK(http::route_label("/v1/sessions/s_abc/messages") == "/v1/sessions/{id}/messages");
  CHECK(http::route_label("/v1/sessions/s_abc") == "/v1/sessions/{id}");
  CHECK(http::route_label("/v1/auth/api-keys/12") == "/v1/auth/api-keys/{id}");
  CHECK(http::route_label("/assets/app.js") == "/assets/*");
  platform::metrics::reset_for_tests();
}


void test_chat_request_parser() {
  platform::Config config;
  config.model = "demo-model";
  config.models = {"demo-model"};
  config.max_context_tokens = 8192;

  nlohmann::json body{
      {"model", "demo-model"},
      {"temperature", 0.3},
      {"max_tokens", 128},
      {"top_p", 0.9},
      {"stop", "STOP"},
      {"stream_options", {{"include_usage", false}}},
      {"messages",
       nlohmann::json::array({{{"role", "system"}, {"content", "sys"}},
                              {{"role", "user"}, {"content", "hi"}},
                              {{"role", "assistant"}, {"content", "hello"}},
                              {{"role", "user"}, {"content", "again"}}})}};

  http::ChatParseError error;
  auto parsed = http::parse_chat_request(body, config, error);
  CHECK(parsed.has_value());
  if (parsed.has_value()) {
    CHECK(parsed->messages.size() == 4);
    CHECK(parsed->user_text == "again");
    CHECK(parsed->temperature == 0.3);
    CHECK(parsed->max_tokens == 128);
    CHECK(parsed->top_p == 0.9);
    CHECK(parsed->stop.size() == 1);
    CHECK(!parsed->include_usage);
  }

  chat::ContextBudget budget;
  budget.max_context_tokens = 2048;
  budget.reserve_output_tokens = 128;
  const auto context = chat::build_context_from_messages("default", parsed->messages, budget);
  CHECK(context.size() == 4);
  CHECK(context.front().role == "system");
  CHECK(context.front().content == "sys");
  CHECK(context.back().role == "user");
  CHECK(context.back().content == "again");

  nlohmann::json bad_model{{"message", "hi"}, {"model", "other-model"}};
  CHECK(!http::parse_chat_request(bad_model, config, error).has_value());
  CHECK(error.type == "model_not_found");

  nlohmann::json bad_last{{"messages",
                           nlohmann::json::array({{{"role", "user"}, {"content", "hi"}},
                                                  {{"role", "assistant"}, {"content", "yo"}}})}};
  CHECK(!http::parse_chat_request(bad_last, config, error).has_value());
  CHECK(error.type == "invalid_request");

  nlohmann::json image{{"messages",
                        nlohmann::json::array(
                            {{{"role", "user"},
                              {"content",
                               nlohmann::json::array({{{"type", "image_url"},
                                                       {"image_url", {{"url", "http://x"}}}}})}}})}};
  CHECK(!http::parse_chat_request(image, config, error).has_value());
  CHECK(error.type == "unsupported_content");

  nlohmann::json too_many_tokens{{"message", "hi"}, {"max_tokens", 99999}};
  CHECK(!http::parse_chat_request(too_many_tokens, config, error).has_value());
  CHECK(error.type == "invalid_request");
}

class CapturingProvider final : public llm::Provider {
 public:
  llm::ChatRequest captured;
  std::string name() const override { return "capturing"; }
  std::unique_ptr<llm::Stream> chat_stream(const llm::ChatRequest& request) override {
    captured = request;
    class Stream final : public llm::Stream {
     public:
      llm::Pull next(llm::Event& out, std::chrono::milliseconds) override {
        if (done_) return llm::Pull::End;
        done_ = true;
        out.kind = llm::EventKind::Delta;
        out.text = "ok";
        return llm::Pull::Event;
      }
      void cancel() override {}
     private:
      bool done_ = false;
    };
    return std::make_unique<Stream>();
  }
};

void test_chat_service_full_messages() {
  CapturingProvider provider;
  chat::MemorySessionStore store(200, 100);
  chat::ChatOptions options;
  options.default_model = "mock-model";
  options.system_prompt = "default system";
  options.max_tokens = 128;
  options.max_context_tokens = 2048;
  chat::ChatService service(provider, store, options);

  auth::Principal principal;
  principal.is_admin = true;
  principal.auth_type = "static";

  chat::ChatService::StartOptions start;
  start.user_text = "second";
  start.messages = {{"system", "client system"},
                    {"user", "first"},
                    {"assistant", "answer"},
                    {"user", "second"}};
  start.temperature = 0.2;
  start.max_tokens = 64;
  start.top_p = 0.9;
  start.stop = {"END"};
  start.include_usage = false;
  auto turn = service.start(principal, start);
  CHECK(!turn.session_id.empty());
  CHECK(turn.prompt_tokens_estimate > 0);

  while (true) {
    llm::Event event;
    const llm::Pull pull = turn.stream->next(event, 5s);
    if (pull == llm::Pull::End || pull == llm::Pull::Cancelled) break;
    if (pull == llm::Pull::Timeout) continue;
    if (event.kind == llm::EventKind::Done) break;
    if (event.kind == llm::EventKind::Error) break;
  }

  CHECK(provider.captured.messages.size() == 4);
  CHECK(provider.captured.messages.front().role == "system");
  CHECK(provider.captured.messages.front().content == "client system");
  CHECK(provider.captured.messages.back().content == "second");
  CHECK(provider.captured.temperature == 0.2);
  CHECK(provider.captured.max_tokens == 64);
  CHECK(provider.captured.top_p == 0.9);
  CHECK(provider.captured.stop.size() == 1);
  CHECK(!provider.captured.include_usage);

  const auto history = store.history(turn.session_id, 10);
  // 客户端已带完整历史，服务端只落本轮 user + assistant，避免重复。
  CHECK(history.size() == 2);
  CHECK(history[0].role == "user");
  CHECK(history[0].content == "second");
  CHECK(history[1].role == "assistant");
}

}  // namespace

int main() {
  std::printf("running unit tests...\n");
  test_event_queue();
  test_queue_cancel_drops_pending();
  test_session_store_trim();
  test_session_store_lru_eviction();
  test_session_metadata_and_ownership();
  test_mock_provider_streams();
  test_chat_service_records_history();
  test_cancel_is_idempotent();
  test_web_assets();
  test_key_ring_round_robin();
  test_config_parses_api_keys();
  test_public_config_has_no_secrets();
  test_public_config_advertises_user_accounts();
  test_metrics_and_request_id();
  test_chat_request_parser();
  test_chat_service_full_messages();

  if (g_failures == 0) {
    std::printf("all tests passed\n");
    return 0;
  }
  std::printf("%d check(s) failed\n", g_failures);
  return 1;
}
