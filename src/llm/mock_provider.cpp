#include "llm/mock_provider.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "llm/event_queue.h"
#include "llm/threaded_stream.h"

namespace llm {
namespace {

// 按 UTF-8 码点切分，保证前端拿到的每个分片都是合法字符。
std::vector<std::string> split_utf8(const std::string& text) {
  std::vector<std::string> pieces;
  for (std::size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    std::size_t len = 1;
    if ((c & 0xE0) == 0xC0) {
      len = 2;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
    }
    if (i + len > text.size()) len = 1;
    pieces.push_back(text.substr(i, len));
    i += len;
  }
  return pieces;
}

class MockStream final : public ThreadedStream {
 public:
  MockStream(std::shared_ptr<EventQueue> queue, std::string prefix, ChatRequest req)
      : ThreadedStream(std::move(queue)), prefix_(std::move(prefix)), req_(std::move(req)) {}

  ~MockStream() override { shutdown(); }

  void start() { start_worker(); }

  std::string describe() const override { return "mock/" + req_.model; }

 private:
  void run() override {
    std::string last_user;
    for (const auto& m : req_.messages) {
      if (m.role == "user") last_user = m.content;
    }

    const std::string reply = prefix_ + "这是 mock 模型的流式回复。你说的是：" + last_user;
    const auto pieces = split_utf8(reply);

    // 每 3 个码点推一个 delta，并加一点延迟，模拟真实的首字与逐字输出。
    std::string buffer;
    int count = 0;
    for (const auto& piece : pieces) {
      if (queue_->cancelled()) return;
      buffer += piece;
      if (++count % 3 == 0) {
        push_delta(buffer);
        buffer.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
      }
    }
    if (!buffer.empty()) push_delta(buffer);

    if (!queue_->cancelled()) {
      Event done;
      done.kind = EventKind::Done;
      done.finish_reason = "stop";
      done.prompt_tokens = static_cast<int>(last_user.size() / 2);
      done.completion_tokens = static_cast<int>(pieces.size() / 2);
      queue_->push(done);
    }
    queue_->close();
  }

  void push_delta(const std::string& text) {
    Event e;
    e.kind = EventKind::Delta;
    e.text = text;
    queue_->push(e);
  }

  std::string prefix_;
  ChatRequest req_;
};

class MockProvider final : public Provider {
 public:
  explicit MockProvider(std::string prefix) : prefix_(std::move(prefix)) {}

  std::string name() const override { return "mock"; }

  std::unique_ptr<Stream> chat_stream(const ChatRequest& req) override {
    auto queue = std::make_shared<EventQueue>();
    auto stream = std::make_unique<MockStream>(queue, prefix_, req);
    stream->start();
    return stream;
  }

 private:
  std::string prefix_;
};

}  // namespace

std::unique_ptr<Provider> make_mock_provider(std::string canned_prefix) {
  return std::make_unique<MockProvider>(std::move(canned_prefix));
}

}  // namespace llm
