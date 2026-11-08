#include "chat/token_estimate.h"

#include <cstddef>

namespace chat {

int estimate_tokens(const std::string& text) {
  if (text.empty()) return 0;

  std::size_t ascii = 0;
  std::size_t non_ascii = 0;
  for (unsigned char c : text) {
    if (c < 0x80) {
      ++ascii;
    } else if ((c & 0xC0) != 0x80) {
      ++non_ascii;  // UTF-8 前导字节 = 一个码点
    }
  }

  const int ascii_tokens = static_cast<int>((ascii + 3) / 4);
  const int total = ascii_tokens + static_cast<int>(non_ascii);
  return total == 0 ? 1 : total;
}

}  // namespace chat
