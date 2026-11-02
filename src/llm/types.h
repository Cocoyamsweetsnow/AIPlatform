// LLM 网关的「领域接口」：Provider 抽象 + 流式事件。
//
// 设计要点：
//   1. 上层（HTTP 层）只依赖 llm::Stream / llm::Provider，不关心上游协议；
//   2. 流是「阻塞拉取」语义（next + 超时），换成本地推理（llama.cpp）或
//      异步协程实现时，只要满足接口即可；
//   3. cancel() 必须能让正在阻塞的上游调用尽快返回（HTTP 客户端断开时用）。
#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace llm {

struct Message {
  std::string role;  // system | user | assistant
  std::string content;
};

struct ChatRequest {
  std::string model;
  std::vector<Message> messages;
  double temperature = 0.7;
  int max_tokens = 0;      // 0 表示交给上游默认值
  bool include_usage = true;
};

enum class EventKind {
  Delta,  // 增量文本
  Done,   // 正常结束（带 finish_reason / usage）
  Error,  // 失败（上游错误、超时、解析失败）
};

struct Event {
  EventKind kind = EventKind::Delta;
  std::string text;           // Delta: 增量文本；Error: 错误描述
  std::string finish_reason;  // Done: stop | length | tool_calls ...
  int prompt_tokens = -1;
  int completion_tokens = -1;
};

enum class Pull {
  Event,      // 取到事件
  Timeout,    // 超时，暂无事件（可发心跳）
  End,        // 流已结束
  Cancelled,  // 已被取消
};

class Stream {
 public:
  virtual ~Stream() = default;

  /// 阻塞等待下一个事件，最多等 timeout。
  /// 返回 End 表示不会再有事件；调用方应停止读取。
  virtual Pull next(Event& out, std::chrono::milliseconds timeout) = 0;

  /// 请求取消。必须线程安全且可重复调用。
  virtual void cancel() = 0;

  /// 便于日志与调试的标识（如 "openai/deepseek-chat"）。
  virtual std::string describe() const { return {}; }
};

class Provider {
 public:
  virtual ~Provider() = default;
  virtual std::string name() const = 0;
  virtual std::unique_ptr<Stream> chat_stream(const ChatRequest& req) = 0;
};

}  // namespace llm
