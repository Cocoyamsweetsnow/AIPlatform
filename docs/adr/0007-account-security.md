# 0007 账号安全闭环：refresh 轮换、邮箱验证、找回密码与登录限流

- 状态：**已落地（M6）**
- 日期：2026-11-09
- 决策者：项目负责人（AI 按 P0 清单执行）
- 影响范围：`src/auth/`、`src/http/`、`src/storage/migrations.cpp`、`src/config.*`、`src/main.cpp`、`web/`

## 背景

M2 的 JWT 是无状态的：refresh token 不能单条吊销，改密后旧 token 仍有效；
注册/登录公开且没有限流，也没有邮箱验证和找回密码。这些是公网部署前的硬门槛。

## 决定

1. **refresh token 轮换 + 服务端吊销**
   - JWT 增加 `jti`；`refresh_tokens` 表记录 `jti/user_id/expires_at/revoked_at/replaced_by`。
   - `POST /v1/auth/refresh` 用一条 CTE 原子地“吊销旧 jti + 写入新 jti”，失败即 401。
   - `POST /v1/auth/logout` 吊销当前 refresh token；重复调用幂等。
2. **token 版本**
   - `users.token_version`；JWT 带 `ver`，每次 JWT 鉴权与用户记录比较。
   - 改密/重置密码把 `token_version + 1` 并吊销该用户全部 refresh token，
     旧 access token 立即失效；改密接口返回新 token 对。
3. **邮箱验证（可选）**
   - `AIP_EMAIL_VERIFICATION_REQUIRED=1` 时注册返回 201 + `verification_required`，
     不签发 token；未验证登录返回 403 `email_not_verified`。
   - 一次性 token 只存 SHA-256；`one_time_tokens` 表带 purpose/expires_at/used_at。
4. **找回密码**
   - `POST /v1/auth/password/forgot` 对不存在的邮箱也返回 202（防枚举）。
   - `POST /v1/auth/password/reset` 原子消费 token、更新密码、踢下线。
5. **限流**
   - `rate_limits(bucket, window_start, count)` 固定窗口；登录/注册/找回密码
     按 IP + 邮箱计数，多副本共享同一张表。
   - 超限 429 + `Retry-After`；`AIP_AUTH_*_RPM=0` 可关闭。
6. **恒定时间**
   - 用户不存在时也执行一次 Argon2 假哈希，避免用响应时间枚举邮箱。
7. **邮件发送**
   - `EmailSender` 接口；生产用 `smtp://`/`smtps://`（libcurl），未配置时用日志发送器。
   - 邮件链接指向内置 `/account.html?mode=verify|reset&token=...` 落地页。
8. **不引入**：httpOnly cookie/BFF、MFA、OIDC/SSO。前端 token 仍存 localStorage，
   生产建议由 BFF 或网关层补 CSRF/HttpOnly；这属于部署形态决策，未在本次强行改造。

## 后果

**正面**

- refresh token 可轮换/吊销，改密后旧 token 立即失效；
- 邮箱验证与找回密码可用，注册/登录有跨副本限流；
- 一次性 token 只存哈希，数据库泄露不能直接重置密码；
- 错误码明确：`invalid_token`、`email_not_verified`、`rate_limited`、`email_unavailable`。

**限制 / 注意**

- 邮件发送依赖 SMTP；未配置时只写日志，不能用于公网；
- 固定窗口限流在窗口边界允许两倍突发；需要更平滑时换滑动窗口/Redis；
- `rate_limits`、`refresh_tokens`、`one_time_tokens` 目前靠 TTL 语义和低频清理，
  长期需要定时归档/删除任务；
- API Key 不随 token_version 失效（它是独立的显式凭据）；如需“改密吊销所有凭据”，
  要额外吊销 api_keys；
- 管理员禁用用户仍只有数据库字段，没有 admin API/页面（P1 管理后台）。

## 验收记录（2026-11-09）

| 用例 | 结果 |
|---|---|
| `auth_tests` | 轮换/重放 401、logout、改密旧 access 失效、找回密码一次性、邮箱验证、限流 Retry-After 全部通过 |
| `pg_tests` | 迁移 v2 可重复执行；轮换、token_version、一次性 token、`PgRateLimiter` 通过 |
| httplib + PG | 注册/登录/refresh/改密/找回密码/重置/邮箱验证全链路通过；第 3 次登录返回 429 + `Retry-After` |
| Drogon + PG | 新增注册/找回/验证/登出/account 页面冒烟通过 |
| Web | 注册返回 `verification_required` 时提示验证；新增忘记密码入口与 account 落地页 |
