// 轻量 token 估算：不引入 tokenizer，按 ASCII/非 ASCII 码点做保守估算。
//
// 用途：
//   - 上游没返回 usage 时给 usage_records 一个近似值；
//   - M3 配额预检查；M5 会接入真正的上下文预算裁剪。
#pragma once

#include <string>

namespace chat {

/// 空串返回 0；ASCII 约 4 字节/token，非 ASCII 码点按 1 token/字估算。
int estimate_tokens(const std::string& text);

}  // namespace chat
