// request_id 幂等：同一 scope + request_id 只执行一次，重复请求返回已保存的结果。
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "chat/turn_observer.h"

namespace chat {

enum class DedupStatus {
  Claimed,     // 本次抢到执行权
  InProgress,  // 另一个请求正在执行
  Completed,   // 已完成，可以重放
  Failed,      // 上一次失败（claim() 会重新抢执行权）
};

struct DedupRecord {
  DedupStatus status = DedupStatus::InProgress;
  std::string session_id;
  std::int64_t assistant_message_id = 0;
  std::string error_type;
  std::string error_message;
};

class IdempotencyStore {
 public:
  virtual ~IdempotencyStore() = default;

  /// 抢执行权；Failed 记录会被重置为 in_progress 并返回 Claimed。
  virtual DedupRecord claim(const std::string& scope, const std::string& request_id) = 0;

  virtual void complete(const std::string& scope, const std::string& request_id,
                        const std::string& session_id,
                        std::int64_t assistant_message_id) = 0;

  virtual void fail(const std::string& scope, const std::string& request_id,
                    const std::string& error_type, const std::string& error_message) = 0;

  virtual void remove(const std::string& scope, const std::string& request_id) = 0;
};

class MemoryIdempotencyStore final : public IdempotencyStore {
 public:
  DedupRecord claim(const std::string& scope, const std::string& request_id) override;
  void complete(const std::string& scope, const std::string& request_id,
                const std::string& session_id,
                std::int64_t assistant_message_id) override;
  void fail(const std::string& scope, const std::string& request_id,
            const std::string& error_type, const std::string& error_message) override;
  void remove(const std::string& scope, const std::string& request_id) override;

 private:
  using Key = std::pair<std::string, std::string>;
  std::mutex mutex_;
  std::map<Key, DedupRecord> records_;
};

/// 把 RecordingStream 的完成/失败写回 IdempotencyStore。
class IdempotencyObserver final : public TurnObserver {
 public:
  IdempotencyObserver(IdempotencyStore& store, std::string scope, std::string request_id)
      : store_(store), scope_(std::move(scope)), request_id_(std::move(request_id)) {}

  void on_success(const std::string& session_id,
                  std::int64_t assistant_message_id) override;
  void on_failure(const std::string& session_id, const std::string& error) override;

 private:
  IdempotencyStore& store_;
  std::string scope_;
  std::string request_id_;
};

}  // namespace chat
