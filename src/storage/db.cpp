#include "storage/db.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "common/log.h"

namespace storage {
namespace {

std::string sqlstate_of(PGresult* result) {
  if (result == nullptr) return {};
  const char* state = PQresultErrorField(result, PG_DIAG_SQLSTATE);
  return state == nullptr ? std::string{} : std::string(state);
}

std::string error_of(PGconn* conn, PGresult* result) {
  if (result != nullptr) {
    const char* message = PQresultErrorMessage(result);
    if (message != nullptr && *message != '\0') return message;
  }
  if (conn != nullptr) {
    const char* message = PQerrorMessage(conn);
    if (message != nullptr && *message != '\0') return message;
  }
  return "PostgreSQL 操作失败（无错误详情）";
}

}  // namespace

DbError::DbError(const std::string& message, std::string sqlstate)
    : std::runtime_error(message), sqlstate_(std::move(sqlstate)) {}

Result::Result(PGresult* result) : result_(result) {}

Result::~Result() {
  if (result_ != nullptr) PQclear(result_);
}

Result::Result(Result&& other) noexcept : result_(other.result_) {
  other.result_ = nullptr;
}

Result& Result::operator=(Result&& other) noexcept {
  if (this != &other) {
    if (result_ != nullptr) PQclear(result_);
    result_ = other.result_;
    other.result_ = nullptr;
  }
  return *this;
}

int Result::rows() const { return result_ == nullptr ? 0 : PQntuples(result_); }

int Result::cols() const { return result_ == nullptr ? 0 : PQnfields(result_); }

bool Result::is_null(int row, int col) const {
  return result_ == nullptr || PQgetisnull(result_, row, col) != 0;
}

std::optional<std::string> Result::optional(int row, int col) const {
  if (is_null(row, col)) return std::nullopt;
  return std::string(PQgetvalue(result_, row, col),
                     static_cast<std::size_t>(PQgetlength(result_, row, col)));
}

std::string Result::value(int row, int col) const {
  auto text = optional(row, col);
  if (!text.has_value()) throw DbError("查询结果字段为 NULL");
  return std::move(*text);
}

long long Result::int64(int row, int col) const {
  const std::string text = value(row, col);
  char* end = nullptr;
  const long long parsed = std::strtoll(text.c_str(), &end, 10);
  if (end == text.c_str()) throw DbError("查询结果不是整数: " + text);
  return parsed;
}

int Result::affected_rows() const {
  if (result_ == nullptr) return 0;
  const char* tuples = PQcmdTuples(result_);
  if (tuples == nullptr || *tuples == '\0') return 0;
  return static_cast<int>(std::strtol(tuples, nullptr, 10));
}

std::string Result::sqlstate() const { return sqlstate_of(result_); }

Lease::Lease(DbPool* pool, PGconn* conn) noexcept : pool_(pool), conn_(conn) {}

Lease::~Lease() {
  if (pool_ != nullptr && conn_ != nullptr) pool_->release(conn_);
}

Lease::Lease(Lease&& other) noexcept : pool_(other.pool_), conn_(other.conn_) {
  other.pool_ = nullptr;
  other.conn_ = nullptr;
}

bool Lease::ok() const noexcept {
  return conn_ != nullptr && PQstatus(conn_) == CONNECTION_OK;
}

Result Lease::exec(const std::string& sql,
                   const std::vector<std::optional<std::string>>& params) {
  if (conn_ == nullptr) throw DbError("PostgreSQL 连接已释放");

  std::vector<const char*> values(params.size(), nullptr);
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (params[i].has_value()) values[i] = params[i]->c_str();
  }

  PGresult* result = PQexecParams(conn_, sql.c_str(), static_cast<int>(params.size()), nullptr,
                                  values.empty() ? nullptr : values.data(), nullptr, nullptr, 0);
  if (result == nullptr) {
    throw DbError(error_of(conn_, nullptr));
  }

  const ExecStatusType status = PQresultStatus(result);
  if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
    const std::string message = error_of(conn_, result);
    const std::string state = sqlstate_of(result);
    PQclear(result);
    throw DbError(message, state);
  }
  return Result(result);
}

Result Lease::exec_simple(const std::string& sql) {
  if (conn_ == nullptr) throw DbError("PostgreSQL 连接已释放");
  PGresult* result = PQexec(conn_, sql.c_str());
  if (result == nullptr) {
    throw DbError(error_of(conn_, nullptr));
  }
  const ExecStatusType status = PQresultStatus(result);
  if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
    const std::string message = error_of(conn_, result);
    const std::string state = sqlstate_of(result);
    PQclear(result);
    throw DbError(message, state);
  }
  return Result(result);
}

