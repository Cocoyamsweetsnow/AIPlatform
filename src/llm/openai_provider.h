// OpenAI 兼容协议的 Provider（DeepSeek / Moonshot / Qwen / vLLM / Ollama 都能直接用）。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "llm/types.h"

namespace llm {

struct OpenAiOptions {
  std::string base_url;        // 例如 https://api.deepseek.com/v1
  std::string api_key;         // 单 key 兼容字段；多 key 请用 api_keys
  std::vector<std::string> api_keys;  // 非空时优先于 api_key，按请求轮转
  std::string default_model;   // 请求未指定 model 时使用
  int timeout_s = 120;         // 单次上游请求的总超时
  bool include_usage = true;   // stream_options.include_usage

  // 只对「还没吐出第一个 token」的失败重试，避免前端收到重复内容。
  // 0 = 关闭重试；退避为 base * 2^attempt，单次最多 5 秒。
  int max_retries = 2;
  int retry_base_ms = 300;

  // 每 Key 的冷却/熔断与 RPM/TPM（0 = 不限）。
  int key_rpm_limit = 0;
  long long key_tpm_limit = 0;
  int key_failure_threshold = 3;
  int key_cooldown_s = 15;
  int key_cooldown_max_s = 300;
};

std::unique_ptr<Provider> make_openai_provider(OpenAiOptions options);

}  // namespace llm
