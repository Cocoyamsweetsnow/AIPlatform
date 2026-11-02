#pragma once

#include <string>

namespace util {

/// 生成带前缀的随机 ID，例如 "s_9f2c1a..."。
std::string random_id(const std::string& prefix);

/// 当前毫秒级 Unix 时间戳。
long long now_ms();

}  // namespace util
