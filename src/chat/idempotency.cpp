#include "chat/idempotency.h"

#include <utility>

#include "common/log.h"

namespace chat {

DedupRecord MemoryIdempotencyStore::claim(const std::string& scope,
                                          const std::string& request_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const Key key{scope, request_id};
  auto it = records_.find(key);
  if (it == records_.end()) {
    DedupRecord record;
    record.status = DedupStatus::InProgress;
    records_.emplace(key, record);
    record.status = DedupStatus::Claimed;
    return record;
  }
  if (it->second.status == DedupStatus::Failed) {
    it->second.status = DedupStatus::InProgress;
    it->second.error_type.clear();
    it->second.error_message.clear();
    DedupRecord record = it->second;
    record.status = DedupStatus::Claimed;
    return record;
  }
  return it->second;
}

void MemoryIdempotencyStore::complete(const std::string& scope, const std::string& request_id,
                                      const std::string& session_id,
                                      std::int64_t assistant_message_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  DedupRecord& record = records_[Key{scope, request_id}];
  record.status = DedupStatus::Completed;
  record.session_id = session_id;
  record.assistant_message_id = assistant_message_id;
  record.error_type.clear();
  record.error_message.clear();
}

void MemoryIdempotencyStore::fail(const std::string& scope, const std::string& request_id,
                                  const std::string& error_type,
                                  const std::string& error_message) {
  std::lock_guard<std::mutex> lock(mutex_);
  DedupRecord& record = records_[Key{scope, request_id}];
  record.status = DedupStatus::Failed;
  record.error_type = error_type;
  record.error_message = error_message;
}

void MemoryIdempotencyStore::remove(const std::string& scope, const std::string& request_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  records_.erase(Key{scope, request_id});
}

void IdempotencyObserver::on_success(const std::string& session_id,
                                     std::int64_t assistant_message_id) {
  try {
    store_.complete(scope_, request_id_, session_id, assistant_message_id);
  } catch (const std::exception& e) {
    platform::log_error("幂等完成状态写入失败: " + std::string(e.what()));
  }
}

void IdempotencyObserver::on_failure(const std::string& session_id, const std::string& error) {
  try {
    store_.fail(scope_, request_id_, "upstream_error", error.empty() ? "unknown" : error);
  } catch (const std::exception& e) {
    platform::log_error("幂等失败状态写入失败: " + std::string(e.what()));
  }
}

}  // namespace chat