DbPool::DbPool(const DbConfig& config) {
  if (config.dsn.empty()) throw DbError("AIP_DB_DSN 为空");
  pool_size_ = std::clamp(config.pool_size, 1, 64);
  connect_timeout_s_ = std::clamp(config.connect_timeout_s, 1, 60);
  dsn_ = with_connect_timeout(config.dsn);
  free_.reserve(static_cast<std::size_t>(pool_size_));

  try {
    for (int i = 0; i < pool_size_; ++i) {
      free_.push_back(connect_one());
    }
  } catch (...) {
    for (PGconn* conn : free_) PQfinish(conn);
    free_.clear();
    throw;
  }
  total_ = free_.size();
  platform::log_info("PostgreSQL 连接池就绪: " + std::to_string(pool_size_) + " 条连接");
}

DbPool::~DbPool() {
  std::vector<PGconn*> connections;
  {
    std::lock_guard<std::mutex> lock(mu_);
    closed_ = true;
    connections.swap(free_);
    total_ = 0;
  }
  cv_.notify_all();
  for (PGconn* conn : connections) PQfinish(conn);
}

std::string DbPool::with_connect_timeout(std::string dsn) const {
  if (dsn.find("connect_timeout=") != std::string::npos) return dsn;
  const std::string timeout = "connect_timeout=" + std::to_string(connect_timeout_s_);
  if (dsn.find("://") != std::string::npos) {
    const char separator = (dsn.find('?') == std::string::npos) ? '?' : '&';
    dsn.push_back(separator);
    dsn += timeout;
  } else {
    if (!dsn.empty() && dsn.back() != ' ') dsn.push_back(' ');
    dsn += timeout;
  }
  return dsn;
}

PGconn* DbPool::connect_one() {
  PGconn* conn = PQconnectdb(dsn_.c_str());
  if (conn == nullptr) throw DbError("PQconnectdb 返回空指针");
  if (PQstatus(conn) != CONNECTION_OK) {
    const std::string message = error_of(conn, nullptr);
    PQfinish(conn);
    throw DbError("连接 PostgreSQL 失败: " + message);
  }
  if (PQsetClientEncoding(conn, "UTF8") != 0) {
    const std::string message = error_of(conn, nullptr);
    PQfinish(conn);
    throw DbError("设置 PostgreSQL 客户端编码失败: " + message);
  }
  return conn;
}

Lease DbPool::acquire() {
  std::unique_lock<std::mutex> lock(mu_);
  cv_.wait(lock, [this] {
    return closed_ || !free_.empty() || total_ < static_cast<std::size_t>(pool_size_);
  });
  if (closed_) throw DbError("PostgreSQL 连接池已关闭");

  // 连接因故障被丢弃后，允许补建到 pool_size_，避免池永久变空。
  if (free_.empty()) {
    ++total_;  // 先占名额，防止并发请求一起补建
    lock.unlock();
    try {
      PGconn* conn = connect_one();
      return Lease(this, conn);
    } catch (...) {
      lock.lock();
      --total_;
      lock.unlock();
      cv_.notify_all();
      throw;
    }
  }

  PGconn* conn = free_.back();
  free_.pop_back();

  if (PQstatus(conn) != CONNECTION_OK) {
    platform::log_warn("PostgreSQL 连接失效，尝试重置");
    PQreset(conn);
    if (PQstatus(conn) != CONNECTION_OK) {
      const std::string message = error_of(conn, nullptr);
      PQfinish(conn);
      --total_;
      lock.unlock();
      cv_.notify_all();
      throw DbError("PostgreSQL 重连失败: " + message);
    }
  }
  return Lease(this, conn);
}

void DbPool::release(PGconn* conn) noexcept {
  if (conn == nullptr) return;
  bool dropped = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (closed_) {
      PQfinish(conn);
      return;
    }
    if (PQstatus(conn) != CONNECTION_OK) {
      PQreset(conn);
      if (PQstatus(conn) != CONNECTION_OK) {
        platform::log_warn("PostgreSQL 连接归还时已失效，丢弃该连接");
        PQfinish(conn);
        --total_;
        dropped = true;
      }
    }
    if (!dropped) free_.push_back(conn);
  }
  if (dropped) {
    cv_.notify_all();
  } else {
    cv_.notify_one();
  }
}

Result DbPool::exec(const std::string& sql,
                    const std::vector<std::optional<std::string>>& params) {
  Lease lease = acquire();
  return lease.exec(sql, params);
}

Transaction::Transaction(Lease lease) : lease_(std::move(lease)) {
  lease_.exec("BEGIN");
}

Transaction::~Transaction() {
  if (!active_) return;
  try {
    lease_.exec("ROLLBACK");
  } catch (const std::exception& e) {
    platform::log_warn(std::string("PostgreSQL ROLLBACK 失败: ") + e.what());
  }
}

Result Transaction::exec(const std::string& sql,
                         const std::vector<std::optional<std::string>>& params) {
  if (!active_) throw DbError("事务已提交，不能继续执行 SQL");
  return lease_.exec(sql, params);
}

void Transaction::commit() {
  if (!active_) return;
  lease_.exec("COMMIT");
  active_ = false;
}

}  // namespace storage
