#include "chat/session_store.h"

#include <algorithm>
#include <utility>

#include "util/id.h"

namespace chat {

SessionStore::SessionStore(std::size_t max_messages_per_session, std::size_t max_sessions)
    : max_messages_(max_messages_per_session == 0 ? 200 : max_messages_per_session),
      max_sessions_(max_sessions == 0 ? 10000 : max_sessions) {}

std::string SessionStore::create() {
  const std::string id = util::random_id("s_");
  std::lock_guard<std::mutex> lock(mutex_);
  evict_if_needed();
  lru_.push_front(id);
  Session session;
  session.updated_at = util::now_ms();
  session.lru_it = lru_.begin();
  sessions_.emplace(id, std::move(session));
  return id;
}

bool SessionStore::exists(const std::string& session_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sessions_.find(session_id) != sessions_.end();
}

void SessionStore::append(const std::string& session_id, const llm::Message& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    // 客户端指定了未知 session_id：隐式创建，避免前端拿到 404 后还要重试。
    evict_if_needed();
    lru_.push_front(session_id);
    Session session;
    session.lru_it = lru_.begin();
    it = sessions_.emplace(session_id, std::move(session)).first;
  } else {
    lru_.splice(lru_.begin(), lru_, it->second.lru_it);
  }
  auto& session = it->second;
  session.updated_at = util::now_ms();
  session.messages.push_back(message);
  if (session.messages.size() > max_messages_) {
    const std::size_t overflow = session.messages.size() - max_messages_;
    session.messages.erase(session.messages.begin(),
                           session.messages.begin() + static_cast<long>(overflow));
  }
}

std::vector<llm::Message> SessionStore::history(const std::string& session_id,
                                                std::size_t limit) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = sessions_.find(session_id);
  if (it == sessions_.end()) return {};

  const auto& all = it->second.messages;
  const std::size_t take = std::min(limit == 0 ? all.size() : limit, all.size());
  std::vector<llm::Message> out(all.end() - static_cast<long>(take), all.end());

  // 上下文不能以 assistant 开头（部分上游会报 400），裁掉开头的 assistant。
  while (!out.empty() && out.front().role != "user" && out.front().role != "system") {
    out.erase(out.begin());
  }
  return out;
}

std::size_t SessionStore::message_count(const std::string& session_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = sessions_.find(session_id);
  return it == sessions_.end() ? 0 : it->second.messages.size();
}

std::size_t SessionStore::session_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sessions_.size();
}

void SessionStore::evict_if_needed() {
  while (sessions_.size() >= max_sessions_ && !lru_.empty()) {
    const std::string victim = lru_.back();
    lru_.pop_back();
    sessions_.erase(victim);
  }
}

}  // namespace chat
