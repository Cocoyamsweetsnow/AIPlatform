// 会话存储的「接口 + 内存实现」。
// 骨架阶段放内存里跑通全链路；M3 时替换成 PostgreSQL 实现（同一接口），
// HTTP 层与业务层完全不用改。
#pragma once

#include <cstddef>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "llm/types.h"

namespace chat {

class SessionStore {
 public:
  explicit SessionStore(std::size_t max_messages_per_session = 200,
                        std::size_t max_sessions = 10000);

  /// 新建会话并返回 ID。
  std::string create();

  bool exists(const std::string& session_id) const;

  /// 追加一条消息（超过单会话上限时丢弃最旧的）。
  void append(const std::string& session_id, const llm::Message& message);

  /// 取最近 limit 条消息（按时间正序）。
  std::vector<llm::Message> history(const std::string& session_id, std::size_t limit) const;

  std::size_t message_count(const std::string& session_id) const;
  std::size_t session_count() const;

 private:
  void evict_if_needed();

  struct Session {
    std::vector<llm::Message> messages;
    long long updated_at = 0;
    std::list<std::string>::iterator lru_it;
  };

  mutable std::mutex mutex_;
  std::unordered_map<std::string, Session> sessions_;
  std::list<std::string> lru_;  // 头部 = 最近使用
  std::size_t max_messages_;
  std::size_t max_sessions_;
};

}  // namespace chat
