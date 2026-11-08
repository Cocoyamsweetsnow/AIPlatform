# libpq 客户端头文件（vendored）

本目录只包含编译期需要的 **PostgreSQL 客户端头文件**，不包含 libpq 实现。
运行时链接系统里的 `libpq.so.5`（Debian/Ubuntu 包名 `libpq5`）。

- 上游：<https://github.com/postgres/postgres>
- 版本：`REL_16_STABLE`
- 文件：
  - `include/libpq-fe.h`（`src/interfaces/libpq/libpq-fe.h`）
  - `include/postgres_ext.h`（`src/include/postgres_ext.h`）
  - `include/pg_config_ext.h`（上游构建期生成；这里只保留 LP64 需要的 `PG_INT64_TYPE`）
- 许可证：PostgreSQL License（与 PostgreSQL 源码一致）

## 为什么 vendored

目标机器上经常只装了 `libpq5` 运行库，没有 `libpq-dev`，而本项目
希望“克隆下来就能构建”。只复制公开头文件可以避免要求 root 安装开发包。

## 兼容性

客户端头文件版本与服务端版本不需要完全一致；本项目只使用
`PQconnectdb` / `PQexecParams` / `PQresultStatus` 等稳定 API。
如果系统只有 `libpq.so.5` 没有 `libpq.so`，CMake 会直接找 `.so.5`
（见根目录 `CMakeLists.txt` 的 `AIPLATFORM_WITH_POSTGRES` 段）。

Windows/LLP64 构建时把 `pg_config_ext.h` 里的 `PG_INT64_TYPE` 改成
`long long`。
