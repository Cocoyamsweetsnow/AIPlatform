// 数据库迁移：只增不改，按 version 顺序执行。
//
// 这里把 SQL 直接编进二进制，部署时不需要额外的 migrations/ 目录；
// 等迁移多到需要人工审阅时，再换成 CMake 嵌入的 .sql 文件。
#pragma once

namespace storage {

class DbPool;

/// 连接数据库后执行所有未应用的迁移；并发安全（pg_advisory_lock）。
void run_migrations(DbPool& pool);

}  // namespace storage
