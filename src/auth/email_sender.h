// 邮件发送：验证邮箱 / 重置密码。
//
// 生产配 AIP_SMTP_URL（smtp:// 或 smtps://，支持 URL 内 user:pass）；
// 未配置时用 LogEmailSender，只把链接写日志，适合本地/测试，不要用于公网。
#pragma once

#include <memory>
#include <string>

namespace auth {

struct EmailMessage {
  std::string to;
  std::string subject;
  std::string body;
};

class EmailSender {
 public:
  virtual ~EmailSender() = default;
  /// 返回 false 时 error 带原因；调用方只记日志，不向客户端泄露。
  virtual bool send(const EmailMessage& message, std::string& error) = 0;
};

std::unique_ptr<EmailSender> make_log_email_sender();

/// url 形如 smtp://user:pass@smtp.example.com:587；starttls=true 时用 STARTTLS。
std::unique_ptr<EmailSender> make_smtp_email_sender(std::string url, std::string from,
                                                    bool starttls);

}  // namespace auth
