// 对话请求解析（Drogon / httplib 共用）。
//
// 目标：把 OpenAI 兼容的字段校验集中在一处，避免两个后端各写一份、慢慢漂移。
// 只做协议层校验，不做鉴权/配额/上下文裁剪。
#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "chat/token_estimate.h"
#include "config.h"
#include "llm/types.h"

namespace http {

struct ChatParseError {
  int status = 400;
  std::string type = "invalid_request";
  std::string message;
};

struct ChatRequestInput {
  std::string user_text;
  std::vector<llm::Message> messages;  // 非空 = OpenAI messages 权威上下文
  std::string session_id;
  std::string model;
  bool stream = true;
  std::string request_id;

  double temperature = -1.0;
  int max_tokens = -1;
  double top_p = -1.0;
  std::vector<std::string> stop;
  bool has_presence_penalty = false;
  double presence_penalty = 0.0;
  bool has_frequency_penalty = false;
  double frequency_penalty = 0.0;
  bool has_seed = false;
  long long seed = 0;
  std::string response_format;
  std::string user;
  bool include_usage = true;
};

namespace detail {

inline bool blank(const std::string& value) {
  return value.find_first_not_of(" \t\r\n") == std::string::npos;
}

inline bool fail(ChatParseError& error, int status, std::string type,
                 std::string message) {
  error.status = status;
  error.type = std::move(type);
  error.message = std::move(message);
  return false;
}

inline bool parse_content(const nlohmann::json& content, std::string& out,
                          ChatParseError& error) {
  if (content.is_string()) {
    out = content.get<std::string>();
    return true;
  }
  if (!content.is_array()) {
    return fail(error, 400, "invalid_request",
                "message.content 必须是字符串或文本 part 数组");
  }
  out.clear();
  for (const auto& part : content) {
    if (!part.is_object()) {
      return fail(error, 400, "invalid_request", "content part 必须是对象");
    }
    const std::string type = part.value("type", "");
    if (type == "text" && part.contains("text") && part["text"].is_string()) {
      out += part["text"].get<std::string>();
    } else if (type == "image_url") {
      return fail(error, 400, "unsupported_content",
                  "当前版本只支持文本输入，图片/多模态请等待后续版本");
    } else {
      return fail(error, 400, "invalid_request",
                  "不支持的 content part 类型: " + type);
    }
  }
  return true;
}

inline bool parse_messages(const nlohmann::json& array, ChatRequestInput& input,
                           ChatParseError& error) {
  if (array.empty() || array.size() > 200) {
    return fail(error, 400, "invalid_request", "messages 长度应在 1~200");
  }
  bool saw_user = false;
  std::size_t total_bytes = 0;
  input.messages.clear();
  input.messages.reserve(array.size());

  for (const auto& item : array) {
    if (!item.is_object()) {
      return fail(error, 400, "invalid_request", "messages 每一项必须是对象");
    }
    const std::string role = item.value("role", "");
    if (role != "system" && role != "user" && role != "assistant" && role != "tool") {
      return fail(error, 400, "invalid_request",
                  "不支持的 role: " + role + "（只支持 system/user/assistant/tool）");
    }
    if (role == "system" && saw_user) {
      return fail(error, 400, "invalid_request", "system 消息只能出现在 messages 开头");
    }
    if (item.contains("tool_calls") && !item["tool_calls"].is_null()) {
      return fail(error, 400, "unsupported_tool",
                  "当前版本不支持 tool_calls，请等待工具调用功能上线");
    }

    std::string content;
    if (item.contains("content") && !item["content"].is_null()) {
      if (!parse_content(item["content"], content, error)) return false;
    } else if (role != "assistant") {
      return fail(error, 400, "invalid_request", "message.content 不能为空");
    }

    if (content.size() > 32 * 1024) {
      return fail(error, 413, "payload_too_large", "单条消息超过 32KB 限制");
    }
    total_bytes += content.size();
    if (total_bytes > 1024 * 1024) {
      return fail(error, 413, "payload_too_large", "messages 总大小超过 1MB 限制");
    }

    if (role == "user") {
      saw_user = true;
      input.user_text = content;
    }
    input.messages.push_back(llm::Message{role, std::move(content)});
  }

  if (input.messages.empty() || input.messages.back().role != "user") {
    return fail(error, 400, "invalid_request", "messages 最后一条必须是 user");
  }
  return true;
}

inline bool parse_number_range(const nlohmann::json& body, const char* key, double min,
                               double max, double& out, ChatParseError& error) {
  if (!body.contains(key)) return true;
  if (!body[key].is_number()) {
    return fail(error, 400, "invalid_request", std::string(key) + " 必须是数字");
  }
  out = body[key].get<double>();
  if (!std::isfinite(out) || out < min || out > max) {
    return fail(error, 400, "invalid_request",
                std::string(key) + " 应在 [" + std::to_string(min) + ", " +
                    std::to_string(max) + "] 区间");
  }
  return true;
}

}  // namespace detail

/// 解析并校验 /v1/chat/completions 请求体；失败时填 error 并返回 nullopt。
inline std::optional<ChatRequestInput> parse_chat_request(
    const nlohmann::json& body, const platform::Config& config, ChatParseError& error) {
  if (!body.is_object()) {
    detail::fail(error, 400, "invalid_json", "请求体必须是 JSON 对象");
    return std::nullopt;
  }

  ChatRequestInput input;
  if (body.contains("message") && !body["message"].is_null()) {
    if (!body["message"].is_string()) {
      detail::fail(error, 400, "invalid_request", "message 必须是字符串");
      return std::nullopt;
    }
    input.user_text = body["message"].get<std::string>();
  } else if (body.contains("messages") && body["messages"].is_array()) {
    if (!detail::parse_messages(body["messages"], input, error)) return std::nullopt;
  } else {
    detail::fail(error, 400, "invalid_request",
                 "缺少 message 字段（或 OpenAI messages 数组）");
    return std::nullopt;
  }

  if (detail::blank(input.user_text)) {
    detail::fail(error, 400, "invalid_request", "message 不能为空");
    return std::nullopt;
  }
  if (input.user_text.size() > 32 * 1024) {
    detail::fail(error, 413, "payload_too_large", "单条消息超过 32KB 限制");
    return std::nullopt;
  }

  if (body.contains("session_id")) {
    if (!body["session_id"].is_string()) {
      detail::fail(error, 400, "invalid_request", "session_id 必须是字符串");
      return std::nullopt;
    }
    input.session_id = body["session_id"].get<std::string>();
  }

  if (body.contains("model")) {
    if (!body["model"].is_string()) {
      detail::fail(error, 400, "invalid_request", "model 必须是字符串");
      return std::nullopt;
    }
    input.model = body["model"].get<std::string>();
    if (input.model.size() > 128) {
      detail::fail(error, 400, "invalid_request", "model 长度不能超过 128");
      return std::nullopt;
    }
    if (std::find(config.models.begin(), config.models.end(), input.model) ==
        config.models.end()) {
      detail::fail(error, 400, "model_not_found",
                   "模型不在允许列表中: " + input.model);
      return std::nullopt;
    }
  }

  if (body.contains("stream")) {
    if (!body["stream"].is_boolean()) {
      detail::fail(error, 400, "invalid_request", "stream 必须是布尔值");
      return std::nullopt;
    }
    input.stream = body["stream"].get<bool>();
  }

  if (body.contains("request_id")) {
    if (!body["request_id"].is_string()) {
      detail::fail(error, 400, "invalid_request", "request_id 必须是字符串");
      return std::nullopt;
    }
    input.request_id = body["request_id"].get<std::string>();
    if (input.request_id.empty() || input.request_id.size() > 128) {
      detail::fail(error, 400, "invalid_request", "request_id 长度应在 1~128");
      return std::nullopt;
    }
  }

  if (!detail::parse_number_range(body, "temperature", 0.0, 2.0, input.temperature,
                                  error)) {
    return std::nullopt;
  }
  if (!detail::parse_number_range(body, "top_p", 0.0, 1.0, input.top_p, error)) {
    return std::nullopt;
  }
  if (!detail::parse_number_range(body, "presence_penalty", -2.0, 2.0,
                                  input.presence_penalty, error)) {
    return std::nullopt;
  }
  if (body.contains("presence_penalty")) input.has_presence_penalty = true;
  if (!detail::parse_number_range(body, "frequency_penalty", -2.0, 2.0,
                                  input.frequency_penalty, error)) {
    return std::nullopt;
  }
  if (body.contains("frequency_penalty")) input.has_frequency_penalty = true;

  if (body.contains("max_tokens")) {
    if (!body["max_tokens"].is_number_integer()) {
      detail::fail(error, 400, "invalid_request", "max_tokens 必须是整数");
      return std::nullopt;
    }
    const long long requested = body["max_tokens"].get<long long>();
    const long long cap = std::max<long long>(1, config.max_context_tokens - 64);
    if (requested <= 0 || requested > cap) {
      detail::fail(error, 400, "invalid_request",
                   "max_tokens 应在 [1, " + std::to_string(cap) + "]");
      return std::nullopt;
    }
    input.max_tokens = static_cast<int>(requested);
  }

  if (body.contains("seed")) {
    if (!body["seed"].is_number_integer()) {
      detail::fail(error, 400, "invalid_request", "seed 必须是整数");
      return std::nullopt;
    }
    const long long seed = body["seed"].get<long long>();
    if (seed < 0 || seed > 2147483647LL) {
      detail::fail(error, 400, "invalid_request", "seed 应在 [0, 2147483647]");
      return std::nullopt;
    }
    input.has_seed = true;
    input.seed = seed;
  }

  if (body.contains("stop")) {
    const auto& stop = body["stop"];
    if (stop.is_string()) {
      const std::string value = stop.get<std::string>();
      if (value.empty() || value.size() > 128) {
        detail::fail(error, 400, "invalid_request", "stop 字符串长度应在 1~128");
        return std::nullopt;
      }
      input.stop.push_back(value);
    } else if (stop.is_array()) {
      if (stop.size() > 4) {
        detail::fail(error, 400, "invalid_request", "stop 数组最多 4 项");
        return std::nullopt;
      }
      for (const auto& item : stop) {
        if (!item.is_string()) {
          detail::fail(error, 400, "invalid_request", "stop 数组每一项必须是字符串");
          return std::nullopt;
        }
        const std::string value = item.get<std::string>();
        if (value.empty() || value.size() > 128) {
          detail::fail(error, 400, "invalid_request", "stop 字符串长度应在 1~128");
          return std::nullopt;
        }
        input.stop.push_back(value);
      }
    } else {
      detail::fail(error, 400, "invalid_request", "stop 必须是字符串或字符串数组");
      return std::nullopt;
    }
  }

  if (body.contains("stream_options")) {
    if (!body["stream_options"].is_object()) {
      detail::fail(error, 400, "invalid_request", "stream_options 必须是对象");
      return std::nullopt;
    }
    if (body["stream_options"].contains("include_usage")) {
      if (!body["stream_options"]["include_usage"].is_boolean()) {
        detail::fail(error, 400, "invalid_request",
                     "stream_options.include_usage 必须是布尔值");
        return std::nullopt;
      }
      input.include_usage = body["stream_options"]["include_usage"].get<bool>();
    }
  }

  if (body.contains("response_format")) {
    if (!body["response_format"].is_object() ||
        !body["response_format"].contains("type") ||
        !body["response_format"]["type"].is_string()) {
      detail::fail(error, 400, "invalid_request",
                   "response_format 必须形如 {\"type\":\"text\"}");
      return std::nullopt;
    }
    const std::string type = body["response_format"]["type"].get<std::string>();
    if (type != "text" && type != "json_object") {
      detail::fail(error, 400, "invalid_request",
                   "response_format.type 只支持 text 或 json_object");
      return std::nullopt;
    }
    input.response_format = type;
  }

  if (body.contains("user")) {
    if (!body["user"].is_string()) {
      detail::fail(error, 400, "invalid_request", "user 必须是字符串");
      return std::nullopt;
    }
    input.user = body["user"].get<std::string>();
    if (input.user.size() > 256) {
      detail::fail(error, 400, "invalid_request", "user 长度不能超过 256");
      return std::nullopt;
    }
  }

  return input;
}

}  // namespace http
