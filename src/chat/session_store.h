// 会话存储的领域接口。
//
// 两个实现：
//   - MemorySessionStore：进程内 LRU，默认模式，适合演示/测试；
//   - PgSessionStore：PostgreSQL，生产模式（见 chat/pg_session_store.h）。
//
// HTTP 层和 ChatService 只依赖这个接口；换存储不影响上层。
#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "llm/types.h"

namespace chat {

struct SessionInfo {
  std::string id;
  std::int64_t user_id = 0;  // 0 = 无归属（兼容旧的静态 Token / 演示会话）
  std::string title;
  std::string model;
  long long created_at = 0;  // Unix 秒
  long long updated_at = 0;
  std::size_t message_count = 0;
};

/// 裁掉上下文开头的 assistant/tool：部分上游要求第一条是 user 或 system。
void trim_context(std::vector<llm::Message>& messages);

class SessionStore {
 public:
  virtual ~SessionStore() = default;

  /// 新建会话；title/model 为空时由实现给默认值。
  virtual std::string create(std::int64_t user_id, const std::string& title,
                             const std::string& model) = 0;

  /// 确保指定 ID 的会话存在（兼容旧客户端自带 session_id 的行为）。
  /// 返回 true 表示本次新建；已存在返回 false。
  virtual bool ensure(const std::string& session_id, std::int64_t user_id,
                      const std::string& title, const std::string& model) = 0;

  virtual bool exists(const std::string& session_id) const = 0;

  /// 归属校验：admin 绕过；普通用户只能访问 user_id 匹配的会话。
  virtual bool owns(std::int64_t user_id, bool is_admin,
                    const std::string& session_id) const = 0;

  /// 追加一条消息，返回消息 ID。
  virtual std::int64_t append(const std::string& session_id, const llm::Message& message) = 0;

  /// 最近 limit 条消息（时间正序，原样返回；上下文裁剪由 ChatService 负责）。
  virtual std::vector<llm::Message> history(const std::string& session_id,
                                            std::size_t limit) const = 0;

  virtual std::size_t message_count(const std::string& session_id) const = 0;
  virtual std::size_t session_count() const = 0;

  /// 按消息 ID 取内容（幂等重放用）。
  virtual std::optional<llm::Message> message(std::int64_t message_id) const = 0;

  virtual std::optional<SessionInfo> get(const std::string& session_id) const = 0;

  /// 按 updated_at 倒序分页；admin 看全部，普通用户只看自己的。
  virtual std::vector<SessionInfo> list(std::int64_t user_id, bool is_admin,
                                        std::size_t limit, std::size_t offset) const = 0;

  virtual bool rename(std::int64_t user_id, bool is_admin, const std::string& session_id,
                      const std::string& title) = 0;

  /// 软删除；返回是否命中。
  virtual bool remove(std::int64_t user_id, bool is_admin,
                      const std::string& session_id) = 0;
};

class MemorySessionStore final : public SessionStore {
 public:
  explicit MemorySessionStore(std::size_t max_messages_per_session = 200,
                              std::size_t max_sessions = 10000);

  std::string create(std::int64_t user_id, const std::string& title,
                     const std::string& model) override;
  bool ensure(const std::string& session_id, std::int64_t user_id, const std::string& title,
              const std::string& model) override;
  bool exists(const std::string& session_id) const override;
  bool owns(std::int64_t user_id, bool is_admin,
            const std::string& session_id) const override;
  std::int64_t append(const std::string& session_id, const llm::Message& message) override;
  std::vector<llm::Message> history(const std::string& session_id,
                                    std::size_t limit) const override;
  std::size_t message_count(const std::string& session_id) const override;
  std::size_t session_count() const override;
  std::optional<llm::Message> message(std::int64_t message_id) const override;
  std::optional<SessionInfo> get(const std::string& session_id) const override;
  std::vector<SessionInfo> list(std::int64_t user_id, bool is_admin, std::size_t limit,
                                std::size_t offset) const override;
  bool rename(std::int64_t user_id, bool is_admin, const std::string& session_id,
              const std::string& title) override;
  bool remove(std::int64_t user_id, bool is_admin,
              const std::string& session_id) override;

 private:
  struct StoredMessage {
    std::int64_t id = 0;
    llm::Message message;
  };

  struct Session {
    SessionInfo info;
    std::vector<StoredMessage> messages;
    std::list<std::string>::iterator lru_it;
  };

  void evict_if_needed();
  Session* find_locked(const std::string& session_id);
  const Session* find_locked(const std::string& session_id) const;
  void touch_locked(Session& session);

  mutable std::mutex mutex_;
  std::unordered_map<std::string, Session> sessions_;
  std::list<std::string> lru_;  // 头部 = 最近使用
  std::size_t max_messages_;
  std::size_t max_sessions_;
  std::int64_t next_message_id_ = 1;
};

}  // namespace chat
