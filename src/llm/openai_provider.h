// OpenAI 兼容协议的 Provider（DeepSeek / Moonshot / Qwen / vLLM / Ollama 都能直接用）。
#pragma once

#include <memory>
#include <string>

#include "llm/types.h"

namespace llm {

struct OpenAiOptions {
  std::string base_url;        // 例如 https://api.deepseek.com/v1
  std::string api_key;         // Bearer token
  std::string default_model;   // 请求未指定 model 时使用
  int timeout_s = 120;         // 整个请求的总超时
  bool include_usage = true;   // stream_options.include_usage
};

std::unique_ptr<Provider> make_openai_provider(OpenAiOptions options);

}  // namespace llm
