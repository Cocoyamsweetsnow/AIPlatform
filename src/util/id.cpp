#include "util/id.h"

#include <chrono>
#include <random>

namespace util {
namespace {

std::mt19937_64& rng() {
  thread_local std::mt19937_64 gen{std::random_device{}()};
  return gen;
}

}  // namespace

std::string random_id(const std::string& prefix) {
  static const char* kHex = "0123456789abcdef";
  std::uniform_int_distribution<int> dist(0, 15);
  std::string out = prefix;
  out.reserve(prefix.size() + 24);
  for (int i = 0; i < 24; ++i) out.push_back(kHex[dist(rng())]);
  return out;
}

long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace util
