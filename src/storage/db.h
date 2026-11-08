// PostgreSQL 连接池与薄封装（libpq）。
//
// 设计目标：
//   - 只暴露 std::string / optional 参数，业务层不直接碰 PGresult；
//   - 每个线程从池里借一条连接，Lease 析构自动归还；
//   - 出错统一抛 DbError（带 SQLSTATE），HTTP 层可以映射成 503。
//
// 这不是 ORM：SQL 仍然手写，迁移见 storage/migrations.cpp。
#pragma once

#include <libpq-fe.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace storage {

struct DbConfig {
  std::string dsn;              // postgresql://user:pass@host:port/db?connect_timeout=5
  int pool_size = 8;            // 1~64
  int connect_timeout_s = 5;    // 仅用于补齐 DSN 里没写的 connect_timeout
};

class DbError : public std::runtime_error {
 public:
  explicit DbError(const std::string& message, std::string sqlstate = {});
  const std::string& sqlstate() const noexcept { return sqlstate_; }

 private:
  std::string sqlstate_;
};

class Result {
 public:
  Result() = default;
  explicit Result(PGresult* result);
  ~Result();

  Result(const Result&) = delete;
  Result& operator=(const Result&) = delete;
  Result(Result&& other) noexcept;
  Result& operator=(Result&& other) noexcept;

  int rows() const;
  int cols() const;
  bool is_null(int row, int col) const;
  std::optional<std::string> optional(int row, int col) const;
  std::string value(int row, int col) const;
  long long int64(int row, int col) const;
  int affected_rows() const;
  std::string sqlstate() const;

 private:
  PGresult* result_ = nullptr;
};

class DbPool;

/// 一条从池里借出的连接；析构时归还。
class Lease {
 public:
  Lease(DbPool* pool, PGconn* conn) noexcept;
  ~Lease();

  Lease(const Lease&) = delete;
  Lease& operator=(const Lease&) = delete;
  Lease(Lease&& other) noexcept;
  Lease& operator=(Lease&& other) = delete;

  PGconn* get() const noexcept { return conn_; }
  bool ok() const noexcept;

  Result exec(const std::string& sql,
              const std::vector<std::optional<std::string>>& params = {});

  /// 简单查询协议：允许一次执行多条语句（迁移脚本用）。
  /// 业务 SQL 请优先用 exec()，参数化可以避免注入。
  Result exec_simple(const std::string& sql);

 private:
  DbPool* pool_ = nullptr;
  PGconn* conn_ = nullptr;
};

class DbPool {
 public:
  explicit DbPool(const DbConfig& config);
  ~DbPool();

  DbPool(const DbPool&) = delete;
  DbPool& operator=(const DbPool&) = delete;

  /// 阻塞直到有空闲连接；池已关闭或连接失败时抛 DbError。
  Lease acquire();

  /// 便捷方法：借一条连接执行一条 SQL。
  Result exec(const std::string& sql,
              const std::vector<std::optional<std::string>>& params = {});

  std::size_t size() const noexcept { return pool_size_; }

 private:
  friend class Lease;
  void release(PGconn* conn) noexcept;
  PGconn* connect_one();
  std::string with_connect_timeout(std::string dsn) const;

  std::string dsn_;
  int pool_size_ = 8;
  int connect_timeout_s_ = 5;
  std::size_t total_ = 0;  // free + 借出中；连接失效时递减，允许补建
  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<PGconn*> free_;
  bool closed_ = false;
};

/// 一个事务；未 commit 时析构自动 ROLLBACK。
class Transaction {
 public:
  explicit Transaction(Lease lease);
  ~Transaction();

  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;
  Transaction(Transaction&&) = delete;
  Transaction& operator=(Transaction&&) = delete;

  Result exec(const std::string& sql,
              const std::vector<std::optional<std::string>>& params = {});
  void commit();

 private:
  Lease lease_;
  bool active_ = true;
};

}  // namespace storage
