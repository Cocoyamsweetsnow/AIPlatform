// 运行期配置：全部来自环境变量，便于容器化部署（12-Factor）。
// 后续可平滑替换为 YAML/TOML + 热加载，接口不变。
#pragma once

#include <string>

namespace platform {

struct Config {
  // 服务
  std::string host{"0.0.0.0"};
  int port{8080};
  std::string api_token;  // 为空表示不鉴权（仅本地开发）

  // 上游模型
  std::string provider{"mock"};  // mock | openai
  std::string base_url{"https://api.deepseek.com/v1"};
  std::string api_key;
  std::string model{"deepseek-chat"};
  int request_timeout_s{120};
  int max_tokens{1024};
  double temperature{0.7};
  bool include_usage{true};

  // 对话
  std::string system_prompt{"你是一个乐于助人的 AI 助手，回答简洁准确。"};
  int max_history_messages{20};
  int max_sessions{10000};

  static Config from_env();

  bool auth_enabled() const { return !api_token.empty(); }
};

}  // namespace platform
