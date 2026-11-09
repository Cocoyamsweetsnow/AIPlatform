// 运行期配置：全部来自环境变量，便于容器化部署（12-Factor）。
// 后续可平滑替换为 YAML/TOML + 热加载，接口不变。
#pragma once

#include <string>
#include <vector>

namespace platform {

struct Config {
  // 服务
  std::string host{"0.0.0.0"};
  int port{8080};
  std::string api_token;  // 为空表示不鉴权（仅本地开发）
  bool web_enabled{true};  // 是否提供内置 Web UI（AIP_WEB=false 可切成纯 API 模式）
  std::string log_format{"json"};  // text | json
  bool metrics_enabled{true};      // AIP_METRICS_ENABLED
  std::string metrics_token;       // 非空时 /metrics 需要 Bearer Token
  bool trust_proxy{false};         // 信任 X-Forwarded-For（部署在 Nginx 后时开启）
  bool usage_async{true};          // usage 流水异步写；false = 流结束时同步写

  // 上游模型
  std::string provider{"mock"};  // mock | openai
  std::string base_url{"https://api.deepseek.com/v1"};
  std::string api_key;                 // AIP_API_KEY（单 key，兼容旧配置）
  std::vector<std::string> api_keys;   // AIP_API_KEYS（逗号分隔，非空时优先）
  std::string model{"deepseek-chat"};
  std::vector<std::string> models;  // AIP_MODELS；为空表示只暴露/允许 AIP_MODEL
  int request_timeout_s{120};
  int max_tokens{1024};
  double temperature{0.7};
  bool include_usage{true};
  int max_retries{2};        // 上游失败重试次数（仅首 token 前）
  int retry_base_ms{300};    // 指数退避基准；实际单次最多 5 秒

  // 多 Key 治理（0 = 不限；仅 openai provider 使用）
  int key_rpm_limit{0};          // 每个 Key 每分钟请求数
  int key_tpm_limit{0};          // 每个 Key 每分钟 token 数
  int key_failure_threshold{3};  // 连续失败多少次进入冷却
  int key_cooldown_s{15};        // 首次冷却秒数
  int key_cooldown_max_s{300};   // 冷却上限

  // 对话
  std::string system_prompt{"你是一个乐于助人的 AI 助手，回答简洁准确。"};
  int max_history_messages{20};
  int max_context_tokens{8192};
  int max_sessions{10000};

  // 持久化（阶段 3）
  std::string storage{"memory"};  // memory | postgres
  std::string db_dsn;             // AIP_DB_DSN，storage=postgres 时必填
  int db_pool_size{8};            // 1~64
  int db_connect_timeout_s{5};    // 1~60

  // 用户鉴权（阶段 4）
  bool auth_required{false};          // AIP_AUTH_REQUIRED；强制所有非公开接口鉴权
  std::string jwt_secret;             // AIP_JWT_SECRET；生产必须显式设置且多副本一致
  std::string jwt_issuer{"aiplatform"};
  int jwt_ttl_s{900};                 // access token 15 分钟
  int jwt_refresh_ttl_s{7 * 24 * 3600};

  // 账号安全（M6）
  bool email_verification_required{false};
  std::string public_base_url{"http://127.0.0.1:8080"};
  std::string smtp_url;
  std::string smtp_from{"noreply@aiplatform.local"};
  bool smtp_starttls{true};
  int auth_login_rpm{20};     // 每 IP 每分钟登录尝试；0 = 不限
  int auth_register_rpm{10};  // 每 IP 每分钟注册尝试
  int auth_email_rpm{5};      // 每邮箱每分钟找回密码/验证邮件
  int email_verify_ttl_s{86400};
  int password_reset_ttl_s{1800};

  // 配额（阶段 4；0 = 不限）
  int rpm_limit{60};                  // 每用户每分钟请求数
  int daily_token_limit{1000000};     // 每用户每日 token
  int max_concurrent_per_user{4};     // 每用户并发流
  int quota_lease_s{900};             // PG 配额预留/并发 lease TTL
  int idempotency_ttl_s{180};         // in_progress 超过该时间视为崩溃，可重试

  // HTTP 层（Drogon 后端；httplib 回退实现只用 heartbeat_s）
  int io_threads{0};         // 0 = 按核数自动（2~8）
  int max_streams{512};      // 同时在途 SSE 流上限，超出直接 503
  int heartbeat_s{15};       // SSE 心跳 / 断连探测周期；最坏取消时延 = 一个周期
  int shutdown_drain_s{30};  // SIGTERM 后等待在途流的最长时间

  static Config from_env();

  bool auth_enabled() const {
    return auth_required || !api_token.empty() || !jwt_secret.empty();
  }
  bool postgres_enabled() const { return storage == "postgres"; }
};

}  // namespace platform
