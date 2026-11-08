#include "chat/session_store.h"

#include <algorithm>
#include <utility>

#include "chat/errors.h"
#include "util/id.h"

namespace chat {
namespace {

constexpr std::size_t kDefaultListLimit = 20;
constexpr std::size_t kMaxListLimit = 200;

long long now_seconds() { return util::now_ms() / 1000; }

std::string title_or_default(std::string title) {
  if (title.empty()) return "新会话";
  return title;
}

}  // namespace

void trim_context(std::vector<llm::Message>& messages) {
  // 上下文不能以 assistant/tool 开头（部分上游会报 400）。
  while (!messages.empty() && messages.front().role != "user" &&
         messages.front().role != "system") {
    messages.erase(messages.begin());
  }
}

MemorySessionStore::MemorySessionStore(std::size_t max_messages_per_session,
                                       std::size_t max_sessions)
    : max_messages_(max_messages_per_session == 0 ? 200 : max_messages_per_session),
      max_sessions_(max_sessions == 0 ? 1 : max_sessions) {}

std::string MemorySessionStore::create(std::int64_t user_id, const std::string& title,
                                       const std::string& model) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string id;
  for (int attempt = 0; attempt < 8; ++attempt) {
    id = util::random_id("s_");
    if (sessions_.find(id) == sessions_.end()) break;
    id.clear();
  }
  if (id.empty()) throw std::runtime_error("生成会话 ID 失败");

  evict_if_needed();
  lru_.push_front(id);
  Session session;
  session.info.id = id;
  session.info.user_id = user_id;
  session.info.title = title_or_default(title);
  session.info.model = model;
  session.info.created_at = now_seconds();
  session.info.updated_at = session.info.created_at;
  session.lru_it = lru_.begin();
  sessions_.emplace(id, std::move(session));
  return id;
}

bool MemorySessionStore::ensure(const std::string& session_id, std::int64_t user_id,
                                const std::string& title, const std::string& model) {
  if (session_id.empty()) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (sessions_.find(session_id) != sessions_.end()) return false;

  evict_if_needed();
  lru_.push_front(session_id);
  Session session;
  session.info.id = session_id;
  session.info.user_id = user_id;
  session.info.title = title_or_default(title);
  session.info.model = model;
  session.info.created_at = now_seconds();
  session.info.updated_at = session.info.created_at;
  session.lru_it = lru_.begin();
  sessions_.emplace(session_id, std::move(session));
  return true;
}

bool MemorySessionStore::exists(const std::string& session_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sessions_.find(session_id) != sessions_.end();
}

bool MemorySessionStore::owns(std::int64_t user_id, bool is_admin,
                              const std::string& session_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = sessions_.find(session_id);
  if (it == sessions_.end()) return false;
  if (is_admin) return true;
  return it->second.info.user_id != 0 && it->second.info.user_id == user_id;
}

std::int64_t MemorySessionStore::append(const std::string& session_id,
                                        const llm::Message& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  Session* session = find_locked(session_id);
  if (session == nullptr) throw SessionNotFound(session_id);

  const std::int64_t message_id = next_message_id_++;
  session->messages.push_back(StoredMessage{message_id, message});
  if (session->messages.size() > max_messages_) {
    const std::size_t overflow = session->messages.size() - max_messages_;
    session->messages.erase(session->messages.begin(),
                            session->messages.begin() + static_cast<long>(overflow));
  }
  session->info.updated_at = now_seconds();
  touch_locked(*session);
  return message_id;
}

std::vector<llm::Message> MemorySessionStore::history(const std::string& session_id,
                                                      std::size_t limit) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const Session* session = find_locked(session_id);
  if (session == nullptr) return {};

  const auto& all = session->messages;
  const std::size_t take = std::min(limit == 0 ? all.size() : limit, all.size());
  std::vector<llm::Message> out;
  out.reserve(take);
  for (std::size_t i = all.size() - take; i < all.size(); ++i) {
    out.push_back(all[i].message);
  }
  return out;
}

std::size_t MemorySessionStore::message_count(const std::string& session_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const Session* session = find_locked(session_id);
  return session == nullptr ? 0 : session->messages.size();
}

std::size_t MemorySessionStore::session_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sessions_.size();
}

std::optional<llm::Message> MemorySessionStore::message(std::int64_t message_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [id, session] : sessions_) {
    (void)id;
    for (const auto& stored : session.messages) {
      if (stored.id == message_id) return stored.message;
    }
  }
  return std::nullopt;
}

std::optional<SessionInfo> MemorySessionStore::get(const std::string& session_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const Session* session = find_locked(session_id);
  if (session == nullptr) return std::nullopt;
  SessionInfo info = session->info;
  info.message_count = session->messages.size();
  return info;
}

std::vector<SessionInfo> MemorySessionStore::list(std::int64_t user_id, bool is_admin,
                                                  std::size_t limit,
                                                  std::size_t offset) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<SessionInfo> out;
  for (const auto& [id, session] : sessions_) {
    (void)id;
    if (!is_admin && (session.info.user_id == 0 || session.info.user_id != user_id)) {
      continue;
    }
    SessionInfo info = session.info;
    info.message_count = session.messages.size();
    out.push_back(std::move(info));
  }

  std::sort(out.begin(), out.end(), [](const SessionInfo& a, const SessionInfo& b) {
    if (a.updated_at != b.updated_at) return a.updated_at > b.updated_at;
    return a.id < b.id;
  });

  const std::size_t take = std::min(limit == 0 ? kDefaultListLimit : limit, kMaxListLimit);
  if (offset >= out.size()) return {};
  const std::size_t end = std::min(out.size(), offset + take);
  return std::vector<SessionInfo>(out.begin() + static_cast<long>(offset),
                                  out.begin() + static_cast<long>(end));
}

bool MemorySessionStore::rename(std::int64_t user_id, bool is_admin,
                                const std::string& session_id, const std::string& title) {
  std::lock_guard<std::mutex> lock(mutex_);
  Session* session = find_locked(session_id);
  if (session == nullptr) return false;
  if (!is_admin && (session->info.user_id == 0 || session->info.user_id != user_id)) {
    return false;
  }
  session->info.title = title_or_default(title);
  session->info.updated_at = now_seconds();
  return true;
}

bool MemorySessionStore::remove(std::int64_t user_id, bool is_admin,
                                const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = sessions_.find(session_id);
  if (it == sessions_.end()) return false;
  if (!is_admin && (it->second.info.user_id == 0 || it->second.info.user_id != user_id)) {
    return false;
  }
  lru_.erase(it->second.lru_it);
  sessions_.erase(it);
  return true;
}

void MemorySessionStore::evict_if_needed() {
  while (sessions_.size() >= max_sessions_ && !lru_.empty()) {
    const std::string victim = lru_.back();
    lru_.pop_back();
    sessions_.erase(victim);
  }
}

MemorySessionStore::Session* MemorySessionStore::find_locked(const std::string& session_id) {
  const auto it = sessions_.find(session_id);
  return it == sessions_.end() ? nullptr : &it->second;
}

const MemorySessionStore::Session* MemorySessionStore::find_locked(
    const std::string& session_id) const {
  const auto it = sessions_.find(session_id);
  return it == sessions_.end() ? nullptr : &it->second;
}

void MemorySessionStore::touch_locked(Session& session) {
  lru_.splice(lru_.begin(), lru_, session.lru_it);
}

}  // namespace chat
